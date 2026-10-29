#!/usr/bin/env bash
# Standalone, CPU-only PR3 tests. All endpoints and certificates are local.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
if [[ ! -f frontend/acquisition.hpp ]]; then
    echo 'SKIP: frontend/acquisition.hpp is not available yet' >&2
    exit 77
fi
for tool in "${CXX:-g++}" openssl timeout; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 2; }
done
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
umask 077
openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
    -subj '/CN=Strata acquisition test CA' \
    -addext 'basicConstraints=critical,CA:TRUE' \
    -keyout "$work/ca.key" -out "$work/ca.pem" >"$work/openssl.log" 2>&1
openssl req -newkey rsa:2048 -nodes -subj '/CN=localhost' \
    -keyout "$work/server.key" -out "$work/server.csr" >>"$work/openssl.log" 2>&1
printf '%s\n' 'subjectAltName=DNS:localhost' 'basicConstraints=critical,CA:FALSE' \
    'keyUsage=critical,digitalSignature,keyEncipherment' 'extendedKeyUsage=serverAuth' >"$work/server.ext"
openssl x509 -req -days 2 -in "$work/server.csr" -CA "$work/ca.pem" \
    -CAkey "$work/ca.key" -CAcreateserial -extfile "$work/server.ext" \
    -out "$work/server.pem" >>"$work/openssl.log" 2>&1
sources=(frontend/tests/acquisition_test.cpp)
[[ ! -f frontend/acquisition.cpp ]] || sources+=(frontend/acquisition.cpp)
# The pinned vendor currently ships split httplib.h/httplib.cpp, not the
# upstream amalgamated header. Support either layout without build-system edits.
if ! grep -q 'Server::bind_to_any_port' third_party/llama.cpp/vendor/cpp-httplib/httplib.h; then
    sources+=(third_party/llama.cpp/vendor/cpp-httplib/httplib.cpp)
fi
"${CXX:-g++}" -std=c++17 -DCPPHTTPLIB_OPENSSL_SUPPORT \
    -I frontend -I third_party/llama.cpp/vendor -I third_party/llama.cpp/common \
    "${sources[@]}" -lssl -lcrypto -pthread -o "$work/acquisition_test"
# Prevent inherited proxy settings from sending fixture requests off-host.
unset HTTP_PROXY HTTPS_PROXY ALL_PROXY http_proxy https_proxy all_proxy
export NO_PROXY='*' no_proxy='*'
# An outer watchdog catches broken cancellation/deadline implementations too.
timeout --kill-after=5s 90s "$work/acquisition_test" \
    "$work/ca.pem" "$work/server.pem" "$work/server.key"
