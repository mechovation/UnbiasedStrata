# PR4 implementation contract — foundation stage

Status: approved sequence; CPU-only foundations may be implemented/tested. This document does **not** freeze unmeasured production budgets or authorize deployment/GPU qualification. Supersedes the architecture draft's blanket implementation stop only for the foundation stage described here.

## Work breakdown and gates

1. Standalone clean-launch primitive and native fake-child tests. Do not switch `Engine::start` until engine transport parity, bounded teardown and parent-death ownership are tested.
2. Bounded wire parser and native fake-reader tests, including truncated/oversized input, stale identity, blocked IO and absolute deadlines. No mtmd loader on untrusted bytes yet.
3. Pin-specific metadata preflight with real tokenizer fixtures, memfd artifact accounting/validation, and a supervisor-owned per-job state core are implemented/tested. Request/result-reader cancellation observation, bounded inbox/wakeup wiring, process-supervisor recovery and whole-process budgets remain.
4. Reader/supervisor, then queue integration, only after resource limits are qualified and parent-death/teardown mechanisms are complete.
5. Separately coordinated 5070 Ti qualification and release review. No automatic deployment.

Existing uncommitted PR1–3, Spike A and deployment work must remain intact. No submodule update. PR5–7 remain excluded.

## Launch boundary

Use `posix_spawn`, never a shell or PATH lookup. The caller supplies the exact executable, argv and an explicit environment snapshot; the primitive must not read `environ`. No request content or secrets in argv. Production callers must construct an allowlist, not copy the service environment. Reader must receive the single approved CUDA mask and only reviewed backend/runtime settings; environment allowlisting and engine compatibility need a separate integration audit.

Use CLOEXEC pipes and explicit stdin/stdout/stderr mappings. With a control socket, map only that socket to FD 3 and close all descriptors >=4; otherwise close all descriptors >=3. Validate borrowed descriptors before any allocation and keep them stable through launch. Normalize internal descriptors above stderr so launching with closed standard descriptors is safe. Reset signal mask and catchable signal dispositions in the spawned process. Direct spawn errors are synchronous; failure closes all created descriptors. With the guard, target exec failures instead surface through child exit/EOF before READY. The returned PID and parent pipe ends have one owner. A successful spawn is not READY or proof of GPU safety.

Initial implementation targets Linux/glibc with `posix_spawn_file_actions_addclosefrom_np`; fail compilation rather than silently omit descriptor isolation on unsupported platforms. Do not use this primitive in production until portability/build checks pass for the production image.

**Implemented, isolated process ownership foundation:** `process_guard.cpp` installs SIGKILL PDEATHSIG, verifies the expected parent PID after installation and execs the target. `protected_spawn` invokes it; plain `clean_spawn` has no parent-death guarantee. Exit 125 means guard setup/identity failure; 126 means target exec failure. Native tests cover actual parent death before guard installation, blocked-target parent death, pipe EOF, startup rollback and TERM/KILL escalation. `process_lifecycle.hpp` bounds reap attempts; timeout/error retains ownership and forbids replacement. This proves process cleanup, not GPU quiescence.

Linux PDEATHSIG follows the **spawning thread**: that thread must live as long as its children. A test verifies that transient-owner thread exit kills the child even while the parent process remains alive. Use one persistent supervisor owner; no concurrent waitpid/kill users. Targets must be trusted non-setuid executables, must not change credentials/clear PDEATHSIG, and must not escape the parent's PID namespace or spawn untracked descendants. PID-1 supervisors are accepted: Linux namespace-init death kills remaining namespace members, covering the otherwise ambiguous reparenting-to-1 case. Namespace/PID-1 deployment qualification remains outstanding. Both engine and reader still need integration; pipe EOF alone cannot interrupt blocked native code.

## Selected IPC envelope

Use a private AF_UNIX stream socket with SCM_RIGHTS descriptor transfer (not paths). Fixed 40-byte header, unsigned little-endian fields, no native struct casting:

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | ASCII `SVR1` |
| 4 | 2 | protocol version = 1 |
| 6 | 2 | kind |
| 8 | 4 | payload byte length |
| 12 | 4 | reserved = 0 |
| 16 | 8 | worker instance nonce, nonzero |
| 24 | 8 | job ID |
| 32 | 8 | operation sequence |

Kinds: READY=1, PREPARE=2, PREPARED=3, ENCODE=4, ENCODED=5, RELEASE=6, RELEASED=7, QUIT=8, ERROR=9. READY and QUIT use job/sequence zero; job operations require both nonzero. ERROR is accepted only for the expected operation or startup. Replies must match expected kind, instance, job and sequence. Unknown kinds, reserved bits, duplicate terminal replies and unsolicited replies are protocol failures. Stale replies cannot complete any job. No progress frame extends a deadline.

