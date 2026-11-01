# PR4 acceptance — COMPLETE

**Implementation, CPU/GPU acceptance and release review passed on 2026-10-30.**
No PR4 gate remains open. The supervised `--native-vision PROFILE` path is qualified
for the pinned backend and reviewed 5070 Ti profile. Deployment/default activation
remains separate; original production is restored healthy and the 3060 is untouched.
See [release sign-off](../../docs/PR4_RELEASE_REVIEW.md),
[GPU results](../../docs/PR4_GPU_TEST_RESULTS.md) and
[closed acceptance ledger](../../docs/PR4_ACCEPTANCE.md). Historical slice limitations
below are preserved, not current tasks.

Reproduce the complete offline CPU acceptance with existing assets/dependencies:

```sh
bash frontend/tests/native-reader-acceptance.sh BUILD MODEL_GGUF PROJECTOR_GGUF NEW_OUTPUT_DIR
```

Final evidence is `/tmp/pr4-completion/qualified/`: one clean dependency/candidate
build and archive receipt; legacy adapter/literal/cache/HTTP/queue/context/acquisition
regressions; ordinary/UBSan suites; real native reader/queue/late-cache/text/recovery
under parent/reader UBSan; real blocked-reader engine-death cases; contained reader
configuration/environment/RLIMIT/READY; and the production composer's 112 image
cases within Spike A's 120-case all-BOS/EOS corpus. Final candidate refresh and
`SHA256SUMS.final` preserve those clean dependency hashes. `http-final.log` passes
all 17 real network HTTP groups, including startup rollback, streaming, exact
ordered finite SVE1, saturation, pre-SSE refusal, STOP/residual/withheld-DONE,
deadline/recovery, normal STOP/stop strings, engine faults and active/queued shutdown.
The namespace/context-retention/XID ABI tests use a **CPU fake NVML library**, not
real GPU proof. Actual frontend-as-PID-1 CPU requests and clean SIGTERM exit 0 are
recorded at `/tmp/pr4-completion/pid1-result.json`.

Cgroup ceilings were 4 CPU / 8 GiB / 256 pids, bounded 64-MiB `/tmp` tmpfs, no GPU
requests/devices, explicit disabled GPU masks and offline existing assets/image.
Peak CPU memory was 2,450,026,496 bytes with zero OOM/max events. Physical GPU
acceptance is additionally recorded at `/tmp/pr4-close/complete/`: 22 byte-identical
GPU row/token/key/grid cases, real-engine quality/SSE/literal/order checks, zero
reader/engine phase overlap, real cancellation with queued uncached vision, full
usable context, high-KV reader restart and active-plus-queued shutdown. Earlier
physical fault-observation injections in `/tmp/pr4-gpu-trial/` failed closed with
exit 70; those injections are synthetic, not destructive physical driver faults.
Reproduction uses `native-reader-gpu-build.sh`, `native-reader-gpu-acceptance.py` and the explicit
`data/vision-runtime-gpu-5070ti.json` profile under a newly authorized window.
Legacy remains the production default solely because deployment is separate.

## Preserved staged evidence and historical limitations

Current gate status: [eight-row acceptance ledger](../../docs/PR4_ACCEPTANCE.md).
Sections below retain earlier slice evidence; the combined engine integration
section supersedes earlier pre-engine-only limitations for the isolated supervisor.

Run:

```sh
bash frontend/tests/native-reader-foundations.sh
SANITIZE=1 bash frontend/tests/native-reader-foundations.sh
# Also available standalone, and included in make test:
make test-pr4 NVCC=
```

The default suite requires Linux/glibc with GNU spawn closefrom actions and a C++17 compiler. It uses no model, CUDA library, service, container or dependency download. Both suites passed locally with `g++ -std=c++17 -D_GNU_SOURCE -Wall -Wextra -Werror -pthread`; SANITIZE adds UBSan with no recovery. `git diff --check` passed.

## Clean spawn/header tests

`clean-process.cpp` (also independently run by `clean-process.sh`):

