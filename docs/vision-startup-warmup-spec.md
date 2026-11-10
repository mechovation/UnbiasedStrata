# PR7 — startup vision warmup: specification draft (no implementation)

Status: **superseded by the owner's revision of 2026-11-01 — implemented behind an explicit opt-in flag,
off by default** (`--native-vision-warmup`; see `docs/vision-native-warmup.md`). §0 records both decisions.
The rest of this document is the qualification plan that still applies before the flag is used in production.

## 0. Owner decisions (2026-11-01)

1. First instruction: *"if warming up the vision stack costs expert slots then let's not warm up — the first
   image will be slower, but we benefit from better text performance until then."*
2. Revised the same day: *"still like image warm-up option, but only if directed by flag."* Implemented that
   way: **off unless `--native-vision-warmup` is passed**, and the flag additionally requires
   `--native-vision` and `--mmproj-gpu`. Nothing about the default startup of either path changes, so the
   slot question only arises for an operator who opts in deliberately.

The premise in (1) still needs the correction below, recorded so the trade is not mistaken later:

The stated premise needs one correction, recorded so the trade is not mistaken later:

- `cp.warmup = true` is **already set on both paths today** (`frontend/strata_context.cpp:626` legacy,
  `frontend/vision_reader_main.cpp:68` native, each with the comment that it reserves the encoder's GPU
  work buffers/graphs up front). That reservation is already inside the memory the engine does **not** get:
  the frontend currently holds **1340 MiB** on the card and the engine sized from **8.12 GiB free**.
- Declining the synthetic encode therefore **does not reclaim an expert slot**, because the encode is not
  what creates the ~1.3 GiB floor — `cp.warmup` already does. Whether a real encode adds anything material
  on top of that reservation is exactly what §4's H1 measurement would have answered; it was never run.
- Supporting evidence that the marginal cost is small: PR6 qualified a **fresh uncached 1024-square encode
  after a reader restart with the reader resident**, and the engine's allocation was byte-identical with the
  reader present vs absent, at a minimum sampled free VRAM of **140.5 MiB**
  (`docs/PR6_GPU_RESULTS.md:22`, `docs/PR5_GPU_TEST_RESULTS.md:90-100`). So on the native path the first
  real encode fits the existing reservation; the 7.111 s first-use event
  (`docs/VISION_STACK_BENCHMARK.md:13`) is therefore better explained by lazy compute init (graph build,
  cuBLAS/CUDA module load) than by allocating gigabytes.
- Consequence when the flag is **not** passed (the default, and production): the **~7 s first-image encoder
  event stays**, in production (legacy) and in the opt-in native path. Reported cost/benefit: text keeps the
  current expert-cache size; first image remains slow. When the flag **is** passed, the same work moves to
  startup and the slot question becomes live and must be measured (§4).
- **Not implied by the flag question:** setting `cp.warmup = false` to win slots. That is the opposite lever and
  the riskier one — it defers ~1.3 GiB of encoder reservation into a card the engine has already filled,
  which is the failure mode documented at `tools/vision/strata_vision.cpp:159-164` (late allocation made the
  GPU page and the engine stall). It requires its own measurement and is not authorized here.
- Open follow-up, **partly answered on 2026-11-01 on the RTX 3060** (`docs/vision-warmup-mechanism-3060.md`):
  one 2048-square warm encode covered every other shape tested, and it grew device memory by only ~68 MiB
  beyond the `cp.warmup` reservation, so the marginal slot cost of the flag is small. The ~7 s event did
  **not** reproduce on the 3060 (866 ms cold vs 736 ms warm), which points at **allocation pressure on a
  card the engine already fills** rather than lazy compute init. The remaining question is therefore
  5070 Ti-specific and must be measured against a loaded engine: `deployment/PR7-5070TI-RUNBOOK.md`.

Everything below §0 is the plan that would have applied had PR7 been approved.

Status: **draft for owner decisions.** No code, no build, no GPU window, no deployment was performed to
produce this. Citations are to the current worktree (`wip/pr4-foundations` @ `a3d765e`).

