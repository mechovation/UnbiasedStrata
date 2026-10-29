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
(cd frontend/tests/data/pr4-metadata && sha256sum --quiet -c SHA256SUMS)
for test in clean-process process-lifecycle vision-transport vision-payload vision-artifact vision-job; do
    "${CXX:-g++}" "${flags[@]}" "frontend/tests/$test.cpp" -o "$build/$test"
    if [[ $test == clean-process || $test == vision-payload || $test == vision-job ]]; then
        timeout 30s "$build/$test"
    else
        timeout 30s "$build/$test" test "$build/process-guard"
    fi
done
