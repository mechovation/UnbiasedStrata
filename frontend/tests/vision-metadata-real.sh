#!/usr/bin/env bash
# CPU-only, real public mtmd serialization. Caller supplies existing assets and a disposable build dir.
# Usage: vision-metadata-real.sh BUILD MODEL PROJECTOR FIXTURE_OUTPUT_DIR
set -euo pipefail
ulimit -c 0
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:?build directory}; model=${2:?model}; projector=${3:?projector}; fixtures=${4:?fixture output}
llama=$root/third_party/llama.cpp
pin=44072a19fb94d0bed63d381d42f6237373f91d54
[[ $(git -C "$llama" rev-parse HEAD) == "$pin" ]]
git -C "$llama" diff --quiet HEAD --
mkdir -p "$build" "$fixtures"
cmake -S "$llama" -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DGGML_CUDA=OFF -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DLLAMA_OPENSSL=OFF \
    -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_TESTS=OFF \
    -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_MTMD=ON -DMTMD_VIDEO=OFF
cmake --build "$build" --target mtmd --clean-first -j "${PR4_JOBS:-4}"
"${CXX:-c++}" -std=c++17 -O1 -g -pthread \
    -I"$llama/include" -I"$llama/ggml/include" -I"$llama/tools/mtmd" \
    "$root/frontend/tests/vision-metadata-real.cpp" -o "$build/mtmd-metadata-real" \
    -Wl,--start-group "$build/tools/mtmd/libmtmd.a" "$build/src/libllama.a" \
    "$build/ggml/src/libggml.a" "$build/ggml/src/libggml-cpu.a" "$build/ggml/src/libggml-base.a" \
    "$build/vendor/hash/libvendor-hash.a" -Wl,--end-group -ldl -lm \
    -Wl,--wrap=mtmd_encode -Wl,--wrap=mtmd_encode_chunk -Wl,--wrap=mtmd_batch_encode
printf 'PR4_METADATA_PIN %s\n' "$pin"
sha256sum "$build/tools/mtmd/libmtmd.a" "$build/src/libllama.a"
CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void \
    timeout --kill-after=10s 300 "$build/mtmd-metadata-real" "$model" "$projector" "$fixtures"
