#!/usr/bin/env bash
# PR5 no-model/no-GPU wire-cache and bounded single-flight regressions.
set -euo pipefail
cd "$(dirname "$0")/../.."
ulimit -c 0
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
flags=(-std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then
    flags+=(-fsanitize=undefined -fno-sanitize-recover=all)
fi
"${CXX:-g++}" "${flags[@]}" frontend/process_guard.cpp -o "$build/process-guard"
for test in vision-late-cache vision-singleflight vision-supervisor-job; do
    "${CXX:-g++}" "${flags[@]}" "frontend/tests/$test.cpp" -o "$build/$test"
    if [[ $test == vision-supervisor-job ]]; then
        timeout 30s "$build/$test" test "$build/process-guard"
    else
        timeout 30s "$build/$test"
    fi
done
