#!/usr/bin/env bash
# Build/run white-box fixtures against the default static, CUDA-free frontend build, without production build edits.
# Usage: bash frontend/tests/context-lifecycle.sh /path/to/build-frontend
# Requires the existing build's C++ compiler/development libraries; may run inside the same CPU build container.
set -euo pipefail
ulimit -c 0
BUILD=$(cd "${1:?usage: context-lifecycle.sh <CPU frontend build directory>}" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
LLAMA=$ROOT/third_party/llama.cpp
if ! grep -q '^STRATA_ENABLE_CUDA:BOOL=OFF$' "$BUILD/CMakeCache.txt" || \
   ! grep -q '^GGML_CUDA:BOOL=OFF$' "$BUILD/CMakeCache.txt"; then
    echo 'context fixtures require a CUDA-free build' >&2
    exit 1
fi
TMP=$(mktemp -d "${TMPDIR:-/tmp}/us2-context-XXXXXX")
trap 'rm -rf "$TMP"' EXIT
"${CXX:-c++}" -std=c++20 -O0 -g -pthread -DLLAMA_SUBPROCESS -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -I"$ROOT/frontend" -I"$LLAMA/tools/server" -I"$LLAMA/tools/mtmd" \
    -I"$LLAMA" -I"$ROOT/frontend/ui_stub" -I"$LLAMA/common" \
    -I"$LLAMA/vendor" -I"$LLAMA/include" -I"$LLAMA/ggml/include" \
    "$HERE/context-lifecycle.cpp" -o "$TMP/context-lifecycle" \
    -Wl,--start-group \
    "$BUILD/libstrata_llama_server.a" \
    "$BUILD/llama.cpp/common/libllama-common.a" \
    "$BUILD/llama.cpp/common/libllama-common-base.a" \
    "$BUILD/llama.cpp/tools/mtmd/libmtmd.a" \
    "$BUILD/llama.cpp/src/libllama.a" \
    "$BUILD/ggml/src/libggml.a" "$BUILD/ggml/src/libggml-cpu.a" "$BUILD/ggml/src/libggml-base.a" \
    "$BUILD/llama.cpp/vendor/hash/libvendor-hash.a" \
    "$BUILD/llama.cpp/vendor/cpp-httplib/libcpp-httplib.a" \
    -Wl,--end-group -lssl -lcrypto -ldl -lm -Wl,--wrap=mtmd_input_chunk_get_type -Wl,--wrap=mtmd_input_chunk_get_n_tokens \
    -Wl,--wrap=mtmd_input_chunk_get_id -Wl,--wrap=mkstemp
CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void \
    timeout 30 "$TMP/context-lifecycle" "$HERE/fake-engine.py" "$LLAMA/models/ggml-vocab-qwen35.gguf"
