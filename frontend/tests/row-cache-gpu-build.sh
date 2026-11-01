#!/usr/bin/env bash
# Offline qualification tools only, in the existing matched CUDA development env.
# No GPU, downloads, production build settings, or shipped instrumentation.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
build=$(cd "${1:?existing matched CUDA build}" && pwd)
out=${2:?new output directory}
mkdir -p "$out"; out=$(cd "$out" && pwd)
[[ ! -e "$out/phase-bin" ]]
[[ $(git -C "$root/third_party/llama.cpp" rev-parse HEAD) == 44072a19fb94d0bed63d381d42f6237373f91d54 ]]
git -C "$root/third_party/llama.cpp" diff --quiet HEAD --
mkdir "$out/phase-bin"
cp "$build/bin/"{unbiased-strata,strata-vision-reader,strata-process-guard} "$out/phase-bin/"
"${CXX:-g++-13}" -std=c++17 -O2 -Wall -Wextra -Werror "$root/frontend/tests/vision-gpu-probe.cpp" -lcrypto -o "$out/probe"
includes=(-I"$root/third_party/llama.cpp/include" -I"$root/third_party/llama.cpp/ggml/include" -I"$root/third_party/llama.cpp/tools/mtmd")
for role in parent reader; do
    flags=(); [[ $role != reader ]] || flags=(-DPR4_READER_PHASE)
    "${CXX:-g++-13}" -std=c++17 -O2 -Wall -Wextra -Werror "${includes[@]}" "${flags[@]}" \
        -c "$root/frontend/tests/row-cache-gpu-phase.cpp" -o "$out/$role-phase.o"
done
python3 - "$root" "$build" "$out" <<'PY'
from pathlib import Path
import json, shlex, subprocess, sys
root, build, out = map(Path, sys.argv[1:])
def link(target):
    return shlex.split((build/('CMakeFiles/'+target+'.dir/link.txt')).read_text())
for target,role,wraps in [('unbiased-strata','parent',['read','write']),
                         ('strata-vision-reader','reader',['mtmd_init_from_file','mtmd_tokenize','mtmd_encode_chunk','stbi_load_from_memory'])]:
    cmd=link(target); cmd[cmd.index('-o')+1]=str(out/'phase-bin'/target)
    cmd += [str(out/(role+'-phase.o'))]+['-Wl,--wrap='+s for s in wraps]
    subprocess.run(cmd,cwd=build,check=True)
# This build exports compile_commands only for the dependency subdirectory.
# Use the parent target's generated flags rather than guessing its definitions.
flags={}
for line in (build/'CMakeFiles/unbiased-strata.dir/flags.make').read_text().splitlines():
    if ' = ' in line:
        k,v=line.split(' = ',1); flags[k]=shlex.split(v)
cmd=['/usr/bin/g++-13',*flags['CXX_DEFINES'],*flags['CXX_INCLUDES'],*flags['CXX_FLAGS'],
     '-UNDEBUG','-O1','-c',str(root/'frontend/tests/row-cache-gpu-fixture.cpp'),'-o',str(out/'gpu-cache.o')]
subprocess.run(cmd,cwd=build,check=True)
cmd=[s for s in link('unbiased-strata') if not s.endswith('frontend/main.cpp.o') and not s.endswith('frontend/strata_context.cpp.o')]
cmd[cmd.index('-o')+1]=str(out/'gpu-cache')
at=next(i for i,s in enumerate(cmd) if s.endswith('.a'))
cmd[at:at]=[str(out/'gpu-cache.o'),str(out/'parent-phase.o')]
cmd += ['-Wl,--wrap=read','-Wl,--wrap=write']
subprocess.run(cmd,cwd=build,check=True)
PY
sha256sum "$build/bin/"{unbiased-strata,strata-vision-reader,strata-process-guard,strata-poly} \
    "$out/phase-bin/"* "$out/probe" "$out/gpu-cache" > "$out/SHA256SUMS"
printf 'PR5_GPU_QUALIFICATION_TOOLS_BUILT gpu=none downloads=none\n'
