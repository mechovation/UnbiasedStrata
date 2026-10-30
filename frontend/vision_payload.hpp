// PR4 v1 command payload validation. Opaque frame storage stays owned by the caller.
// Returned views MUST NOT outlive the immutable frame; no borrowed view crosses async boundaries.
#pragma once
#include "vision_protocol.hpp"
#include "vision_metadata.hpp"
#include <array>
#include <string_view>

namespace strata_frontend::vision_payload {
using Digest = std::array<uint8_t, 32>;
// Deliberately narrow: generic stb/decoder failures are NOT input rejection.
enum class InputError : uint32_t { none = 0, webp_header = 1 };
struct InputFailure { uint32_t occurrence; InputError reason; };
struct Config {
    Digest generation; // parent-derived hash including pin, model/vocabulary, projector, settings and limits
    std::array<uint8_t, 16> device;
    uint32_t width, pixel_stride, vocabulary_size;
};
struct Limits {
    uint32_t payload_bytes, occurrences, text_tokens;
    uint64_t source_bytes, row_bytes;
    vision_metadata::Limits metadata;
};
struct Binding {
    uint32_t occurrence;
    Digest source;
    std::string_view metadata_id; // parent-assigned image identity, NOT a child-provided path
    uint64_t source_bytes, handle;
    uint32_t grid_x, grid_y; // admitted shape for ENCODE/ENCODED
};
struct View {
    uint64_t handle = 0, row_bytes = 0;
    uint32_t error_class = 0;
    InputError input_error = InputError::none;
    vision_metadata::Shape shape{};
    std::string_view prefix_ids, suffix_ids, metadata; // IDs are u32 little endian, already vocab-checked
};
class Cursor {
public:
    explicit Cursor(std::string_view input) : remaining_(input) {}
    bool bytes(size_t n, std::string_view & out) {
        if (n > remaining_.size()) return false;
        out = remaining_.substr(0, n); remaining_.remove_prefix(n); return true;
    }
    template<size_t N> bool equal(const std::array<uint8_t, N> & value) {
        std::string_view got;
        return bytes(N, got) && std::memcmp(got.data(), value.data(), N) == 0;
    }
    bool u32(uint32_t & value) {
        std::string_view b; if (!bytes(4, b)) return false;
        value = uint32_t(vision_protocol::little(reinterpret_cast<const uint8_t *>(b.data()), 4)); return true;
    }
    bool u64(uint64_t & value) {
        std::string_view b; if (!bytes(8, b)) return false;
        value = vision_protocol::little(reinterpret_cast<const uint8_t *>(b.data()), 8); return true;
    }
    bool tokens(uint32_t & remaining_budget, uint32_t vocabulary, std::string_view & out) {
        uint32_t count;
        if (!u32(count) || count > remaining_budget || count > remaining_.size() / 4 || !bytes(size_t(count) * 4, out)) return false;
        for (size_t i = 0; i < out.size(); i += 4)
            if (vision_protocol::little(reinterpret_cast<const uint8_t *>(out.data() + i), 4) >= vocabulary) return false;
        remaining_budget -= count; return true;
    }
    bool empty() const { return remaining_.empty(); }
private:
    std::string_view remaining_;
};
inline bool rows(uint32_t x, uint32_t y, uint32_t width, uint64_t cap, uint64_t & result) {
    // Check products before multiplication, even when caller uses very large test/config limits.
    uint64_t cells = uint64_t(x) * y;
    if (!cells || !width || cells > uint64_t(INT32_MAX) || cells > cap / 4 / width) return false;
    result = cells * width * 4; return true;
}
// Binding comes from the expected operation, never from this payload. Header identity must
// independently be checked by transport. No mtmd public loader is called here.
inline bool validate(vision_protocol::Kind kind, std::string_view payload, size_t fd_count,
                     const Config & config, const Limits & limits, const Binding & binding, View & out) {
    if (payload.size() > limits.payload_bytes || !config.width || !config.pixel_stride ||
        !config.vocabulary_size || config.vocabulary_size > uint32_t(INT32_MAX) ||
        config.pixel_stride != limits.metadata.pixel_stride) return false;
    Cursor c(payload);
    if (!c.equal(config.generation)) return false;
    View pending;
    using vision_protocol::Kind;
    if (kind == Kind::ready) {
        uint32_t family, width, stride, vocabulary;
        if (fd_count || !c.u32(family) || family != 1 || !c.u32(width) || width != config.width ||
            !c.u32(stride) || stride != config.pixel_stride || !c.u32(vocabulary) || vocabulary != config.vocabulary_size ||
            !c.equal(config.device) || !c.empty()) return false;
    } else if (kind == Kind::quit) {
        if (fd_count || !c.empty()) return false;
    } else if (kind == Kind::error) {
        if (fd_count || !c.u32(pending.error_class) || pending.error_class < 1 || pending.error_class > 4 || !c.empty()) return false;
    } else {
        uint32_t occurrence;
        if (!c.u32(occurrence) || occurrence != binding.occurrence || occurrence >= limits.occurrences || !c.equal(binding.source)) return false;
        if (kind == Kind::input_rejected) {
            uint64_t bytes;
            uint32_t reason;
            if (fd_count || !c.u64(bytes) || !bytes || bytes != binding.source_bytes ||
                bytes > limits.source_bytes || bytes > 10 * 1024 * 1024 ||
                !c.u32(reason) || reason != uint32_t(InputError::webp_header) || !c.empty()) return false;
            pending.input_error = InputError(reason);
        } else if (kind == Kind::prepare) {
            uint64_t bytes;
            if (fd_count != 1 || !c.u64(bytes) || !bytes || bytes != binding.source_bytes ||
                bytes > limits.source_bytes || bytes > 10 * 1024 * 1024 || !c.empty()) return false;
        } else {
            if (!c.u64(pending.handle) || !pending.handle) return false;
            if (kind != Kind::prepared && (!binding.handle || pending.handle != binding.handle)) return false;
            if (kind == Kind::prepared) {
                uint32_t x, y, width, metadata_size, text_budget = limits.text_tokens;
                if (fd_count || !c.u32(x) || !c.u32(y) || !c.u32(width) || width != config.width ||
                    !rows(x, y, width, limits.row_bytes, pending.row_bytes) ||
                    !c.tokens(text_budget, config.vocabulary_size, pending.prefix_ids) ||
                    !c.tokens(text_budget, config.vocabulary_size, pending.suffix_ids) ||
                    !c.u32(metadata_size) || metadata_size > limits.metadata.bytes || !c.bytes(metadata_size, pending.metadata) ||
                    !c.empty() || !vision_metadata::preflight(pending.metadata, binding.metadata_id, limits.metadata, pending.shape) ||
                    pending.shape.x != x || pending.shape.y != y) return false;
            } else if (kind == Kind::encode || kind == Kind::encoded) {
                uint32_t x, y, width;
                uint64_t bytes;
                if (fd_count != (kind == Kind::encode ? 1u : 0u) || !c.u32(x) || !c.u32(y) || !c.u32(width) ||
                    x != binding.grid_x || y != binding.grid_y || width != config.width ||
                    !rows(x, y, width, limits.row_bytes, pending.row_bytes) ||
                    !vision_metadata::grid_shape(x, y, limits.metadata, pending.shape) ||
                    !c.u64(bytes) || bytes != pending.row_bytes || !c.empty()) return false;
            } else if (kind == Kind::release || kind == Kind::released) {
                if (fd_count || !c.empty()) return false;
            } else return false;
        }
    }
    out = pending; return true;
}
} // namespace strata_frontend::vision_payload
