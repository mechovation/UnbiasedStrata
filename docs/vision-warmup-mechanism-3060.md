# Encoder warm-up: mechanism measurement on the development GPU (RTX 3060)

2026-11-01. Development evidence only — `AGENTS.md` reserves the 5070 Ti for production and final
qualification. Nothing here is a production or qualification claim.

## How it was measured

Container `unbiasedstrata-dev` (RTX 3060 attached by UUID, sm_86, 12288 MiB, production card
untouched), CUDA 13.0.2 build tree `/build/cuda86` (`make configure CUDA_ARCHS=86 …`), target
`vision-warmup-probe` (`frontend/tests/vision-warmup-probe.cpp`, wired in
`frontend/tests/local-test-targets.cmake`). The probe mirrors the native reader's setup:
vocab-only LLM load, then `mtmd_init_from_file` with `image_max_tokens=1024`,
`image_min_tokens=8`, `batch_max_tokens=1024`, `n_threads=4`, flash-attn auto, then uniform-grey
synthetic encodes per shape, with NVML device memory sampled around each step.

Real production artifacts were used read-only: projector
`mmproj-Qwen3.8-Flash-Next-BF16.gguf` (the same 2560-wide projector production serves) and the
Qwen3.8 Flash IQ3_S GGUF for the vocab-only load.

Raw output: `logs/pr7-3060-probe-warmup.log`, `logs/pr7-3060-probe-nowarmup.log`.

## Results (MiB of device memory as reported by NVML, ms per step)

| Step | `cp.warmup = true` (what the reader always does today) | `cp.warmup = false` |
| --- | --- | --- |
| after vocab-only model load | 438 | 438 |
| after `mtmd_init` (277 ms / 235 ms) | **1554** (+1116) | **1304** (+866) |
| first 2048² encode → 1024 tokens (866 ms / 857 ms) | 1622 (**+68**) | 1494 (+190) |
| 1920×1080 → 1008 tokens | 699 ms, no growth | 700 ms, no growth |
| 800×1200 → 950 tokens | 620 ms, no growth | 621 ms, no growth |
| 64×64 → 9 tokens | 7 ms | — |
| 2048² again (warmed control) | **736 ms** | **738 ms** |

## What that says

1. **The synthetic encode itself is cheap in VRAM.** With `cp.warmup=true` it grew device memory by
   only ~68 MiB. The ~1.1 GiB step belongs to `cp.warmup`, which **both paths already enable**
   (`frontend/vision_reader_main.cpp:68`, `frontend/strata_context.cpp:626`). So the marginal
   expert-slot cost of `--native-vision-warmup` is small; the big reservation exists either way.
2. **`cp.warmup=true` over-reserves for this shape.** With warm-up the steady state after a
   1024-token encode was 1622 MiB; without it, 1494 MiB. Roughly 128 MiB of the always-on
   reservation is not needed by a 1024-token encode on this card — a separate, larger lever than
   PR7, and the risky one (`docs/vision-startup-warmup-spec.md` §0).
3. **One warm shape covered every other shape tested** (1920×1080, 800×1200, 64×64 all ran at
   warmed speed after the first encode). No per-shape penalty observed at this pin/card.
4. **The 7.1 s event did NOT reproduce on the 3060.** Cold first encode was 866 ms versus 736 ms
   warmed — a ~130 ms one-time effect, not seconds. So on the 3060 there is no large lazy-init cost
   to remove. The production-relevant suspicion is therefore **allocation pressure**, not lazy
   init: on the 5070 Ti the encoder allocates while the engine holds ~13.5 GiB of 16.3 GiB, with
   under 1 GiB free (`docs/PR6_GPU_RESULTS.md:22` records 140.5 MiB minimum sampled free). A 3060
   with 12 GiB and an idle card cannot reproduce that, and no 3060 number may be used as the
   substitute.

## Consequence for PR7

`--native-vision-warmup` (opt-in, off by default) is mechanistically sound and cheap: it performs
exactly the sequence the probe ran. Whether it removes the 5070 Ti first-image cost is still
unmeasured, and that measurement needs the pressure condition — i.e. the native A/B against a
loaded engine on the 5070 Ti, run out of session: `deployment/PR7-5070TI-RUNBOOK.md`.

## Reproduce

```sh
deployment/dev-container.sh create            # attaches the 3060 by UUID
deployment/dev-container.sh gpu-build         # CUDA_ARCHS=86 tree in /build/cuda86
deployment/dev-container.sh run bash -lc '/build/cuda86/bin/vision-warmup-probe \
  --model /snapshots/<snapshot>/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf \
  --projector /models/mmproj-Qwen3.8-Flash-Next-BF16.gguf \
  --shapes 2048x2048,1920x1080,800x1200,64x64,2048x2048'
# add --no-buffer-warmup to separate cp.warmup's reservation from the encode's growth
```
