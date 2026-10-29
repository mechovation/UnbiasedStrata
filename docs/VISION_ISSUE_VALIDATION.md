# UnbiasedStrata vision: reported-issue reproduction

2026-10-29. Follow-up to the [source review/specification](VISION_STACK_REVIEW.md) and [live performance comparison](VISION_STACK_BENCHMARK.md). **This phase tests functionality and failure paths, not just speed.**

## Scope and safety

- Ran the **unchanged production C++ frontend binary and real GPU projector** from image ID `sha256:493bdac6c7688b76d4c074c1d689bfd6e3a249621f35b5714334c52928fe0fed`.
- Replaced only the text-generation child with a **native C++ protocol probe**, which validates SVE1 records, checks row/pad counts, captures projected rows, and returns a fixed `OK` token. Thus HTTP parsing, decoding, preprocessing, projector inference, caching, admission, cancellation and frontend scheduling are real; **the answers are not LLM inference or semantic-quality evidence**.
- Four isolated test processes, sequentially on the **5070 Ti UUID only**. Test port 18082 was loopback-only; controlled HTTP image sources were served on the host Docker-bridge interface. No external image hosts or actual cloud/metadata endpoints were contacted.
- Production was stopped/restored by a bounded supervisor. Test containers had 8-GiB RAM limits, no swap, disabled core dumps, no restart policy, and ran as the secret-file owner's unprivileged UID with all capabilities dropped. Only containers labelled as belonging to this run were torn down.
- Fault injection was a **native, CPU-side, glibc allocation hook**. It did not exhaust memory, fail an allocation, induce a real GPU fault, or touch the 3060. No shim was installed in production.
- **No serving-code fixes or startup-warmup implementation were made. C++/CUDA remains the target architecture.** Python generated fixtures, orchestrated requests and checked evidence externally; it was not used as an UnbiasedStrata server/worker/backend.

## Status matrix

“Reproduced” means observed against the native frontend/projector in this phase, not merely inferred from Strata's reference tests.

| Report / gap | Status | Observed behavior and control |
| --- | --- | --- |
| HTTPS image URLs, #21 item 20 / V6 | **Reproduced** | HTTP source succeeds, HTTPS returns **500** with `HTTPS is not supported. Please rebuild...`. No TLS handshake/certificate test was possible in this build. |
| WebP support, #21 item 18 / V8 | **Reproduced for this image** | Opaque and transparent WebP, correctly labelled `image/webp`, both return **400**, `Failed to load image or audio file`. PNG and JPEG controls succeed. Optional ffmpeg is absent in this runtime; do not generalize to every mtmd build. |
| EXIF orientation, #21 item 17 / V7 | **Reproduced** | Orientation-6 JPEG produces **byte-identical SVE1** to the untagged, sideways JPEG: grid **24×12**. Physically upright JPEG and Strata-normalized PNG give **12×24**. The tag is ignored, not merely interpreted differently by the language model. |
| Unreadable tool-result image, #21 item 19 / V10 | **Reproduced** | Valid tool image returns **200**. Replace it with malformed bytes: **400**; resend same history: **400** again. Manually substituting an omission note restores **200**. No automatic omission/fallback occurs. |
| Between-image cancellation, #21 item 4 / V3 | **Reproduced** | Closed the streaming client after **2 of 16** images had encoded. All **16** completed encoding and all records reached one probe request. A subsequent text request waited **2.496 s**, versus **0.009 s** after the work drained. |
| Context admission after encoding, V4 | **Reproduced** | A 4096-context test request tokenized to **4636** prompt tokens. All **6** images encoded before the **400** context refusal, taking **9.368 s** including queue wait/first-use overhead. These tokens were already known before projector execution. |
| Deferred queue after early launch rejection, #21 comment | **Reproduced** | A queued valid request remained pending **2 s after** the over-context refusal while `/health` said `busy:false`. A **new arrival** completed, then the original queued request completed, **14.284 s** after submission. This is lost queue advancement, not continued model work. |
| URL byte/timeout policies, #21 item 20 | **Differences confirmed** | Valid PNG padded to **9 MiB** succeeds; the same valid prefix padded to **11 MiB** returns generic **500**. A source delaying headers **11 s** fails at **10.014 s**, also generic 500. Reference Python allows 32 MiB / 60 s. Different limits may be deliberate; unclear 500 diagnostics are a separate usability concern. |
| Literal `<\|image_pad\|>` / `<\|vision_start\|>` strings, #14 | **Fixed behavior confirmed** | Literal text returns **200**, with **zero projected records**. The probe also verifies there are no orphan image-pad placeholders. This representative case supports the existing fix, not exhaustive special-token qualification. |
| “Transparency flattened onto white” as a universal lost behavior, #21 item 18 | **Not a general Python-port regression** | Transparent PNG gives byte-identical SVE1 to a black-background RGB control, not the white control. **Strata passes the same PNG through unchanged too.** Its white compositing applies on conversion paths such as WebP, not universally to PNG. |
| Native fault containment, #21 item 4 / V3 | **Gap demonstrated by deliberate injection** | A confirmed main-thread allocation hang blocks completions; a confirmed native abort terminates the HTTP process/container with **exit 134**, `OOMKilled:false`. These are injected process failures, not naturally occurring encoder bugs; details below. |
| `/metrics` blocking during encoding, #21 item 4 | **Not tested by this endpoint** | `/metrics` returns **404 even with `--metrics`**, including the idle baseline. It cannot establish queue blocking. Completion latency/queue behavior supplied the observations above instead. |

