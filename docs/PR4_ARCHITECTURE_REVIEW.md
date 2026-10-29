# PR4 — native vision reader: architecture review draft

**Status: architecture proposal; CPU-only foundation sequence subsequently authorized by the user.** See [implementation contract](PR4_IMPLEMENTATION_CONTRACT.md) for selected decisions, implemented foundations and remaining integration/release gates. This review alone does not authorize production or GPU work.

Scope: ownership, integration boundaries, IPC trust, lifecycle, cancellation, recovery and acceptance gates. Numeric budgets and the detailed wire schema remain review items. No code, services, GPU settings or deployment files are changed by this review.

Prerequisites: PR1 queue/admission fixes, PR2 actual-media dispatch, PR3 bounded acquisition, and [Spike A](../frontend/tests/SPIKE_A.md). Spike A passed exact reconstruction/composition for the existing Qwen3.8 / Qwen3-VL still-image path; it did **not** qualify a production IPC parser or new model families.

## 1. Recommended boundary

```text
HTTP / acquisition / template / text tokenizer / queue / native engine adapter
                       PARENT (C++)
                            |
                 bounded native IPC + owned files
                            |
       decode / preprocessing / projector / prepared image handles
                  ONE READER CHILD (C++/CUDA)
```

- Parent owns HTTP/authentication, PR3 URL acquisition, original occurrence-to-role mapping, immutable source bytes, template/literal processing, text tokens, admission, response correlation and engine transport.
- Reader exclusively owns bitmap decoding, preprocessing, projector context and encodable chunks. Parent no longer invokes image decode/preprocess/encode, directly or through `process_mtmd_prompt()`.
- Parent may decode base64 into bounded **source bytes**; this is not decoded-pixel processing.
- Reader produces isolated image recipes using actual pinned mtmd tokenization; parent composes them with text through the Spike A path. Preserve current BOS/EOS/literal segmentation exactly.
- Reader performs no HTTP requests and receives no API keys, URLs, cookies, full chat history, tool definitions or rendered prompt. Large source bytes and rows never appear in command-line arguments or diagnostic logs.
- Loaded metadata placeholders are never passed to an encoder. Reader-side prepared handles retain actual encodable chunks.

**Not PR4:** early recipe-cache hits/single-flight (PR5), EXIF/WebP/tool-image omission (PR6), explicit synthetic encode warmup (PR7), a new engine protocol, GPU concurrency, multi-GPU, persistent caching, new model families or new metrics endpoints.

## 2. Ownership and request envelope

Create an owned envelope before asynchronous preparation. It contains:

- Request/task ID, correlation to the existing result channel, generation ID and a shared request-control object.
- Owned rendered prompt, literal byte spans, source descriptors and occurrence coordinates `(message, part, role, source index)`.
- Generation parameters and validated immutable request options needed by the main queue.
- A ledger of source files, prepared handles, row references and engine files, each with one explicit releasing owner.

The parser must retain a single occurrence ordering map; duplicate images remain distinct occurrences even if their source identity matches. Acquisition is performed once. No role-based omission or opportunistic prompt regeneration in PR4.

The request-control object holds atomic cancellation/reason plus monotonic timestamps/deadlines. It contains no `server_http_req&`, borrowed `should_stop`, `Slot*`, task reference, or HTTP stack pointer. A request/result-reader-owned observer copies disconnect detection into this object while that HTTP lifetime is valid. Detection must remain active while queued/preparing, not start only after GEN or SSE headers.

### Queue integration

Use a pending-envelope sidecar keyed by task ID, with a distinct main-queue path for unprepared vision work. Do **not** send an empty-token task through today's `launch()` checks or treat missing tokens as valid admission.

Only the main queue mutates the active slot, task states and result accounting. Supervisor threads deliver bounded events into an inbox and wake the queue; they never call `launch()` or `finish_slot()` directly. Audit `NEXT_RESPONSE` wakeups for coalescing and lost-wakeup races before choosing the exact mechanism.

Keep PR1's terminal discipline: exactly one result, one resource cleanup and at most one deferred promotion for an occupied-to-idle transition. Late cancellations match task ID; stale worker events match job **and** worker instance **and** operation sequence.

## 3. Main-queue state contract

