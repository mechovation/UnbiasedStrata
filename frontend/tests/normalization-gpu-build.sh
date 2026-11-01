#!/usr/bin/env bash
# C++-only qualification tooling for an existing matched CUDA build. No GPU/downloads.
# Supply installed codec/TLS development prefixes via CXXFLAGS/LDFLAGS.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${1:?existing CUDA build}" && pwd)
out=${2:?new qualification directory}
[[ ! -e $out ]]
mkdir -p "$out"; out=$(cd "$out" && pwd)
[[ $(git -C "$root/third_party/llama.cpp" rev-parse HEAD) == 44072a19fb94d0bed63d381d42f6237373f91d54 ]]
git -C "$root/third_party/llama.cpp" diff --quiet HEAD --
mkdir "$out/phase-bin"
cp "$build/bin/"{unbiased-strata,strata-vision-reader,strata-process-guard} "$out/phase-bin/"
cxx=${CXX:-g++-13}
includes=(-I"$root/third_party/llama.cpp/include" -I"$root/third_party/llama.cpp/ggml/include" -I"$root/third_party/llama.cpp/tools/mtmd")
for role in parent reader; do
    flags=(); [[ $role != reader ]] || flags=(-DPR4_READER_PHASE)
    "$cxx" -std=c++17 -O2 -Wall -Wextra -Werror "${includes[@]}" "${flags[@]}" \
        -c "$root/frontend/tests/normalization-gpu-phase.cpp" -o "$out/$role-phase.o"
done
cd "$build"
for target in unbiased-strata strata-vision-reader; do
    IFS= read -r line < "CMakeFiles/$target.dir/link.txt"
    [[ $line == *" -o bin/$target "* ]]
    printf -v destination '%q' "$out/phase-bin/$target"
    line=${line/ -o bin\/$target / -o $destination }
    if [[ $target == unbiased-strata ]]; then
        printf -v object '%q' "$out/parent-phase.o"
        line+=" $object -Wl,--wrap=read -Wl,--wrap=write"
    else
        printf -v object '%q' "$out/reader-phase.o"
        line+=" $object -Wl,--wrap=mtmd_init_from_file -Wl,--wrap=mtmd_tokenize -Wl,--wrap=mtmd_encode_chunk -Wl,--wrap=stbi_load_from_memory -Wl,--wrap=_ZN15strata_frontend12vision_image6decodeEPKhmNS0_6LimitsE"
    fi
    # Trusted CMake-generated compiler command, not request input or a credential.
    eval "$line"
done
# shellcheck disable=SC2086
"$cxx" -std=c++17 -O2 -Wall -Wextra -Werror ${CXXFLAGS:-} "$root/frontend/tests/vision-gpu-probe.cpp" \
    ${LDFLAGS:-} -l:libcrypto.so.3 -o "$out/probe"
# shellcheck disable=SC2086
"$cxx" -std=c++20 -D_GNU_SOURCE -DCPPHTTPLIB_OPENSSL_SUPPORT -O1 -g -Wall -Wextra -Werror \
    -I"$root/frontend" -I"$root/third_party/llama.cpp/common" -I"$root/third_party/llama.cpp/vendor" \
    -I"$root/third_party/llama.cpp/vendor/stb" ${CXXFLAGS:-} "$root/frontend/tests/normalization-gpu-acceptance.cpp" \
    ${LDFLAGS:-} -l:libwebpmux.so -l:libwebp.so -l:libexif.so -l:libyuv.so -l:libjpeg.so \
    "$build/llama.cpp/vendor/cpp-httplib/libcpp-httplib.a" -l:libssl.so.3 -l:libcrypto.so.3 -ldl -pthread -o "$out/pr6-gpu-acceptance"
sha256sum "$build/bin/"{unbiased-strata,strata-vision-reader,strata-process-guard,strata-poly} \
    "$out/phase-bin/"* "$out/probe" "$out/pr6-gpu-acceptance" > "$out/SHA256SUMS"
printf 'PR6_GPU_TOOLS_BUILT cpp_only=1 gpu=none downloads=none\n'
