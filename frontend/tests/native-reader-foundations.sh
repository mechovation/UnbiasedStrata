#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=undefined -fno-sanitize-recover=all)
fi
"${CXX:-g++}" "${flags[@]}" frontend/process_guard.cpp -o "$build/process-guard"
(cd frontend/tests/data/mtmd-metadata && sha256sum --quiet -c SHA256SUMS)
for test in clean-process process-lifecycle vision-transport vision-payload vision-artifact vision-job vision-control vision-worker vision-supervisor vision-supervisor-job vision-engine-handoff vision-late-cache; do
    "${CXX:-g++}" "${flags[@]}" "frontend/tests/$test.cpp" -o "$build/$test"
    if [[ $test == clean-process || $test == vision-payload || $test == vision-job || $test == vision-control || $test == vision-engine-handoff || $test == vision-late-cache ]]; then
        timeout 30s "$build/$test"
    else
        timeout 30s "$build/$test" test "$build/process-guard"
    fi
done
# Always use the local ABI fixture: no NVIDIA library/device/GPU operation.
"${CXX:-g++}" "${flags[@]}" -shared -fPIC frontend/tests/vision-safety-fake.cpp -o "$build/libnvidia-ml.so.1"
"${CXX:-g++}" "${flags[@]}" frontend/tests/vision-safety.cpp -ldl -o "$build/vision-safety"
LD_LIBRARY_PATH="$build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" timeout 30s "$build/vision-safety" "$build/process-guard"
