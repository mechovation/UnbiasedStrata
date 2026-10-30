// Native PR4 still-image worker. No HTTP, prompt text, engine or filesystem paths.
#pragma once
#include "vision_payload.hpp"
#include <chrono>
#include <string>
struct mtmd_context;
namespace strata_frontend::vision_reader {
struct Limits {
    uint32_t source_dimension;
    uint64_t source_pixels, retained_rgb_bytes, recipe_bytes;
    std::chrono::steady_clock::duration io;
};
// Caller owns a qualified Qwen3-VL context (merge=2, matching width/vocabulary,
// bounded preprocessing policy). Caller must enforce process/RSS/diagnostic
// containment separately. No production defaults or GPU safety policy here.
// Takes ownership of socket; returns 0 on QUIT, nonzero on any failed operation.
// Prepared chunks are real encodable objects, never public-load placeholders.
int serve(mtmd_context *, const vision_payload::Config &, const vision_payload::Limits &,
          Limits, uint64_t instance, int socket);
// PREPARE carries SHA-256, not a second ID string. The native reader and parent
// must use this lowercase hex identity, matching the existing mtmd/cache path.
std::string source_id(const vision_payload::Digest &);
} // namespace strata_frontend::vision_reader
