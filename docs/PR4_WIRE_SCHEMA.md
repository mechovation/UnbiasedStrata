# PR4 v1 command payloads and metadata boundary

CPU-tested foundation, not wired into serving. Header/transport rules are in [the implementation contract](PR4_IMPLEMENTATION_CONTRACT.md). Every payload below is validated with exact consumption by `frontend/vision_payload.hpp`; a valid header alone is insufficient. All integers below are unsigned little endian. No padding, native structs, URLs, paths, prompt strings, diagnostics or credentials.

## Parent-selected context

`Config` contains a 32-byte configuration-generation digest, approved device UUID as 16 bytes, embedding width, effective pixel stride and actual parent vocabulary size. The generation must bind the dependency/ABI, vocabulary, projector, preprocessing policy and operational limits. Generation calculation and production configuration allowlisting remain integration work: the parser compares a supplied parent expectation; it does not establish its provenance. Real serving must use only GPU-d14daf49-45c8-58c5-cf61-5a40342672af. Tests use synthetic identities and no GPU.

All numeric resource caps are explicit parent inputs with no production defaults. `Binding` is the expected occurrence index, source SHA-256 bytes, parent-assigned mtmd image ID, source length, live handle and admitted shape for the operation. Keep task/instance/job/sequence checks in transport and the supervisor registry independently of payload validation.

Common prefixes:

- `C`: exactly 32 bytes, matching parent configuration generation.
- `O`: `C`, occurrence u32, source SHA-256 (32 bytes). Occurrence must match the expected request occurrence and be below the occurrence cap. A repeated source remains a separate occurrence.
- `H`: `O`, nonzero prepared handle u64. Except on PREPARED, this must equal the expected live handle.

## Operations

| Kind | Payload following the prefix | FDs |
| --- | --- | --- |
| READY | `C`, family u32 (=1, Qwen3-VL still), width u32, pixel stride u32, vocabulary size u32, device UUID (16 bytes) | 0 |
| PREPARE | `O`, source byte length u64 | 1 immutable source |
| PREPARED | `H`, grid x u32, grid y u32, width u32, prefix text run, suffix text run, metadata length u32, metadata bytes | 0 |
| ENCODE | `H`, grid x u32, grid y u32, width u32, row byte length u64 | 1 parent-created row output |
| ENCODED | same payload as ENCODE | 0 |
| RELEASE / RELEASED | `H` only | 0 |
| QUIT | `C` only | 0 |
| ERROR | `C`, error class u32 | 0 |

A text run is u32 count followed by that many u32 token IDs. Prefix and suffix share one aggregate text-token budget; count/remaining-byte checks happen before examining IDs. Every ID must be below the actual parent vocabulary size (which itself must fit signed int32); negative/sentinel encodings are refused. The reader derives runs from actual isolated mtmd tokenization with `add_special=false`; no fabricated delimiters or role-dependent regeneration. Parent text/BOS/EOS composition remains Spike A's independently qualified responsibility.

PREPARE/ENCODE operate on **one occurrence at a time**. Multiple images are processed as distinct sequential operations under the same reserved request turn, retaining instance/job-owned handles until admission/encode/release. This avoids a bulk descriptor list and keeps wire framing independent of the eventual occurrence limit. No coalescing or early cache lookup. Existing late row-cache ownership is still an integration acceptance gate.

PREPARED describes exactly one image chunk, with all tokenizer-derived text before/after it. Audio/video, multiple image chunks per isolated source, other families and non-still layouts are rejected, not silently flattened. This representation was checked with actual Qwen3-VL preprocessing/tokenization; see [fixture provenance](../frontend/tests/data/pr4-metadata/README.md). Native tests also build synthetic command envelopes, explicitly not a prompt-parity claim.

READY fields must equal parent expectations, not enlarge them. PREPARE source length must equal the acquired parent source, fit its hard cap and never exceed PR3's 10 MiB cap. PREPARED/ENCODE/ENCODED grid/width/row products are checked before multiplication; pixel/grid/row caps apply independently. ENCODE shape must equal the admitted binding. ERROR classes are 1 invalid source/layout, 2 resource refusal, 3 native failure, 4 protocol failure; HTTP mapping remains parent-owned. Unknown classes and arbitrary child error strings are refused.

## Metadata preflight

`frontend/vision_metadata.hpp` is a no-allocation, no-mtmd-call preflight specifically for pin `44072a19fb94d0bed63d381d42f6237373f91d54`, serialization version 2, same-host ABI. Metadata remains host endian (unlike the outer wire). It is not a generic serialization parser.

Permitted form:

1. Version u64=2; chunk type u32=IMAGE(1); text count u64=0; image-present u8=1.
2. Positive grid x/y u32; position u32=MROPE(1); image index u32=0; temporal merge u32=2.
3. ID length u64 and bytes exactly equal to the bounded parent-assigned ID; no embedded NUL.
4. is-audio u8=0; batch count u64=1.
5. Single entry: view-separator u8=0, newline u8=0, lead-pad i32=0, positive pixel x/y i32.
6. Audio-present u8=0; exact end of blob.

Pixel dimensions must equal grid dimensions times the qualified `patch_size * 2` stride. The Qwen3-VL qualification explicitly requires effective spatial merge 2, including the metadata override; another merge setting requires new review. Checked geometry must fit configured grid/pixel caps, public u32 multiplication and signed int32 position accounting. Serialized placeholders contain no pixel buffers and are never encodable.

All nested lengths use remaining-length/equality checks before any public loader call. The validator rejects version/type/flag mismatches, overflow-sized ID/count fields, unsupported batch/temporal/layout variants and trailing bytes. It never manufactures mtmd bytes. The default no-model tests use real saved fixtures and corrupted copies; malformed inputs cannot reach mtmd because those tests do not link it.

After successful preflight, an integration adapter may call public `mtmd_input_chunk_load()` and must compare public ID, counts/grid/decoder positions to the validated declaration before composing. The CPU real-tokenizer test exercises that order and public positions. The serving adapter is not implemented yet; do not reuse the old Spike A restore helper on untrusted worker data.

## What validation does not establish

- Returned string views borrow the caller-owned immutable frame. Copy/own them before asynchronous handoff; do not keep them after moving, modifying or freeing its payload.
- Payload validation checks FD **counts**, not FD type/seals/content. The separate `vision_artifact.hpp` helper now checks exact sizes, required seals, parent-owned identity/shape and finite row values. Sources are created immutable; row output is size-sealed before handoff and write-sealed before scanning. Caller computes the source digest from acquired bytes; the helper compares supplied parent-owned tags, not image hashes. Poisoned cells detect incomplete untouched output but cannot establish that an encoder honestly computed valid-looking floats. No arbitrary reader-selected output FD replaces the parent-created backing.
- A nonzero handle in PREPARED is not sufficient proof of live job ownership. `vision_job.hpp` now checks per-job operation ordering, duplicate handles, reply identity and owned artifact lifetimes. A running supervisor, cross-job scheduling, reader-side RELEASE idempotence and restart policy remain. The owner must retain an active Job through safe drain/teardown; the core is not a process owner or GPU arbiter.
- Shape metadata caps do not prevent decoder allocations that happened earlier. Worker decode/preprocess admission and measured host/GPU budgets remain blocking integration gates.
- This schema does not change engine GEN/GENI/SVE1, HTTP behavior or deployment.
