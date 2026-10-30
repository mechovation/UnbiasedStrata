#!/usr/bin/env bash
# Default: no Docker, downloads, rebuilds, models, serving or GPU use. Existing CPU archives.
# Optional contained CPU startup: PR4_RUNTIME_CONTAINED_CPU=1 ... BUILD MODEL PROJECTOR
# Requires an existing 4-CPU/8-GiB/pids=256 cgroup-v2 test container; never production.
# Usage: CXXFLAGS='-I...installed TLS includes...' LDFLAGS='-L...TLS libs...' \
#   bash frontend/tests/vision-runtime.sh /tmp/ub-pr4-native-clean
set -euo pipefail
ulimit -c 0
root=$(cd "$(dirname "$0")/../.." && pwd)
llama=$root/third_party/llama.cpp
build=$(cd "${1:?existing CPU build}" && pwd)
for setting in STRATA_ENABLE_CUDA STRATA_FRONTEND_CUDA GGML_CUDA BUILD_SHARED_LIBS; do
    grep -Fxq "$setting:BOOL=OFF" "$build/CMakeCache.txt"
done
[[ $(git -C "$llama" rev-parse HEAD) == 44072a19fb94d0bed63d381d42f6237373f91d54 ]]
tmp=$(mktemp -d "$build/pr4-runtime-XXXXXX")
trap 'rm -rf "$tmp"' EXIT
includes=(-I"$root/frontend" -I"$llama/tools/server" -I"$llama/tools/mtmd" -I"$llama"
    -I"$root/frontend/ui_stub" -I"$llama/common" -I"$llama/vendor" -I"$llama/vendor/hash"
    -I"$llama/vendor/stb" -I"$llama/include" -I"$llama/ggml/include")
libs=(-Wl,--start-group "$build/libstrata_llama_server.a"
    "$build/llama.cpp/common/libllama-common.a" "$build/llama.cpp/common/libllama-common-base.a"
    "$build/llama.cpp/tools/mtmd/libmtmd.a" "$build/llama.cpp/src/libllama.a"
    "$build/ggml/src/libggml.a" "$build/ggml/src/libggml-cpu.a" "$build/ggml/src/libggml-base.a"
    "$build/llama.cpp/vendor/hash/libvendor-hash.a" "$build/llama.cpp/vendor/cpp-httplib/libcpp-httplib.a"
    -Wl,--end-group)
for source in tests/vision-runtime.cpp vision_reader_main.cpp; do
    name=$(basename "$source" .cpp)
    extra=()
    if [[ $source == vision_reader_main.cpp ]]; then extra=("$root/frontend/vision_reader.cpp"); fi
    # shellcheck disable=SC2086
    "${CXX:-c++}" -std=c++20 -D_GNU_SOURCE -DLLAMA_SUBPROCESS -DCPPHTTPLIB_OPENSSL_SUPPORT \
        -O0 -pthread ${CXXFLAGS:-} "${includes[@]}" "$root/frontend/$source" "${extra[@]}" \
        -o "$tmp/$name" "${libs[@]}" ${LDFLAGS:-} -lssl -lcrypto -ldl -lm
 done
CUDA_VISIBLE_DEVICES=-1 NVIDIA_VISIBLE_DEVICES=void timeout 30 "$tmp/vision-runtime" "$root/frontend/tests/data/vision-runtime-cpu.json"
# Executable rejects all legacy modes and missing explicit config before native init.
for arg in '' 0 01 1 --reader; do
    if env -i "$tmp/vision_reader_main" "$arg" >"$tmp/out" 2>"$tmp/err"; then exit 1; fi
    grep -Fxq 'native vision reader configuration/startup failed' "$tmp/err"
    test ! -s "$tmp/out"
done
printf 'vision reader executable fail-closed smoke tests passed\n'
if [[ ${PR4_RUNTIME_CONTAINED_CPU:-0} == 1 ]]; then
    "${CXX:-c++}" -std=c++17 -D_GNU_SOURCE -O0 "$root/frontend/process_guard.cpp" -o "$tmp/guard"
    timeout --kill-after=10s 240 "$tmp/vision-runtime" --contained-reader \
        "$root/frontend/tests/data/vision-runtime-cpu.json" "${2:?existing CPU test model}" "${3:?existing CPU test projector}" \
        "$tmp/vision_reader_main" "$tmp/guard"
fi
