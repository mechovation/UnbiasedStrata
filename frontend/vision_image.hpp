// PR6 worker-owned orchestration of unmodified OSS image libraries.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace strata_frontend::vision_image {
inline constexpr const char * policy = "pr6-image-v1/jpeg-exif-ifd0-1to8-ignore-unusable/webp-first-canvas-bgra-premult-white/stb-other-rgb";
// This bounds application-owned intermediates and reserves canvas allowance.
// Opaque codec allocations additionally require the reader's process/cgroup cap.
inline constexpr uint64_t workspace_per_pixel = 32;
struct Limits { uint32_t dimension; uint64_t pixels, rgb_bytes, workspace_bytes; };
// unreadable is the legacy generic header result, NOT necessarily resource-safe.
// Only invalid_webp_header has a typed bitstream-only public-library status.
enum class Status { ok, unreadable, resource, decode_failed, internal, invalid_webp_header };
struct Image {
    Status status = Status::internal;
    uint32_t width = 0, height = 0;
    std::vector<unsigned char> rgb;
};
Image decode(const unsigned char *, size_t, Limits);
// Paths are obtained from the loader, not supplied by image input. Hash the
// actual shared objects into generation; do not forbid compatible replacements.
struct Library { std::string role, path; };
std::vector<Library> libraries();
} // namespace strata_frontend::vision_image
