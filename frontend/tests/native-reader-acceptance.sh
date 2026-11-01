#!/usr/bin/env bash
# Full CPU release-candidate acceptance. Existing assets/build dependencies only.
# Run in an isolated offline CPU cgroup (<=8 GiB, <=256 pids), /tmp tmpfs <=64 MiB.
# Usage: native-reader-acceptance.sh CPU_STATIC_BUILD MODEL_GGUF PROJECTOR_GGUF NEW_OUTPUT_DIR
set -euo pipefail
ulimit -c 0
for tool in cmake python3 curl openssl "${CXX:-c++}"; do command -v "$tool" >/dev/null; done
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
build=$(cd "${1:?CPU build}" && pwd)
model=${2:?existing model}; projector=${3:?existing projector}
output=${4:?new output directory}
mkdir -p "$output"
output=$(cd "$output" && pwd)
[[ -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]]
for setting in STRATA_ENABLE_CUDA STRATA_FRONTEND_CUDA GGML_CUDA BUILD_SHARED_LIBS; do
    grep -Fxq "$setting:BOOL=OFF" "$build/CMakeCache.txt"
done
[[ $(git -C third_party/llama.cpp rev-parse HEAD) == 44072a19fb94d0bed63d381d42f6237373f91d54 ]]
git -C third_party/llama.cpp diff --quiet HEAD --
export TMPDIR="$output"
# One clean dependency/candidate build, shared by every integration test below.
if [[ ${PR4_REUSE_BUILD:-0} != 1 ]]; then
    cmake --build "$build" --target unbiased-strata --clean-first -j "${PR4_JOBS:-4}" >"$output/build.log" 2>&1
    cmake --build "$build" --target engine_test literals_test picture_cache_test -j "${PR4_JOBS:-4}" >>"$output/build.log" 2>&1
else
    printf 'Reusing separately clean-built candidate/dependency archives; SHA256s follow.\n' >"$output/build.log"
fi
bin="$build/bin"
[[ -x "$bin/unbiased-strata" && -x "$bin/strata-process-guard" && -x "$bin/strata-vision-reader" ]]
sha256sum "$bin/unbiased-strata" "$bin/strata-process-guard" "$bin/strata-vision-reader" \
    "$build/libstrata_llama_server.a" "$build/llama.cpp/tools/mtmd/libmtmd.a" "$build/llama.cpp/src/libllama.a" >"$output/SHA256SUMS"
"$bin/engine_test" frontend/tests/fake-engine.py >"$output/engine.log" 2>&1
"$bin/literals_test" >"$output/literals.log" 2>&1
"$bin/picture_cache_test" >"$output/legacy-cache.log" 2>&1
bash frontend/tests/e2e.sh "$bin/unbiased-strata" >"$output/legacy-http.log" 2>&1
bash frontend/tests/context-lifecycle.sh "$build" >"$output/legacy-context.log" 2>&1
bash frontend/tests/acquisition.sh >"$output/acquisition.log" 2>&1
bash frontend/tests/native-reader-foundations.sh >"$output/foundations.log" 2>&1
SANITIZE=1 bash frontend/tests/native-reader-foundations.sh >"$output/foundations-ubsan.log" 2>&1
PR4_RUNTIME_CONTAINED_CPU=1 bash frontend/tests/vision-runtime.sh "$build" "$model" "$projector" >"$output/runtime.log" 2>&1
SANITIZE=1 bash frontend/tests/vision-supervisor-reader-health.sh "$build" "$model" "$projector" >"$output/reader-health-ubsan.log" 2>&1
PR4_REUSE_BUILD=1 CXXFLAGS="${CXXFLAGS:-} -fsanitize=undefined -fno-sanitize-recover=all" \
    bash frontend/tests/vision-native.sh "$build" "$model" "$projector" >"$output/native-ubsan.log" 2>&1
PR4_REUSE_BUILD=1 bash frontend/tests/vision-recipe.sh "$build" "$model" "$projector" >"$output/recipe.log" 2>&1
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Werror frontend/tests/http-engine-probe.cpp -o "$output/http-engine"
timeout --kill-after=10s 1800s python3 frontend/tests/http-probe-client.py \
    --server "$bin/unbiased-strata" --engine "$output/http-engine" --model "$model" --projector "$projector" \
    --profile frontend/tests/data/vision-runtime-cpu.json --output "$output/http" >"$output/http.log" 2>&1
python3 - "$output" <<'PY'
from pathlib import Path
import json,sys
out={}
for name in ('memory.max','memory.peak','memory.events','pids.max','pids.peak','cpu.max'):
    p=Path('/sys/fs/cgroup')/name
    out[name]=p.read_text().strip() if p.exists() else 'unavailable'
Path(sys.argv[1],'containment.json').write_text(json.dumps(out,indent=2)+'\n')
PY
git diff --check
git -C third_party/llama.cpp diff --quiet HEAD --
printf 'PR4_CPU_ACCEPTANCE_PASS gpu=none production=untouched output=%s\n' "$output"
