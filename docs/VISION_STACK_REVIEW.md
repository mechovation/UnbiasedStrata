# Vision stack review and improvement specification

Review date: 2026-10-29. Scope: **reading images in chat**, especially repeated screenshots in coding-agent conversations, not image generation.

## Executive findings

The initial source inspection did not establish an end-to-end speed ranking. A subsequent [live comparison](VISION_STACK_BENCHMARK.md) found similar warmed small-image performance, a repeatable first-image warmup delay, and higher cached-4K overhead in UnbiasedStrata; its runtime-library limitations are documented there. Source inspection establishes several avoidable costs and missing safeguards:

1. **Warm images still incur image decoding and mtmd preprocessing before the embedding-cache lookup.** Strata's Python layer checks cached embeddings before invoking its native reader.
2. **Loading a projector routes even text-only chat through `GENI`.** This creates an empty temporary file and, on the `strata` backend, rebuilds/uploads the image-position table unnecessarily.
3. **Vision inference runs synchronously on the queue thread**, without an encode deadline or cancellation check between images. A hung encoder can block subsequent work indefinitely; a native abort can terminate the HTTP server.
4. **GPU allocation ordering is not coordinated.** The engine child starts before the in-process projector initializes/warms up. Strata normally warms its reader before starting the engine, or uses optional elastic reservation.
5. **Input compatibility and quality differ:** default builds cannot fetch HTTPS images; EXIF orientation is not applied; WebP depends on an optional ffmpeg path; actual image-token budgets are not independently checked.
6. **CPU vision misses Strata's AVX-512 flash-attention safeguard.** This is relevant to CPU fallback/default builds, not evidence of a GPU-vision regression.

Reported functional issues have now also been exercised against the unchanged native frontend and real projector: see [the issue-validation report](VISION_ISSUE_VALIDATION.md) for reproduced failures, controls, injected-fault qualifications and unverified items. That phase is distinct from both the reference Python unit tests and the performance A/B.

The old “images are not cached” defect is **already fixed**. Preserve the C++ cache, literal-token protection, projector-width validation, image-key handling, and bounded retention. The local deployment **already enables GPU vision**; describing it as CPU-only would be incorrect.

## 1. Evidence and baselines

| Component | Reviewed revision / state |
| --- | --- |
| UnbiasedStrata | `22abfdb87f3465a43ce1e2b2269a462bc3cf60f0` |
| Its llama.cpp submodule | `44072a19fb94d0bed63d381d42f6237373f91d54` |
| Sibling Strata checkout | `5e8a4cca7d690ac60ca37eb09e7d0e010099ad6d` (0.2.7 release notes) |
| Local UnbiasedStrata modifications | Existing `CMakeLists.txt` change adds `STRATA_FRONTEND_CUDA`; existing `deployment/Dockerfile` enables it. These are not upstream-default behavior. |
| Local deployment configuration | `docker-compose.yaml`: GPU projector, 1024 image tokens, `strata` backend, 262144 context, 1200-MiB engine VRAM reserve, only RTX 5070 Ti UUID `GPU-d14daf49-45c8-58c5-cf61-5a40342672af`. Configuration inspected; running-container state not verified. |

GitHub's public all-state issue/PR listing was screened (23 entries), then issue #14/#21 and their comments were read. This compares local source snapshots, **not a claim that either checkout is current remote main**. Strata and UnbiasedStrata need not use identical mtmd revisions; a server-only A/B must control that variable.

### Issue trail

