# PR4 handoff — resume here

## Request and authorization

The user approved continuing PR4 in this sequence: resolve the contract, build CPU-only clean-launch/fake-reader foundations, then move toward native reader/supervisor and queue integration. They repeatedly requested continuation **within PR4**, and requested this handoff before a fresh session.

**PR4 is not complete or deployed.** Production serving paths and GPUs were not changed during this work. PR5–7 are excluded. The user subsequently authorized audited checkpoint commits and a push to their public fork, excluding local deployment files and credentials. Do not interpret the architecture draft's original blanket stop as prohibiting the subsequently approved foundation work; equally, do not interpret approval as permission to pause production, run GPU qualification or deploy.

Suggested next-session prompt:

> Continue PR4 from `docs/PR4_HANDOFF.md`. Preserve all existing uncommitted work. Start with the CPU-only supervisor/recovery and inbox/control contracts; do not touch production or GPUs.

## Hard restrictions

Read root `AGENTS.md` before acting:

- Exactly one authoritative `docker-compose.yaml` and its single `docker-compose.yaml.bak`. No overrides, alternative Compose files or stacked settings.
- Only RTX 5070 Ti `GPU-d14daf49-45c8-58c5-cf61-5a40342672af` may eventually be used. Never use/disrupt the 3060.
- Large base-image pulls must happen independently in the background, not as a build side effect. Use the existing `hf-download` container for any approved background Hugging Face downloads.
- Never put API keys in source, images, logs or command-line arguments.
- **Current phase remains GPU-free, with no automatic production deployment.** Coordinate/authorize any eventual 5070 Ti qualification separately.

## Read first

1. `docs/PR4_IMPLEMENTATION_CONTRACT.md` — selected decisions, implemented foundations and unresolved gates.
2. `docs/PR4_WIRE_SCHEMA.md` — exact v1 payloads, accepted metadata layout and trust boundaries.
3. `frontend/tests/PR4.md` — evidence and explicit limitations.
4. `docs/PR4_ARCHITECTURE_REVIEW.md` — overall lifecycle/GPU/ownership requirements; some original questions are now resolved by the contract/schema above.
5. PR4 section of `docs/VISION_IMPLEMENTATION_PLAN.md`.
6. `frontend/tests/PR1.md`, `PR2.md`, `PR3.md`, `SPIKE_A.md` for prerequisite invariants.

## GitHub checkpoint and local-work preservation

Checkpoint branch: `wip/pr4-foundations`; target public fork: `https://github.com/mechovation/UnbiasedStrata`. Use `git log` and remote refs for the latest checkpoint IDs; this handoff accompanies the PR4 foundations commit, not a completed PR4 release.

- `80f7a70` — `vision: checkpoint PR1-3 prerequisites and Spike A`. Includes the prior queue/admission/acquisition work, tests/evidence, root instructions and pre-existing opt-in frontend CUDA build support. No deployment settings are included.
- Following commit — PR4 CPU foundations, tests, real metadata fixtures, contract/schema and this handoff. The PR4 Makefile additions are separated from the earlier acquisition/test changes.

The repository was already substantially dirty before PR4. The audited source/test/documentation changes are now checkpointed, rather than discarded or incorrectly attributed entirely to PR4. **Local deployment work remains uncommitted:** `.dockerignore`, `deployment/`, `docker-compose.yaml`, `docker-compose.yaml.bak`. Secrets remain ignored, and model data/logs are not included. Do not reset, clean, overwrite or stage these local files wholesale. Local deployment references in historical evidence documents may therefore be unavailable in a fresh clone; this is intentional.

Inspect `git status --short`, not just `git diff --stat`, when resuming. The original upstream `origin` is not the intended push target; use the fork remote/URL and WIP branch explicitly.

Dependency remains at `third_party/llama.cpp` commit:

```text
44072a19fb94d0bed63d381d42f6237373f91d54
```

Its tracked worktree was unchanged at the last check. No submodule bump or patch was made.

## Implemented PR4 files

### Process foundations

- `frontend/clean_process.hpp`: Linux/glibc `posix_spawn`, explicit argv/environment, CLOEXEC pipes, standard descriptor mapping, optional control socket at FD 3, closefrom isolation and signal reset. Caller owns returned process/FDs. `send`/engine production paths are not switched to it.
- `frontend/process_guard.cpp`: native exec trampoline installs SIGKILL PDEATHSIG and checks expected parent after installation. Setup/parent failure exits 125; target exec failure exits 126.
- `frontend/process_lifecycle.hpp`: single-owner, bounded TERM/KILL/reap helpers. Timeout/error retains ownership; reaping is not proof of GPU-driver quiescence.

Important: Linux PDEATHSIG follows the **spawning thread**. That owner thread must remain alive throughout child lifetime. Targets must not change credentials, clear PDEATHSIG, escape the PID namespace or spawn untracked descendants. PID-1 operation is allowed by the code but namespace deployment qualification remains outstanding. Borrowed launch FDs must stay stable throughout launch.

### Protocol and validation