Validate the whole header, expected reply identity and the caller's hard frame cap before allocating payload storage. Command schemas and exact FD counts are now specified in [PR4_WIRE_SCHEMA.md](PR4_WIRE_SCHEMA.md), with a separate no-allocation validator in `vision_payload.hpp`. PREPARE/ENCODE operate on one occurrence per operation; repeated sources remain distinct. The implemented `vision_transport.hpp` is a generic bounded byte/FD transport, **not** a payload/recipe validator; integration must call both layers. Tests use explicit synthetic caps; production caps are not inferred from them. Use subtraction-based remaining-length checks and checked products.

SCM_RIGHTS travels with exactly the first byte of each frame; subsequent header/body reads use recvmsg too and reject ancillary data. The transport supports at most four descriptors per frame; the caller declares the exact expected count (ERROR requires zero). Use MSG_CMSG_CLOEXEC; reject MSG_CTRUNC and close all installed received descriptors on failure. Extra protocol bytes are never ignored: the next header must match the next expected operation. Complete-consumption checks inside each payload remain the schema validator's responsibility.

One supervisor owns reads/writes and one operation at a time. MSG_DONTWAIT plus poll bounds partial reads and writes with monotonic absolute deadlines; cancellation is checked at most every 10 ms while polling. Progress never resets a deadline. Any failed operation closes/poisons the channel; no resynchronization or retry on partial frames. Received frames own their FDs; outgoing sends do not relinquish local ownership. Failure leaves the caller's prior output untouched. Duplicate replies require the owner to advance its expected sequence; transport alone has no job-state registry. Bound diagnostics independently; never put source bytes or child library dumps into HTTP errors.

## Artifact immutability decision

Parent creates sealable memfds for immutable sources; finish writing and require WRITE/GROW/SHRINK/SEAL seals before passing them. Reader never receives arbitrary paths. For rows, parent creates an exactly sized memfd, seals GROW/SHRINK **before** passing it to the reader, and after completion requires WRITE/SEAL before reading/validation. Failure to seal (including a remaining writable mapping) fails the operation. Merely closing a worker descriptor or hashing does not establish immutability. Do not publish any artifact before seals, exact length, finite-float/shape/identity validation and hard-budget accounting succeed. `vision_artifact.hpp` now implements this parent-owned backing, exact-length/type/seal checks, identity/shape binding and a fixed-buffer finite-float scan. Row initialization uses NaN poison to detect untouched cells, but cannot prove encoder correctness: a writer can deliberately write finite zeros or hole-punch before the WRITE seal. Final seals prevent subsequent mutation. Admission still needs reader process limits; sealed output sizes do not contain unrelated allocations/files created by native code.

The artifact ledger reserves logical backing bytes and one parent FD per artifact before creation; shared references retain their entire charge until the last owner releases them. It does not account for source-acquisition heap buffers, recipe heap capacity, decoder/preprocessor memory, reader-side duplicate descriptors, cache row copies, SVE1 disk bytes or kernel/allocator overhead. Those remain separate qualification ledgers, not inferred from this cap. `send_borrowed` transmits the parent-owned FD without an untracked local duplicate or adoption; keep the artifact alive through RELEASE acknowledgement or certified child termination. Memfd operations run only off the main queue; deadlines/cancellation are checked between bounded IO blocks, not guaranteed to interrupt a kernel stall.

Parent alone creates engine-visible SVE1 files in private job storage. Keep these through GENI completion or confirmed engine termination, not merely reader RELEASE. RELEASE is instance/job-bound and idempotent. No cross-job handle reuse. Failed jobs are never retried automatically.

## Metadata trust decision and audit

Implemented a narrow no-allocation structural preflight in `vision_metadata.hpp`, without a dependency change. Ten genuine public-save fixtures and corrupted/truncated variants exercise it; the CPU real-tokenizer harness calls the public loader only after preflight and checks loaded public metadata. Exact accepted layout and remaining integration responsibilities are in [PR4_WIRE_SCHEMA.md](PR4_WIRE_SCHEMA.md). Pinned `third_party/llama.cpp/tools/mtmd/mtmd.cpp` serializes version 2 using host endian values. Keep this blob separate from little-endian IPC; only the qualified same-build ABI is supported.

The image form is: u64 version; u32 chunk type; u64 text-token count plus i32 tokens; u8 image-present; five u32 fields (nx, ny, pos, image_idx, temporal merge); u64 ID length plus ID bytes; u8 is_audio; u64 batch count; each batch entry is two u8 flags plus three i32 values (lead_pad, nx, ny); u8 audio-present. Image recipes must have no text payload, image-present=1, is_audio=0, audio-present=0 and exact end consumption. Text fragments travel separately and every ID is checked against the parent vocabulary.