| State | Owns the serving turn? | Allowed work / exit |
| --- | --- | --- |
| ACQUIRING / QUEUED | No | Bounded parent acquisition, then existing fair queue; cancellation releases envelope |
| PREPARING | Yes | Reader decode/preprocess and recipe reply; no engine GEN |
| ADMITTING | Yes | Parent validates/composes exact prompt, counts full cells, normalizes prediction budget |
| ENCODING | Yes | Reader encodes admitted prepared handles; validates/collects rows |
| ENGINE_ACTIVE | Yes | Existing GEN/GENI protocol; reader GPU work prohibited |
| DRAINING | Yes | Existing STOP/terminal drain; residual output cannot belong to successor |
| FINALIZING | Yes until safe | Release handles/files/references; terminal result/accounting and one promotion |
| IDLE | No | Next queued work or an explicitly scheduled reader recovery turn |

These names are conceptual, not committed enum/API definitions. Parent operations that can block on files, IPC, allocation-heavy validation or large writes require an asynchronous boundary; “supervised worker” must not leave the queue waiting on those operations.

The phase order is mandatory:

```text
bounded source acquisition → queued envelope → reserve turn
→ PREPARE → validate recipe → compose exact prompt → context/resource admission
→ ENCODE → validate rows → create engine request file → GENI → terminal drain
```

Text-only work keeps GEN and does not require reader availability, **provided GPU quiescence is known**. Context refusal happens before encode; PR4 does not claim that shape-dependent context checks happen before preparation.

## 4. IPC proposal

One clean-spawned reader, one supervisor owner, one active reader operation initially. Separate framed protocol traffic from human diagnostics; never parse free-form logs as protocol.

Every frame has a fixed bounded header: protocol version, message kind, payload length, worker-instance nonce, job ID and operation sequence. Validate the header before allocating its body. Use fixed-width integers and an explicit byte order for the new envelope; handle the pinned mtmd blob separately as below.

| Operation | Meaning |
| --- | --- |
| READY | Reports pinned implementation/config generation, family, embedding width, device and agreed limits; does not itself authorize a changed configuration |
| PREPARE | Accepts parent-owned immutable source descriptors; returns occurrence-indexed recipes and instance/job-bound prepared handles, without encode |
| ENCODE | Accepts only this job's admitted live handles; produces bounded row artifacts and their descriptors |
| RELEASE | Idempotently drops specified handles for that instance/job |
| QUIT | Graceful shutdown request; not the mechanism for interrupting a stuck operation |
| ERROR | Classified bounded error, no source content or arbitrary library dump |

Parent verifies READY against its expected configuration. No child-requested enlargement of budgets. A handle is invalid after child restart; a complete parent-owned cache entry is configuration-bound, not PID-bound.

### Data transfer and file lifetime

Recommendation: private parent-created per-job storage with explicit descriptor/file ownership, rather than bulk bytes in control frames. Finalize descriptor passing versus fixed inherited channels in the wire-level review.

- Parent supplies immutable sources; reader must not choose arbitrary source paths.
- Row output goes only to parent-authorized bounded storage. Completion is explicit; partial files never become usable rows/cache entries.
- Parent validates exact byte length, checked size products, family/width/grid, finite floats and source/occurrence identity before publication.
- Reader must relinquish write access before the parent trusts a supposedly immutable artifact. A hash alone does not prevent later mutation. Descriptor closure/sealing or parent-owned copy is a required decision, not an optimization.
- Parent retains engine-visible SVE1 files until engine completion or completed STOP/drain. Reader RELEASE does not release engine files.
- On cancellation/failure, remove only this job's owned artifacts; do not depend on child cleanup to recover abandoned files.

### Blocking trust issue: mtmd metadata loading

Spike A uses public mtmd save/load on trusted internal data. It is **not safe to promote its helper unchanged into a worker trust boundary**. At this pin there are length checks, but arithmetic/semantic validation and complete-consumption guarantees are insufficient for hostile/corrupted metadata; an outer frame cap alone does not solve this.

**Recommended review direction:** a narrow, pin/version-specific structural preflight validator for the permitted still-image serialized form, before invoking public `mtmd_input_chunk_load()`. It must bound every nested length/count, check overflow and dimensions, reject unexpected types/flags/trailing bytes, and then verify the loaded public metadata against the declared recipe. It may validate real upstream-generated bytes; it must not manufacture serialization or bypass the public loader.