- `frontend/vision_protocol.hpp`: fixed 40-byte SVR1 header, explicit little endian, kind/length/identity checks.
- `frontend/vision_transport.hpp`: single-owner AF_UNIX stream transport, absolute deadlines, bounded cancellation polling, SCM_RIGHTS only with the first byte, CLOEXEC on receipt, cleanup on rejection and poisoned/closed channels after failed operations.
  - `send(Frame)` keeps local ownership in Frame.
  - `send_borrowed(...)` does not copy/adopt/locally duplicate artifact FDs. Borrowed buffers/FDs must remain stable until return.
- `frontend/vision_payload.hpp`: no-allocation exact-consumption validation for all nine command kinds. Configuration/device/vocabulary/source/occurrence/handle/shape/FD-count checks. PREPARE and ENCODE operate on **one occurrence per operation**; duplicates remain distinct.
- `frontend/vision_metadata.hpp`: no-allocation, pin-specific mtmd v2 Qwen3-VL still-image preflight, **before** public loading. No mtmd calls or manufactured serialization.

Metadata accepts only the qualified still form: image chunk, no text payload/audio, MROPE, image index 0, temporal merge 2, one batch entry, canonical flags, zero lead padding, parent-assigned bounded ID and exact end consumption. Pixel dimensions must match grid times configured stride. Qualification used spatial merge 2, patch 16, stride 32. Native metadata is host endian; the outer protocol is little endian. Returned payload views borrow the immutable frame and cannot be retained across asynchronous handoff without ownership/copying.

Generation digest construction and configuration allowlisting are not implemented. Validators compare caller-supplied parent expectations; they do not establish their provenance.

### Artifacts and per-job ownership