The earlier live comparison confirmed that the embedding cache is already fixed and working. It separately reproduced unnecessary text-only GENI, which remains **unfixed**; see that report. Neither finding means embedding caching is absent.

## Stronger reproductions

### EXIF: compare actual projected data, not model guesses

The baseline/tagged JPEGs contain the same compressed image data, with the latter carrying orientation 6. Strata's real `Vision.normalize()` was used externally to generate the normalization reference and record its dimensions.

| Input | Projected grid | SVE1 relation |
| --- | --- | --- |
| Untagged landscape JPEG, 768×384 | 24×12 | Baseline |
| Same pixels with EXIF orientation 6 | 24×12 | Exactly identical bytes to baseline |
| JPEG physically rotated upright, 384×768 | 12×24 | Different grid |
| Strata-normalized tagged JPEG → PNG, 384×768 | 12×24 | Different grid |

Captured files are compared with SHA-256 and byte identity, not only the probe's FNV log hash. The physically rotated JPEG is independently re-encoded, so no bitwise-equivalence claim is made between it and the normalized PNG.

### Queue starvation following a vision rejection

The native probe introduces a **1.5-s text reply delay** to establish an active slot, analogous to a real ongoing generation. Submit these three requests in order:

1. Valid leading text request, occupying the slot.
2. Over-context request containing six fresh images.
3. Valid text request, which becomes deferred.

The first request finishes. The second is launched from the deferred queue, encodes its six pictures, and fails context admission. The third remains deferred even though the slot is now free. A fourth, newly arriving request runs; its normal completion drains the original third request.

This matches `frontend/strata_context.cpp`: early `launch()` failures call `slot.release()` directly; normal `finish_slot()` additionally calls `queue_tasks.pop_deferred_task(-1)`. The 4096 context and probe delay are **test settings**, not changes to the production 262144 context or a new backend performance result.

### Cancellation: correlate the canceled request, not another request's log

The supervisor waited for fresh encode logs, recorded **2 completed encodes**, then closed the streaming socket. Final logs show **16 fresh encodes** and **16 SVE1 records under probe request ID 15**. The later text request has no image records and cannot account for that observation. Thus at least 14 more encode completions occurred after the client closed; no claim of immediate socket-disconnect detection or preemption of an individual GPU call is made.

The source lacks between-picture cancellation checks, and encode work executes synchronously on the main task queue. Strata's reference implementation also does not immediately interrupt the current native encode, but checks cancellation before proceeding to the next image.

## Native fault experiments: what they establish

`vision-probe-native-fault.cpp` interposes glibc `malloc()` **only on the main thread, only for allocations ≥1 MiB**, and only after a test marker is written. The frontend's queue runs on the main thread. A valid image request triggers the marker during native processing. The fixture either waits indefinitely before forwarding the allocation or calls `abort()`.

- **Hang marker was reached.** Image request timed out at **15 s**; a later text completion timed out too. `/health` still returned **200**, `busy:true`, in ~2 ms. The test container was explicitly killed afterward.
- **Abort marker was reached.** The request lost its connection in **0.222 s**. The test container exited **134**, not OOM; subsequent health connection was refused.
- This demonstrates no independent process boundary between that native vision-task path and HTTP serving. It does **not** identify an exact `mtmd_encode_chunk` symbol location, establish an actual clip/CUDA crash, reproduce an allocation failure, or show an actual GPU OOM.
- We **did not wait 300 s**. Absence of an encoder deadline/restart mechanism remains a source-inspection finding; the bounded injected hang demonstrates loss of responsiveness, not a measured 300-s deadline violation.
- The test used **no container restart policy** to observe termination. Production has `unless-stopped` and may restart the **entire service** after a real fatal failure. That is different from preserving HTTP/text-engine availability while restarting an isolated native reader.

