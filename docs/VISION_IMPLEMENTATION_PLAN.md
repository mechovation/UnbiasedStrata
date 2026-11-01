# Vision improvement implementation plan

**Original planning snapshot; final PR4 status:** PR1–3 and Spike A are checkpointed. **PR4 implementation, CPU/GPU acceptance and release review are COMPLETE (2026-10-30)**. No PR4 gate remains open. Production is restored healthy; deployment/default activation is separate. **PR5 CPU/GPU acceptance passed for the original Flash profile and explicit frozen poly/whole-KV test policy**, including static readerless headroom and fresh encode after restart; unfrozen poly remains disqualified and production activation remains separate. See [PR5 scope/evidence](PR5_CACHE.md) and [GPU results](PR5_GPU_TEST_RESULTS.md). PR6 normalization, role-aware typed WebP-header omission and CPU/GPU qualification passed for the exact Flash/frozen-poly policies. [Scoped security review](PR6_SECURITY.md), [candidate codec distribution checks](PR6_DISTRIBUTION.md) and [durable GPU results](PR6_GPU_RESULTS.md) are recorded. Broader unreadable-input coverage remains explicitly open, with release caveats retained; full PR6 acceptance is not claimed. Resume from the [current PR6 checklist](PR6_HANDOFF.md). PR7 and deployment remain excluded. Historical planning milestones below are not current unfinished PR4 work.

Prepared 2026-10-29 against UnbiasedStrata `22abfdb87f3465a43ce1e2b2269a462bc3cf60f0`, llama.cpp submodule `44072a19fb94d0bed63d381d42f6237373f91d54`. This is the technical execution plan, not a replacement for the [behavioral specification](VISION_STACK_REVIEW.md), [performance evidence](VISION_STACK_BENCHMARK.md), or [issue reproductions](VISION_ISSUE_VALIDATION.md).

**Historical initial gate:** the original plan called for authorization of **PR1 only**. That gate was followed by separately authorized prerequisite and PR4 foundation work; resume from the current handoff above, not this old scheduling instruction. This plan is not approval to implement every phase or deploy. Local operational/session details are in `deployment/VISION_HANDOFF.md`, intentionally excluded from the public checkpoint.

## 1. Constraints and decisions

### Fixed constraints

- **C++/CUDA serving end to end.** No Python interpreter, server, encoder worker, runtime package or Python implementation port. Existing external Python test clients/fake fixtures are development tools only.
- Preserve the pinned llama.cpp parser/template/tokenizer and literal-token protections. No dependency updates, downloaded replacement components, or silent build-time patching.
- Keep existing engine `GEN`/`GENI` and SVE1 transport initially. Do not combine this work with new model families, a new engine, multi-GPU, elasticity, persistent image caching or zero-copy protocols.
- Do not fork from a CUDA-initialized/multithreaded parent to launch/restart native processes. Clean exec-based creation via `posix_spawn` is the shipped mechanism (`frontend/clean_process.hpp`) and covers both the reader and the engine launch; with native vision enabled the legacy engine fork path is rejected (`frontend/strata_context.cpp:1435`).
- First fixes must not depend on the later worker/cache redesign.
- Startup image warmup is a **confirmed plan item, not an approved implementation**. Do not add its encode, change readiness, or change production launch settings in PR1.

### Evidence that drives priority

1. A rejected vision request stranded a valid deferred request while health reported idle.
2. Six images encoded before a known 4636-token prompt was rejected at context 4096.
3. A client disconnected after two images; all sixteen completed and reached the backend.
4. HTTPS/WebP/EXIF and unreadable tool-image history failures are reproduced.
5. Native allocation injection demonstrates shared HTTP/vision process fate, not a naturally occurring CUDA fault or a measured 300-second timeout violation.
6. Cached 4K input exposed ~132 ms extra time before headers; small warmed cases were much closer. Do not optimize on a claim that all vision is dramatically slower.

## 2. PR sequence and scope