Recipe text fragments also cross this trust boundary: validate every token ID against the actual parent vocabulary before composition/append, reject sentinel/null/negative/out-of-range IDs, and bind fragments to the expected generation and occurrence layout. Metadata validation does not substitute for text-token validation.

This is a dependency-coupled validation surface and needs its own tests/review. **If that surface cannot be made defensible, stop and propose a separately reviewed pinned mtmd API extension.** Do not silently update the submodule, trust the child, or call post-load checks sufficient. The exact choice is still open; IPC implementation is blocked on it.

## 5. GPU exclusion and lifecycle

Reader preparation, encoding, startup and recovery allocation all require the same exclusive GPU turn as engine activity. A “CPU preprocessing” label is not proof that mtmd performs no GPU allocation. First implementation permits no overlap.

Recommended startup sequence:

1. Establish the single permitted GPU mask.
2. Clean-spawn reader; load projector and preserve existing backend allocation behavior.
3. Validate reader READY.
4. Clean-spawn engine; retain existing native engine settings and automatic sizing behavior.
5. Advertise initial global readiness only after both required components are ready.

Use `posix_spawn` with explicit descriptor actions, close-on-exec discipline, filtered environment and no shell. Switch the engine launch to the reviewed clean launcher before introducing threaded reader startup/recovery; today's `fork()` assumes no threads exist. Preserve engine launch, error, EOF and shutdown semantics with parity tests.

No explicit synthetic encode warmup in PR4. No production reserve/cache-sizing changes are implied. Reader memory must fit during normal engine KV growth **and** reader restart; a freed reader allocation is not permission to grow the engine into that space. Headroom qualification is a release gate.

On runtime reader failure, temporarily retain GPU exclusion until process cleanup/reaping and the applicable GPU safety checks finish. Reaping is necessary but not proof of recovery from every driver fault. If quiescence remains uncertain, fail closed for **all** GPU work, including text; do not start another reader or silently use another device/backend.

### Whole-service teardown and startup rollback

Stop admission and disable recovery before shutdown. On partial startup failure, terminate/reap any already-created reader/engine and release owned artifacts. Graceful shutdown must have a bounded escalation path; reader QUIT is advisory, not sufficient evidence of exit.

Parent death/control-channel EOF must terminate the reader, including a stuck GPU operation via an independently effective mechanism. Specify parent-death signaling/monitoring and process ownership in the launcher review; pipe EOF alone cannot interrupt a child blocked in CUDA. Ensure no orphan engine or reader survives to conflict with a service restart.

Engine death during reader work triggers whole-service fail-closed cleanup rather than reader recovery. Retain GPU exclusion until both children are safely quiescent. Release engine-visible files only after terminal drain or confirmed engine termination makes further consumption impossible. If teardown cannot establish quiescence, do not advertise readiness or spawn replacements.

## 6. Cancellation, deadlines and recovery

- Cancellation is sticky and independent of the main queue's current phase. Queued cancellation prevents PREPARE; cancellation during PREPARE/ENCODE requests supervisor termination rather than waiting for the reader's command loop.
- Supervisor monitors blocked reads/writes, not merely message arrival. Operation deadlines use monotonic absolute time and do not reset on progress.
- Before GEN: no engine STOP. Cancel/error releases the job after safe reader teardown/handle cleanup.
- After GEN: retain the current engine STOP and terminal drain contract. Do not restart the reader into an active engine turn.
- The failed request is not automatically retried. Once safe, later requests may use a restarted reader.
- Recovery itself is scheduled GPU work. No background reader load while text inference runs. Bound retries/backoff and provide an explicit failed/unavailable state rather than an infinite restart loop.
- PR4 has no single-flight. Reserve IDs/lifetimes so PR5 can add waiter ownership later; do not implement partial coalescing now.

Proposed ready and encode deadlines remain **300 seconds**, configurable, from the implementation plan. PREPARE, queue wait, total request lifetime, termination/reap grace, and restart windows are **not yet numerically frozen**. PR3's acquisition cap/deadline stays unchanged and is not the worker-operation deadline. Short injected deadlines exercise control flow, not the real 300-second release behavior.