- `frontend/vision_artifact.hpp`: parent-created memfds with explicit aggregate logical-byte/artifact-FD caps; shared ownership retains charges until the last reference dies.
  - Sources are size-bounded (never above PR3's 10 MiB cap) and fully sealed before handoff.
  - Row storage is exact-sized and GROW/SHRINK sealed **before** reader handoff.
  - Rows start with NaN poison. On completion, WRITE/SEAL must succeed before finite-float scanning with a fixed-size buffer; writable mappings anywhere prevent sealing.
  - Identity/shape checks bind rows to parent-owned tags. The caller computes source hashes; this helper does not hash image contents.
  - Poison detects untouched cells, not honest encoder provenance: overwriting/hole-punching to finite zeros before sealing is indistinguishable from valid-looking output. Final seals prevent subsequent mutation.
  - Deadlines/cancellation are checked between bounded IO blocks, not guaranteed to interrupt kernel stalls.
- `frontend/vision_job.hpp`: **single-owner per-job state core, not a running supervisor**.
  - Owns copied recipe strings, source/row references, handle records and one pending instance/job/sequence operation.
  - Sequential PREPARE → admission → ENCODE; refuses duplicate handles and same-size wrong-shape/cross-job outputs.
  - Aggregate recipe/image-cell/position caps include wrapper text positions; full recipe cell accounting also fits signed int32.
  - Exposes only validated immutable rows. Retains resources after failure until explicit safe teardown certification.
  - Gates engine start and STOP/terminal-drain cleanup; engine terminal success/failure is explicit.
  - Sticky request cancellation does **not** interrupt cooperative RELEASE after engine drain/context refusal. Future transport cleanup must use its own bounded deadline and separate service-shutdown abort control, not reset the request flag.
  - Once-only terminal result within this core; no main-queue promotion/accounting proof yet.

**Lifetime precondition:** the owner must retain an active Job through safe completion. Destroying it is not a cancellation mechanism. `reader_stopped(true)` is trusted parent certification of reaping AND applicable GPU safety, not information supplied by the child. Whole-service failure may require engine termination proof as well.

The artifact cap is not a process memory bound. It excludes acquisition buffers, recipe heap capacity/overhead, decoder/preprocessor memory, reader-side descriptors, row-cache copies, engine SVE1 disk bytes, allocator/kernel overhead, etc. Numeric test values are not approved production defaults. Existing late cache lookup/eviction integration is unchanged; tests prove shared artifact lifetime accounting, not cache parity or an early-cache feature.

## Tests and last results

Run from repository root:

```sh
make test-pr4 NVCC=
SANITIZE=1 bash frontend/tests/pr4-foundations.sh
git diff --check
git -C third_party/llama.cpp diff --quiet HEAD --
```

All passed at the end of the implementation session. The default runner needs Linux/glibc and a C++17 compiler, not CMake, models, CUDA, Docker or downloads. It compiles with warnings as errors; SANITIZE adds UBSan with no recovery.

Expected suite output:

```text
clean-process/header foundations: PASS (25 threaded spawn cycles)
process lifecycle: PASS
vision transport: PASS
vision payload/metadata: PASS real_fixtures=10 rejected=3734
vision artifact: PASS
vision job ownership: PASS
```

Sources: `frontend/tests/{clean-process,process-lifecycle,vision-transport,vision-payload,vision-artifact,vision-job}.cpp`; runner `frontend/tests/pr4-foundations.sh`. Original standalone `clean-process.sh` remains.

Coverage includes actual parent death before guard installation and during blocked target work, transient spawning-thread death, startup rollback, blocked/fragmented IO, stale frames, FD cleanup, metadata corruption, shared-budget reservation races, RLIMIT_NOFILE-injected memfd failure, post-sealing cancellation, engine error outcomes, sticky cancellation through RELEASE, and held rows after job cleanup.

The native fake-reader artifact test intentionally uses synthetic empty control payloads to isolate FD/mapping/seal behavior. It is not a complete reader/schema/job end-to-end test. During checkpoint preparation, the existing engine adapter's seven fake-engine cases also passed again. The full HTTP/real-projector/Spike A acceptance suite was **not** rerun for this checkpoint; do not claim full PR4 acceptance.

### Real metadata qualification already performed

- `frontend/tests/vision-metadata-real.cpp` and `.sh`: fresh pinned CPU library build, vocabulary-only language model plus existing projector, actual preprocessing/tokenization at budgets 64/1024, public metadata save, preflight, public load, ID/count/position comparison and PREPARED validation with real wrapper tokens.
- `frontend/tests/data/pr4-metadata/`: ten genuine 84-byte saved fixtures, verified `SHA256SUMS`, detailed provenance in README. No fabricated accepted mtmd bytes.
- Result: `PR4_METADATA_REAL_PASS fixtures=10 gpu=none wrapped_encode_entries=not_reached`.
- Ran in an existing local build-dependencies image using `runc`, no GPU requests, disabled GPU masks, network disabled, source/assets read-only, 4 CPU / 8 GiB. No downloads/deployment.
- Existing assets and exact provenance are documented in the fixture README. Optional local scratch evidence: `/tmp/ub-pr4-metadata/run3.log`, `result3` (=0), `build/`, `fixtures/`. Earlier logs include a fixed test bug; use final `run3.log`. Temporary paths may disappear and are not required by the default tests.
- The expanded test initially used `llama_model_n_embd_inp()` on a vocabulary-only model, which has no initialized hparams. Fixed to mirror production's GGUF model/projector metadata width check. Spatial merge override is explicitly checked. Wrapped encoder entry points were not reached; this is not comprehensive internal encoder instrumentation or GPU qualification.

## Review findings already addressed

- Validate borrowed control FD before allocating stderr duplicates; otherwise a closed FD could be silently reused.
- Bound Frame cleanup when malformed local outgoing fd_count exceeds its array capacity.
- Actual pre-install parent-death and spawning-thread-death tests added, not just wrong-PID tests.
- Successful fake-child startup uses a longer deadline than injected timeout cases.
- Explicit engine failure outcome prevents false success after RELEASE.
- Sticky cancellation through final cleanup is tested with the atomic request flag still true.
- Artifact poisoning claims narrowed; hole-punch limitation explicitly tested/documented.
- Cancellation during row validation returns the cancellation reply consistently.

## Recommended next work, still PR4

1. Read/audit the foundation APIs and rerun `test-pr4`; preserve prior work.
2. Design/implement a **CPU fake-worker supervisor loop** around the existing process, transport and Job primitives:
   - one persistent spawning/IO owner, one operation, absolute ready/prepare/encode/cleanup deadlines;
   - explicit request-control lifetime, sticky disconnect/deadline propagation, separate cleanup/service shutdown control;
   - bounded event inbox and queue wakeup ownership; no slot mutation on supervisor threads;
   - startup rollback, fail-closed whole-service death, bounded restart/backoff, no automatic retry of failed requests;
   - explicit GPU-turn authorization interface without pretending CPU tests establish GPU quiescence;
   - configuration/READY checks and source/artifact/operation builders; test full wire schema + artifact + job composition with a native fake worker.
3. Before production engine launch changes, audit its environment requirements and preserve adapter start/READY/EOF/STOP/drain/quit parity. `frontend/engine.hpp` still uses the original fork launch. Do not switch it casually; guard errors and spawning-thread ownership change assumptions.
4. Finish hard-resource/deadline/restart qualification ledger; no silent adoption of fixture caps as production budgets. Decoder/pixel admission must happen before decoder/preprocessor allocations.
5. Implement native reader and safe public mtmd load/composition adapter, then main-queue integration. Preserve PR1 exactly-once terminal/promotion, PR2 dispatch, PR3 acquisition and Spike A literal/BOS/EOS/image-order parity, plus existing late row-cache held-entry semantics.
6. Only after CPU/integration gates: separately coordinated 5070 Ti-only exclusion/cancellation/restart/headroom qualification and release review. Never auto-deploy or involve the 3060.

### Queue integration trap already identified

`frontend/strata_context.cpp` currently treats NEXT_RESPONSE as a no-op; `update_slots()` returns immediately when no slot is active. A new inbox must be drained **before** that early return, or idle recovery events can be lost. Inbox append/wake-pending flag and consumer drain/clear need one synchronized contract. Reserve event capacity before operations. Only the main queue may mutate slots/results/promotion. Use an owned pending-envelope sidecar keyed by task ID; never pass an empty-token vision placeholder through today's `launch()` checks.