| Phase | Deliverable | Dependencies / stop gate |
| --- | --- | --- |
| **PR1** | Queue terminal cleanup + known context admission before encoding | Ready to implement after explicit authorization; no architecture redesign |
| **PR2** | Actual-media dispatch: text-only uses GEN | PR1; independent small correctness/performance fix |
| **PR3** | TLS-backed acquisition and useful URL errors | Freeze URL/trust/redirect policy; no worker prerequisite |
| **Spike A** | Exact prompt reconstruction from tokenizer-derived recipes | Required proof before early-cache or worker prompt-path commitment |
| **PR4** | Clean process launcher + supervised native reader, two-phase jobs, cancellation/deadlines | PR1 + Spike A; separate review of IPC and lifetime contracts |
| **PR5** | Early metadata/row cache + bounded single-flight | PR4 + recipe parity; keep existing row-cache ownership guarantees |
| **PR6** | Native EXIF/WebP normalization + tool-result omission | Worker-owned decode preferred; freeze codec/dependency/normalization policy |
| **PR7** | Explicit startup encode and coordinated readiness/VRAM qualification | Separate authorization; native reader lifecycle established in PR4 (`frontend/vision_supervisor.hpp`) |

PR3 and Spike A may be prepared independently after PR1/PR2 review. PR6 may be split into smaller codec and tool-history PRs, but should use one normalization path rather than temporary duplicate decoders. Do not turn one PR into all seven phases.

## 3. PR1 — implementable first slice

### Intended files

- **Production:** `frontend/strata_context.cpp` only, unless review establishes a concrete necessity for another file.
- **Tests:** extend `frontend/tests/fake-engine.py` and `frontend/tests/e2e.sh`; add a dedicated external concurrency test helper under `frontend/tests/` if that keeps shell assertions manageable.
- No engine/kernel, CMake, Makefile, submodule, codec, worker, production Compose or sampling changes. Existing `make test` already runs the frontend e2e suite.

### Production changes

1. Make `finish_slot()` the single handled **occupied → idle** transition:
   - Idempotent: inactive slot means no extra deferred promotion.
   - Release task-owned files/state, then promote **one** deferred task.
   - Promotion must enqueue through `queue_tasks.pop_deferred_task(-1)`, **not recursively call `launch()`**.
   - No result sending or success accounting inside this cleanup helper.
2. Route the five early `launch()` failures and failed-STOP cleanup through that transition. Preserve current error types/messages and engine-death shutdown behavior.
3. Replace launch's unconditional storage reset with an inactive-slot invariant/assertion. `Slot::release()` remains a storage reset, not a general queue-capacity event.
4. Move known context validation and prediction-budget normalization **before** token copying, `encode_pictures()`, and picture-file creation:

   ```text
   acquire task → engine available? → prompt nonempty?
   → room = n_ctx - task.n_tokens() - 8; room > 0?
   → normalize output cap → encode/copy → send GEN/GENI
   ```

   Preserve negative/oversized output caps being limited to remaining room. Count full image **token cells**, not text tokens alone or compressed M-RoPE positions. This does not eliminate earlier HTTP decode/preprocessing.
5. After terminal cleanup, return without accessing the destroyed task reference.

### Lifecycle invariants

| Event | Required behavior |
| --- | --- |
| Pre-GEN rejection | One error, no GEN or SSE-begin, clean resources, one queue promotion |
| Successful send | Slot retained until the corresponding terminal boundary |
| Active client cancellation | Mark cancelled, send STOP, suppress output, **drain through terminal** before reusing slot |
| Failed STOP/send | Never send successor GEN to an unavailable engine; preserve existing supervisor/shutdown contract |
| Successful DONE | One final response, one existing metrics contribution, then cleanup/promotion |
| ERR / mismatch / nonfinite | At most one terminal error, no success accounting, cleanup/promotion |
| Duplicate helper invocation | No second promotion or double cleanup |
| Late cancellation | Must match task ID, never cancel whichever successor happens to occupy the slot |
| EOF/shutdown | No successor generation; do not promise engine recovery in this PR |

The engine stream is untagged. Tokens/progress remaining **before** cancelled DONE must never become the next request's output. A slot-cleanup fix is not permission to discard that drain requirement. Backend protocol violations after terminal completion are outside this PR's recovery promise.

### Deterministic tests, before the fix

- Use the existing vocabulary-only GGUF from the submodule; no model download/GPU needed for lifecycle tests.
- Extend the external fake engine with a first-request gate, command capture, selected terminal errors, and residual output before cancelled DONE.
- Establish actual deferral before releasing the gate. Prefer existing queue debug logs in this synthetic test or a narrow unit observer; **do not invent a production `/metrics` endpoint/test API**. `/metrics` was 404 in the reproduced build.
- Bound every client/gate wait; preserve diagnostics on failure. No sleep-only race assertions.

