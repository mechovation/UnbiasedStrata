#!/usr/bin/env bash
# Excluded real-projector CPU integration. Existing assets only; no serving/GPU.
# Usage: vision-native.sh CUDA_FREE_STATIC_BUILD MODEL_GGUF PROJECTOR_GGUF
# PR6_NATIVE_CODECS_ONLY=1 selects only targeted PR6 reader/projector coverage.
set -euo pipefail
ulimit -c 0
root=$(cd "$(dirname "$0")/../.." && pwd)
llama=$root/third_party/llama.cpp
build=$(cd "${1:?CPU build}" && pwd)
model=${2:?existing model}; projector=${3:?existing projector}
pin=44072a19fb94d0bed63d381d42f6237373f91d54
[[ $(git -C "$llama" rev-parse HEAD) == "$pin" ]]
git -C "$llama" diff --quiet HEAD --
for setting in STRATA_ENABLE_CUDA STRATA_FRONTEND_CUDA GGML_CUDA BUILD_SHARED_LIBS; do
    grep -Fxq "$setting:BOOL=OFF" "$build/CMakeCache.txt"
done
grep -Fxq "CMAKE_HOME_DIRECTORY:INTERNAL=$root" "$build/CMakeCache.txt"
# A combined acceptance run may clean-build once, then reuse those exact
# fingerprinted archives across native, HTTP and recipe tests.
if [[ ${PR4_REUSE_BUILD:-0} != 1 ]]; then
    cmake --build "$build" --target strata_llama_server --clean-first -j "${PR4_JOBS:-4}"
fi
printf 'PR4_NATIVE_PIN %s\n' "$pin"
sha256sum "$build/libstrata_llama_server.a" "$build/llama.cpp/tools/mtmd/libmtmd.a" "$build/llama.cpp/src/libllama.a"
# Some offline build containers mount /tmp noexec; executables belong in the
# caller's disposable build directory, which must be writable and executable.
tmp=$(mktemp -d "$build/pr4-native-XXXXXX")
trap 'rm -rf "$tmp"' EXIT
# CXXFLAGS/LDFLAGS support an already-installed nonstandard TLS development prefix.
# shellcheck disable=SC2086
"${CXX:-c++}" -std=c++20 -D_GNU_SOURCE -O1 -g -pthread ${CXXFLAGS:-} \
    -DLLAMA_SUBPROCESS -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -I"$root/frontend" -I"$llama/tools/server" -I"$llama/tools/mtmd" -I"$llama" \
    -I"$root/frontend/ui_stub" -I"$llama/common" -I"$llama/vendor" \
    -I"$llama/vendor/hash" -I"$llama/vendor/stb" -I"$llama/include" -I"$llama/ggml/include" \
    "$root/frontend/tests/vision-native.cpp" "$root/frontend/vision_reader.cpp" "$root/frontend/vision_image.cpp" -o "$tmp/vision-native" \
    -Wl,--start-group "$build/libstrata_llama_server.a" \
    "$build/llama.cpp/common/libllama-common.a" "$build/llama.cpp/common/libllama-common-base.a" \
    "$build/llama.cpp/tools/mtmd/libmtmd.a" "$build/llama.cpp/src/libllama.a" \
    "$build/ggml/src/libggml.a" "$build/ggml/src/libggml-cpu.a" "$build/ggml/src/libggml-base.a" \
    "$build/llama.cpp/vendor/hash/libvendor-hash.a" "$build/llama.cpp/vendor/cpp-httplib/libcpp-httplib.a" \
    -Wl,--end-group ${LDFLAGS:-} -l:libwebpdemux.so -l:libwebpmux.so -l:libwebp.so -l:libexif.so -l:libyuv.so -l:libjpeg.so -lssl -lcrypto -ldl -lm -Wl,--wrap=mtmd_encode_chunk -Wl,--wrap=stbi_load_from_memory -Wl,--wrap=mkdtemp
"${CXX:-c++}" -std=c++17 -D_GNU_SOURCE -O1 "$root/frontend/process_guard.cpp" -o "$tmp/guard"
printf 'VISION_NATIVE_BINARIES\n'
sha256sum "$tmp/vision-native" "$tmp/guard"
# External bound, independent of supervisor phase/total deadlines.
if [[ ${PR6_NATIVE_CODECS_ONLY:-0} != 1 ]]; then
    CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void \
        timeout --kill-after=10s 600 "$tmp/vision-native" "$model" "$projector" "$tmp/guard"
fi
CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void \
    timeout --kill-after=10s 600 "$tmp/vision-native" --codecs "$model" "$projector" "$tmp/guard"
