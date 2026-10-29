#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
"${CXX:-g++}" -std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread \
    frontend/tests/clean-process.cpp -o "$build/clean-process"
timeout 30s "$build/clean-process"
