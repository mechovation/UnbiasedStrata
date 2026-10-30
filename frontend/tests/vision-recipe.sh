#!/usr/bin/env bash
# Excluded, real-projector CPU spike. No serving process, GPU, inference or downloads.
# Usage: vision-recipe.sh <CUDA-free static build> <model.gguf> <mmproj.gguf>
set -euo pipefail
ulimit -c 0
BUILD=$(cd "${1:?CPU build directory required}" && pwd)
MODEL=${2:?existing vocabulary/model GGUF required}
PROJECTOR=${3:?existing projector GGUF required}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LLAMA=$ROOT/third_party/llama.cpp
if ! grep -q '^STRATA_ENABLE_CUDA:BOOL=OFF$' "$BUILD/CMakeCache.txt" || \
   ! grep -q '^GGML_CUDA:BOOL=OFF$' "$BUILD/CMakeCache.txt"; then
    echo 'Spike A requires a CUDA-free build' >&2; exit 1
fi
PIN=$(git -C "$LLAMA" rev-parse HEAD)
git -C "$LLAMA" diff --quiet HEAD -- || { echo 'Pinned dependency has local changes' >&2; exit 1; }
grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$ROOT" "$BUILD/CMakeCache.txt" || {
    echo 'Build must be configured from this source path' >&2; exit 1;
}
# A source label alone does not prove stale archives implement that pin. Rebuild
# all linked libraries from this checkout, then record their content fingerprints.
if [[ ${PR4_REUSE_BUILD:-0} != 1 ]]; then
    cmake --build "$BUILD" --target strata_llama_server --clean-first -j "${SPIKE_JOBS:-4}"
fi
printf 'SPIKE_SOURCE_PIN %s\n' "$PIN"
sha256sum "$BUILD/libstrata_llama_server.a" "$BUILD/llama.cpp/tools/mtmd/libmtmd.a" \
    "$BUILD/llama.cpp/src/libllama.a" "$BUILD/llama.cpp/common/libllama-common.a"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/us2-recipe-XXXXXX")
trap 'rm -rf "$TMP"' EXIT
"${CXX:-c++}" -std=c++20 -O1 -g -pthread -DLLAMA_SUBPROCESS -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -I"$ROOT/frontend" -I"$LLAMA/tools/server" -I"$LLAMA/tools/mtmd" \
    -I"$LLAMA" -I"$ROOT/frontend/ui_stub" -I"$LLAMA/common" \
    -I"$LLAMA/vendor" -I"$LLAMA/include" -I"$LLAMA/ggml/include" \
    "$HERE/vision-recipe.cpp" -o "$TMP/vision-recipe" \
    -Wl,--start-group \
    "$BUILD/libstrata_llama_server.a" \
    "$BUILD/llama.cpp/common/libllama-common.a" "$BUILD/llama.cpp/common/libllama-common-base.a" \
    "$BUILD/llama.cpp/tools/mtmd/libmtmd.a" "$BUILD/llama.cpp/src/libllama.a" \
    "$BUILD/ggml/src/libggml.a" "$BUILD/ggml/src/libggml-cpu.a" "$BUILD/ggml/src/libggml-base.a" \
    "$BUILD/llama.cpp/vendor/hash/libvendor-hash.a" "$BUILD/llama.cpp/vendor/cpp-httplib/libcpp-httplib.a" \
    -Wl,--end-group -lssl -lcrypto -ldl -lm \
    -Wl,--wrap=mtmd_encode -Wl,--wrap=mtmd_encode_chunk -Wl,--wrap=mtmd_batch_encode
CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void \
    timeout --kill-after=10s 300 "$TMP/vision-recipe" "$MODEL" "$PROJECTOR" "$PIN"