- 25 repeated clean spawns while another thread runs, plus closed-stdin/stdout launch.
- Explicit environment only; unrelated non-CLOEXEC descriptor absent in native fake child.
- Child signal mask/dispositions reset; caller settings restored by test.
- Native fake READY delivered one byte at a time; child receives shutdown byte and exits.
- Synchronous missing-executable errors; invalid empty argv/environment rejected; unchanged result on failure.
- Parent FD count returns to baseline on every cycle.
- Truncated/extra header bytes, oversized body declaration, wrong magic/version/kind/reserved bits and invalid READY job/sequence rejected.
- Wrong worker instance, job, operation sequence and reply kind do not match.

## Protected process ownership tests

`process-lifecycle.cpp`, using the real `process_guard.cpp` executable:

- Guard installs SIGKILL PDEATHSIG and transfers the explicit control socket at FD 3.
- Closed control FD is rejected before stderr duplication can reuse its descriptor number.
- TERM-ignoring child is killed/reaped with bounded escalation; stdin EOF permits cooperative exit.
- Wrong expected parent prevents target exec (125); missing target yields exec-failure exit (126).
- Partial startup rollback terminates the already-started child after the second target fails.
- A transient spawning thread exiting kills its child while the parent process remains alive (Linux PDEATHSIG semantics).
- Actual parent death before guard installation: child waits for parent-pipe EOF before guard exec, then guard rejects the changed parent.
- Actual parent death during a blocked, TERM-ignoring target: target dies by SIGKILL. A test subreaper observes/reaps the adopted child, rather than interpreting `kill(pid,0)` or a zombie as proof.
- No remaining test children after completion.

PID-1/namespace deployment, uninterruptible kernel/driver waits and production engine lifecycle are not qualified by these tests.

## Bounded byte/FD transport tests

`vision-transport.cpp`, with clean-spawned native fake peers and socketpair fault injection:

- Successful bounded payload plus sealed memfd transfer; received FD is CLOEXEC and writes are refused by seals.
- Peer abort, partial header EOF, silent peer, gradual header bytes and blocked large writes.
- Absolute 120 ms injected deadlines do not reset on progress. Successful child startup gets a separate 2-second allowance.
- Atomic cancellation interrupts blocked reads/writes; tests assert return within one second. Poll checks cancellation every 10 ms; this is not a hard OS scheduling guarantee.
- Malformed/oversized headers and wrong instance/job/sequence/reply-kind are rejected before body allocation.
- SCM_RIGHTS truncation, unexpected/missing rights, rights after the first byte, and partial header/body EOF after receiving rights all close installed descriptors.
- All failure paths poison/close the channel; descriptor counts return to baseline.
- Sending to a closed peer returns an error without SIGPIPE.
- Invalid local outgoing FD count is rejected and its destruction cannot overrun the FD array.
- Back-to-back frames are read without over-read; a duplicate reply fails the next expected sequence. Failed receive leaves previous output untouched.

All limits/deadlines here are injected test values, **not production budgets**. The test runner's 30-second watchdog is separate from the transport's own deadlines.

## Payload/metadata validation

`vision-payload.cpp` exercises the separate `vision_payload.hpp` and `vision_metadata.hpp` validators:

- All nine command kinds, exact configuration/device/occurrence/source/handle/shape bindings and FD counts.
- Complete-consumption checks, truncated/trailing payloads, vocabulary sentinel/out-of-range IDs and aggregate prefix/suffix token budget.
- Checked grid/pixel/row products, including row-byte overflow with otherwise admissible cell counts.
- Ten genuine public-save metadata fixtures; every truncation, trailing bytes, mismatched IDs, corrupt nested counts/lengths/types/flags, negative dimensions and unsupported batch/temporal/layout variants.
- Allocation-free preflight before any public loader call. These default tests do not link mtmd, so invalid input cannot accidentally reach its loader.
- Output remains unchanged on validation failure; fixture hashes are checked before tests.

Result: `vision payload/metadata: PASS real_fixtures=10 rejected=3734`, both normally and under UBSan. Fixtures and exact generation provenance are in `data/mtmd-metadata/README.md`.

The separate `vision-metadata-real.sh` qualification clean-built pinned CPU-only mtmd/llama libraries in an existing local network-disabled, GPU-free 4-CPU/8-GiB container. It loaded only the existing language-model vocabulary and the projector, generated five image shapes at token budgets 64/1024, saved real tokenizer metadata, ran preflight **before** the public loader, compared public IDs/counts/decoder positions, and validated PREPARED with real prefix/suffix tokens. Regenerated fixture hashes match all ten stored fixtures. Result:

