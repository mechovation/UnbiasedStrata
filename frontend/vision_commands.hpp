// PR4 schema-aware payload builders. No FD ownership or process/queue policy.
#pragma once
#include "vision_payload.hpp"
#include <new>
#include <vector>

namespace strata_frontend::vision_commands {
using Kind = vision_protocol::Kind;
struct Prepared {
    uint64_t handle = 0;
    uint32_t x = 0, y = 0;
    // Token runs are already little-endian u32 bytes. Metadata is genuine public-save
    // output, never manufactured here. All views are borrowed only during build().
    std::string_view prefix, suffix, metadata;
};
struct Payload {
    std::vector<uint8_t> bytes;
    size_t fd_count = 0; // schema count only; caller supplies and retains actual FDs
};
enum class Result { ok, invalid, resource };
// Output unchanged on failure. Explicit cap checked before allocation/copy; the
// resulting bytes pass the same validator used at the receiving trust boundary.
inline Result build(Kind kind, const vision_payload::Config & config,
                    const vision_payload::Limits & limits, const vision_payload::Binding & binding,
                    Payload & out, const Prepared & prepared = {}, uint32_t error_class = 0) {
    size_t size = 32;
    const auto add = [&](size_t n) {
        if (size > limits.payload_bytes || n > limits.payload_bytes - size) return false;
        size += n; return true;
    };
    size_t fds = 0;
    uint64_t row_bytes = 0;
    switch (kind) {
        case Kind::ready: if (!add(32)) return Result::invalid; break;
        case Kind::quit: if (!add(0)) return Result::invalid; break;
        case Kind::error: if (!add(4)) return Result::invalid; break;
        case Kind::prepare:
            if (!add(44)) return Result::invalid;
            fds = 1; break;
        case Kind::prepared:
            if (prepared.prefix.size() % 4 || prepared.suffix.size() % 4 ||
                prepared.prefix.size() / 4 > limits.text_tokens ||
                prepared.suffix.size() / 4 > limits.text_tokens - prepared.prefix.size() / 4 ||
                prepared.metadata.size() > limits.metadata.bytes ||
                !add(68) || !add(prepared.prefix.size()) || !add(prepared.suffix.size()) ||
                !add(prepared.metadata.size())) return Result::invalid;
            break;
        case Kind::encode: case Kind::encoded:
            if (!add(64) || !vision_payload::rows(binding.grid_x, binding.grid_y, config.width,
                                                limits.row_bytes, row_bytes)) return Result::invalid;
            fds = kind == Kind::encode ? 1 : 0; break;
        case Kind::release: case Kind::released:
            if (!add(44)) return Result::invalid;
            break;
        default: return Result::invalid;
    }
    Payload pending;
    pending.fd_count = fds;
    try {
        pending.bytes.reserve(size);
        auto raw = [&](const void * data, size_t n) {
            if (!n) return;
            const auto * p = static_cast<const uint8_t *>(data);
            pending.bytes.insert(pending.bytes.end(), p, p + n);
        };
        auto integer = [&](uint64_t value, unsigned n) {
            for (unsigned i = 0; i < n; ++i) pending.bytes.push_back(uint8_t(value >> (8 * i)));
        };
        raw(config.generation.data(), config.generation.size());
        if (kind == Kind::ready) {
            integer(1, 4); integer(config.width, 4); integer(config.pixel_stride, 4);
            integer(config.vocabulary_size, 4); raw(config.device.data(), config.device.size());
        } else if (kind == Kind::error) integer(error_class, 4);
        else if (kind != Kind::quit) {
            integer(binding.occurrence, 4); raw(binding.source.data(), binding.source.size());
            if (kind == Kind::prepare) integer(binding.source_bytes, 8);
            else {
                integer(kind == Kind::prepared ? prepared.handle : binding.handle, 8);
                if (kind == Kind::prepared) {
                    integer(prepared.x, 4); integer(prepared.y, 4); integer(config.width, 4);
                    integer(prepared.prefix.size() / 4, 4); raw(prepared.prefix.data(), prepared.prefix.size());
                    integer(prepared.suffix.size() / 4, 4); raw(prepared.suffix.data(), prepared.suffix.size());
                    integer(prepared.metadata.size(), 4); raw(prepared.metadata.data(), prepared.metadata.size());
                } else if (kind == Kind::encode || kind == Kind::encoded) {
                    integer(binding.grid_x, 4); integer(binding.grid_y, 4); integer(config.width, 4);
                    integer(row_bytes, 8);
                }
            }
        }
    } catch (const std::bad_alloc &) { return Result::resource; }
    vision_payload::View view;
    if (pending.bytes.size() != size || !vision_payload::validate(kind,
        {reinterpret_cast<const char *>(pending.bytes.data()), pending.bytes.size()}, fds,
        config, limits, binding, view)) return Result::invalid;
    out = std::move(pending);
    return Result::ok;
}
} // namespace strata_frontend::vision_commands
