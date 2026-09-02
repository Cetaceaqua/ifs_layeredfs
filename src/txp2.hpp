#pragma once

#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "utils.hpp"

class Txp2Texture {
  public:
    uint32_t index = 0;
    istring name;
    uint32_t entry_offset = 0;
    uint32_t name_offset = 0;
    uint32_t orig_len = 0;
    uint32_t orig_data_offset = 0;

    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t fmt = 0;
    uint32_t fmtflags = 0;
    bool is_compressed = false;
    std::vector<uint8_t> orig_tdxt_header;

    std::optional<std::vector<uint8_t>> replacement_payload = std::nullopt;
};

class Txp2 {
  public:
    bool is_big_endian = true;
    uint32_t features = 0;
    std::vector<uint8_t> raw_container;
    std::vector<Txp2Texture> textures;
    std::map<istring, size_t> texture_name_map;

    static std::optional<Txp2> from_bytes(std::vector<uint8_t> bytes);
    static std::optional<Txp2> from_stream(std::istream& stream);

    bool add_or_replace_image(const char* name, const char* png_path);
    bool save(const char* path);
    std::vector<uint8_t> serialize();
};