Required cases:

1. Gate valid A, defer over-context B, defer valid C, release A. Require A=200/B=400/C=200 **without a fourth completion request**, for streaming and non-streaming B. B must never reach the engine.
2. Bounded concurrent rejection chain followed by success, below HTTP worker capacity. Use a queue/unit test for very large chains rather than exhausting the HTTP thread pool. Verify iterative progress, one terminal result and no duplicate GEN per request.
3. Cancel deferred B; it never reaches GEN and C runs. Cancel active A with C pending; C waits for A's terminal drain. Residual A tokens/PP must not contaminate C. Repeated/late cancellation must not affect C.
4. Empty prompt, `room=0`, `room=1`; negative/small/oversized output caps; retain context-error metadata and pre-SSE rejection behavior.
5. Cover picture-preparation failure, send/STOP failure, engine ERR, prompt-size mismatch, nonfinite DONE and EOF. Use test fixtures, not production fault switches. Verify resources and exactly-once accounting.

### PR1 acceptance and stop point

- Existing frontend adapter/literal/cache/e2e tests pass in a **CUDA-free test build**.
- New gated starvation test fails on the baseline, passes after the fix, and never uses a nudge request.
- Authorized real-projector follow-up: repeat the six-fresh-image refusal with a below-limit positive control. Require **zero projector encode events**, no image request file/GEN for the refused task, and a completing queued follower. Zero backend rows alone is insufficient evidence of zero encoding.
- GPU validation, if needed, uses only the 5070 Ti and a coordinated production pause; CPU projector validation is also possible but may be slower.
- No claims that PR1 fixes cancellation during encoding, late preprocessing-cache lookup, HTTPS, codecs, native fault containment, warmup or metrics.
- Review the explicit file diff and PR allowlist; stop before PR2 unless authorized.

## 4. PR2 — remove text-only image transport

**Files:** `frontend/strata_context.cpp` plus focused frontend tests; no engine ABI change.

- Determine whether a request contains actual media chunks/placeholder cells, **not** whether `tokens.has_mtmd` or `mctx` is set.
- Only call the picture writer for actual supported images. Reject unsupported media explicitly; do not silently strip audio/video/null cells.
- Text-only extraction can use `get_text_tokens()` after confirming no media. Do not use `get_tokens()`, which asserts when `has_mtmd` is true.
- Preserve literal-mark/unmark spans, BOS/EOS handling and projector availability advertisement.

Gate: text-only chat with/without literal spans emits GEN and creates no picture file; valid image prompts still emit ordered GENI. Include an engine fixture that refuses GENI to show text no longer needs `--vision`. A text request immediately after an image may legitimately incur **one engine M-RoPE identity reset**; do not mistake that for repeated text-only GENI.

## 5. PR3 — acquisition/TLS boundary

**Likely files:** `CMakeLists.txt`, new frontend acquisition helper/tests, `frontend/strata_context.cpp` integration. Do not modify engine sources. A local Dockerfile dependency change belongs to a separate deployment change unless a generic upstream build artifact is intended.

- Prefer installed OpenSSL through the pinned llama.cpp/common linkage; prohibit BoringSSL/LibreSSL source downloads or fallback replacement. Build configuration should fail clearly if required TLS dependencies are absent.
- Verify certificates/hostnames; never solve HTTPS by disabling verification. Use local CA-controlled tests, including untrusted cert and hostname mismatch.
- Give the frontend ownership of acquisition errors and cancellation/budgets. Do not wrap all upstream parser exceptions as one generic 500.
- Do not forward service Bearer credentials to image hosts. Redact credential-bearing URLs and sensitive query values; the current common URL path can log the full source URL.
- Audit automatic redirects before enabling them. Enforce bounded redirect count, address/DNS validation on **every hop**, and an explicit private/loopback/link-local/metadata policy. A naive first-URL allowlist is insufficient.
- Preserve a single body/source ordering map for later role-aware tool-image fallback and worker preparation.

**Decisions required before this PR:** URL egress defaults/allowlists; CA trust configuration; whether to retain current 10-MiB/10-s policy initially or adopt configurable 32-MiB/60-s reference defaults; acquisition wall deadline; status mapping for source refusal/timeouts versus retryable service failures. Do not silently change body limits or local-file access for parity.

