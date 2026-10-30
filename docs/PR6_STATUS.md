# Native vision checkpoint: PR6 status

**2026-10-30 — implementation checkpoint, not full PR6 release approval.**
This is the single PR6 status document included with this push. PR5/PR6 tests,
qualification tools, detailed documents, evidence files and local deployment/
working files are intentionally excluded and remain in the development workspace.
Existing planning/test files in earlier commits are historical, not the updated
PR5/PR6 acceptance suite. Required third-party notices/licenses are included.

## Implemented code

- PR5 complete tokenizer-recipe/row caching before reader PREPARE, bounded
  single-flight sharing, held-entry accounting, cancellation-safe ownership and
  certified readerless cache hits. Misses do not implicitly restart the reader.
- Worker-owned JPEG EXIF orientations 1–8 through libexif/libyuv; missing,
  malformed or unsupported orientation retains orientation 1. Existing stb JPEG
  decoding and pass-through PNG alpha behavior are unchanged.
- WebP first reconstructed canvas, including animation offsets, with library
  premultiplied-alpha compositing over white; no later frame is decoded.
- Typed, source/occurrence/generation-bound WebP-header rejection. Only a rejected
  **tool** image becomes `[Image omitted: unreadable tool-result image.]`.
  User images remain clear pre-SSE errors. Faults/resources/cancellation/deadlines
  cannot become omissions.
- Prompt/template/literal regeneration from already-acquired owned inputs, without
  URL/file reacquisition, request retry or deletion of expanded image-pad runs.
  Surviving image IDs/rows stay aligned; all-omitted requests use ordinary GEN.
- Policy, host workspace allowance and actual loaded shared-library hashes bind
  cache generation. Runtime ABI: **`pr6-runtime-v3-tool-omission`**. Parent and
  reader must be rebuilt/restarted together; compatible library replacement is
  supported and selects a new cache namespace.

Legacy remains the default. No engine implementation, llama.cpp pin, production
settings, GPU reserve or PR7 synthetic warmup was changed.

## Validation completed before this checkpoint

- Ordinary/UBSan CPU protocol, ownership, cache, cancellation, cleanup, handler/
  queue and reader regressions; 25 codec/projector parity cases and five rejects.
  Normalizer controls passed ASan/UBSan default and scalar dispatch. Prebuilt
  dependency libraries/archives were not fully sanitizer-instrumented.
- On the RTX 5070 Ti only: **44/44** codec/reference row/token pairs across
  2560- and 2048-wide projectors; both real engines passed eight EXIF orientation
  semantics controls and WebP/PNG semantics at matched actual prompt-token counts.
- Mixed/repeated bad tool history, all-omitted GEN, streaming, warm-cache zero-
  reader-work and user-image pre-SSE 400 controls passed on both real engines.
- Full-allocation readerless/restart gates passed at **262135** evaluated tokens
  for Flash and **32759** for explicitly frozen `strata-poly --kv-whole`.
  Resident/absent-reader engine allocations matched (14929625088 / 14092861440
  bytes respectively); fresh uncached 1024-square encode preceded cached GEN
  after restart. No reader/engine overlap, parent GPU context, XID or OOM observed.
- Candidate codec packaging/source/import checks and compatible official libexif
  replacement passed in disposable CPU-only containers. Replacement changed only
  its library digest and resulting generation; clean-process config agreed.
  Missing notices/source, wrong pinned library and existing output were rejected.

These summarize completed workspace qualification, **not a test run from this
reduced checkout**. The excluded test suites are not supplied as public
reproduction evidence. Production was restored to its original image/settings
and healthy at the completed GPU window's close. No subsequent GPU window or
production deployment is authorized by this push.

## Build and codec distribution

CMake requires installed public headers and **shared** WebP/demux, libexif and
libyuv libraries. There are no configure-time downloads, library patches, static
libexif embedding or custom codec/parser/pixel-transform implementations.

Evaluated Ubuntu packages:

| Source package | Version | Terms |
| --- | --- | --- |
| WebP, demux, SharpYUV | 1.3.2-0.4build3 | BSD-3-Clause + patent grant |
| libyuv | 0.0~git202401110.af6ac82-1 | BSD-3-Clause + patent grant |
| libexif | 0.6.24-1ubuntu0.24.04.1 | LGPL; replaceable shared linkage |
| libjpeg-turbo (libyuv dependency) | 2.1.5-2ubuntu2 | IJG/BSD/zlib |

See [required notices and licenses](../third_party/vision-codecs/NOTICE.md).
`ARTIFACTS.sha256` records the evaluated packages and exact libexif corresponding
source; `RUNTIME.sha256` records the six added runtime shared objects. Binary
packages, source archives and compiled artifacts are **not** committed.

The offline candidate packager requires operator-supplied matching artifacts:

```sh
cmake -DINPUT_BIN_DIR="$BUILD/bin" -DCODEC_PREFIX="$CODEC_PREFIX/usr" \
  -DSOURCE_CACHE="$DEPENDENCY_CACHE/source" -DOUTPUT_DIR="$NEW_BUNDLE" \
  -P cmake/PackageVisionCodecs.cmake
```

It preserves executable bytes and packages the six libraries, notices/licenses,
exact original + Ubuntu source/build archives and `.dsc`, this status document
and a file-hash manifest. Its pin identifies the evaluated bundle, not a runtime
replacement allowlist. A future shipping image must retain these materials and
verify compatible shared-library replacement and baseline CUDA/TLS obligations.
The local deployment Dockerfile/Compose are not part of this checkpoint.

## Open limitations / release conditions

1. **Explicitly left open:** omission covers only explicit WebP header failures.
   JPEG/PNG/unknown-format and ambiguous post-header WebP decoder failures remain
   request errors, including in tool history. Generic stb header failure can mean
   allocation failure; do not broaden classification or match diagnostic strings.
2. **Security/support disposition:** scoped review is not vulnerability-free
   clearance. Ubuntu's exact libexif package includes the published fixes for
   CVE-2026-40386, CVE-2026-40385 and CVE-2026-32775. libjpeg-turbo Noble records
   CVE-2026-75466 and CVE-2018-10126 still require vendor/release-owner disposition;
   their described newer PNG-loader/encoder paths are not used by PR6, but that
   is not blanket library clearance. No libyuv CVE records returned is not a
   support/security guarantee. Recheck current advisories/support before release.
3. **Resource/qualification boundary:** 32 bytes per maximum source pixel is host
   admission allowance, not a hard bound on opaque codec heaps or a VRAM reserve.
   Process/cgroup limits and safety containment remain mandatory; process
   separation is not an OS security sandbox. Flash's minimum sampled free VRAM
   was only **140.5 MiB** (poly: **1032.5 MiB**), not an endurance/capacity promise.
   Unfrozen poly and unreviewed profiles/shapes/budgets are not qualified.
4. **Final artifact approval remains separate:** candidate codec-compliance checks
   do not approve an unspecified shipping image or baseline runtime redistribution.
   No PR7, reserve/engine changes or production activation are included.
