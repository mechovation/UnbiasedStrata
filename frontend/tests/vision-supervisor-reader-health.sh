#!/usr/bin/env bash
# Excluded real-reader/adapter health observation test. Existing CPU assets only.
# Usage: vision-supervisor-reader-health.sh CPU_STATIC_BUILD MODEL_GGUF PROJECTOR_GGUF
# SANITIZE=1 instruments this test and vision_reader.cpp with UBSan, not the supplied libraries.
set -euo pipefail
ulimit -c 0
root=$(cd "$(dirname "$0")/../.." && pwd)
llama=$root/third_party/llama.cpp
build=$(cd "${1:?CPU static build}" && pwd)
model=${2:?existing model}; projector=${3:?existing projector}
for setting in STRATA_ENABLE_CUDA STRATA_FRONTEND_CUDA GGML_CUDA BUILD_SHARED_LIBS; do
    grep -Fxq "$setting:BOOL=OFF" "$build/CMakeCache.txt"
done
grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$build/CMakeCache.txt"
tmp=$(mktemp -d "$build/pr4-reader-health-XXXXXX")
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c++17 -D_GNU_SOURCE -O1 -g -Wall -Wextra -Werror -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=undefined -fno-sanitize-recover=all)
fi
"${CXX:-c++}" "${flags[@]}" \
    -I"$root/frontend" -I"$llama" -I"$llama/tools/mtmd" -I"$llama/include" -I"$llama/ggml/include" \
    -I"$llama/vendor/hash" -I"$llama/vendor/stb" \
    "$root/frontend/tests/vision-supervisor-reader-health.cpp" "$root/frontend/vision_reader.cpp" \
    -o "$tmp/test" -Wl,--start-group \
    "$build/llama.cpp/tools/mtmd/libmtmd.a" "$build/llama.cpp/src/libllama.a" \
    "$build/ggml/src/libggml.a" "$build/ggml/src/libggml-cpu.a" "$build/ggml/src/libggml-base.a" \
    "$build/llama.cpp/vendor/hash/libvendor-hash.a" -Wl,--end-group -ldl -lm \
    -Wl,--wrap=mtmd_encode_chunk -Wl,--wrap=stbi_load_from_memory
"${CXX:-c++}" "${flags[@]}" "$root/frontend/process_guard.cpp" -o "$tmp/guard"
CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void \
    timeout --kill-after=5s 180s "$tmp/test" "$model" "$projector" "$tmp/guard"