```text
PR4_METADATA_REAL_PASS fixtures=10 gpu=none wrapped_encode_entries=not_reached
```

The expanded harness initially exposed a test bug: vocabulary-only loading leaves `llama_model_n_embd_inp()` uninitialized. The harness now follows the existing frontend's GGUF model/projector metadata width check; the final clean-build run passed. Spatial merge overrides are checked explicitly (only merge=2 qualified). Wrapped encode entry points abort if reached; this is not exhaustive instrumentation of internal calls or encode/GPU qualification. Numeric caps remain test-only, and this does not replace Spike A's whole-prompt parity acceptance gate.

## Artifact ownership and per-job state core

`vision-artifact.cpp` tests `vision_artifact.hpp` with injected byte/FD caps:

- Parent-created sealable memfds; immutable sources, source length/cap rejection, exact-size checks, and no growth/shrink of reader outputs even before completion.
- Rows are initialized with NaN poison before handoff. Untouched cells and explicit NaN/positive or negative infinity fail the final scan; finite signed zeros, subnormals and maximum finite values pass.
- Sealing occurs before the scan. A clean-spawned native fake reader keeps a writable shared mapping after its synthetic completion frame: finalization refuses it. After unmapping, finalization succeeds and subsequent writes through the still-open reader descriptor fail.
- Explicit source/occurrence/instance/job and shape mismatch checks; cancellation/deadlines prevent publication. Cancellation after sealing permits a safe standalone rescan, not an automatic job retry.
- Source creation failures and RLIMIT_NOFILE-injected memfd failure unwind accounting and preserve caller outputs.
- Concurrent reservations remain bounded; shared held/evicted references retain their full charge; the accounting state outlives the Budget facade.
- `send_borrowed` neither duplicates nor adopts the parent's artifact FD; success and failure leave it owned/open. Receiver-side FD cleanup is still transport-owned.
- Hole punching before the final WRITE seal can replace poison with finite zeros; the test deliberately documents this limit. Poison detects untouched cells, **not** whether an honest encoder computed every row. Hole punching after final sealing is refused.

`vision-job.cpp` tests `vision_job.hpp`, a single-owner per-job state core, **not a running process supervisor**:

- Ordered duplicate-source occurrences, PREPARE → admission → ENCODE, context refusal before ENCODE, owned recipe copies after original frame mutation, aggregate recipe/cell/position limits.
- Same-size wrong-shape and cross-job output artifacts are refused before ENCODE; wrong reply handles and duplicate prepared handles fail closed.
- Only validated immutable rows are exposed; sources/rows/handles survive native failures and cancellation until an explicitly certified teardown boundary.
- Sticky cancellation in every preparation/encoding/engine/finalization phase. After GEN, no cleanup/result before engine terminal drain. The request cancellation flag remains set during cooperative RELEASE cleanup; it is never reset to hide cancellation.
- Engine terminal failure produces a failure outcome, not success. Late instance/job/sequence events and duplicate RELEASE replies cannot consume a current operation.
- Exactly one terminal result from this core; held rows remain charged after job cleanup. This is not yet proof of main-queue promotion/result accounting.

Both new suites passed normally and under UBSan. All resource caps are test values. The fake reader's artifact test uses synthetic empty control payloads to isolate FD/seal behavior; it does not claim complete worker/schema integration.

## Request control and reserved notification inbox

`vision-control.cpp` tests `vision_control.hpp`: sticky cancellation and deadline
reasons, cancellation independent of event capacity, bounded reservations,
move/destruction accounting, once-only publication, wake coalescing and idle
wakeups, shutdown admission with outstanding completions, and 8,000 raced unique
completions consumed only after wakes. Ordinary and UBSan runs passed.

The inbox uses a synthetic owned wake sink, not the production queue. Its adapter
must meet nonblocking/noexcept/reliable-post and lifetime requirements. HTTP
observation, deadline polling integration, sidecar accounting and the running
supervisor are not implemented. See `docs/PR4_SUPERVISOR_CONTRACT.md`.

