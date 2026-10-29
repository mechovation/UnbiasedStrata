# Real mtmd metadata fixtures for PR4

These ten 84-byte files are **public `mtmd_input_chunk_save()` output**, not hand-written serialization. `vision-metadata-real.cpp` generated RGB inputs, used actual CPU preprocessing and isolated mtmd tokenization (`add_special=false`), preflighted the saved bytes, then loaded them through the public loader and checked counts/positions. The updated harness also validates PREPARED envelopes with real tokenizer-derived prefix/suffix IDs. Metadata mutations in the no-model test are rejection cases only.

Provenance:

- Dependency pin: `44072a19fb94d0bed63d381d42f6237373f91d54`; dependency worktree clean.
- Native serialization v2, Linux x86-64 little-endian host ABI. Not portable cross-endian serialization.
- Vocabulary-only model: `Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf`, snapshot `ed59f92082b1e93c0e96d60a8b11aab089b52f09` from ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF.
- Projector: `mmproj-Qwen3.8-Flash-Next-BF16.gguf`, family `qwen3vl_merger`; SHA-256 `b1a82259702816a5330d7bd7607cd9676b11780e79ff7348c21103ff3ce49bd0`.
- Effective spatial merge 2, patch size 16, pixel stride 32. Other merge configurations are explicitly refused by the qualification harness.
- Fresh CPU-only Release library build with GCC 13.3; static mtmd SHA-256 `41c4c10d21c85183ca1942f5565566d42cce502a946a495204cabbbe52e43e31`; static llama SHA-256 `f1e1d1530ec1e0e5da5219a89c92fc8fd2f04be466124ac6c04bc297c0dc92b9` (first capture).
- Existing local build-dependencies image, `runc`, network disabled, no GPU device requests, GPU environment masks disabled, 4-CPU/8-GiB limit, source/assets read-only. No downloads, engine, production changes or language-model tensor loading.
- RGB byte `i` is `i % 251`. Every bitmap has explicit fixture ID `pr4-fixture`. The fixtures deliberately contain no request content, keys or source paths.

| File suffix | Source dimensions | Image token budget | Saved grid |
| --- | --- | --- | --- |
| 0 | 224×224 | 64 | 7×7 |
| 1 | 448×112 | 64 | 14×4 |
| 2 | 112×448 | 64 | 4×14 |
| 3 | 333×197 | 64 | 10×6 |
| 4 | 2048×1024 | 64 | 11×5 |
| 5 | 224×224 | 1024 | 7×7 |
| 6 | 448×112 | 1024 | 14×4 |
| 7 | 112×448 | 1024 | 4×14 |
| 8 | 333×197 | 1024 | 10×6 |
| 9 | 2048×1024 | 1024 | 45×22 |

Files 0–3 intentionally match 5–8: the larger token cap does not change those images. `SHA256SUMS` is verified by `pr4-foundations.sh`. The baseline tests need neither models nor mtmd libraries. All injected numeric caps are test values, not production budget approval.

Regeneration in an isolated CPU-only environment with existing assets:

```sh
bash frontend/tests/vision-metadata-real.sh BUILD MODEL_GGUF PROJECTOR_GGUF OUTPUT_DIR
```

The runner pins/verifies source and clean-rebuilds linked libraries. It does not download assets or modify the dependency. Explicit warmup is disabled. Wrapped encode entry points abort if reached; this is not exhaustive internal-function instrumentation or an encode/GPU qualification. Compare regenerated file hashes before updating fixtures.
