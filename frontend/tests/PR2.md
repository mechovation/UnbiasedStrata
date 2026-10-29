# PR2: dispatch by actual media

## Scope

`frontend/strata_context.cpp` now validates placeholder spans before picture work:

- Text-only requests use `GEN` and create no picture file, even when mtmd is loaded.
- Supported image requests keep the existing ordered `GENI`/SVE1 path and cache ownership.
- Unsupported/missing chunks return an explicit not-supported error (501); malformed
  image spans return an invalid-request error (400), before picture-file creation.
- PR1's context admission still runs first. Every new rejection uses terminal cleanup
  and returns immediately. Literal tokenization, BOS/EOS, projector advertisement,
  sampling, engine ABI, dependencies and deployment settings are unchanged.

No worker/cache redesign, TLS/codecs, warmup, performance threshold or new model family
is included. Existing local CMake/ignore-file changes remain outside this slice.

## Reproduce CPU tests

```sh
make test BUILD_DIR=/tmp/ub-vision-pr2 JOBS=8 \
  CMAKE_EXTRA="-DSTRATA_FRONTEND_CUDA=OFF"
bash frontend/tests/context-lifecycle.sh /tmp/ub-vision-pr2-frontend
```

The native runner uses the default static Linux CPU build and pinned vocabulary-only
GGUF. It requires no model weights, projector, downloads or GPU. Tests use link-time
observers for file creation, chunk kinds/counts and distinct synthetic chunk identities;
production has no test switches. Each fixture runs through clean `posix_spawn`/exec
before starting its fake engine, avoiding fork after a prior fixture's logger/threads.

Checks include:

- Text with/without literal spans, streaming/non-streaming, with mtmd capability set:
  exact token IDs preserved, `GEN`, zero picture-file attempts. The fake backend
  explicitly refuses `GENI`, representing a backend without `--vision`.
- Distinct cached A/B/A payloads, pad runs and `img_keys` retain occurrence order.
- Image-file lifetime extends through production terminal handling; a deferred text
  follower completes through the queue using `GEN`, without another picture file.
- Unsupported kinds, orphan null cells with either capability flag, valid-image-then-
  unsupported-media, and zero/oversized/non-null image spans reject before writer work.
- All prior adapter, literals, row-cache, queue, context, cancellation and e2e checks.

Baseline PR1 code failed the new text/no-file and unsupported-before-writer checks;
its image transport control passed. After PR2, all ten native fixture groups and the
existing frontend/e2e suite passed. Static review found no remaining actionable defect.

## CPU-projector serving check — passed 2026-10-29

An isolated **CPU-only**, offline, 8-CPU/8-GiB test container loaded the existing real
Qwen model vocabulary and projector. GPU devices were not mounted; CUDA was disabled
in the frontend build. Production was never paused or restarted for this CPU check.
The small synthetic-image fixture used a 64-token image budget, not production's
1024-token setting.

| Check | Result |
| --- | --- |
| Loaded projector + backend refusing GENI | Four text chat requests succeeded: literal/plain × streaming/non-streaming |
| Text token/layout oracle | Exact IDs matched rendered-template/split-literal tokenization |
| Text work | Four GEN commands, zero encodes and zero picture-file attempts |
| Real image A, B, then mixed A/B/A | Two encodes; mixed SVE1 bytes exactly equalled A + B + A |
| Text immediately after images | GEN, no additional picture file |
| Vision advertisement | Remained enabled before/after text requests |
| Isolation/cleanup | No GPU access; temporary container removed; production configuration/start time unchanged |

Local evidence: `logs/vision-pr2-20261029/`, including scalar results, SVE captures and
`run.exit=0`. An archived first harness attempt used a token ID from the vocabulary-only
fixture against the real model; its reply-content assertion failed. Correcting the
fixture to the real vocabulary's OK token resolved it; this was not a product failure.

These projector checks used native fake text engines, not LLM inference. They do not
qualify general semantic quality, GPU performance or real-engine M-RoPE reset costs.
A real engine may legitimately perform one identity reset on image→text transition;
that must not be mistaken for repeated text-only GENI. No deployment occurred.

## 5070 Ti GPU serving trial — passed 2026-10-29

A separately authorized live trial used only the permitted RTX 5070 Ti, with
production paused and restored by an ownership-scoped maintenance supervisor.
The candidate used CUDA 13.0.2 / SM120, matching production's CUDA runtime/cuBLAS
package versions. Existing pinned CUDA libraries and unchanged main object were
reused; the PR2 frontend implementation was recompiled and relinked with the same
Release flags. No production image or deployment file was rebuilt or edited.

| Gate | Result |
| --- | --- |
| GPU projector loaded; native backend refuses GENI | Four plain/literal × streaming/non-streaming chat requests passed |
| Text token and work checks | Exact template/literal token IDs; four GEN, zero encodes, zero picture-file attempts |
| Actual GPU images A, B, then A/B/A | Two fresh encodes, three image files, five image records; mixed SVE1 bytes exactly A + B + A |
| Text following images | GEN with no additional picture file; vision advertisement remained enabled |
| PR1 queue/admission regressions | Both 4636-token/4096-context refusals returned pre-SSE 400; zero encodes/files for all A/B/C text/refusal work; followers completed without nudges |
| Uninstrumented candidate + real native engine | Authentication, models, arithmetic text/SSE and two real vision responses passed |
| Real text after vision | Both non-streaming and streaming arithmetic passed; observed four GEN and two GENI across the six real-engine requests |
| Original production restored | Authentication, models, text, streaming and two vision checks passed; original image/settings and Compose/backup unchanged |

The first two stages used native test probes plus observational file/log hooks, not
LLM inference. The real-engine stage used the **uninstrumented** candidate frontend
and original native engine/settings. Its successful image→text transitions confirm
transport correctness, not a measurement of M-RoPE reset costs or a performance/
quality benchmark. The 1024-token image budget matched production; 4096 context in
the projector/queue fixture was intentional, not a production setting change.

Local evidence: `logs/vision-pr2-gpu-20261029/trial1/`, including scalar results,
synthetic token/SVE captures, candidate/restoration smoke logs and `run.exit=0`.
These local supervisors/probes remain excluded from the upstream PR by default.
Production is running its **original image**; the candidate was not deployed, the
temporary serving container was removed, and the upstream service remains stopped.
PR3 has not started.
