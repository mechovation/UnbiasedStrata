# Live Strata / UnbiasedStrata vision comparison

2026-10-29, RTX 5070 Ti only. Companion to [the source review and specification](VISION_STACK_REVIEW.md).

Functional reproductions and native fault-containment probes are recorded separately in [VISION_ISSUE_VALIDATION.md](VISION_ISSUE_VALIDATION.md); this performance report alone does not validate those reported issues.

**Target architecture: C++/CUDA only.** Strata's Python server is the reference arm; Python clients/bootstrap scripts here are external benchmark tools. They are not part of UnbiasedStrata's serving path, and no Python runtime dependency or Python implementation port is proposed.

## Findings

- **No large warmed latency regression on these small synthetic fixtures.** UnbiasedStrata's median end-to-end times were approximately 1–3% higher for warmed small-image, mixed-image/text, and conversation workloads.
- **Cached 4K input exposed a larger avoidable cost:** 2.200 s versus 2.054 s, about **7%** higher. Time until HTTP headers was 140 ms versus 9 ms. This is consistent with the source finding that the C++ cache lookup occurs after decode/preprocessing; headers are not a direct isolated decode timer.
- **First-image latency was repeatably much higher in UnbiasedStrata:** 9.714 s versus 2.651 s. Its first projector inference took 7.111 s; subsequent dashboard encodes took about 148 ms. Strata performed a 6.7-s synthetic encode **before readiness**, so this is largely a warmup/readiness-policy difference, not a 7-s warmed GPU encoding deficit.
- **Caching works in both implementations:** each recorded 25 fresh encodes and 50 embedding-cache hits in the final run. Both reused conversation prefixes.
- **All 150 final-run responses passed the expected-facts checks.** All 75 paired request hashes, response texts, and prompt-token counts matched exactly. This is not a general OCR-quality or numerical-equivalence qualification.
- Production **UnbiasedStrata was restored**, authenticated smoke tests passed, and its Compose file remained byte-for-byte unchanged. The upstream production container remains stopped; the 3060 was not used.

## Final run: matched engine residency

Each regular workload has 10 measurements. Primes and text warmup are retained separately, yielding **75 requests per frontend**. Times run from issuing the HTTP request through SSE `[DONE]`; TTFC means first nonempty content token.

| Workload | Strata median total | UnbiasedStrata median total | Strata median TTFC | UnbiasedStrata median TTFC |
| --- | ---: | ---: | ---: | ---: |
| Text-only, projector loaded | 0.472 s | 0.476 s | 0.405 s | 0.408 s |
| New image bytes + text, 1024×768 | 2.175 s | 2.189 s | 1.887 s | 1.894 s |
| New image bytes, image-only user content | 2.089 s | 2.098 s | 1.892 s | 1.903 s |
| Cached image + text, fresh prompt | 1.899 s | 1.924 s | 1.708 s | 1.736 s |
| Two cached images interleaved with text | 2.311 s | 2.337 s | 1.864 s | 1.898 s |
| Cached 3840×2160 image + text | 2.054 s | 2.200 s | 1.853 s | 1.998 s |
| Followup turns resending image history | 0.852 s | 0.876 s | 0.592 s | 0.627 s |

The new-image/text median includes its first-use outlier. All 10 images are embedding-cache misses, but only the first is a first-use encoder event. This distinction matters:

| First-use / resource observation | Strata | UnbiasedStrata |
| --- | ---: | ---: |
| Container launch to healthy, polled every 2 s | 41.62 s | 33.48 s |
| First actual image request, total | 2.651 s | 9.714 s |
| First actual image request, TTFC | 2.138 s | 9.202 s |
| Native encoder time for that image | 155 ms | 7111 ms |
| Expert cache after sizing | 3107 slots / 5.95 GiB | 3107 slots / 5.95 GiB |
| Engine-reported free VRAM at startup | 1506 MiB | 1472 MiB |
| Maximum sampled workload GPU memory | 14523 MiB | 14659 MiB |

The first image follows 11 short text requests, not immediately after container readiness. Readiness and image timings must not be presented as a single measured startup-to-answer interval. Strata's longer readiness wait already includes its synthetic encoder warmup. GPU memory is sampled every ~0.5 s, not an exact allocation peak; startup was not sampled.

P95 values use nearest rank. With only 10 measurements this is the maximum, not a robust tail estimate:

| Workload | Strata total p95 | UnbiasedStrata total p95 |
| --- | ---: | ---: |
| New image bytes + text, including first use | 2.651 s | 9.714 s |
| Cached small image + text | 1.943 s | 1.968 s |
| Mixed two images + text | 3.095 s | 3.135 s |
| Cached 4K image + text | 2.110 s | 2.269 s |
| Conversation followups | 0.994 s | 0.972 s |

Mixed-image replies were not fixed-length: preserve their completion counts when interpreting end-to-end tails. Paired final-run responses were nevertheless identical, including longer responses at corresponding trials.

### What the live logs confirmed