Gate: local HTTP and trusted HTTPS succeed; cert/redirect/credential/egress failures are bounded and informative; 9/11-MiB and delayed-source regressions have explicit configured expectations. No real network/metadata endpoint is used in security tests.

## 6. Spike A — prove prompt recipes before changing the image path

**Blocking API fact:** pinned mtmd exposes no arbitrary image-chunk constructor. `mtmd_input_chunk_save/load()` restores **metadata-only placeholders**, which cannot be encoded. SVE1 rows/grid are not a complete tokenizer recipe.

Prototype under `frontend/tests/` or an excluded test target; do not change production behavior merely to make the spike pass.

### Candidate artifact

Derive a still-image recipe from **actual pinned mtmd tokenization**:

- Ordered text-token fragments surrounding each media chunk.
- Serialized placeholder metadata, token count, position count, decoder grid and embedding width.
- Image identity, vocabulary/projector/preprocessing generation and supported-family marker.
- Exact ownership rules: copied token/metadata bytes, no borrowed pointers into worker buffers.

The parent uses `server_tokens::push_back()` with text IDs and public placeholder-loading APIs only after validating the recipe. It must not manufacture mtmd serialization or guess `<|vision_start|>` delimiters. It must not treat restored placeholders as encodable chunks.

### Parity gate

Compare **exact token IDs, chunk boundaries, full token count and positions** against the current path, not just generated answers. Cover BOS/EOS, plain literal spans, tools, assistant reasoning, multiple text segments, adjacent/duplicate/interleaved images, varied aspect ratios and context boundaries. Preserve the current recursive literal-token splitting; blanket `parse_special=false` is not a substitute.

Per-image composition may fail for prompt-dependent image indexing, alignment, tiling or other model families. Scope the initial supported path to existing Qwen still images; reject unsupported layouts explicitly.

**If exact parity fails:** stop early-cache implementation. Use isolated full-prompt tokenization with late caching as an explicitly limited interim design, or propose a narrow pinned mtmd API extension in a **separate submodule review**. Never fabricate chunks or silently bump the pin.

## 7. PR4 — one supervised native reader

### Selected direction, subject to the spike and interface review

One resident **C++/CUDA child**, one supervisor/IPC owner, one active vision turn; preserve the existing HTTP server and text-engine transport. No Python worker, worker pool, persistent cache, elasticity or shared-memory optimization initially.

**Shipped native units (PR4 complete 2026-10-30):** the resident child is `frontend/vision_reader.cpp` with the standalone entry point `frontend/vision_reader_main.cpp`; the supervisor/IPC owner is `frontend/vision_supervisor.hpp` (header-only, no separate translation unit); request/cancellation objects are in `frontend/vision_control.hpp` and job/recipe/ticket types in `frontend/vision_job.hpp`; bounded framing is `frontend/vision_protocol.hpp`; process creation is `frontend/clean_process.hpp`. The originally planned `frontend/vision_worker.cpp` and `frontend/vision_request.hpp` were never created — do not implement them. Contracts and evidence are the local PR4 records under `docs/` (`PR4_*.md`), which are kept out of the public repository; `docs/PR6_STATUS.md` is the published status document. The same applies to the `PR5_*` and `PR6_*` records cited throughout this plan: they are local working documents, not published files.

### Ownership and request states

- Worker exclusively owns projector context, decoded pixels, preprocessing buffers and encodable mtmd chunks. Moving only `mtmd_encode_chunk()` would leave decoder/preprocessor costs and failures in the parent.
- Parent owns immutable source bytes, request cancellation/control objects, validated recipes/rows and engine request files.
- Keep large data out of CLI arguments/control lines/logs. Private parent-created files and bounded metadata frames are adequate initially; no zero-copy promise.
- Distinguish `vision_configured`, worker availability and full cache-hit availability. Do not keep equating capability with an in-process `mctx != nullptr`.

Proposed flow:

```text
acquire bounded sources → enqueue owned request envelope
→ reserve the existing single turn
→ PREPARE sources asynchronously → exact recipe/prompt assembly
→ context/token/pixel/memory admission
→ ENCODE validated handles asynchronously → validate rows
→ write GENI → engine terminal drain → release/promote
```

Queue callbacks must not wait on worker I/O, GPU calls, or blocked large writes. An owned pending-request envelope/sidecar may be needed before `server_task.tokens` exists; do not pass an empty-token placeholder through today's `launch()` checks. Keep API result correlation with its task ID.