Requirement source: `docs/VISION_STACK_REVIEW.md:173` (§5.D.3) and `docs/VISION_IMPLEMENTATION_PLAN.md:284-290`
(§10). Acceptance wording: *"after a fresh process start, the first real image must not repeat the observed
~7-second encoder first-use cost."*

## 1. What is actually observed today

Baseline (legacy path, production, read-only observation 2026-11-01):

- Card: RTX 5070 Ti, 16303 MiB total; **15009 MiB used, 925 MiB free** at idle.
  Holders: frontend `unbiased-strata` **1340 MiB** (its `mtmd` context, `cp.warmup = true`,
  `frontend/strata_context.cpp:626`), engine `strata` **13560 MiB**.
- Startup-to-healthy: **33.48 s** (`logs/vision-ab-20261029/controlled/UnbiasedStrata-startup.json`).
  Reference Strata: **41.62 s**, which already contains its 6.7-s synthetic encode
  (`docs/VISION_STACK_BENCHMARK.md:13`, `Strata-startup.json`).
- First real image after 11 text requests: **9.714 s total / 9.202 s to first content**; warmed repeats of
  the same fixture: **2.16-2.36 s / 1.89-1.91 s**. The first projector inference took **7.111 s**;
  subsequent encodes ~148 ms. So the excess is **~7.3 s of one-time encoder first-use cost**, and
  `cp.warmup=true` demonstrably does not remove it.
- Engine expert sizing is a direct function of free VRAM at the moment the engine sizes:
  `slots = (free_b − ((vram_reserve_mib + prefill_mib) << 20) − mtp_bind − pipe_first) / blob`
  (`src/program/generate.cpp:5734-5741`). Production log: `expert cache auto: 8.12 GiB free, 1200 MiB
  reserved (+218 MiB for the draft head) -> 2714 slots`, then a few lines later
  `expert cache 3517 slots, 6.73 GiB of VRAM`. **That 2714-vs-3517 gap is unexplained and must be resolved
  before any slot-count headroom claim is used.** A reserve given on the command line is never auto-shrunk
  (`src/program/generate.cpp:5775-5790`), and production gives `--vram-reserve-mib 1200`.
- Legacy ordering is a race, not a contract: the engine is forked at `frontend/main.cpp:203`
  (`Engine::start` → `::fork()`, `frontend/engine.hpp:78-84`) *before* `init_vision()` at
  `frontend/strata_context.cpp:1526`; the log shows vision initialised at 13:11:41.879 and the engine
  sizing at 13:12:04.436, i.e. the reservation was counted **by timing accident** (model bind is slower
  than vision init). The native path makes it an ordering contract
  (`frontend/vision_supervisor.hpp:34, 386, 403-412`: reader spawn → reader READY → engine initialize).

Prior GPU headroom evidence (do not re-derive, do not extend — `docs/PR6_HANDOFF.md:404`):

- Reader held **1408 MiB**; engine arena with reader resident vs absent at the same full KV: identical for
  `strata` at 262135 tokens, **+1288 MiB growth for unfrozen `strata-poly`** — that growth is a violation of
  the static restart-headroom rule, and unfrozen poly is **not** qualified (`docs/PR5_GPU_TEST_RESULTS.md:90-100`).
- Qualified (Flash + explicitly frozen `strata-poly --kv-whole`): engine allocations identical with the
  reader resident vs absent; **minimum sampled free VRAM 140.5 MiB (Flash) / 1032.5 MiB (poly)**
  (`docs/PR6_GPU_RESULTS.md:22`). A cached post-restart GENI can shrink KV and conceal inadequate encoder
  headroom (`docs/PR5_GPU_TEST_RESULTS.md:99`).
- Nominal `--expert-cache 3200` was capped by free VRAM to **3642 vs 3631** actual slots in an earlier pass
  (`docs/VISION_STACK_BENCHMARK.md:120`): expert budget is a ceiling, free VRAM is the binding term.

Consequence: **PR7 trades ~7 s of first-request latency for roughly the same cost moved into startup, plus a
permanently larger reader VRAM floor that is subtracted from expert slots at engine sizing.** It is not free
and must not be presented as making work disappear.

## 2. Decision D1 — which execution path is qualified (owner decision)

Production runs the **legacy** path: `docker-compose.yaml` has no `--native-vision`; `docs/vision-native-status.md`
states legacy remains the default.

