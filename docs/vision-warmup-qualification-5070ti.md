# PR7 encoder warm-up qualification — RTX 5070 Ti

Date: 2026-11-06 UTC  
GPU: NVIDIA GeForce RTX 5070 Ti, `GPU-d14daf49-45c8-58c5-cf61-5a40342672af`  
Candidate image: `local/unbiased-strata:cuda13-sm120-vision-pr7`

## Evidence

- Create-only preflight after final harness correction:
  `logs/pr7-create-only-20261106T111904Z/`
- Warm-up off:
  `logs/pr7-gpu-20261106T111926Z-off/`
- Warm-up on:
  `logs/pr7-gpu-20261106T112132Z-on/`
- Preserved failed attempt that exposed the final harness defect:
  `logs/pr7-gpu-20261106T111746Z-off/`

The authoritative production container was `strata-5070ti`, created from
`/home/eric/Docker/Stack/docker-compose.yaml`. Both successful manifests record stable
Compose and backup hashes before and after their windows.

## Harness correction discovered during qualification

The first 2026-11-06 off attempt exited in 1.13 seconds. Its preserved log shows that
`--cap-drop ALL` prevented container root from reading the bind-mounted API key, whose host
mode is `0600` and owner is UID 1000. The candidate therefore exited while parsing
`--api-key-file`. The harness now mirrors production by not dropping those capabilities.
It also leaves image-default `NVIDIA_VISIBLE_DEVICES=all` in place while Docker's device
request and `CUDA_VISIBLE_DEVICES` pin the sole visible device by approved UUID.

The parent process still logs `ggml_cuda_init: failed to initialize CUDA` in both successful
arms. This is not the reader result: durable `nvidia-smi` samples show
`strata-vision-reader` on the 5070 Ti, and all image requests completed through the native
reader. Treat the parent line as non-fatal for this wiring.

## A/B result

| Measurement | Warm-up off | Warm-up on | Delta (on - off) |
|---|---:|---:|---:|
| Startup to healthy | 37.50 s | 43.64 s | +6.14 s |
| Reader VRAM at READY | 1,340 MiB | 1,446 MiB | +106 MiB |
| Engine VRAM at READY | 13,812 MiB | 13,692 MiB | -120 MiB |
| Peak reader + engine sampled sum | 15,334 MiB | 15,210 MiB | -124 MiB |
| First 1024-square image | 9.311 s | 2.589 s | -6.722 s |
| Startup + first image | 46.811 s | 46.229 s | -0.582 s |
| 1920x1080 cold image | 2.459 s | 2.421 s | -0.038 s |
| 800x1200 cold image | 2.167 s | 2.172 s | +0.005 s |
| 64x64 cold image | 2.344 s | 3.059 s | +0.715 s |
| Cached 1024-square repeats 1-4 | 0.234-0.277 s | 0.247-0.267 s | equivalent |

Warm-up moved essentially the entire one-time first-image event into startup: startup grew
6.14 seconds while first-image latency fell 6.722 seconds. End-to-first-image time from
container start improved slightly rather than regressing.

The warmed 2048-square path covered the tested 1024-square, 1920x1080, 800x1200, and 64x64
requests: no approximately seven-second event remained on any first user image in the on arm.

## Expert-slot derivation

At READY, the on arm's reader held 106 MiB more VRAM and the engine held 120 MiB less. Using
the runbook's legacy estimate of approximately 2.01 MiB per expert slot:

```text
120 MiB / 2.01 MiB per slot = 59.7 slots
```

Therefore warm-up costs approximately **60 expert slots** through pre-sizing allocation
pressure. This is derived from the process VRAM split; it is not an engine-reported slot
count. Peak engine VRAM differed by 124 MiB, consistent with the same order of effect.

## Safety and restoration

- Both successful candidates recorded `memory.events` values of zero for `max`, `oom`,
  `oom_kill`, and `oom_group_kill`.
- No `NVRM: Xid`/`Xid (` kernel-journal lines were present for the qualification interval.
- The RTX 3060 remained at 1 MiB in every pre/post inventory.
- The harness required the 5070 Ti compute-process list to be empty between production and
  candidate ownership; no production/candidate overlap occurred.
- No `strata-pr7-*` containers remained after either arm.
- Production `strata-5070ti` returned healthy after each arm.
- Restored text and image requests completed after each arm; restored image requests used
  1,089 prompt tokens.
- Final production GPU use returned to 15,189 MiB.

## Conclusion

The PR7 warm-up is effective on the production-class RTX 5070 Ti. It removes the 6.7-second
first-image penalty by paying the cost during startup, while exposing the expected allocation
tradeoff before engine sizing: +106 MiB reader residency and approximately 60 fewer expert
slots. The evidence supports enabling warm-up when this small capacity reduction is accepted
in exchange for predictable first-image latency.