## Full-schema native fake worker composition

`vision_commands.hpp` builds all nine payload kinds with cap checks before
allocation/copy, then validates the resulting exact payload. It owns no FDs and
leaves output unchanged on rejection. It copies genuine metadata only; it does
not manufacture serialized mtmd chunks. Header identity, FD provenance and live
handle ownership remain separate transport/job responsibilities.

`vision-worker.cpp` now composes these builders, protected spawn/transport,
artifact validation, Job state and a reserved terminal notification:

- Two ordered duplicate sources with distinct handles and synthetic row values;
  full PREPARE/PREPARED/ENCODE/ENCODED/RELEASE/RELEASED/QUIT schemas on both sides.
- Real saved metadata, nonzero synthetic identity fields, source seals, exact row
  sizing, finite scan and row seals; held rows remain charged after completion.
- Context refusal before ENCODE and sticky cancellation through cooperative RELEASE.
- Silent/bad READY, stale PREPARED, corrupt metadata, remote ERROR, abrupt EOF,
  blocked PREPARE/ENCODE, operation timeout and total-request expiry.
- Retained writable mapping prevents publication; silent/stale RELEASE requires
  bounded termination. Job resources remain owned until the CPU-only child is
  reaped. Cancellation and deadline reasons stay sticky; terminal is once-only.
- Builder undersized caps for all commands, invalid token runs/IDs/error classes
  and oversized shape rejection, preserving previous output on failure.
- One persistent spawning/IO test owner, bounded test handshakes/observer waits,
  child reaping and parent FD/artifact counts returning to baseline.

Result: `vision full-wire fake worker: PASS (16 CPU-only scenarios)`, ordinary,
UBSan and ten repeated standalone runs. A running supervisor/restart scheduler,
actual queue wake adapter, HTTP observer, engine protocol, source hashing/decoder,
real encoder and GPU safety are **not** implemented/qualified by this test. The
fake child and synthetic data are explicitly CPU-only; its reap certification
must never be reused as a real GPU-quiescence policy.

## Isolated lifecycle supervisor

`vision-supervisor.cpp` tests the reusable `vision_supervisor.hpp` loop with native
CPU-only children. Normal and UBSan suites pass. These original lifecycle tests retain the exclusive authority turn for the
**whole reader lifetime**; READY never releases it for engine work. Later sections
cover Job and engine integration. No serving path is wired.

Coverage includes:

- One persistent process/IO owner; exact READY validation and cooperative QUIT.
- Startup config/exec/EOF/silence failures, bounded TERM/KILL/reap and rollback.
- Explicit recovery after backoff, fresh instance IDs, overflow refusal, lifetime
  attempt and elapsed restart-episode exhaustion, and no autonomous respawn.
- Turn denial, post-reap safety denial, injected reap timeout, retained PID/lease,
  forbidden replacement, and successful later cleanup/certification.
- Idle process exit, control EOF and unsolicited frames, including health checks
  under sustained command traffic.
- Capacity reserved before launch, wake-driven consumer, shutdown/engine death
  independent of full inbox capacity, and 50 concurrent submit/shutdown race runs
  requiring one completion per accepted command and eventual inbox settlement.
- Separate-process permanent proof/reap failure and missing engine-proof tests:
  the service terminates instead of releasing the turn or reporting success. A
  test subreaper verifies actual PDEATHSIG cleanup/reaping of an orphan reader.
- Parent FD counts return to baseline. Explicit environment excludes inherited
  HOME; this is a synthetic test, not a production environment allowlist audit.

Review caught and fixed an accepted-command/shutdown race and command-traffic
starvation of idle failure checks. The tests now consume only after wakes and
exercise permanent fail-stop behavior. See `docs/PR4_SUPERVISOR_CONTRACT.md` for
precise authority, restart, notification and shutdown contracts.

No real engine, decoder/projector, driver quiescence, request/result accounting,
turn transfer to engine work, production budgets or serving availability are
qualified by these tests. The authority certifies only known CPU-only fake children.

## Persistent-owner pre-engine Job integration

`vision-supervisor-job.cpp` tests `vision_job_io.hpp` and the supervisor Job API.
Normal and UBSan suites pass: `vision supervisor Job IO: PASS (20 CPU-only scenarios)`.

