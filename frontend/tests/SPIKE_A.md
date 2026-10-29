# Spike A — tokenizer-derived vision recipes

**Result: PASS for the pinned, existing Qwen3.8 / Qwen3-VL still-image path.**

This is a test-only proof, not a production cache, worker, IPC implementation, or authorization to implement PR4/PR5. No production source, dependency pin, deployment setting, or serving process was changed during the spike.

## What was proved

Using llama.cpp `44072a19fb94d0bed63d381d42f6237373f91d54`, the existing Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S vocabulary/template (`general.architecture=qwen4exp`) and its BF16 `qwen3vl_merger` projector:

1. A complete request's token stream can be captured into owned text IDs and public mtmd serialized image metadata, then reconstructed with `mtmd_input_chunk_load()` and individual `server_tokens::push_back(token/chunk)` calls.
2. An isolated still-image recipe can be derived by **actual preprocessing and mtmd tokenization with `add_special=false`**, retaining every emitted wrapper token and image metadata chunk. No delimiters, token IDs, serialization bytes, grid dimensions, or image placeholders are invented.
3. Reusing those isolated recipes inside surrounding request text reproduces the current frontend's complete prompt exactly, including its recursive literal-token splitting and its existing BOS/EOS behavior.
4. Recipes remain usable after all originating vocabularies, projector contexts, image buffers, prompt buffers, and chunks are destroyed. Loaded metadata can be appended/copied again without retaining borrowed storage.

The qualification is narrow: **Qwen3-VL-style still images using the tested vocabulary/template/projector and settings.** No claims for audio, video, other Qwen generations, image-index-dependent families, alignment-dependent families, or future pins. The harness rejects other projector family identifiers; recipe capture rejects non-still/non-Qwen grid/decoder layouts. An existing public upstream non-Qwen-layout fixture tests that rejection without manufactured serialization.

## Files and reproduction

Only new files under `frontend/tests/`:

- `vision-recipe.hpp`: trusted, test-only owned recipe representation, capture/restore and exhaustive parity checks.
- `vision-recipe.cpp`: includes the actual frontend implementation as a white-box oracle; generated native BMP fixtures and request corpus. No engine launch or GPU access.
- `vision-recipe.sh`: excluded real-projector test runner; not added to the no-model `make test` path.
- `SPIKE_A.md`: this result and its limitations.

Configure a CUDA-free static frontend build with installed OpenSSL 3 development files, using the existing pinned checkout. Supply existing model/projector paths:

```sh
cmake -S . -B /tmp/recipe-build \
  -DSTRATA_ENABLE_CUDA=OFF -DSTRATA_FRONTEND_CUDA=OFF \
  -DSTRATA_BUILD_TESTS=OFF -DSTRATA_BUILD_DSV41=OFF \
  -DCMAKE_BUILD_TYPE=Release
bash frontend/tests/vision-recipe.sh /tmp/recipe-build /path/to/model.gguf /path/to/mmproj.gguf
```

The model is loaded **vocabulary-only**; the CPU projector supplies real preprocessing/tokenization. No model downloads, engine inference, projector encode, or production key is required. The runner refuses CUDA builds and locally modified dependency sources, checks the configured source path, clean-rebuilds all linked frontend/mtmd libraries, and records the actual source pin plus archive hashes. It does not accept an environment-provided pin label in lieu of provenance.

The final run used a fresh Release build inside a 4-CPU / 8-GiB limited container, with no GPU device requests and its network disconnected before configuration/build/test. Existing local assets and an already-local base image were used. Temporary build dependencies were installed only in that disposable container. Production remained running and healthy throughout.

## Final measured result

```text
SPIKE_A_PASS cases=120 rejected_invalid=89 detached_replays=160 isolated_replays=40 family=qwen3vl_merger gpu=none encode_calls=0
```

- **120 exact parity cases:** 15 accepted request layouts × 4 BOS/EOS flag combinations × image budgets 64 and 1024.
- **89 negative checks:** 80 generation/width/metadata/count/grid/position mutations, 8 inherited internal-media-marker collision refusals, and 1 unsupported-layout capture refusal.
- **160 detached replays:** 120 full-prompt recipes plus 40 isolated-image recipes, restored and appended after original contexts/buffers are gone; serialized output remained identical to the saved owned snapshots.
- **Zero observed encoder calls:** test link seams refuse `mtmd_encode`, `mtmd_encode_chunk`, and `mtmd_batch_encode`. The CPU projector is initialized without explicit encode warmup. Existing backend allocation/shape initialization is not an inference benchmark.