Main-queue states can be `IDLE / PREPARING / ENCODING / ENGINE_ACTIVE / DRAINING / FINALIZING`. Supervisor completions enter a thread-safe inbox with a queue wakeup (existing NEXT_RESPONSE may suffice); only the main queue mutates live slot state. No worker thread may retain `Slot*`, `server_task&`, or a borrowed HTTP request callback.

**GPU exclusion:** initially serialize worker preparation/encoding with the text engine's active turn. Do not launch GPU inference from HTTP handlers while the engine is decoding. More CPU-only overlap is a later optimization requiring proof that preparation is GPU-quiescent.

### Two-phase IPC contract (frozen in PR4)

Versioned bounded framing, job IDs, worker-instance IDs and explicit error categories are implemented in `frontend/vision_protocol.hpp`. Shipped operations (the `Kind` enumeration at `frontend/vision_protocol.hpp:9` also adds paired completions for `prepared`/`encoded`/`released` plus `error` and `input_rejected` replies):

```text
READY(capabilities, config_generation, budgets)
PREPARE(source) → recipe + prepared_handle
ENCODE(prepared_handle) → validated SVE1 rows
RELEASE(prepared_handle)
QUIT
```

- PREPARE must establish shape/counts **before inference**, otherwise the late-admission issue survives the redesign.
- Validate sizes and multiplication overflow, positive dimensions, finite values, exact width/grid/payload, supported layouts, and per-occurrence pad placement. Reject partial/trailing output and stale/cross-job replies.
- A handle belongs to one worker instance and becomes invalid on restart. A validated complete cache entry belongs to a configuration generation and can survive an identical worker restart.
- Bound source bytes, image counts, decoded pixels, prepared buffers/handles, IPC frames and outstanding shared rows **separately from the soft LRU limit**. Freeze numeric defaults with dedicated stress evidence before implementation; do not infer a hard process bound from the current 1-GiB row-cache accounting.

### Cancellation/deadlines

- Allocate a shared native request-control object before preparation. HTTP/result-reader detection marks it independently of the blocked queue; supervisor observes it directly. Do not borrow `req.should_stop` beyond its owning HTTP lifetime.
- Once detected: cancel queued work; stop between pictures; kill/reap an active worker operation for cancellation or deadline if necessary. Do not wait for the worker command loop to acknowledge a GPU-stuck operation.
- Initial proposed ready/encode deadlines are 300 s, configurable; kill/reap grace and restart backoff must be separately bounded and reviewed. A short fault-window test is not proof of the 300-s path.
- No engine STOP before GEN. After GEN, retain current STOP/terminal-drain semantics.
- Reject stale completions; cancellation during single-flight must not kill a job still needed by another live waiter.
- Do not immediately retry a failed request behind the caller's back. Fail it with a classified error; permit bounded reader recovery for later work.
- If a process/driver cannot be proven quiescent, fail closed rather than allowing conflicting engine GPU work. Native process isolation does not guarantee recovery from every GPU/driver failure.

### Process launch/startup

Shipped launcher: `frontend/clean_process.hpp` — `clean_spawn` (`:49`) and `protected_spawn` with parent-death protection (`:127`), using explicit file actions, `posix_spawn_file_actions_addclosefrom_np` descriptor closing, an explicit environment array and no shell or `PATH` search.

Engine-adapter start/stop parity is settled, not pending: `Engine::start` already spawns through `protected_spawn` (`frontend/vision_engine_adapter.hpp:262`), and the native path forbids the legacy fork (`frontend/strata_context.cpp:1435`).

Reader-then-engine ordering is implemented rather than future — `frontend/vision_supervisor.hpp:34` (`initialize_engine`), `:386` (reader spawn), `:403-405` (reader READY → engine initialize): establish GPU mask → clean-spawn reader → reader load (graph/buffer reservation; the PR7 representative encode is not implemented) → reader READY → clean-spawn engine → engine READY → global ready. Parent GPU allocation should be avoided/verified, but clean exec must not rely on an assumption that backend enumeration never initializes CUDA.

Keep **explicit synthetic warmup disabled/unimplemented until PR7 is separately approved**; do not hide that change inside worker integration. Existing backend warmup behavior and the new representative encode are distinct.