- All preparation, transport, admission-triggered row allocation/encoding and
  validation run on the persistent owner. Genuine saved metadata is copied into
  bounded immutable snapshots; only sealed/finite rows are exposed.
- No ENCODE before admission. Admission is once-only and matches task/instance/job.
  Two sequential refused Jobs share the reader without accepting retired IDs.
- Context refusal releases all handles. Sticky cancellation during RELEASE does
  not abort cleanup; its transport uses an independent service flag/deadline.
- Blocked PREPARE/ENCODE cancellation, total/phase deadline failures, malformed
  metadata, duplicate handles, retained writable mapping, nonfinite rows, row and
  snapshot budget failures, silent/stale RELEASE.
- Teardown denial retains artifacts, authority and terminal reservation until
  later certification. Cancellation needs no free inbox capacity. Shutdown and
  injected engine-death signals interrupt blocked ENCODE and produce safe terminals.
- Terminal is once-only and acknowledgement is identity-bound. Published recipes
  and rows survive independently through held references; artifact and recipe-copy
  charges follow final ownership rather than acknowledgement.
- An aggregate shared recipe ledger limits logical copied bytes and snapshot count.
  Retaining two acknowledged snapshots refuses the third copy. Separate tests cover
  independent byte/count caps, concurrent reservation and facade lifetime.
- Wake-driven completion consumption and final FD/byte/object accounting checks.

Review fixed missing aggregate recipe retention accounting and a race where shutdown
could become visible before sticky request cancellation. Snapshot objects are now
noncopyable; consumers share them rather than silently duplicating charged strings.

These original 20 scenarios remain **pre-engine only** and cancel retained rows
rather than pretending an engine drained. They are now complemented by the combined
native engine scenarios below. A production full-prompt composer, HTTP observer and
main-queue integration remain absent. Counters are not process-RSS budgets.

## Isolated engine-handoff ownership core

`vision-engine-handoff.cpp` exercises `vision_engine_handoff.hpp` with in-process,
explicitly CPU-only authority/engine adapters. Ordinary and UBSan suites pass.
Coverage includes all four stale identity fields, one start attempt, turn denial,
pre-GEN cancellation/deadline, cancellation during authority transfer, definitely
unsent and ambiguous starts, successful/failed terminal drain, broken transport,
once-only STOP, failed STOP, drain expiry/containment escalation, immutable cleanup
deadlines, denied containment/proof and held resource lifetime. Total-deadline
observation and sticky cancellation preserve cancelled outcomes after successful
drain. Results are once-only. An isolated subprocess verifies fail-stop destruction
with an engine turn still owned.

These standalone tests use synthetic callbacks/resources, not native engine
protocol or GPU proof. The following combined suite now integrates that core into
Supervisor/Job with native CPU processes. See `docs/PR4_SUPERVISOR_CONTRACT.md`.

## Combined persistent-owner reader/engine integration

`vision-supervisor-engine.inc`, included in `vision-supervisor-job.cpp`, adds
**20 native CPU-only scenarios and two permanent-proof fail-stop cases**. Ordinary
and UBSan PR4 suites, three repeated standalone Job/engine runs, diff checks and
the unchanged-submodule check pass. Independent static re-review found no remaining
critical issue under the bounded callback contracts. The same
supervisor now runs PREPARE/admission/ENCODE, accepted `generate()`, native engine
start/STOP/drain, certified handback, and reader cleanup. Both children are spawned
on the persistent owner through the guard; the resident fake engine is reaped on
that owner before thread exit. A single CPU-only safety authority asserts exclusion.

Coverage:

- Successful generation and two consecutive Jobs on resident processes; no engine
  before validated ENCODE, stale task/instance/job rejection, no duplicate start,
  no successor before terminal acknowledgement.
- An owned synthetic request file is read by the native fake engine at GENI and
  again immediately before terminal. File and four source/row artifacts remain
  retained during active work, residual T/PP output after STOP and denied proof.
- Sticky cancellation, total deadline observation, full-inbox cancellation, explicit
  stop and shutdown; exactly one STOP after sent GEN. Cancellation during authority
  transfer avoids GEN; cancellation/shutdown during native startup contains the
  child without falsely classifying the start as safe or issuing pre-GEN STOP.