An earlier CUDA-launch interposer **did not trigger** in this build. Its timeout window overlapped normal first-use encoding, and its abort case returned 200. Those attempt2 fault cases are **invalid as fault-containment evidence**, and are retained only for transparency. The final allocation-hook markers are required by the supervisor/checker.

## Remaining unverified items

- GPU allocation ordering under actual low-memory/OOM conditions: **not deliberately induced**.
- A model/mtmd path ignoring `image_max_tokens`, or hostile decoded-pixel/allocation limits: **not reproduced**. Current Qwen fixtures stay within their configured token budget.
- CPU AVX-512 flash-attention behavior and CPU fallback performance: **not tested in this phase**.
- Explicit projector-device selection: **not tested with another GPU**; only the permitted 5070 Ti was visible.
- Other codecs, EXIF orientations beyond 6, pathological multi-frame images, and malformed-input fuzzing: **not qualified**.
- Full native text-engine/model semantic recovery after an encoder failure: **not tested by the fixed-token probe**. Earlier real-engine tests covered successful inference, not these failure paths.
- Body-size limit, queued-stream keepalives, the engine-silence watchdog, and a full timeout-duration endurance test: **not run here**.

## Evidence and reproducibility

Final evidence is local/gitignored at `logs/vision-issues-20261029/attempt3/`:

```text
experiment.json                      immutable image ID, probe binary hashes, GPU, Compose hash
results.jsonl                        49 case/control/lifecycle observation records
summary.json                         classified observations and captured-file hashes
formats-server.log                   real encoder/cancellation/probe row logs
admission_queue-server.log           slot/refusal/deferred recovery sequence
encoder_hang-server.log              confirmed native CPU hang marker
encoder_abort-server.log             confirmed native abort marker
captures/formats/request-*.sve       actual projected floats for EXIF/alpha comparisons
restored-smoke.log
run.exit                             0 = probes completed, not product issues fixed
```

`fixtures/` links to the generated inputs, MIME-correct JPEG/WebP cases, hashes, and `reference-normalization.json`; the latter records real Strata normalization behavior, including PNG pass-through and WebP conversion.

Tools added for reproduction:

- `deployment/vision-probe-engine.cpp`: native protocol/capture fixture, no model/CUDA execution in the fake text engine.
- `deployment/vision-probe-native-fault.cpp`: test-only CPU fault hook; never deploy/preload in production.
- `deployment/run-vision-issues.py`: external supervisor/client; requires prepared fixtures and compiled probes, explicit test-container ownership, and automatic production restoration.
- `deployment/check-vision-issues.py`: offline evidence classifier, including expected failure observations.

```sh
python3 deployment/check-vision-issues.py logs/vision-issues-20261029/attempt3
python3 -m unittest discover -s deployment -p test_vision_stack_ab.py
```

The **13 external-tool accounting/security tests passed**. Native probe compilation used `g++-13 -std=c++17 -O2 -Wall -Wextra -Werror`; protocol smoke checks accepted valid image records/empty text GENI and rejected orphan/mismatched image pads. Original native fixture development also exercised malformed SVE1 records. These are tooling checks, not claims that the product bugs pass regression tests.

Setup attempts are preserved: the first capability-dropped root container could not read the UID-owned 0600 secret file, so it never reached readiness and production was restored. Subsequent runs used the owning unprivileged UID. Attempt2 reproduced the functional cases but did not trigger its original CUDA fault hook; attempt3 uses the confirmed native allocation hook and correct image MIME labels.

A credential scan found no literal API key in the retained text artifacts. Production Compose was unchanged; the test container was removed; the real `unbiased-strata` frontend and `strata` engine are running again. Final authentication, model listing, text, streaming and two GPU-vision smoke requests passed. No image downloads/builds, production-source fixes, or warmup implementation occurred.

## Improvement-plan consequences — implementation still pending

The phased technical execution plan and dependency gates are in [VISION_IMPLEMENTATION_PLAN.md](VISION_IMPLEMENTATION_PLAN.md); the points below summarize priorities, not a single authorized implementation change.

1. **Queue correctness:** every slot-release/error/cancel path must advance deferred work. Add the three-request rejection regression to native frontend tests before changing behavior.
2. **Admission and cancellation:** reject known over-context/token-budget requests before projector execution; check cancellation between pictures and keep completion scheduling responsive.
3. **Input compatibility:** TLS-backed HTTPS, explicit native codec support and EXIF normalization; visible omission fallback for unreadable tool-result images. Define URL limits and return useful client-facing errors instead of opaque 500s.
4. **Fault containment:** evaluate a supervised **C++/CUDA reader process**, with deadlines and restart behavior, not a Python worker. Keep the exact failure model and production restart-policy tradeoffs explicit.
5. **Warmup remains planned only**, as separately requested; no implementation is authorized by these reproduction results.