Current loader string check uses `read_pos + len`; batch minimum-entry check accounts for only two of the three i32 fields; booleans accept arbitrary nonzero bytes. These are reasons to preflight **before** public loading, not to trust post-load accessors. Preflight must use remaining-length checks, canonical booleans, bounded ID/batch counts, positive bounded dimensions, checked grid/position products and the actual qualified Qwen still-image position/temporal/flag values. Bind the ID and occurrence to parent expectations. Derive accepted layout predicates from real tokenizer fixtures, not invented serialized chunks. Invoke the public loader only after complete preflight; compare loaded public metadata against declarations afterward. Never encode a loaded placeholder. If the permitted layout cannot be validated defensibly, stop for a separately reviewed mtmd API change.

## Per-job ownership core (not yet a live supervisor)

`vision_job.hpp` owns copied recipes, source/row references, prepared-handle records and one pending operation identified by instance/job/sequence. It enforces sequential occurrence preparation, context admission before any encode, checked aggregate recipe bytes/image cells/positions (including wrapper text positions), and signed-int32 recipe cell accounting. Only sealed, finite validated rows are exposed. Duplicate handles are refused; retired handle IDs remain reserved within the job. Late inbox events cannot consume a current operation. Worker-side RELEASE idempotence and global handle/process registries remain implementation work.

The core gates ENGINE_ACTIVE on all validated outputs and preserves engine STOP/drain before cleanup after cancellation. Engine terminal success/failure is explicit. Request cancellation is sticky but **does not cancel cooperative RELEASE after engine drain or context refusal**. The eventual IO owner must use a bounded cleanup deadline and a separate service-shutdown abort control for RELEASE transport, not reset/reuse the request cancellation flag to hide cancellation. A timeout or native/protocol failure retains artifacts/handles until the owner certifies reader reaping and applicable GPU quiescence; uncertain safety cannot produce a terminal result. Whole-service failure requires the applicable engine safety proof too.

`take_terminal()` is once-only in this core, not proof of main-queue result/promotion accounting. The owner must retain each Job until safe completion; destroying an active Job is a lifetime error, not a cancellation mechanism. No background process loop, GPU arbiter, restart/backoff scheduler, HTTP observer or result sidecar is implemented here. Cold-path state tests and held-row accounting do not qualify existing late-cache lookup/eviction integration. GPU safety is an explicit external certification, never inferred from a boolean set by a child.

## Queue and cancellation contract

Owned pending envelopes remain keyed by task ID; no empty-token completion through `launch()`. Only the main queue owns slot transitions, results and promotion. A shared request-control object owns sticky cancellation and steady-clock deadlines; no borrowed HTTP callback, slot or task pointer crosses the boundary. Disconnect observation must run while queued/preparing and remain scoped to the result-reader/HTTP owner.

Current `update_slots()` returns immediately if no active slot; NEXT_RESPONSE itself is a no-op. Therefore adding an inbox without changing that ordering loses idle recovery events. Integration must drain the inbox **before** the active-slot early return. Use a bounded inbox and a single wake-pending flag protected by the same mutex: producer appends and posts a wake on false→true; consumer swaps/drains and clears under that mutex. A producer racing the drain must either be included or post a new wake. Queue posting and shutdown need explicit ownership tests; never discard a completion because a wake is already pending. Reserve inbox capacity before accepting an operation; cancellation cannot depend on free event capacity.

No supervisor calls `launch`/`finish_slot`. Match every event by instance/job/sequence; match cancellation by task. One result, one cleanup, at most one promotion. Before GEN, no STOP; after GEN, STOP and terminal drain before promotion. Reader load, prepare, encode and restart all reserve the exclusive GPU turn. Uncertain quiescence fails all GPU work closed.

## Limits and availability

Adopt proposed API semantics: initially vision-configured startup requires both components; contained runtime reader failure permits text only after known quiescence. Vision unavailable=503, worker deadline=504, protocol/native/internal failure=500, unsupported/source/layout/context/resource refusal=400; retain distinct PR3 acquisition errors. No early SSE headers. Report configured capability separately from availability on existing props; no new metrics endpoint.

Keep PR3's 10 MiB source cap. READY/ENCODE remain proposed configurable 300-second deadlines. **Do not freeze other numeric production limits from test constants.** A required qualification ledger must record, for each limit: measurement/fixture, checked arithmetic, admission phase, owning counter, release boundary and stress/error test. It must cover occurrences/unique sources; queued count/bytes; decoded dimensions/pixels/buffers; handles; recipe tokens/cells/positions/bytes; frames; held plus cached rows; artifacts/FDs/disk; inbox/diagnostics; host/GPU peaks; queue/prepare/total deadlines; TERM/KILL/reap; restart attempts/window/backoff. No unlimited default in production.

CPU synthetic tests may use explicit injected caps/deadlines and must label them as test values. Real decoder/preprocessor limits require evidence that admission occurs before allocation, not an after-the-fact shape check. GPU headroom and recovery budgets remain blocked on separately scheduled qualification. Preserve existing late-cache held-row semantics; do not introduce early lookup or single-flight.