- Definitely-unsent rollback, denied engine turn, ambiguous start, native terminal
  ERR, EOF, failed STOP, silent drain timeout, reader EOF during engine work and an
  engine-death shutdown signal. Engine containment revokes global admission rather
  than allowing reader recovery into an uncertain engine turn.
- Temporary handback-proof denial retains resources and terminal capacity. Permanent
  denial after DONE and after definitely-unsent start escalates on a fixed proof
  budget, then fails the service closed. Subprocess terminate handlers verify the
  lease/resources remain owned; a subreaper verifies remaining reader PDEATHSIG
  cleanup. Engine reaping is checked before fail-stop.
- Parent FD/artifact/recipe accounting returns to baseline; engine callbacks assert
  persistent-owner thread identity, including final resident-engine shutdown.

Review found and fixed unbounded handback-proof retention. Drain, proof and
containment now have separate once-only injected deadlines; no progress extends
them. The original 20 pre-engine cases continue to pass.

This is an actual combined CPU integration test, **not** production `Engine` parity:
the fake uses a deliberately small READY/GENI/T/PP/DONE/ERR/STOP protocol and a
synthetic file, not real SVE1/prompt composition or LLM inference. The native reader
still supplies synthetic rows from genuine saved metadata. No GPU proof follows.

## Existing native line-protocol adapter in the combined supervisor

`vision-engine-adapter.inc` adds **35 integrated CPU scenarios** using
`vision_engine_adapter.hpp`, protected native spawn and the same Supervisor/Job
path. This is the implemented adapter, not the earlier hand-written test adapter.
Production `frontend/engine.hpp` remains unchanged; its seven legacy fake-engine
cases also pass.

Coverage includes exact GEN/GENI request bytes; READY/INFO and context refusal;
two requests on resident processes; native request-file lifetime; STOP/residual
T/PP/terminal drain; 20,000-token backpressure; non-STOP backends; bounded READY,
startup cancellation/shutdown/deadline, blocked command writes, closed-input SIGPIPE,
exec failure, startup chatter caps, output sink refusal and progress-flood deadlines.
ERR/nonfinite/prompt mismatch cannot report success; FATAL, EOF/partial EOF, invalid
T/PP/DONE, excess tokens, count mismatch, malformed statistics and duplicate keys
fail closed. A full lookup-chain statistics record is accepted. Idle health catches
already-queued duplicate terminals before reuse.

Review added token-total enforcement **before** sink callbacks, complete statistics
and PP validation, and absolute deadline checks between records/read blocks. The
legacy engine wire has no response IDs: this adapter relies on a trusted exactly-once
engine and cannot identify an arbitrarily delayed duplicate after a later GEN.
See `docs/PR4_ENGINE_ADAPTER.md` for that trust boundary, source/environment audit,
and remaining initial READY, result-sidecar and blocked-reader monitoring gates.
No real engine weights, SVE1 generation, inference, production service or GPU was used.

## Real native reader → parent composition → SVE1/GENI

`vision_reader.{hpp,cpp}` now performs actual source decode, isolated mtmd
preprocessing/tokenization, retained-handle encoding and RELEASE through the
existing Supervisor/Job protocol. No saved fixture supplies its recipes or rows.
`vision_composition.hpp` validates native metadata **before** public load, compares
public shape/coordinates, composes individual chunks/text with the existing literal
callback, and owns private SVE1 files through the native-line adapter's drain.
The parent never sends rendered text/history to the reader. Source hashes supply
the same lowercase-hex IDs and engine keys as the existing path.

Reproduce with existing assets and a CUDA-free static frontend build configured
from this checkout (installed OpenSSL development files required):

```sh
bash frontend/tests/vision-native.sh BUILD MODEL_GGUF PROJECTOR_GGUF
```

The runner verifies the pin/clean dependency and CPU flags, clean-rebuilds linked
libraries and records their hashes. It remains excluded from no-model tests.
The final run passed inside the existing local build-dependencies image with
`runc`, network disabled, **no GPU requests**, 4 CPU / 8 GiB, source/assets read-only
and explicit disabled GPU masks. No downloads, production changes or LLM tensor
loading. Same model/projector provenance as the metadata fixtures; image budget
64 and every other cap are **test inputs**, not approved production defaults.

