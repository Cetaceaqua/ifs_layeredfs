#include "txp2.hpp"

#include <cstring>
#include <fstream>
#include <algorithm>

#include "3rd_party/libsquish/squish.h"
#include "3rd_party/lodepng.h"
#include "3rd_party/stb_dxt.h"

#include "avs.hpp"
#include "log.hpp"
#include "utils.hpp"

static inline uint32_t read_u32_be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) | (uint32_t(p[1]) << 8) | uint32_t(p[0]);
}

static inline uint16_t read_u16_be(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

static inline uint16_t read_u16_le(const uint8_t* p) {
    return (uint16_t(p[1]) << 8) | uint16_t(p[0]);
}

static inline void write_u32_be(uint8_t* p, uint32_t v) {
    p[0] = uint8_t((v >> 24) & 0xFF);
    p[1] = uint8_t((v >> 16) & 0xFF);
    p[2] = uint8_t((v >> 8) & 0xFF);
    p[3] = uint8_t(v & 0xFF);
}

static inline void write_u32_le(uint8_t* p, uint32_t v) {
    p[3] = uint8_t((v >> 24) & 0xFF);
    p[2] = uint8_t((v >> 16) & 0xFF);
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[0] = uint8_t(v & 0xFF);
}

static inline void write_u16_le(uint8_t* p, uint16_t v) {
    p[1] = uint8_t((v >> 8) & 0xFF);
    p[0] = uint8_t(v & 0xFF);
}

std::optional<Txp2> Txp2::from_stream(std::istream& stream) {
    stream.seekg(0, std::ios::end);
    size_t size = stream.tellg();
    stream.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    stream.read(reinterpret_cast<char*>(buffer.data()), size);
    return from_bytes(std::move(buffer));
}

std::optional<Txp2> Txp2::from_bytes(std::vector<uint8_t> bytes) {
    if (bytes.size() < 32) {
        return std::nullopt;
    }

    bool is_be = false;
    if (bytes[0] == 'T' && bytes[1] == 'X' && bytes[2] == 'P' && bytes[3] == '2') {
        is_be = true;
    } else if (bytes[0] == '2' && bytes[1] == 'P' && bytes[2] == 'X' && bytes[3] == 'T') {
        is_be = false;
    } else {
        return std::nullopt;
    }

    auto r32 = [is_be](const uint8_t* p) -> uint32_t {
        return is_be ? read_u32_be(p) : read_u32_le(p);
    };
    auto r16 = [is_be](const uint8_t* p) -> uint16_t {
        return is_be ? read_u16_be(p) : read_u16_le(p);
    };

    uint32_t total_len = r32(&bytes[12]);
    if (total_len != bytes.size()) {
        log_verbose("Txp2 size mismatch: hdr {} != actual {}", total_len, bytes.size());
    }

    uint32_t features = r32(&bytes[20]);
    if ((features & 0x01) == 0) {
        return std::nullopt;
    }

    uint32_t tex_count = r32(&bytes[24]);
    uint32_t tex_offset = r32(&bytes[28]);

    if (tex_offset + tex_count * 12 > bytes.size()) {
        log_warning("Txp2 texture table exceeds file length");
        return std::nullopt;
    }

    Txp2 txp2;
    txp2.is_big_endian = is_be;
    txp2.features = features;
    txp2.raw_container = std::move(bytes);

    bool text_obfuscated = (features & 0x20) != 0;
    bool modern_lz = (features & 0x40000) != 0;

    for (uint32_t i = 0; i < tex_count; ++i) {
        uint32_t entry_off = tex_offset + i * 12;
        uint32_t name_off = r32(&txp2.raw_container[entry_off]);
        uint32_t tex_len = r32(&txp2.raw_container[entry_off + 4]);
        uint32_t tex_data_off = r32(&txp2.raw_container[entry_off + 8]);

        if (name_off >= txp2.raw_container.size() || tex_data_off + tex_len > txp2.raw_container.size()) {
            log_warning("Txp2 texture {} offset out of bounds", i);
            continue;
        }

        std::string raw_name;
        for (size_t s = name_off; s < txp2.raw_container.size() && txp2.raw_container[s] != 0; ++s) {
            raw_name.push_back((char)txp2.raw_container[s]);
        }

        std::string name = raw_name;
        if (text_obfuscated && !raw_name.empty() && (uint8_t(raw_name[0]) - 0x20) > 0x7F) {
            for (size_t c = 0; c < name.size(); ++c) {
                name[c] = (char)((uint8_t(name[c]) + 0x80) & 0xFF);
            }
        }

        Txp2Texture tex;
        tex.index = i;
        tex.name = istring(name);
        tex.entry_offset = entry_off;
        tex.name_offset = name_off;
        tex.orig_len = tex_len;
        tex.orig_data_offset = tex_data_off;
        tex.is_compressed = modern_lz;

        std::vector<uint8_t> tdxt_uncomp;
        if (modern_lz) {
            if (tex_len < 8) {
                log_warning("Txp2 compressed texture too small");
                continue;
            }
            uint32_t inflated_sz = read_u32_be(&txp2.raw_container[tex_data_off]);
            uint32_t deflated_sz = read_u32_be(&txp2.raw_container[tex_data_off + 4]);

            auto decomp = lz_decompress(
                std::span<const uint8_t>(&txp2.raw_container[tex_data_off + 8], deflated_sz),
                inflated_sz);
            if (!decomp || decomp->size() < 64) {
                log_warning("Txp2 decompress failed for {}", name);
                continue;
            }
            tdxt_uncomp = std::move(*decomp);
        } else {
            if (tex_len < 64) {
                log_warning("Txp2 raw texture too small");
                continue;
            }
            tdxt_uncomp.assign(
                txp2.raw_container.begin() + tex_data_off,
                txp2.raw_container.begin() + tex_data_off + std::min<size_t>(tex_len, 64));
        }

        tex.width = r16(&tdxt_uncomp[16]);
        tex.height = r16(&tdxt_uncomp[18]);
        tex.fmtflags = r32(&tdxt_uncomp[20]);
        tex.fmt = (uint8_t)(tex.fmtflags & 0xFF);
        tex.orig_tdxt_header.assign(tdxt_uncomp.begin(), tdxt_uncomp.begin() + 64);

        log_verbose("Txp2 Texture {}: '{}' {}x{} fmt={:#x} len={}", i, name, tex.width, tex.height, tex.fmt, tex_len);

        txp2.texture_name_map[tex.name] = txp2.textures.size();
        txp2.textures.push_back(std::move(tex));
    }

    return txp2;
}

bool Txp2::add_or_replace_image(const char* name, const char* png_path) {
    istring lookup_name(name);
    auto it = texture_name_map.find(lookup_name);
    if (it == texture_name_map.end()) {
        if (textures.size() == 1) {
            it = texture_name_map.begin();
        } else {
            log_warning("Txp2: texture '{}' not found in container", name);
            return false;
        }
    }

    Txp2Texture& tex = textures[it->second];

    std::vector<uint8_t> png_image;
    unsigned png_w = 0, png_h = 0;
    unsigned error = lodepng::decode(png_image, png_w, png_h, png_path, LCT_RGBA);
    if (error) {
        log_warning("Txp2: failed to load PNG '{}': {}", png_path, lodepng_error_text(error));
        return false;
    }

    if (png_w != tex.width || png_h != tex.height) {
        log_warning("Txp2: PNG size {}x{} does not match texture {}x{}", png_w, png_h, tex.width, tex.height);
        return false;
    }

    std::vector<uint8_t> raw_pixels;
    switch (tex.fmt) {
        case 0x20:
            raw_pixels.resize(png_image.size());
            for (size_t i = 0; i < png_image.size(); i += 4) {
                raw_pixels[i + 0] = png_image[i + 2];
                raw_pixels[i + 1] = png_image[i + 1];
                raw_pixels[i + 2] = png_image[i + 0];
                raw_pixels[i + 3] = png_image[i + 3];
            }
            break;

        case 0x0E:
            raw_pixels.resize((png_image.size() / 4) * 3);
            for (size_t i = 0, j = 0; i < png_image.size(); i += 4, j += 3) {
                raw_pixels[j + 0] = png_image[i + 2];
                raw_pixels[j + 1] = png_image[i + 1];
                raw_pixels[j + 2] = png_image[i + 0];
            }
            break;

        case 0x0B:
            raw_pixels.resize((png_image.size() / 4) * 2);
            for (size_t i = 0, j = 0; i < png_image.size(); i += 4, j += 2) {
                uint16_t r = (png_image[i + 0] >> 3) & 0x1F;
                uint16_t g = (png_image[i + 1] >> 2) & 0x3F;
                uint16_t b = (png_image[i + 2] >> 3) & 0x1F;
                uint16_t val = (r << 11) | (g << 5) | b;
                write_u16_le(&raw_pixels[j], val);
            }
            break;

        case 0x13:
            raw_pixels.resize((png_image.size() / 4) * 2);
            for (size_t i = 0, j = 0; i < png_image.size(); i += 4, j += 2) {
                uint16_t a = (png_image[i + 3] >= 128) ? 0x8000 : 0x0000;
                uint16_t r = (png_image[i + 0] >> 3) & 0x1F;
                uint16_t g = (png_image[i + 1] >> 3) & 0x1F;
                uint16_t b = (png_image[i + 2] >> 3) & 0x1F;
                uint16_t val = a | (r << 10) | (g << 5) | b;
                write_u16_le(&raw_pixels[j], val);
            }
            break;

        case 0x1F:
            raw_pixels.resize((png_image.size() / 4) * 2);
            for (size_t i = 0, j = 0; i < png_image.size(); i += 4, j += 2) {
                uint16_t b = (png_image[i + 2] >> 4) & 0x0F;
                uint16_t g = (png_image[i + 1] >> 4) & 0x0F;
                uint16_t r = (png_image[i + 0] >> 4) & 0x0F;
                uint16_t a = (png_image[i + 3] >> 4) & 0x0F;
                uint16_t val = b | (g << 4) | (r << 8) | (a << 12);
                write_u16_le(&raw_pixels[j], val);
            }
            break;

        case 0x16: {
            size_t dxt_size = squish::GetStorageRequirements(png_w, png_h, squish::kDxt1);
            raw_pixels.resize(dxt_size);
            squish::CompressImage(png_image.data(), png_w, png_h, raw_pixels.data(), squish::kDxt1);
            break;
        }

        case 0x1A: {
            size_t dxt_size = png_w * png_h;
            raw_pixels.resize(dxt_size);
            rygCompress(raw_pixels.data(), png_image.data(), png_w, png_h, 1);
            break;
        }

        default:
            log_warning("Txp2: unsupported format {:#x} for {}", tex.fmt, tex.name);
            return false;
    }

    std::vector<uint8_t> tdxt_new = tex.orig_tdxt_header;
    auto w32 = [this](uint8_t* p, uint32_t v) {
        if (is_big_endian) write_u32_be(p, v); else write_u32_le(p, v);
    };

    uint32_t raw_len = 64 + (uint32_t)raw_pixels.size();
    w32(&tdxt_new[12], raw_len);
    tdxt_new.insert(tdxt_new.end(), raw_pixels.begin(), raw_pixels.end());

    if (tex.is_compressed) {
        auto compressed = lz_compress(tdxt_new);
        if (!compressed) {
            log_warning("Txp2: LZ77 compression failed for {}", tex.name);
            return false;
        }

        std::vector<uint8_t> payload(8 + compressed->size());
        write_u32_be(&payload[0], (uint32_t)tdxt_new.size());
        write_u32_be(&payload[4], (uint32_t)compressed->size());
        std::memcpy(&payload[8], compressed->data(), compressed->size());

        tex.replacement_payload = std::move(payload);
    } else {
        std::vector<uint8_t> payload(8 + tdxt_new.size());
        write_u32_be(&payload[0], (uint32_t)tdxt_new.size());
        write_u32_be(&payload[4], (uint32_t)tdxt_new.size());
        std::memcpy(&payload[8], tdxt_new.data(), tdxt_new.size());

        tex.replacement_payload = std::move(payload);
    }

    log_verbose("Txp2: successfully updated texture '{}'", tex.name);
    return true;
}

std::vector<uint8_t> Txp2::serialize() {
    bool has_modifications = false;
    for (const auto& tex : textures) {
        if (tex.replacement_payload.has_value()) {
            has_modifications = true;
            break;
        }
    }

    if (!has_modifications) {
        return raw_container;
    }

    uint32_t first_tex_off = (uint32_t)raw_container.size();
    for (const auto& tex : textures) {
        if (tex.orig_data_offset < first_tex_off) {
            first_tex_off = tex.orig_data_offset;
        }
    }

    std::vector<uint8_t> out;
    out.assign(raw_container.begin(), raw_container.begin() + first_tex_off);

    auto w32 = [this](uint8_t* p, uint32_t v) {
        if (is_big_endian) write_u32_be(p, v); else write_u32_le(p, v);
    };

    for (const auto& tex : textures) {
        while (out.size() % 4 != 0) {
            out.push_back(0);
        }

        uint32_t new_off = (uint32_t)out.size();
        uint32_t new_len = 0;

        if (tex.replacement_payload.has_value()) {
            new_len = (uint32_t)tex.replacement_payload->size();
            out.insert(out.end(), tex.replacement_payload->begin(), tex.replacement_payload->end());
        } else {
            new_len = tex.orig_len;
            out.insert(
                out.end(),
                raw_container.begin() + tex.orig_data_offset,
                raw_container.begin() + tex.orig_data_offset + tex.orig_len);
        }

        w32(&out[tex.entry_offset + 4], new_len);
        w32(&out[tex.entry_offset + 8], new_off);
    }

    w32(&out[12], (uint32_t)out.size());
    return out;
}

bool Txp2::save(const char* path) {
    auto data = serialize();
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    return f.good();
}
