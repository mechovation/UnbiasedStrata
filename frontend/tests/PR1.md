# PR1 queue lifecycle and early context admission

Scope: `frontend/strata_context.cpp` and the test files in this directory. No engine,
submodule, build-system, deployment, worker, cache redesign, codec/TLS or warmup changes.
Pre-existing local CMake and ignore-file edits are **not** part of this slice.

## Run without CUDA or model weights

```sh
make test BUILD_DIR=/tmp/ub-vision-pr1 JOBS=8 \
  CMAKE_EXTRA="-DSTRATA_FRONTEND_CUDA=OFF"
# Additional white-box fixtures, using the same default static CPU build:
bash frontend/tests/context-lifecycle.sh /tmp/ub-vision-pr1-frontend
```

The HTTP regressions are included in `e2e.sh` and can also run independently:

```sh
python3 frontend/tests/queue-lifecycle.py \
  /tmp/ub-vision-pr1-frontend/bin/unbiased-strata \
  third_party/llama.cpp/models/ggml-vocab-qwen35.gguf
```

The native runner requires the build compiler and static development libraries. It
includes the production implementation in a separate test translation unit rather
than adding production test APIs, fault switches or build targets. It does not
support shared-library or CUDA builds. Run it inside the CPU build container when
the host lacks development dependencies. No downloads are needed.

## Deterministic oracles

- First-request engine gate plus actual queue deferral logs establish A/B/C ordering.
  B is rejected with HTTP 400 (including before SSE headers), and C completes with
  no fourth request. Engine capture contains exactly two GEN commands.
- A six-rejection HTTP chain stays below the configured 16 HTTP workers. A separate
  2,000-task native queue fixture checks iterative promotion without HTTP saturation.
- Deferred cancellation waits for the queue's cancellation post, after pending-task
  cleanup. Active cancellation emits residual token/progress records and gates DONE;
  the follower must not reach GEN until the old terminal boundary drains.
- Admission covers empty prompts, room zero/one, and negative/small/oversized output
  caps, preserving context-error metadata.
- Native fixtures cover duplicate cleanup, task-owned file removal, unavailable
  engine, picture-preparation refusal, deterministic failed GEN/STOP writes, late
  repeated cancellation, terminal error cleanup and exactly-once success accounting.
- HTTP fixtures additionally cover ERR, prompt-size mismatch, nonfinite DONE and
  EOF/shutdown without successor generation.

All client/gate waits are bounded. Failed HTTP runs preserve their server logs and
fake command captures in the printed temporary directory. These captures contain
synthetic fixture tokens, not production requests. Use these fault modes only in
tests. The native runner bounds execution with `timeout` and disables core dumps.

## Validation recorded for this slice

Against baseline `22abfdb87f3465a43ce1e2b2269a462bc3cf60f0`, the new HTTP suite
failed in the bounded rejection chain and both streaming/non-streaming A/B/C
cases: B returned 400 but C timed out. Other lifecycle controls passed.

After the minimal source fix:

- Existing engine adapter (7 cases), literal, picture-cache and HTTP e2e tests passed.
- All seven new HTTP test groups, including parameterized cases, passed.
- Native lifecycle fixtures, including the 2,000-task chain, passed.
- The CPU build had `STRATA_ENABLE_CUDA`, `STRATA_FRONTEND_CUDA` and `GGML_CUDA`
  all OFF. Compilation used an existing local development container with no network
  or GPU access; tests used no model weights or projector assets.

## Projector and serving acceptance — passed 2026-10-29

The synthetic native media-cell fixture proves **branch ordering**: an over-context
media task receives a context error before the picture-preparation error, while a
below-context control reaches preparation. It is not a real-image encode counter
or a real-projector qualification.

A separately authorized serving trial completed on the 5070 Ti, with production
paused and restored by an ownership-scoped supervisor. The candidate was compiled
with CUDA 13.0.2 / SM120; CUDA runtime/cuBLAS package versions matched production.

| Check | Result |
| --- | --- |
| Below-limit real-projector control | HTTP 200; one encode, 768 rows, one picture file |
| Six fresh images, 4636 prompt tokens / 4096 context | HTTP 400 before SSE, for both streaming and non-streaming requests |
| Rejected task's work | Zero encodes, zero picture files, zero GEN/GENI |
| Deferred valid follower | HTTP 200 without a fourth/nudge request, in both cases |
| Uninstrumented candidate with the real native engine | Authentication, models, text, streaming and two vision requests passed |
| Restored original production | Same smoke checks passed; image, command, environment, GPU mask and Compose/backup unchanged |

The projector/queue stage used a native gated text probe, not an LLM, plus test-only
file-creation and safe-log observers. Actual queue deferral was observed before gate
release. Each A/B/C run created exactly two files, both consumed by the successful
text-only GENI controls; the rejected image task created none. This deliberately
preserves the text-only GENI behavior deferred to PR2. The positive control proved
the file observer and encoder counter were active. A subsequent separate stage used
the uninstrumented candidate frontend and original real native engine for semantic
smoke checks. No Python component was introduced into either serving path.

Local evidence is under `logs/vision-pr1-20261029/trial1/` (ignored), including
scalar results, native protocol observations, candidate/restoration smoke logs and
`run.exit=0`. Local supervisors/probes under `deployment/` remain outside the upstream
PR allowlist. This table is the sanitized aggregate; neither private logs nor fixed
probe answers substitute for general image-quality or performance qualification.

The candidate was **not deployed**. Production is running its original image, the
upstream container remains stopped, and the temporary serving container was removed.

Cancellation during synchronous encoding, HTTP preprocessing costs, TLS/codecs,
worker isolation, warmup and new metrics endpoints remain outside PR1.
