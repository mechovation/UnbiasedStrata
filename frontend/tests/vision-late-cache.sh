#!/usr/bin/env bash
# Focused no-model/no-GPU native Session cache integration; SANITIZE=1 adds UBSan.
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=undefined -fno-sanitize-recover=all)
fi
"${CXX:-g++}" "${flags[@]}" frontend/tests/vision-late-cache.cpp -o "$build/vision-late-cache"
timeout 30s "$build/vision-late-cache"