Evidence on that same combined path:

- Real rendered chat history with protected control-token spellings and A/B/A
  image occurrences exactly matches the legacy prompt's IDs/chunks/positions.
  The test oracle's projector is destroyed before supervised requests begin.
- Actual CPU projector rows are finite and immutable; a native engine probe reads
  real SVE1, checks A/B/A row order and consumes exact legacy-compatible GENI.
  Owned files disappear only after engine terminal drain.
- Full-cell context refusal happens before any additional encode or GENI.
- Cancellation interrupts an **actual native encode**, safely reaps the CPU reader,
  then explicit fresh-instance restart permits a later successful SVE1/GENI request.
  The failed request is never automatically retried.
- An excessive-dimension BMP header is rejected before the pixel-decoder call
  (link-wrapped counter), bitmap/preprocessing or encode. Corrupt metadata and
  per-file disk refusal also fail without publication.

```text
PR4_NATIVE_PASS real_reader=1 real_rows=1 prompt_parity=1 sve1_geni=1 context_refusal=1 encode_cancel=1 explicit_recovery=1 predecode_refusal=1 gpu=none queue=not_integrated
```

Local scratch evidence: `/tmp/ub-pr4-native-clean/native3.log` (exit 0, no OOM).
Earlier attempts failed on extracted TLS-header include setup and a noexec scratch
mount, not native pipeline behavior; final runner places executables in the build
directory. The earlier full no-model PR4 baseline passed; after command-receive
changes, targeted transport tests pass normally and under UBSan, including reader
kind/instance/FD/header rejection. Reader compilation also passes warnings-as-errors.
The real native run is **not claimed UBSan-instrumented**.

**Initial slice limitation (superseded by the queue section below):** this used
a test-owned consumer, not the actual main queue/result path. The native engine probe is not LLM inference.
It does not qualify all BOS/EOS/template cases, late-cache lookup/eviction, whole
process/preprocessor/diagnostic/disk budgets, production configuration/startup or
GPU safety. Native source-dimension admission is not a measured RSS bound.

## Actual request handler/main queue/slot/result reader

`vision_queue.hpp`, conditional `strata_context.cpp` wiring and
`vision-native-queue.inc` extend the **same real native path** into the actual
completion handler, server_queue, Slot, response reader, streaming formatter and
PR1 terminal/promotion code. This is no longer a test-owned queue consumer.
Envelope bytes/count, output records, worker handoff, artifacts/recipes and per-file
disk have explicit injected limits; these are not whole-process/RSS qualification.
The actual queue is shared-owned through producer shutdown. Native inbox drain
precedes idle handling; source/metadata composition/SVE1 work never blocks the main
queue. No empty-token envelope enters legacy launch.

Recorded on that same path:

- Idle reader READY reaches the real queue with no active slot.
- Gate A's real GENI, actually defer over-context B and valid C plus a cancelled
  queued request, then release A: A/C succeed, B is pre-SSE 400 without encoding,
  and C completes without a nudge completion. Streaming success uses real result
  reader/formatting; cancelled/deferred failures add no success metrics.
- The request-owner observer cancels an outstanding real ENCODE independently of
  the queue. A queued follower is refused 503 after safe teardown, not retried;
  explicit fresh-instance recovery permits a later successful request.
- After GENI, disconnect causes exactly one STOP. Native residual T/PP and a
  withheld DONE retain the old slot/files. The follower does no prepare/encode/GEN
  until terminal drain, then succeeds. Raw DONE/ERR is never passed early to the
  slot cleanup path.
- Normal output-limit STOP is distinct from disconnect: a valid native cancel
  terminal yields one successful final result. Recovery races final shutdown/join.
- Public legacy start/load refuse mixing fork/in-process projector initialization
  with an installed native bridge. There is **no CLI/default production switch**.

```text
PR4_NATIVE_QUEUE_PASS handler=real queue=real slot_results=real pre_sse_context=1 queued_cancel=1 encode_disconnect=1 recovery_success=1 engine_disconnect_drain=1 normal_stop=1 text_gen_parity=1 text_cancel_drain=1 gpu=none
```