## 7. Availability and API semantics to approve

Recommendation:

- Initially configured vision requires reader readiness before global startup readiness.
- After a safely contained reader failure, text can continue while vision returns a clear 503 during unavailable/backoff periods.
- If GPU quiescence is uncertain, readiness fails and all generation is refused; `/health` must not suggest idle capacity.
- Preserve configured capability separately from current availability. Propose additive `vision_status` information on existing `/props`, rather than claiming `mctx != nullptr` means available vision or adding a metrics endpoint.
- Keep successful streaming headers at the current boundary. Pre-GEN context/source refusals remain ordinary HTTP errors; no early SSE headers merely to hide queue time.
- Proposed mapping: invalid/unsupported source/layout/budget/context → 400; unavailable/restarting reader → 503; worker deadline → 504; internal/protocol/native failure → 500. Keep PR3 acquisition mappings distinct (including upstream 502).

These are proposed observable semantics, particularly runtime readiness and status codes. Review them before coding. Do not expose raw child diagnostics in HTTP responses.

## 8. Hard budgets — required before implementation freeze

The current 64-entry / soft 1-GiB row-cache setting is **not** a process memory bound. Preserve the existing late row-cache hit/eviction/held-row behavior as an acceptance gate, while accounting for held rows and in-flight allocations separately; any deviation needs explicit review. Do not introduce early recipe lookup or single-flight.

The budget review must specify and test independently:

- Image occurrences and unique sources per request.
- Per-source bytes and aggregate queued/acquired bytes.
- Queue/envelope count and aggregate pending request storage.
- Decoded dimensions/pixels/channels and aggregate decoded/preprocessed memory.
- Prepared handles/buffers per job and reader instance.
- Recipe text IDs, image cells, positions, metadata bytes and frame sizes.
- Row bytes per image/request and aggregate live rows (including held/evicted cache entries).
- Artifact count/disk bytes/open descriptors, supervisor event count, and diagnostic rate limits.
- Reader/engine peak GPU and host memory during startup, long-context operation, cancellation and recovery.

For each: define measurement, checked arithmetic, admission phase, owner, release boundary, failure status and stress case. Numeric defaults require targeted evidence; do not reuse the spike's arbitrary test limits or quietly broaden PR3's source cap. Linux process/container limits may provide a final containment layer but do not replace image/pixel admission.

## 9. Review and implementation gates

### Before implementation

1. Approve this ownership/state/exclusion model and runtime availability semantics.
2. Resolve mtmd metadata trust and artifact immutability.
3. Specify the exact IPC/framing, envelope/result-reader lifetime and queue wakeup contract.
4. Approve measured hard budgets, deadlines and restart policy; approve any narrowly scoped qualification probes separately.
5. Review a PR4 file/commit breakdown. Likely units: clean launcher, native fake-worker/protocol tests, reader/supervisor, then frontend queue integration. No mixed PR5/6/7 work.

### Required acceptance evidence

- Existing adapter/literal/cache/HTTP/queue tests plus Spike A remain passing.
- Native fake-reader cases: malformed/oversized/truncated/trailing frames, corrupt metadata, blocked IO, silence, EOF/abort, stale replies, cancellation in every phase, deadline/reap failures, bounded recovery and descriptor/file cleanup.
- Startup rollback, graceful/forced shutdown, parent death, control-channel EOF and engine death during reader work: no orphan children, unsafe artifact release or replacement GPU work.
- Exactly-once results/promotion; no successor GEN before cancelled engine terminal drain.
- Decode/prepare/encode faults do not crash the parent. Native allocation injection is explicitly not called a real GPU fault.
- Full source→recipe→rows→GENI integration preserves ordered duplicates, literals, tool history, prompt cells/positions and finite exact-shape rows. Text-only remains GEN.
- Coordinated 5070 Ti-only qualification proves exclusion, actual cancellation behavior, restart/headroom and restoration. No 3060 use, alternate Compose YAML, automatic production deployment or dependency downloads.

**Stop point:** review this architecture and its open decisions. No PR4 implementation, worker process launch, production pause or GPU qualification is authorized by this document itself.