Corpus: text-only, image-only, leading/trailing images, adjacent images, duplicates, interleaving, odd/wide/tall/large resized images, protected control-token spellings, UTF-8 text boundaries, assistant reasoning history, tool definitions, tool-result images, and long text around images. Images are generated in native test code at 224×224, 448×112, 112×448, 333×197 and 2048×1024.

For both full-prompt reconstruction and isolated-image composition, comparisons check:

- Every text token ID and every placeholder token cell.
- Media occurrence order, start boundaries, IDs, serialized metadata, token/position counts and decoder grids.
- Canonical maximal text runs surrounding media (the current `server_tokens` representation does not retain arbitrary adjacent raw-mtmd text chunk boundaries).
- Every decoder coordinate at base 0, base 17, and the actual prompt position.
- `pos_next()` at every token prefix, and `size_up_to_pos()` immediately around every resulting position boundary.
- Remaining-room arithmetic and the admission predicate at room −1, 0, 1, and 32 using full image token cells. This is **recipe count/admission-predicate parity**, not another execution of the PR1 queue/admission suite.

Generation identity includes the complete model GGUF metadata fingerprint (including tokenizer scores/merges/configuration, without reading model tensor payloads), vocabulary token pieces/attributes, chat template and BOS/EOS flags, full projector-file SHA-256, pin and preprocessing budget, and the supported-family marker. Image metadata owns its ID/grid/positions and bytes; the isolated recipe cache is keyed by raw source-byte SHA-256 within that generation. Embedding width is carried and checked, not inferred from placeholder row counts.

## Findings that constrain PR4/PR5

### Metadata placeholders are not encodable

Public `mtmd_input_chunk_save/load()` is sufficient for reconstruction and position accounting, but loaded chunks contain no preprocessed image data. They must **never** be sent to an encoder. A worker must retain its actual prepared chunks/handles for misses; an embedding cache must supply validated rows for hits. SVE1 rows alone are not a prompt recipe.

### Do not use the pinned concatenation overload for media

`server_tokens::push_back(server_tokens &)` attempts to push null placeholder IDs through the text-token overload, which rejects them. Its media-map copy loop also lacks an iterator increment. The spike encountered the null-token exception before reaching that loop. Use the public individual token/chunk append operations; no upstream patch or pin change was made.

### Preserve literal segmentation, including current EOS behavior

`tokenize_prompt()` adds BOS once. Each media-containing special segment goes through `process_mtmd_prompt(add_special=true)`, strips segment BOS, but **does not strip segment EOS**. With `add_eos=true`, literal spans can therefore expose intermediate EOS tokens. The spike deliberately matches this current behavior; changing it requires a separate correctness review, not an incidental worker migration.

The exact random internal media-marker spelling is not itself a vocabulary control token. Injecting it into request text currently produces a marker/file-count rejection. Both the baseline and recipe path retain that rejection; the spike does not claim to fix or broaden literal handling. Ordinary `<|image_pad|>`, vision/control delimiters, `<think>`, `<tool_call>` and default marker-like text remain covered by the existing literal protections.

### Decoder getters already include the base

For this pin, `mtmd_image_tokens_get_decoder_pos(image, base, i)` already incorporates `base`. Do not add it again despite the header's ambiguous wording. The image position advance is the maximum grid dimension, not its full token-cell count.

### A feasible recipe is not yet a safe IPC protocol

The recipe helper is **trusted internal spike data**, not production-hardened input validation. Pinned mtmd serialization is native-endian/version-dependent and its loader is not a hostile-input boundary; post-load validation cannot prevent allocations triggered by malformed internal lengths. A future worker transport needs separately reviewed framing, pre-load bounds or a safer metadata boundary, generations, checksums/identity as appropriate, and lifecycle ownership. The spike's numeric test limits are not approved production budgets. Text IDs also need validation against the actual vocabulary at that boundary.

## Gate decision and stop point

**Spike A's reconstruction/composition gate passes for the tested Qwen still-image scope.** A full-prompt-only fallback or dependency API extension is not required by these results.

PR4 may now be designed around tokenizer-derived recipes, but implementation still needs its own approved IPC/lifetime/cancellation/resource-budget contract. PR5 early-cache behavior remains dependent on that worker ownership design. No persistent cache, worker, readiness/warmup change, or production deployment was introduced.

Local development evidence (not required upstream artifacts): `logs/vision-spike-a/final2.log`, `final2.exit`, `final-configure.log`. Earlier compile/harness failures and the first parity run remain separate rather than being overwritten. The final clean rebuild and tightened layout checks passed with exit 0.