Static reader memory headroom must survive engine KV/workspace growth and reader restart. Do not regrow the expert cache into the reader's freed share during a restart. No automatic elasticity in the first implementation.

### PR4 gates

Native fake-worker tests: malformed/oversized frames, blocked reads/writes, silence, process abort, cancellation, stale IDs, partial output, restart, descriptor/file cleanup and backoff. Native allocation fault probes must show HTTP/text-engine survival and subsequent successful vision recovery **without** attributing CPU hooks to real GPU faults.

Keep initial HTTP/SSE error semantics explicit. Sending headers early for queued keepalives changes whether later errors can be HTTP 400 versus SSE errors; that is a separate API decision, not an incidental worker change. `/metrics` endpoint creation is also out of scope.

## 8. PR5 — early cache without rebuilding image chunks by hand

**Likely files:** `frontend/picture_cache.hpp` extension or a dedicated `vision_cache.hpp`, parent vision integration, cache/concurrency tests.

- Check configuration-aware raw-byte identity before worker decode/preparation. First use still requires real preprocessing/tokenization; complete warm hits require **no decode, preprocess, or inference**.
- Store immutable recipe + validated rows together. Preserve shared ownership, prompt occurrence order and duplicate image occurrences.
- Namespace by model/vocabulary, projector fingerprint, preprocessing implementation/policy, token limits, and output-affecting execution choices. Do not key only by URL, bytes alone across configurations, or worker PID.
- Retain the current 64-entry / soft 1-GiB behavior initially, plus independent hard admission bounds. A newly retained oversized entry and outstanding shared references must not escape process-budget accounting.
- Bound single-flight and waiter registries. One cancelled waiter does not cancel work needed by another; all-waiters-gone can cancel it. Incomplete entries are never published as valid cache hits.
- Version engine-visible image keys when output-affecting policy changes; preserve identical identities across a same-config reader restart. `strata` and `strata-poly` have different key consumption: test them separately.

Gate: counted warm hits perform zero worker preparation/encode calls; eviction and concurrent cancellation never invalidate held rows; config changes miss; repeated/mixed ordering and literal-token parity remain exact. Keep request SVE1 files alive until engine completion/drain. Both old servers still transfer rows on hits; removing that shared cost needs a later engine protocol proposal.

## 9. PR6 — normalization and tool-result recovery

Current slices: [library selection/license proof](PR6_CODEC_AUDIT.md) and
[native-reader normalization CPU integration](PR6_NORMALIZATION.md) passed. Mandatory [dependency constraints](PR6_DEPENDENCY_POLICY.md): C++ or Rust,
no Python or bespoke codecs/parsers/transforms, compatible OSS terms and no library
modifications. [Codec-specific CPU reader/projector fixtures](PR6_PROJECTOR_TESTS.md)
now pass. [PR6D](PR6_INPUT_ERRORS.md) adds a narrow typed WebP header rejection
with safe request termination for user images and role-aware omission for tool
images. Owned-input template/literal regeneration and repeated-history CPU tests
are implemented; generic stb/decoder failures remain fatal. [Real-engine/GPU
qualification](PR6_GPU_RESULTS.md) passed for the exact Flash/frozen-poly policies.
[Scoped security review](PR6_SECURITY.md) and [candidate codec distribution
checks](PR6_DISTRIBUTION.md) are recorded. Broader unreadable-input coverage stays
open by explicit direction; vendor-triage/support caveats and final shipping-image
verification remain release conditions. See the [current checklist](PR6_HANDOFF.md).

Freeze one native worker-owned normalization path. Prefer established native codec/EXIF libraries; audit build/runtime dependencies before choosing them. Do not introduce Pillow, ffmpeg subprocess requirements by accident, or a general image framework without a size/security review.

Required policy:

- Correct JPEG EXIF orientations 1–8, including mirrored cases; define malformed metadata behavior.
- Explicit native WebP support, bounded dimensions and frame selection. First-frame behavior for animated input must be documented/tested.
- Preserve pass-through PNG alpha behavior unless a separately approved policy changes it. Strata's white compositing is conditional on conversion; it is not evidence that all PNGs must be white-flattened. Namespace changed preprocessing accordingly.
- Map source occurrences back to roles before templating. On unreadable **tool-result** images, insert a visible omission note and regenerate the prompt; unreadable user images remain clear errors.
- Recompute template/literal spans after omission. Do not delete already-expanded pad runs or mutate a prepared prompt in place.
- Keep acquisition, normalization, identity and prompt-order maps aligned; acquisition must not run twice or log credential-bearing URLs through the old parser path.