- [#14: tokenizer reading special images; images not cached](https://github.com/robertkarl/UnbiasedStrata/issues/14), **closed**. The maintainer describes literal strings being interpreted as special tokens and images re-encoded every turn. Its comment links fixes `698b02e1` and `cd3be94f`. Current `frontend/literals.hpp`, `picture_cache.hpp`, and `strata_context.cpp` contain these protections. Do not reopen this as wholesale missing caching.
- [#21: Python-server port audit](https://github.com/robertkarl/UnbiasedStrata/issues/21), **open**. Vision-relevant reports are encoder isolation/timeouts, EXIF, formats, unreadable tool images, and URL/size limits. Treat its generated audit as a lead, not verification; findings below were checked against source. In particular, Python transparency handling is conditional, and the current mtmd pin has an optional WebP/ffmpeg path.
- The #21 comment additionally identifies deferred requests not being advanced on early `launch()` failures. Vision/context rejection uses those early returns, so any admission/error-path work must include a queued-follow-up regression test.

## 2. Current pipelines

### UnbiasedStrata

```text
HTTP image source -> bytes -> SHA-256 + stb decode -> mtmd preprocessing/tokenization
 -> queued task -> embedding-cache lookup -> [miss: mtmd inference]
 -> write all image rows to request SVE1 file -> GENI + ids + img_keys
 -> engine reads rows/builds M-RoPE -> conversation-prefix reuse -> prefill/decode
```

Evidence: `frontend/strata_context.cpp:1079-1089, 540-568, 590-628`; submodule `tools/mtmd/mtmd-helper.cpp:379-417`, `tools/mtmd/mtmd.cpp:1369-1376`; native `src/program/generate.cpp:11723-11795`.

### Strata Python orchestration

```text
load bytes (URL fetch outside inference lock) -> normalize -> hash/cache lookup
 -> [miss: send ENC to resident native strata-vision child]
 -> collect cached SVE1 files -> concatenate request file -> GENI + ids + img_keys
 -> engine reads rows/builds M-RoPE -> conversation-prefix reuse -> prefill/decode
```

Evidence in Strata: `serve/server.py:2818-2956, 4899-5084`; `tools/vision/strata_vision.cpp:283-343`.

**Python is the orchestrator, not the numerical vision implementation.** Both paths use native mtmd/projector execution. Port behavior and resource management into C++; adding Python to the serving path is not required.

## 3. Gap register

“Confirmed” below means a code-path difference was inspected, not that its latency impact was measured. Priority P1 means next implementation tranche; P2 means subsequent optimization/compatibility work.

| ID / priority | Finding and evidence | Impact / qualification |
| --- | --- | --- |
| V1 / P1 | Late cache lookup: HTTP tokenization calls mtmd before `pictures.find()`. `frontend/strata_context.cpp:1085, 593`; submodule `mtmd-helper.cpp:385-417`, `mtmd.cpp:1369-1376`. Python lookup precedes native reader at Strata `serve/server.py:2914-2926`. | Repeated screenshots still allocate/decode/copy/resize on every turn. C++ already avoids projector inference on a hit. Python still loads/hashes/normalizes bytes, so its hits are not zero-cost either. |
| V2 / P1 | `has_mtmd` is used as “contains images” in `launch():312-315`, but tokenization sets it when a projector exists (`:1013`; submodule `tools/server/server-common.cpp:968`). `encode_pictures():502` creates a file even with no image chunks. | Text-only chat becomes `GENI`, including possible refusal when the backend lacks `--vision`. Native `generate.cpp:11725-11795` rebuilds positions, synchronizes CUDA, and uploads the full table. Benefit of fixing this needs measurement; table size scales with configured context. |
| V3 / P1 | Synchronous `mtmd_encode_chunk()` at `:611` runs inside queue launch. No between-image cancellation checks at `:540-568`, no encoder timeout. Python has a child, 300-s startup/encode waits, and checks cancellation between pictures. | Slow encode delays queue callbacks, cancellation, queue-backed metrics, and next requests. Native crash/OOM can take down HTTP serving. Python also cannot interrupt an already-running encode cooperatively; it is not instantaneous cancellation. |
| V4 / P1 | Context room is checked **after** `encode_pictures()` (`:312-330`); configured image limit is only passed to mtmd (`:475`), not checked against actual chunk count. | Requests known to exceed context can waste inference first. Oversized tokenization can cause excessive work if mtmd ignores a cap for a model. Current Strata helper rejects actual tokens above cap + integer cap/8, but only **after** inference (`tools/vision/strata_vision.cpp:308-319`). Improve on that placement. |
| V5 / P1 | Engine spawned at `frontend/main.cpp:178`, projector loaded later through `:204` and `strata_context.cpp:1292`. Python's normal reader starts/warmups before engine sizing (Strata `tools/vision/strata_vision.cpp:249-283`). | Allocation order is nondeterministic relative to engine loading. Local 1200-MiB reserve is real mitigation, **not proof of sufficient headroom** or of an observed OOM. `cp.warmup=true` alone does not fix ordering. |
| V6 / P1 compatibility | `CMakeLists.txt:2235` forces `LLAMA_OPENSSL=OFF`; ordinary HTTPS image fetch throws without a TLS backend (`common/http.h:107-114` in submodule). Dockerfile does not enable another TLS provider. | HTTPS image URLs fail despite Python support. This is availability, not a measured inference-speed issue. Use installed TLS libraries; no runtime/build-time source downloads. |
| V7 / P2 quality | Direct stb decode has no EXIF transpose (`mtmd-helper.cpp:409-417`). Python uses optional Pillow for rotated JPEGs (`serve/server.py:2858-2887`). | Phone images can be interpreted sideways. Python parity itself depends on Pillow availability. |
| V8 / P2 compatibility | WebP falls through to ffmpeg only under `MTMD_VIDEO` (`mtmd-helper.cpp:419-430`); local runtime Dockerfile does not install ffmpeg. Python converts non-stb formats with optional Pillow. | Do not claim all mtmd builds lack WebP. For this runtime, WebP support is not provided by the listed packages and needs an explicit codec policy. Alpha-to-white behavior also needs a specified test, not an assumption. |
| V9 / P2 CPU | Frontend leaves `cp.flash_attn_type` at AUTO (`strata_context.cpp:466-479`), rather than forwarding the parsed option. Strata disables default AUTO on AVX-512 CPU and caps its implicit CPU thread count at 16 (`tools/vision/strata_vision.cpp:217-230`). | Upstream comments report a slow/less-accurate fallback for head size 72. Those historical figures are **not measurements of this deployment**. Expose vision-specific FA/thread policy and test it. |
| V10 / P2 recovery | No Python-equivalent “omit unreadable tool-result image and continue” behavior. Strata `serve/server.py:4915-4948`, `serve/test_vision_tool_image.py`. | An image failure in resent tool history can poison subsequent turns. Keep user-image failures explicit; make tool-result fallback visible. |
| V11 / P2 device | `params.mmproj_device` is not assigned to `cp.device` in `init_vision()`. | Ignored explicit device selection outside a masked environment. Local Compose already masks to the 5070 Ti, so this is **not evidence of RTX 3060 use**. |

### Existing strengths and shared costs (not regressions)

- C++ LRU defaults to **64 entries / 1 GiB accounted row storage**, has shared ownership, hit/miss/eviction counters, and cache-disable mode. Python uses 64 disk files without a byte budget. C++ eviction cannot invalidate rows currently held by a caller.
- The C++ byte budget is **soft**: the newest entry survives even if it alone exceeds the limit; outstanding shared references and preprocessing memory are outside accounting (`frontend/picture_cache.hpp:10-12, 54-101, 159-165`). It is not a process-memory safety limit.
- Stable SHA-derived `img_keys` are emitted (`strata_context.cpp:396, 564`). The `strata` backend combines the 64-bit key with image grid for conversation identity (`generate.cpp:11765-11772`). This is distinct from the embedding cache.
- Both servers still assemble a request file and both native GENI paths read embeddings before conversation reuse. C++ writes cached vectors (`strata_context.cpp:559`); Python copies cached files (`serve/server.py:5077-5084`). Reducing this requires an engine/protocol extension, **not merely copying Python caching code**.
- `strata-poly` in this tree derives picture identity from rows (`src/poly/main.cpp:212-254`), rather than using `img_keys` as `strata` does. Test backends separately.
- Frontend pictures are currently restricted to the Qwen `<|image_pad|>` convention (`strata_context.cpp:445-448`). This specification does not establish Gemma or DeepSeek vision support. DeepSeek-V4.1 has a different native vision path and needs a separate adapter audit.
- `serve/images.py` in Strata is optional **image generation**, not the chat image reader. Its diffusion/job policies should not be copied into this vision-input specification.

## 4. Strata reference behavior to preserve or deliberately revise

All paths in this section are relative to the reviewed Strata checkout.

### Acquisition and normalization

- Accepts data URLs, HTTP(S), and trusted local files; rejects network/UNC paths. URL reads are capped at **32 MiB** with a **60-s** socket timeout and redirect handling. Downloads occur outside the inference lock (`serve/server.py:2818-2855, 4995-5002`). This timeout is not a guaranteed wall-clock job deadline.
- JPEG/PNG/BMP/GIF normally pass through unchanged. A rotated JPEG and other Pillow-readable formats are EXIF-transposed and converted to RGB PNG. On that conversion path, transparency is composited onto white. Pass-through PNG/GIF do **not** get unconditional alpha normalization (`:2858-2899`).
- Unsupported/malformed user images fail; unreadable tool-result images can become `[image omitted: reason]` (`:4915-4948`).
- Local-file acceptance is not automatically a requirement for a network API: retain an explicit allowlisted-root policy rather than broadening filesystem exposure for parity.

### Embedding cache and lifecycle

- Key: first 32 hex SHA-256 characters (128 bits) of **normalized bytes**, not URL or decoded pixels (`:2914-2915`).
- Effective LRU, normally 64 entries. A single request protects earlier files, making 64 a soft bound; a 70-image request can retain all 70 (`:2901-2947`; `serve/test_server.py:5927-5976`).
- Reader unload/restart retains embeddings; shutdown removes its work directory. Recreates a missing directory, but individual missing cache files are not validated on hits (`:2723-2728, 2800-2815, 2988-2993`).
- Startup and encode reply defaults: **300 s** each, configurable through `STRATA_VISION_READY_S` / `STRATA_VISION_ENCODE_S`. Timeout kills the reader; later service loading can restart it. Startup CPU fallback exists only when a fallback executable is configured (`:2730-2815, 4296-4333`).
- Cancellation checks between downloads/pictures and while waiting for the turn; not an interrupt inside a native inference call (`:2901-2910, 4899-4912`).

### Resource policy

- Defaults: GPU **1024 image tokens**, CPU **300**; explicit `max_tokens: 0` means uncapped. Optional minimum-token setting (`:2696-2721`). C++ currently treats nonpositive `--image-max-tokens` as “use default”; document or revise that incompatibility.
- GPU reader explicitly tokenizes/encodes a synthetic 2048-square image before READY, then the engine sizes its cache. CPU reader skips warmup, hides GPU devices, and defaults to half hardware concurrency capped at 16 threads (`tools/vision/strata_vision.cpp:131-140, 217-283`).
- Elastic vision is **opt-in**, requiring both vision elasticity and engine VRAM elasticity. Defaults are 2048-MiB encoder reservation and 120-s idle unload; encoder reserve stacks with the user's reserve (`serve/server.py:2688-2693, 4302-4358`). A refused shrink can still permit startup: this is not a hard safety guarantee to imitate.
- Combined embeddings prefer `/dev/shm` only with sufficient free space, otherwise the vision directory (`serve/server.py:165-191`). Current C++ uses `/tmp`; actual disk-vs-tmpfs placement depends on deployment.

### Existing wire contract

```text
reader startup: READY <embedding_width>
encode:         ENC <input-file> <output-file>
reply:          OK <tokens> <nx> <ny> <milliseconds> | ERR <reason>
shutdown:       QUIT

engine: GENI <max_new> [sampling options] [img_keys=<hex>,...]
             image_pad=<token-id> <request-file> <comma-separated-token-ids>
```

The engine request is one line; the wrapping above is for readability. File paths must not contain whitespace in this protocol.

Each SVE1 record is five `int32` fields followed by float32 rows:

```text
magic=0x31455653, n_tokens, nx, ny, embedding_width
embedding[n_tokens][embedding_width]
```

Native writers use host byte order (little-endian on the supported hosts), not explicit endian conversion. Concatenate records in **prompt image order**. Require positive dimensions/count, `nx * ny == n_tokens`, matching model width, complete payload, and exact correspondence with pad-token runs. M-RoPE cell positions are `(p, p+y, p+x)`; following text advances from `p + max(nx, ny)`. See `tools/vision/strata_vision.cpp:308-326` and `src/program/generate.cpp:13821-13864`.

## 5. Proposed C++ specification

**Architecture constraint, confirmed by the user:** UnbiasedStrata remains **C++/CUDA end to end**. No Python server, worker, interpreter, or Python runtime dependency is to be introduced into its deployed request path. Strata's Python implementation is a behavioral reference only. Python benchmark/unit-test clients are external development tooling, not the target architecture. If process isolation is chosen, the worker must be a native C++/CUDA executable.

These are **proposed requirements**, not claims that Strata already meets all of them.

### A. Admission before inference

1. Apply bounded source-byte, image-count, decoded-pixel, aggregate-memory, and actual image-token limits. Check dimensions before full decode where supported; check tokenized shape and context **before** projector execution.
2. Distinguish “projector available” from “request contains media.” Text-only requests MUST use `GEN`; a loaded projector must not impose per-request image-file work.
3. Reject excessive input with a stable client error before engine submission. Every rejection/cancellation MUST release the slot **and advance deferred work**.
4. Preserve configurable CPU/GPU token defaults. Define zero/unlimited semantics explicitly; reject actual tokens above the configured budget plus documented rounding allowance (reference: integer budget/8).

### B. Early, configuration-aware cache

1. Compute a raw-byte identity before decode. Cache enough immutable image metadata (grid/token count/width) alongside rows to construct warm prompts **without invoking image decode/preprocess or projector inference**.
2. Identity MUST include an encoder/configuration generation: projector fingerprint, preprocessing revision, min/max-token policy, and other output-affecting settings. A normalized-byte identity can be canonical, with a bounded raw-byte alias map to avoid repeat normalization.
3. Keep shared ownership, LRU retention, the current byte/entry limits, and cache-disable mode. Add a separate **hard admission bound**; do not reinterpret the existing soft LRU bound as a safety guarantee.
4. Deduplicate simultaneous misses for the same key. Preserve input order and duplicate image occurrences in the prompt. Never key by URL alone or reuse rows across incompatible model/projector settings.
5. Keep embedding reuse and engine-prefix reuse separate in metrics/tests. Version engine-visible image identity if output-affecting preprocessing changes; stable byte identity must not authorize stale KV reuse across generations.

### C. Responsive, recoverable execution

1. Execute vision outside the main queue callback. A supervised native C++ reader child is preferred for crash/OOM containment and enforceable deadlines; no Python dependency is needed. A worker thread alone cannot safely recover from a stuck GPU call or native abort.
2. Default ready/encode deadlines: 300 s each, configurable. Check cancellation before admission, while waiting, and between images. Define whether an active cancelled encode is allowed to complete or its worker is killed; do not claim immediate interruption without implementing it.
3. Keep HTTP health/metrics and queued-stream keepalives responsive during encoding. Return a retryable service failure for worker startup/death/timeout; malformed media remains a client error.
4. Reap failed children, clean partial request files, retain validated cache entries, and recover on the next request. Logs MUST omit source URLs with credentials, base64, image contents, prompts, and API keys.

### D. GPU/CPU coordination

1. Resolve and enforce the allowed GPU **before** CUDA initialization. In this project only the RTX 5070 Ti UUID is permitted; never probe/use the RTX 3060 for inference or fallback.
2. Do not simply move CUDA initialization ahead of the existing `fork()`. Preserve the no-CUDA-before-fork safety requirement: e.g. fork an engine child that waits on a startup barrier, start/warm an isolated reader, then release the engine to allocate its cache. Alternatively use clean exec-based processes with explicit coordination.
3. **Planned, not implemented: explicit startup image encode (user-approved plan item).** Run a representative synthetic image through the complete native C++/CUDA preprocessing and projector path before reporting healthy. Follow Strata's reference approach: a 2048×2048 synthetic input scaled to the configured image-token budget. The existing `cp.warmup=true` is not sufficient in the measured build. Coordinate this encode with process startup so its work buffers are resident before the text engine sizes its expert cache; record actual peak VRAM, retain safety headroom, and fail cleanly if the budget cannot be satisfied. Expect a longer readiness wait in exchange for lower first-image latency, not elimination of the work. Acceptance: after a fresh process start, the first real image must not repeat the observed ~7-second encoder first-use cost. Do not implement until separately authorized.
4. Optional later elasticity may release reader VRAM on idle and regrow the expert cache, but reservation updates must be atomic with GPU work scheduling and must stack with the user reserve. Static coordinated startup is the first milestone.
5. Support explicit vision device, thread count, and flash-attention policy. Preserve Strata's default AVX-512 CPU safeguard unless current-pin measurements justify changing it. Never silently fall back to CPU without reporting it.

### E. Input and protocol compatibility

1. Add TLS using pinned/installed build dependencies, not downloads or runtime replacement. Verify certificates; bound redirects and source reads. Define an explicit URL egress/SSRF policy, including DNS/address validation and revalidation at every redirect for private/metadata destinations. Proposed parity defaults: 32-MiB URL source cap and 60-s timeout, plus an explicit total acquisition deadline.
2. Specify supported codecs explicitly. Add EXIF transpose, consistent alpha handling, and WebP through a deliberate dependency choice. Test user and tool-result images separately; a tool-image omission must be visible in the prompt.
3. Preserve SVE1/GENI ordering, width/grid checks, literal-token safety, and current `img_keys` semantics. Do not modify the engine ABI for the first cache/admission fixes.
4. Engine-resident image references/shared buffers are a later, versioned protocol optimization. Both current servers pay the row-transfer cost, so first measure it; do not promise zero-copy from an HTTP cache change.

## 6. Acceptance and performance plan

### Correctness / failure tests

| Test group | Required checks |
| --- | --- |
| Text-only with mmproj | Chat paths with/without literal spans emit `GEN`, create no SVE1 file, and do not trigger repeated image-position uploads. |
| Warm repeated image | One image over 10 turns: one initial decode/preprocess/inference; later turns use cached rows/metadata; identical prompt tokens/grid and valid engine prefix reuse. Separately count URL acquisition/hash work. |
| Ordering / cache lifetime | A/B, B/A, duplicate A/A, interleaved text, >64 unique images, byte-budget eviction, concurrent identical misses, and cache disabled; no stale or freed rows. |
| Identity | Changed bytes at same URL miss; changed projector/config misses; unchanged image/config hits. Cross-backend behavior recorded, not assumed. |
| Admission | Over-context, extreme aspect ratio, malformed/truncated file, excessive decoded pixels/tokens/count; zero projector calls for pre-inference rejection; next queued request always runs. |
| Lifecycle | Cancel before/during/between pictures, worker crash/silence/start failure, temp-directory removal, queue keepalive; next request recovers and worker is reaped. |
| Inputs / quality | HTTPS/redirect/certificate failure, source caps/deadlines, WebP, transparent images, EXIF 1/6/8, unreadable user vs tool-result image, literal `<|image_pad|>` text. |
| Numerics | Same pixels, budget, projector, mtmd revision, device and attention policy: compare grid, row shape, finite outputs, embeddings and teacher-forced text logits. Establish tolerances from controlled reference variation, not arbitrary equality across CPU/GPU. |
| GPU resources | Reader warmup precedes engine allocation; worst-case image fits measured headroom; no unexpected expert-cache displacement after first real image; 3060 never visible/used. |

### Benchmark design

Use the **same RTX 5070 Ti sequentially**, same model/projector hashes, native engine build, mtmd revision, context, image budget, expert profile, KV mode, threads, sampling, and request corpus. First compare matched CPU paths and matched GPU paths; an unmatched CPU-vs-GPU result is not a frontend comparison. Do not run concurrent Strata and UnbiasedStrata inference. Any eventual service switch requires a separate coordinated maintenance step; none was performed in this review.

Workloads: text-only with/without projector; cold 1024-square image; 1920x1080 screenshot; tiny-text dashboard; portrait/EXIF photo; repeated image over 10 turns; 4/16-image prompts; cache pressure; near-context-limit rejection; concurrent queued request plus cancellation. Use synthetic/public fixtures and deterministic quality questions. Compare quality at matched **actual** image-token counts; a lower budget can be faster while losing OCR detail.

Record cold and warmed runs separately, at least 10 measured repetitions per latency case, with median/p95 and failed/cancelled requests retained. Do not silently mix cache states.

Required timings/counters:

```text
client headers / first content token / full request latency
source acquisition + normalization + hashing
cache lookup / hits / misses / evictions / resident bytes
decode + preprocess + tokenization
queue wait + projector inference
SVE1 write/read + engine position setup + prefix reuse
prefill / decode tokens and time
peak RSS / decoded-pixel bytes / peak GPU VRAM / expert-cache capacity
```

Current `picture_rows()` timing measures miss-side encoding, not prior HTTP preprocessing; prompt timing starts after engine submission (`frontend/strata_context.cpp:351, 599-603`). Therefore existing encode or prompt timings alone cannot establish end-to-end vision performance.

Proposed release gate: functional tests pass; repeated-image decode/preprocess/inference counts meet the cache contract; no unnecessary text-only GENI; no quality degradation at matched token budget; no unexplained median/p95 TTFT or memory regression against the frozen baseline. Set numerical latency/headroom thresholds **after** collecting the baseline, rather than inventing a speedup target.

## 7. Validation performed in this review

- Compiled `frontend/tests/picture_cache_test.cpp` directly with host `c++ -std=c++20 -O2 -pthread -Ifrontend` into a temporary directory: **all checks passed**.
- Ran 27 selected Strata unit tests covering token defaults, workdir, startup diagnostics, cache eviction, lazy start/shutdown, encode/ready timeout, and between-image cancellation: **27 passed**.
- A broader initial host selection ran 33 tests: **17 passed, 4 skipped, 12 errored** because host Pillow/`regex` are absent. These environment limitations were resolved by using the existing Strata Docker image, not by installing host dependencies.
- **Container validation: 54 tests passed, no skips/errors**, including EXIF, tool-result images, elastic reader policies, HTTP startup-failure/recovery/child-reaping, busy state, and full cancellation coverage. Used existing image `strata:0.2.7.1-5e8a4cc-sm86-sm120-vision` (Pillow 12.3.0, regex 2026.9.10), read-only source/root, unprivileged user, no network access or GPU mounts. HTTP tests use loopback and fake engines/readers. Their printed VRAM figures are mock values, not GPU measurements.
- The first container attempt exposed a test-environment issue: default noexec temporary storage prevented execution of the generated fake reader (missing `started-pids`, 12 subtest errors). Explicit `exec` on the disposable `/tmp` tmpfs resolved it; no source/test modification was required. ResourceWarnings about upstream telemetry file handles were also observed.
- The **initial source-review phase** performed no real-projector inference, real-engine HTTP A/B, numerical comparison, GPU benchmark, image download, container image build, production-container restart, or production setting change. Existing deployment edits were left intact.
- A **subsequent user-authorized live phase** ran 278 synthetic responses across two sequential 5070 Ti A/B passes, with production stopped/restored as needed. See [the benchmark report](VISION_STACK_BENCHMARK.md) for results, controls, runtime-library caveats and successful restoration checks. No production settings or serving implementation were changed.
- A **separate issue-focused phase** exercised the unchanged C++ frontend and real projector with a native fake text engine. It reproduced HTTPS/WebP/EXIF, unreadable tool-image history, between-image cancellation, late context admission and deferred-queue starvation issues. Native allocation fault hooks demonstrated bounded hang/fatal-process containment gaps, not actual GPU faults or a 300-s timeout violation. See [the issue-validation report](VISION_ISSUE_VALIDATION.md); production was restored and smoke-tested.

Reproduce the expanded container suite from the UnbiasedStrata checkout (uses the already-present image; no GPU access):

```sh
docker run --rm --network none --read-only --cap-drop ALL \
  --security-opt no-new-privileges --user "$(id -u):$(id -g)" \
  --tmpfs /tmp:rw,exec,nosuid,nodev,size=256m \
  -e PYTHONDONTWRITEBYTECODE=1 -e CUDA_VISIBLE_DEVICES=-1 \
  -e NVIDIA_VISIBLE_DEVICES=void \
  -v "$(realpath ../Strata):/review:ro" -w /review \
  --entrypoint /opt/strata/.venv/bin/python3 \
  strata:0.2.7.1-5e8a4cc-sm86-sm120-vision \
  -W ignore::ResourceWarning -m unittest \
  serve.test_vision_token_limit serve.test_exif_orientation \
  serve.test_vision_tool_image serve.test_vision_workdir \
  serve.test_vision_elastic serve.test_vision_elastic_http \
  serve.test_vision_busy serve.test_vision_start_error \
  serve.test_server.VisionCacheEviction serve.test_server.LazyVision \
  serve.test_server.VisionShutdown \
  serve.test_server.UntimedReads.test_vision_encode_read_times_out \
  serve.test_server.UntimedReads.test_vision_ready_read_times_out \
  serve.test_server.UntimedReads.test_a_vision_answer_in_time_is_unchanged \
  serve.test_server.PrepareCancelled
```

Existing frontend e2e coverage checks image refusal without mmproj, not successful vision. Its comment references absent `frontend/tests/images-real-engine.sh` (`frontend/tests/e2e.sh:83-88`); `picture_cache_test.cpp` tests the container, and the fake engine does not validate SVE1 records. Add the acceptance matrix above rather than treating current fake-engine tests as vision qualification.

## Recommended implementation order

The detailed dependency order, PR boundaries, technical decisions and proof gates are now defined in [VISION_IMPLEMENTATION_PLAN.md](VISION_IMPLEMENTATION_PLAN.md). Use that execution plan for implementation; the broad priorities below are not permission to skip its recipe/worker design gates.

**Planning only: implementation has not been authorized.** The explicit native startup image encode in §5.D.3 is confirmed as an improvement-plan item; no serving-code or startup-setting changes have been made.

1. **Availability/correctness first:** fix and regression-test deferred-queue advancement on early launch failure; pre-inference context/token admission; then text-only `GEN` and stage-level timing. The deferred-queue failure is now reproduced, not merely a source-audit concern.
2. **Warm-turn optimization:** early metadata/row cache and concurrent-miss deduplication, preserving literal safety and backend identity.
3. **Reliability/resource control:** supervised C++ reader, cancellation/deadlines/restart, coordinated GPU startup and measured reserve.
4. **Compatibility/quality:** TLS, EXIF/codecs/tool-image fallback, explicit CPU/device/FA policy.
5. **Only after profiling:** elastic unloading and a versioned engine-side image-reference protocol.