- UnbiasedStrata sent **GENI even for all 11 text-only requests** with the projector loaded. The source-review V2 finding is now reproduced. The measured overhead here was small; this does not establish a large text throughput penalty.
- Small-image cached request median time until headers was **29.7 ms vs 6.4 ms**; mixed images **35.5 ms vs 8.3 ms**; cached 4K **140.2 ms vs 8.6 ms** (UnbiasedStrata vs Strata). These include frontend work, request-file assembly, and scheduling, not just image decode. Increasing decoded pixels while keeping compressed input small makes the late-cache placement worth improving.
- Actual image grids matched: dashboard **768 rows (32×24)**, service card **300 (20×15)**, enlarged dashboard **1008 (42×24)**, all width 2560.
- The high-resolution workload's internal group ID is `highres_cached_prefix`, but **both engines reported zero reused prompt tokens** for it. Its cache hits are embedding reuse, **not KV-prefix reuse**. Do not infer a broken image cache or claim this isolates frontend overhead with prefill eliminated.
- The separate conversation workload did reuse KV: 814 to 1520 cached prompt tokens as the history grew. Some intermediate reused counts differed by up to four tokens despite identical total prompt counts; no cause is established here.

## Controls and provenance

Both test services ran sequentially on GPU UUID `GPU-d14daf49-45c8-58c5-cf61-5a40342672af`, at a temporary loopback-only port 18081. The production service on port 8080 was stopped during each comparison. No Compose alternatives/overrides or production setting changes were used.

Shared controls:

- Same **native engine binary**, copied from the UnbiasedStrata image and mounted into both test containers, and the same `libstrata_kq.so`.
- Same read-only IQ3_S GGUF shards, pack, MTP directory, projector, and byte-identical expert profile.
- 262144 context, int8 growing KV, prompt cache 6, conversation parking disabled for the experiment, prefill auto, CLI `--spec 4` (MTP cap) with minimum probability 0.5, direct PLE, 23 expert workers plus host thread. Suffix drafting remains enabled; the backend can use a six-token verification window in both arms.
- Same 48 logical CPUs (`0-47`), 96-GiB RAM/no-swap limit, GPU visibility mask, and 1200-MiB reserve setting.
- Explicit `--expert-cache 2400` sizing budget, verified to produce **3107 actual native variable-size slots in both arms**. The numeric option is a uniform-blob budget, not an exact native slot count; the actual log is authoritative.
- Same `STRATA_PF_FUSED=1`, `STRATA_PREFILL_CPU_SHARE=0`, `STRATA_PLE_PROBE=0`, and `STRATA_ROUTE_TAIL_SKIP=0`. Disabling route-tail skipping removes a known approximation; it differs from production's implicit default.
- GPU vision, 24 encoder CPU threads, maximum 1024 image tokens, default attention policy, and embedding caches enabled (64 entries; C++ additionally has its existing soft 1-GiB row-storage limit).
- Greedy requests, thinking disabled, 128 output-token cap, explicit sampling fields, same raw image payloads and messages. Canonical assistant history makes conversation requests byte-identical independently of previous response wording.

This intentionally tests **the vision/frontend stacks around one common text engine**, not upstream Strata's newer native text engine versus UnbiasedStrata's engine, and not production-default throughput/capacity.

### Identifiers

| Artifact | Identity |
| --- | --- |
| UnbiasedStrata image | `local/unbiased-strata:cuda13-sm120-vision`, SHA-256 `493bdac6c7688b76d4c074c1d689bfd6e3a249621f35b5714334c52928fe0fed` |
| Strata image | `strata:0.2.7.1-5e8a4cc-sm86-sm120-vision`, SHA-256 `03d3ffeebd3e8a304558e822d69e585d6db25561a2940c253d9ccdf2b91e72cf` |
| Shared engine | SHA-256 `e328bf8c34f9b55e6427f16e7d3097d0596aa8e33801a015fcdd34bd8f92ee26` |
| Shared KQ library | SHA-256 `1cbc94e09ba7f87873122c1ac40f82884561b1e3e1f87c196ddf88a1fe167e15` |
| Expert profile, both images | SHA-256 `8f59b4aa8873209dff11c11e37bcda9529a1335b724a1afeea37bf6388975baf` |
| GGUF snapshot | `ed59f92082b1e93c0e96d60a8b11aab089b52f09`, same host directory in both arms |

The Strata image's `serve/server.py` and `tools/vision/strata_vision.cpp` were copied out and verified byte-identical to the reviewed sibling source. Its `mtmd.cpp` and `clip.cpp` were also byte-identical to the UnbiasedStrata submodule files. This is stronger than relying on image tags, but is **not proof of identical build flags or all dependencies**.

### Important runtime limitation

Installed runtime packages differ:

| Package | Strata image | UnbiasedStrata image |
| --- | --- | --- |
| `cuda-cudart-13-0` | `13.0.48-1` | `13.0.96-1` |
| `libcublas-13-0` | `13.0.0.19-1` | `13.1.0.3-1` |

The shared engine therefore does **not** run against identical CUDA math libraries. Treat this as a comparison of the available deployed vision stacks with matched engine residency, not a fully isolated frontend microbenchmark. Small 1–3% differences are not sufficient to assign causation to Python vs C++, or to declare a general regression. An implementation speedup claim should repeat the A/B with matched runtime libraries/build flags and interleaved/reversed repetitions.