Gate: EXIF grids/pixels, transparent PNG/WebP controls, malformed image/metadata, animation policy, valid tool images, repeated bad tool history and omission recovery. Then real-engine answer/logit checks at matched actual token counts; a fixed-token probe is not semantic validation.

## 10. PR7 — separately approved startup warmup

- Explicit native preprocessing + projector encode of a synthetic 2048-square image scaled to the effective budget, before readiness and before engine automatic cache sizing.
- Do not equate `cp.warmup=true` with this operation, or a single warm shape with a worst-case memory proof.
- Qualify varied shapes/aspect ratios, allowed budget extremes and reader restart with a loaded/long-context engine. Record actual token grids, peak VRAM and expert residency.
- Expect increased startup-to-ready time. Report readiness, first-image latency and warmed latency separately, not a claim that work disappeared.
- Gate: the baseline first-image shape no longer repeats the observed ~7-s encoder event; existing semantics/quality survive; measured restart/headroom limits are safe. Production reserve changes require their own reviewed direct-Compose edit.

## 11. Cross-phase release gates

- **CPU/no-model CI first:** adapter, literals, row cache, deterministic queue/error/cancel tests, framing, cache ownership and codec/TLS fixtures. No downloads required.
- **Projector fixture tests second:** real decode/preprocess/projector with native protocol probe; distinguish projected-row proof from LLM semantics.
- **Real engine last:** image/mixed/history cases on the 5070 Ti only, positive/negative controls, restoration smoke checks and bounded fault tests.
- Numerical gates compare exact token/chunk layout first, then embeddings/logits using a reference-variation-derived tolerance; do not demand bitwise equality across unrelated GPU/CPU builds.
- Performance gates use identical engine binary **and runtime libraries/build flags**, model/projector, actual image tokens, expert residency, CPU/thread limits and cache states. Existing images differ in CUDA packages; tiny deltas from that A/B are not release thresholds.
- Collect decode/preprocess/cache/encode/I/O/queue/engine timings with safe scalar telemetry. No prompts, source URLs with credentials, images/base64, API keys or raw worker command payloads in logs.
- Record failures/cancellations, explicit completion state and missing counters. Preserve old evidence rather than overwriting it with post-fix observations.

## 12. PR boundaries and rollback

### Upstream allowlist policy

- Stage files explicitly. Nothing is currently staged; untracked does **not** mean ignored.
- PR1 allowlist starts with the one production source file and its deterministic test changes. Documentation should describe that slice, not promise later phases are complete.
- Promote reusable probes/clients selectively into `frontend/tests/` and make paths/ports configurable. Do not wholesale-stage `deployment/`, whose existing files mix local operational work and test tooling.
- Raw `logs/`, SVE captures, copied binaries/model assets, secrets, local Compose/backup, `AGENTS.md`, local Docker deployment files and `deployment/VISION_HANDOFF.md` are excluded by default.
- Existing local `.gitignore` and `CMakeLists.txt` changes predate these fixes. Preserve them; do not include, revert or overwrite them casually. Later generic CMake edits need hunk-level review against that dirty baseline.
- The llama.cpp submodule pin stays unchanged unless Spike A produces an explicitly reviewed API-extension proposal. No configure-time patches.
- For public evidence, prepare a small sanitized aggregate/fixture generator or approved CI artifact. Reviewers will not have gitignored local logs; do not claim a private artifact link is sufficient reproduction evidence.

### Operational rollback

Each PR is independently reviewable/revertible. No production redeploy is part of planning or an ordinary source edit. Before a future live trial, record the production image ID/settings, validate the single allowed GPU mask, assign test-container ownership, bound waits, and establish cleanup/restoration before stopping anything.

Keep exactly the authoritative `docker-compose.yaml` and its one `.bak`; no override/alternate YAML. If a large base image ever changes, pull it independently in the background before building; any future Hugging Face download uses the designated downloader, not a build/start path. Existing assets suffice for the next slice.

A failed test must not trigger an automatic fallback to upstream Strata or the 3060. Restore the original **UnbiasedStrata** production container and verify health, authentication, text, streaming, vision and temporary-container cleanup. Follow-up implementation begins in a fresh session, from PR1, only after explicit user authorization.
