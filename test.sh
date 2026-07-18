#!/usr/bin/env bash

set -eu

# leading 32/64 args pick the arch(es); the rest is passed to meson test
ARCHS=()
while [ $# -gt 0 ] && { [ "$1" = 32 ] || [ "$1" = 64 ]; }; do
    ARCHS+=("$1")
    shift
done
if [ "${#ARCHS[@]}" -eq 0 ]; then ARCHS=(32 64); fi

if [[ " ${ARCHS[*]} " == *" 32 "* ]]; then
    meson test -C build32 --print-errorlogs "$@"
fi

if [[ " ${ARCHS[*]} " == *" 64 "* ]]; then
    meson test -C build64 --print-errorlogs "$@"
fi
