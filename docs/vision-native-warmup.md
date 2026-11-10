# Opt-in encoder warm-up in the native reader

Status: **implemented, off by default, not GPU-qualified.** Production is unaffected: it runs the legacy
path and `--native-vision` is not set in `docker-compose.yaml`.

## What the flag does

```
--native-vision-warmup          # only valid with --native-vision and --mmproj-gpu
```

When set, the reader (`frontend/vision_reader_main.cpp`) tokenizes and encodes one synthetic **2048x2048**
uniform-grey picture after the projector context exists and **before it publishes READY**. Because the
native path establishes reader READY → engine initialize
(`frontend/vision_supervisor.hpp:34, 386, 403-412`), whatever the encoder reserves is resident before the
engine sizes its expert cache.

Without the flag nothing happens; the reader logs `encoder warm-up off` and the first real picture pays the
encoder's lazy first-use cost. That cost is real and measured: **7.111 s** for the first projector inference
versus ~148 ms warmed (`docs/VISION_STACK_BENCHMARK.md:13`), and `cp.warmup=true` — the ggml graph/work-buffer
reservation that both paths already enable — does not remove it.

This mirrors the working CPU reference in `tools/vision/strata_vision.cpp:165-188` (same shape, scaled to the
configured image-token budget by mtmd; on the CPU the reference skips warm-up, and so does the reader).

## What it costs

- **Startup seconds**: roughly the cost of one large encode, inside `ready_ms` (180000 in the shipped
  profile). Reader restarts pay it again.
- **Possible expert-cache slots**: the engine sizes from free VRAM
  (`slots = (free − reserve − …) / blob`, `src/program/generate.cpp:5734-5741`), so any *additional* memory
  this encode holds is memory the engine cannot use. How much extra a real encode needs on top of the
  existing `cp.warmup` reservation is **not yet measured**; the 1.3-1.4 GiB encoder floor already exists
  today whether or not this flag is used.
- **Cold image cache**: the setting is part of the reader's immutable config document, so it is folded into
  `config.generation` (`frontend/vision_runtime.hpp`). Also, adding the `warmup` key changed that document
  for everyone, so rows cached by an older build are cold after this change.

## Fail-closed behaviour

The flag is not silently ignored:

- the parent rejects `--native-vision-warmup` without `--native-vision`/`--mmproj-gpu` (`frontend/main.cpp`);
- `Runtime::validate_operations()` rejects `warmup && !gpu`;
- the reader re-reads the flag from the same verified JSON document it byte-compares
  (`frontend/vision_runtime.hpp:190, 197-199, 207`), so parent and reader cannot disagree;
- if the synthetic encode fails, the reader exits and startup fails, rather than starting unwarmed.

## Qualification state

Done (CPU, in the `unbiasedstrata-dev` container, no model, no GPU):
`bash frontend/tests/vision-runtime.sh /build/ub-frontend` → schema/hash/environment/rlimit tests pass,
including the new `warmup` field, the `warmup && !gpu` rejection and the reader-config rejection.

Not done: any GPU measurement. Mechanism-level GPU smoke (does the synthetic encode run, how long,
how much VRAM) belongs on the **RTX 3060** in `unbiasedstrata-dev` (`deployment/dev-container.sh
gpu-build`, `CUDA_ARCHS=86`); it is never qualification evidence. Production-grade gates — the reader
VRAM-floor delta, expert-slot delta and restart-at-full-allocation in `docs/vision-startup-warmup-spec.md`
§4/§6 — need a 5070 Ti window run **out of session** by GPT or a human:
see `deployment/PR7-5070TI-RUNBOOK.md`. The passed PR4/PR5/PR6 GPU matrix is not rerun
(`docs/PR6_HANDOFF.md:404`).