Reproduction remains `vision-native.sh BUILD MODEL PROJECTOR`; add
`CXXFLAGS='-fsanitize=undefined -fno-sanitize-recover=all'` for the native parent/
reader translation units. Pinned static libraries remain a Release build, not
fully UBSan-instrumented. CPU limits/isolation/assets are as above. The ordinary
initial queue run and subsequent UBSan native/queue runs passed. The no-model PR4
suite passed after the handoff changes, as did targeted UBSan supervisor/adapter
checks and the legacy context lifecycle/media fixtures. Local evidence is under
`/tmp/pr4-native-integration/` and `/tmp/ub-pr4-native-clean/native-queue-final.log`.

The two concrete CPU integration milestones now have evidence. The request object
and disconnect signal are synthetic, **not a network HTTP test**; the engine is a
native SVE1 protocol probe, not an LLM. Upstream queue post takes a mutex and may
allocate; failure is fail-stop, not a qualified production nonblocking/latency
claim. Native cache/startup parity, text availability after safely contained reader
failure, full fault/saturation coverage, HTTP/result-queue memory and production
configuration/budgets remain gates. Resident text now uses this same engine, below.

## Resident native text GEN on the same main queue

Explicit text-only submissions reuse Supervisor/Session/Job's reserved stage and
terminal notifications, increasing identities, engine authority, STOP/drain,
handback and acknowledgement. They perform no reader IPC; accidentally empty vision
submissions remain invalid. Zero-byte recipe snapshots retain a snapshot-count
charge. Parent text tokenization is unchanged, including recursive literal splitting
and raw-token prompts; owned IDs are charged to the pending envelope. The parent
worker builds GEN directly, without calling the image-file writer.

The real native queue test now includes:

- Exact engine-side token comparison for plain/literal × streaming/non-streaming
  text with protected control spellings and UTF-8, and explicit raw token IDs.
- Empty/over-context/batched refusals before successful SSE or GEN, followed by
  valid work. These do not contribute success metrics.
- A text follower deferred behind real GENI cancellation: no text GEN or new image
  file before residual output/withheld DONE drains, then successful streaming GEN.
- Text cancellation and deferred text follower on the same native STOP/drain gate;
  no follower GEN until the old terminal is certified. Twelve total successful
  requests contribute metrics; cancelled/failed requests do not.
- Zero decoder/encoder/image-file attempts during the text-only section. Image-file
  attempts use a link-wrapped `mkdtemp` observer with real vision positive controls.
  Final artifacts, recipe snapshots, engine/reader owners and deferred queue settle.

Final evidence: `/tmp/pr4-text-integration/native-text-final-ubsan.log` and
`legacy-context-final.log`. The parent/reader translation units were rebuilt with
UBSan/no recovery against the previously qualified pinned CPU Release archives
(not a new dependency rebuild or a fully sanitized dependency build). Existing
assets/build-dependency image, offline `runc`, no GPU requests/devices, 4 CPU / 8 GiB;
no downloads or production changes. Native/queue and legacy context/media fixtures
pass; targeted Job and combined supervisor/engine/adapter UBSan checks also pass.
The full no-model suite was not repeated for this slice.

This advances resident text integration, not gate 6 closure. It remains a synthetic
HTTP request and native protocol probe, not a network HTTP/LLM test. Only the model's
current BOS/EOS configuration is tested here. The supervisor still admits Jobs only
with a ready resident reader, so text availability after safely contained reader
failure remains unimplemented. Cache/startup/configuration/resource and separately
authorized GPU/release gates remain open. There is no production activation switch.

## Remaining gates

This is not complete PR4 acceptance. The real reader/composition/SVE1 path now
works through the actual CPU handler/main queue, including resident text GEN, but
contained-reader-failure text availability, cache/startup parity, network HTTP,
full engine parity, environment allowlisting and GPU
qualification remain.
Existing late-cache lookup/eviction wiring is unchanged and remains an integration
gate. Native process cleanup is not proof of GPU quiescence. Test caps are not
production resource qualification.

Default production activation remains legacy; the new queue path is CPU-test injection only. No deployment or GPU operations occurred. See `docs/PR4_IMPLEMENTATION_CONTRACT.md` for remaining integration/release gates.