## Fixtures and interpretation

- Synthetic 1024×768 dashboard: order **7342**, **READY**, **128.50 USD**, **GREEN** indicator.
- Synthetic 640×480 service card: code **B917**, **STABLE**, count **42**, **BLUE** indicator.
- Synthetic 3840×2160 enlargement of the same dashboard; 81,669 compressed bytes but ~24 MiB of RGB pixels. This deliberately stresses repeated decoding without a large HTTP payload. It is **not** a dense native-4K OCR accuracy fixture.
- Twenty-three PNG encodings of the small dashboard have identical RGB pixel hashes but distinct metadata/byte hashes. These create controlled embedding-cache misses without changing visual content.
- Image-only means **no text in the user content**; the system message supplies the extraction instruction.
- Expected-facts checks are substring checks on these synthetic values, with every final response ending normally. They are not a comprehensive semantic scorer. No natural photographs, EXIF/WebP live inputs, tiny-text quality sweep, or adversarial images were tested.

Independent prompts change their system prefix to avoid active prefix reuse. Conversation history grows over 10 followups; image acquisition/preprocessing is repeated by the client resending that history. Both embedding and prompt cache states are recorded, not conflated.

## Preliminary pass and evidence

An earlier Strata-first pass made 64 requests per frontend (**128 responses**, all fact checks passed). The same nominal `--expert-cache 3200` budget was capped by free VRAM, yielding **3642 vs 3631** actual slots and low-headroom warnings. That pass is retained, but is not the final table. It reproduced the first-image difference (2.650 vs 9.714 s) and broadly similar warmed latencies.

The final pass reversed the order (**UnbiasedStrata, then Strata**), lowered the budget to match residency with better headroom, and added the 4K workload. Thus **278 successful live responses** were retained across the two passes. This is still a small sequential experiment, not randomized/interleaved statistical qualification.

Local, gitignored evidence:

```text
logs/vision-ab-20261029/                 preliminary pass, shared binaries, source identities
logs/vision-ab-20261029/controlled/      final residency-matched pass
  experiment.json                     settings, shared binary hashes, pre-test Compose hash
  fixtures/manifest.json               compressed/pixel hashes, dimensions, byte sizes
  {Strata,UnbiasedStrata}-results.jsonl request hashes, timing, content, usage, cache counts
  {Strata,UnbiasedStrata}-server.log    protocol/cache/encoder observations
  Strata-engine.log                   helper warmup and engine residency logs
  {Strata,UnbiasedStrata}-resources.jsonl
  {Strata,UnbiasedStrata}-startup.json
  summary.json
  restored-health.json
  restored-smoke.log
  run.exit                            0
```

A benchmark-only Strata bootstrap logs native `OK` encode replies and per-image hit/elapsed information; it does not change the encoder/cache implementation. Resource sampling and logging run in both arms, except that bootstrap instrumentation is specific to Python. API keys were read from secret files, never supplied as literal command arguments or emitted in result files. No downloads or image builds were needed.

Reusable clients:

```sh
# Against an already-started, explicitly verified isolated test service:
python3 deployment/vision-stack-ab.py --frontend Strata \
  --fixtures logs/vision-ab-20261029/controlled/fixtures \
  --output /tmp/new-strata-results.jsonl
# Use --frontend UnbiasedStrata for the other arm; never run both GPU engines together.

python3 deployment/summarize-vision-stack-ab.py logs/vision-ab-20261029/controlled
python3 -m unittest discover -s deployment -p test_vision_stack_ab.py
```

The original five client-accounting tests passed (successful SSE, truncated streams, SSE errors, malformed JSON, stable request hashes). Subsequent tooling hardening expanded this to **13 passing tests**, including redirect refusal, credential redaction, normal-finish requirements, duplicate trial rejection and missing-counter accounting. The client refuses to overwrite a result file and retains failed requests. Starting/stopping services is separate; inspect existing settings and arrange restoration before another experiment.

## Consequences for the improvement plan

1. **Real encoder warmup before readiness — planned only, not implemented.** The user confirmed adding an explicit native C++/CUDA startup image encode to the improvement plan, without authorizing implementation. Coordinate it with expert-cache sizing; `cp.warmup=true` did not eliminate the first real encode cost in this build. See the detailed requirement and acceptance criterion in [§5.D of the specification](VISION_STACK_REVIEW.md#d-gpucpu-coordination).
2. **Move cache lookup ahead of decode/preprocessing.** The cached 4K case supplies a measurable target, while also showing that small-image benefits may be only tens of milliseconds.
3. **Fix text-only GENI** for correctness and avoidable work, without promising a large speedup from this workload.
4. Preserve cancellation, admission, isolation, TLS/EXIF/codec and resource-safety work from the specification. This run did not fault-inject or otherwise qualify those missing safeguards.
5. Before claiming a release speedup, rerun with matched runtime libraries, broader real-world images and stronger sampling. Do not translate this benchmark into “all vision is 7% slower” or a general OCR-quality ranking.
