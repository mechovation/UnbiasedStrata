# PR4 foundation evidence (partial)

Run:

```sh
bash frontend/tests/pr4-foundations.sh
SANITIZE=1 bash frontend/tests/pr4-foundations.sh
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

Result: `vision payload/metadata: PASS real_fixtures=10 rejected=3734`, both normally and under UBSan. Fixtures and exact generation provenance are in `data/pr4-metadata/README.md`.

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

## Remaining gates

This is not complete PR4 acceptance. Transport, payload/preflight validation, artifact helpers and the per-job state core await serving integration. No production loader/composition adapter, live supervisor/recovery loop, queue integration, environment allowlist qualification, production engine parity or GPU qualification is claimed. Existing late-cache lookup/eviction wiring is unchanged; only shared artifact lifetime accounting has been tested. Native process cleanup is not proof of GPU quiescence. A sealed test memfd is not artifact resource-budget qualification.

Production code paths and deployment remain unchanged. See `docs/PR4_IMPLEMENTATION_CONTRACT.md` for remaining integration/release gates.
