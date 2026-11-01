#!/usr/bin/env bash
# Link qualification-only probes against an existing matched static CUDA build.
# No GPU access, downloads, source/production rebuilds or deployment operations.
# Run inside the build's existing CUDA development environment with OpenSSL SDK.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${1:?existing CUDA build}" && pwd)
out=${2:?new qualification tools directory}
mkdir -p "$out"
out=$(cd "$out" && pwd)
[[ ! -e "$out/phase-bin" && ! -e "$out/probe" ]]
[[ $(git -C "$root/third_party/llama.cpp" rev-parse HEAD) == 44072a19fb94d0bed63d381d42f6237373f91d54 ]]
git -C "$root/third_party/llama.cpp" diff --quiet HEAD --
mkdir "$out/phase-bin"
cp "$build/bin/"{unbiased-strata,strata-vision-reader,strata-process-guard} "$out/phase-bin/"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror "$root/frontend/tests/sve1-capture-oracle.cpp" -lcrypto -o "$out/probe"
includes=(-I"$root/third_party/llama.cpp/include" -I"$root/third_party/llama.cpp/ggml/include" -I"$root/third_party/llama.cpp/tools/mtmd")
for role in parent reader; do
    flags=(); [[ $role != reader ]] || flags=(-DPR4_READER_PHASE)
    "${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror "${includes[@]}" "${flags[@]}" \
        -c "$root/frontend/tests/native-reader-gpu-phase.cpp" -o "$out/$role-phase.o"
done
python3 - "$build" "$out" <<'PY'
from pathlib import Path
import shlex, subprocess, sys
build, out = map(Path, sys.argv[1:])
for target, role, wraps in [('unbiased-strata', 'parent', ['read', 'write']),
                           ('strata-vision-reader', 'reader', ['mtmd_init_from_file', 'mtmd_tokenize', 'mtmd_encode_chunk'])]:
    cmd = shlex.split((build / ('CMakeFiles/' + target + '.dir/link.txt')).read_text())
    cmd[cmd.index('-o') + 1] = str(out / 'phase-bin' / target)
    cmd += [str(out / (role + '-phase.o'))] + ['-Wl,--wrap=' + name for name in wraps]
    subprocess.run(cmd, cwd=build, check=True)
PY
sha256sum "$build/bin/"{unbiased-strata,strata-vision-reader,strata-process-guard} \
    "$out/phase-bin/"* "$out/probe" >"$out/SHA256SUMS"
printf 'PR4_GPU_QUALIFICATION_TOOLS_BUILT (no GPU used)\n'