| Option | Mechanism | Qualification cost | Verdict |
|---|---|---|---|
| **A. Native path only** (recommended) | Insert the encode in the reader between `mtmd_init_from_file` (`frontend/vision_reader_main.cpp:73`) and `vision_reader::serve()` (`:75`), i.e. strictly before reader READY. Buffers persist in the same process that will serve requests. | Bounded: one new GPU window; ordering contract already exists; nothing about production changes. | Recommended. |
| B. Legacy in-parent warmup | Same encode inside `init_vision()` (`frontend/strata_context.cpp:626-641`). | The frontend becomes a larger GPU holder, and the sizing order relative to the forked engine stays a race. Conflicts with the "avoid/verify parent GPU allocation" goal. | Not recommended for PR7. |
| C. Separate throwaway warmup process | Warm, exit, then start reader/engine. | Pays CUDA init + projector load twice and leaves **no** resident buffers behind — does not satisfy the acceptance criterion. | Rejected. |

If the owner wants the legacy path to benefit too, that is a **separate** decision with its own qualification;
PR7 should say plainly: *native path qualified, legacy path unchanged, operator gets the ~7 s first-image
event in legacy either way.*

## 3. Proposed mechanism (native path)

1. Location: in `run()` of `frontend/vision_reader_main.cpp`, after `mtmd_init_from_file` succeeds and
   before `vision_reader::serve()`. No supervisor, wire, engine-adapter or parent change is required for
   ordering; readiness already gates the engine (`vision_supervisor.hpp:403-412`).
2. Work: mirror the working CPU reference `tools/vision/strata_vision.cpp:165-188` — uniform 128 RGB
   2048×2048 bitmap, `mtmd_tokenize` with `mtmd_default_marker()`, then `mtmd_encode_chunk` on the image
   chunk; free bitmap/chunks afterwards and keep the `mtmd_context` alive. GPU-only: the reference skips
   warmup on CPU because there is no VRAM to reserve and it only delays the engine (~6 s) — keep that.
3. Fit inside existing admission caps (no new limits needed): 2048² = 4 194 304 px ≤ profile `pixels`
   16 777 216; 12.58 MiB RGB ≤ `retained_rgb_bytes` 67 108 864; `source_pixels × 32` workspace allowance
   unchanged (`frontend/vision_runtime.hpp:263-264`, `frontend/vision_image.hpp:11`).
   mtmd scales the square to the configured budget (`--image-max-tokens 1024`, profile
   `image_min_tokens` 8, `batch_max_tokens` 1024).
4. Reporting: the reader must emit one bounded, credential-free line (tokens actually encoded, elapsed ms,
   and the sampled free VRAM before/after) to stderr, which the parent already bounds. It must not use
   mtmd/llama/ggml log callbacks (silenced at `vision_reader_main.cpp:40-42`).
5. Config surface: exactly one profile-controlled switch plus its shape, expressed as part of
   `reader_policy` — see D3/§8 for the cost of that choice.

Non-goals for PR7: no engine implementation change, no reserve change, no elasticity/idle release
(`docs/VISION_STACK_REVIEW.md:174` item 4 stays future work), no wire/ABI protocol change.

## 4. VRAM and restart-headroom proof (the hard part)

PR7 must produce numbers, not a claim. Required measurement set:

- **H1 reader floor**: reader-held VRAM at READY with `cp.warmup` only, and after the synthetic encode;
  delta = the new permanent floor. (Expect the delta to be significant: Strata's own comment on the same
  operation says "allocating ~1 GB later, on a GPU the engine has filled, made Windows page GPU memory and
  the engine crawl to a standstill", `tools/vision/strata_vision.cpp:159-164`.)
- **H2 slot cost**: engine log `expert cache auto: … -> N slots` for warmed vs unwarmed startup, same
  model/projector/profile. Report the slot delta. Resolve the 2714/3517 logging gap in §1 as part of this.
- **H3 restart at full allocation**: with the engine loaded and KV at the qualified full allocation, kill the
  reader, let the supervisor restart it (reader must re-run its warmup), then prove a **fresh uncached**
  encode succeeds before any cached GEN. Sampled free VRAM during that restart must stay above a stated
  floor. Reference points: PR5's reader floor 1408 MiB and its +1288 MiB unfrozen-poly violation;
  PR6's 140.5 MiB minimum sampled free VRAM on Flash. **The current `--vram-reserve-mib 1200` is not proven
  to cover the post-PR7 reader floor + growth; it may need to be raised, which per
  `docs/VISION_IMPLEMENTATION_PLAN.md:290` requires its own reviewed direct-Compose edit.**
- **H4 no-regress**: no XID, no cgroup memory max/OOM/OOM-kill, no engine overlap during warmup, 3060 never
  visible/used, parent holds no GPU context in the native path.

Enough proof, proposed: H1+H2+H3 measured on the qualified Flash policy and the explicitly frozen poly
policy only, with the restart-at-full-allocation fresh encode succeeding **and** the minimum sampled free
VRAM during restart ≥ (H1 delta + 256 MiB). Anything that needs a reserve change must be reported as a
required Compose edit, not absorbed silently.

## 5. Startup cost and deadlines (what the operator waits for)

- Current shipped profile: `ready_ms` 180000, `start_ms` 600000, `engine_ready_ms` 600000,
  `quit_ms` 2000, `backoff_ms` 1000, `restart_window_ms` 300000, `attempts` 2
  (`frontend/tests/data/vision-runtime-gpu-5070ti.json`). Container healthcheck:
  `--start-period=600s` (`deployment/Dockerfile:31`).
- The encode lands **before reader READY**, so it consumes the 180 s reader budget, not a new one. A ~7-10 s
  encode is ~5% of `ready_ms`. **No deadline change is proposed.** (Note again that the plan's "300 s"
  line, `docs/VISION_IMPLEMENTATION_PLAN.md:218`, does not match what ships.)
- Expected operator-visible change on the native path: startup-to-healthy from ~33.5 s to ~41-44 s
  (Strata's own observed figure for the same policy), first-image latency down by ~7 s. Report **three
  separate numbers** — reader READY, engine healthy, first-image first-content — never one merged figure
  (`docs/VISION_IMPLEMENTATION_PLAN.md:289`).
- Reader restarts also pay the warmup again (bounded by the same `ready_ms`); say so in the operator note,
  because it lengthens the restart window's user-visible stall.

## 6. Shape coverage (one warm shape is not a universal proof)

`docs/VISION_IMPLEMENTATION_PLAN.md:288` forbids equating a single warm shape with a worst-case proof. The
7.111 s event may be dominated by one-time costs that are shape-independent (CUDA module/cuBLAS init, graph
build) or partly per-shape (grid/work-buffer sizes). PR7 must attribute it, not assume:

- Cold first use after a warmed start, one trial each, for: 1024-square, 1920×1080 screenshot, portrait
  photo, tiny dashboard, EXIF-rotated photo (the benchmark corpus in `docs/VISION_STACK_BENCHMARK.md`).
- If any shape still exceeds the §7 gate, decide explicitly between (a) accepting it, (b) a bounded shape
  set (more startup seconds, e.g. square + 16:9), or (c) a larger synthetic shape. Record the decision and
  its measured cost; do not silently add shapes.

## 7. Proposed acceptance gates

- **G1 startup**: reader READY ≤ 30 s; container healthy ≤ 50 s; healthcheck `start-period` untouched.
- **G2 first image**: after a fresh start, no image-shape encode > 500 ms, and first-content for each §6
  shape ≤ warmed median + 400 ms (warmed median here = 1.89-1.91 s). **The 7.111 s event must not recur.**
- **G3 headroom**: §4 H1-H4 recorded; restart-at-full-allocation fresh encode passes; no reserve/Compose
  change unless separately reviewed.
- **G4 semantics**: omission classification, cache keys, prompt composition, SVE1/GENI ordering, literal
  protection and engine ABI behaviour unchanged; `make test` CPU suite green in a fresh build dir
  (pattern in `deployment/VISION_HANDOFF.md`), plus existing reader/queue/cache regressions.
- **G5 reporting**: readiness, first-image and warmed latencies reported separately; missing counters stated.

## 8. Upgrade side effects to disclose

- `config.generation = hash(resolved config)` and `reader_policy` (which already contains
  `{"warmup", gpu}`) is inside that hashed document (`frontend/vision_runtime.hpp:375, 385`). **Adding any
  warmup policy field changes the cache generation → every previously cached recipe/row namespace is cold
  after the upgrade.** That is acceptable but must be stated, and it is a second reason not to hand more
  knobs than needed.
- The schema is **exact-key**: `fields()` requires `j.size() == names.size()` and every named key
  (`frontend/vision_runtime.hpp:43-46`). A new profile key must be added to the operator-profile list
  (`:215-226`) **and** to the parent→reader resolved-document list (`:190`); otherwise a new profile with an
  old binary fails profile parsing (startup error before any GPU work) and an old profile with a new binary
  fails the count check. That is fail-loud, which is the desired behaviour — no silent default — but it means
  **profile and binaries must be versioned together**.
- `pinned_abi` (`frontend/vision_runtime.hpp:23`) — whether warmup warrants an ABI-string bump is decision
  D7 below; the wire protocol does not change, but parent+reader still ship from one build.

## 9. Bounded test plan (does not re-run the PR4/PR5/PR6 matrix)

1. **CPU/no-model first** (no GPU, no production impact, fresh build dir): `make test`,
   `frontend/tests/native-reader-foundations.sh`, `frontend/tests/vision-late-cache.sh`. Add deterministic
   checks that warmup runs **before** READY, that a warmup failure propagates as startup failure under the
   profile's attempt/backoff policy, and that profile validation accepts/rejects the new field exactly.
2. **CPU real-projector control** (no CUDA): existing contained CPU reader path — warmup must be **skipped**
   on the CPU and say so, matching the reference behaviour.
3. **One new GPU window**, purpose-built and phase-instrumented via the existing harnesses
   (`frontend/tests/native-reader-gpu-build.sh`, `native-reader-gpu-phase.cpp`,
   `native-reader-gpu-acceptance.py`, `vision-gpu-phases.inc`) with the 5070-Ti profile JSON: §4 H1-H4 and
   §6 shape battery, plus a negative control (warmup disabled) reproducing the first-use cost. Sampling at
   ≤0.5 s, as prior evidence used. Owner authorizes maintenance (`--authorize-maintenance`) explicitly;
   production is then restored to its current image/settings, still legacy.
4. Activation (`--native-vision` in `docker-compose.yaml`) stays a **separate** owner decision: single-file
   edit by rule, but parent + reader must be rebuilt/restarted together because the ABI binds loaded library
   hashes to the generation (`docs/vision-native-status.md`).

## 10. Decisions needed before any code

- **D1** path qualification: native only (recommended) — and confirm the operator note says legacy keeps its
  ~7 s first-image event.
- **D2** shape policy: single 2048-square (reference parity) vs bounded shape set; and whether the shape is
  profile-configurable at all.
- **D3** failure semantics: fail closed (reader exits → supervisor backoff/`attempts` → startup fails with a
  classified reason) vs degrade to unwarmed with a loud log. Recommendation: **fail closed**, because a
  silently unwarmed reader re-introduces exactly the event PR7 exists to remove, and the sizing contract
  (§1) depends on the floor being real.
- **D4** accepted startup cost ceiling (G1 numbers) and confirmation that no shipped deadline changes.
- **D5** whether a `--vram-reserve-mib` increase is pre-authorized if H3 demands it, or whether failing H3
  means PR7 does not ship.
- **D6** whether PR7 ships as code-only (native opt-in, production still legacy) or is bundled with
  activation. Recommendation: **code-only**, activation separate.
- **D7** ABI-string disposition (D8 note: no wire change) — bump `pinned_abi` or rely on generation alone.
- **D8** CPU behaviour: skip warmup on CPU (recommended, reference parity) and never fall back silently.

## 11. Explicitly unchanged by this document

No code changed. `docker-compose.yaml`, `docker-compose.yaml.bak`, image, profile JSON, engine reserve,
`cp.warmup` semantics, and the running `unbiased-strata-5070ti` container are untouched. The passed GPU
matrix was not rerun or extended (`docs/PR6_HANDOFF.md:404`).
