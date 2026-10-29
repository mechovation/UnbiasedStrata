// PR4 header-only framing foundation. Payload/FD transport is not implemented here.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace strata_frontend::vision_protocol {
constexpr size_t header_size = 40;
enum class Kind : uint16_t { ready = 1, prepare, prepared, encode, encoded, release, released, quit, error };
struct Header {
    Kind kind;
    uint32_t payload_size;
    uint64_t instance, job, sequence;
};
inline uint64_t little(const uint8_t * p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
// Requires exactly one complete header, before allocating a body. Output untouched on rejection.
inline bool parse_header(const uint8_t * bytes, size_t size, uint32_t payload_cap, Header & out) {
    if (size != header_size || !bytes) return false;
    if (bytes[0] != 'S' || bytes[1] != 'V' || bytes[2] != 'R' || bytes[3] != '1' ||
        little(bytes + 4, 2) != 1 || little(bytes + 12, 4) != 0) return false;
    const auto kind = little(bytes + 6, 2);
    const auto payload = little(bytes + 8, 4);
    Header h{static_cast<Kind>(kind), static_cast<uint32_t>(payload),
             little(bytes + 16, 8), little(bytes + 24, 8), little(bytes + 32, 8)};
    if (kind < 1 || kind > 9 || payload > payload_cap || !h.instance) return false;
    if (h.kind == Kind::ready || h.kind == Kind::quit) {
        if (h.job || h.sequence) return false;
    } else if (h.kind == Kind::error) {
        if ((h.job == 0) != (h.sequence == 0)) return false;
    } else if (!h.job || !h.sequence) return false;
    out = h;
    return true;
}
inline std::array<uint8_t, header_size> encode_header(const Header & h) {
    std::array<uint8_t, header_size> bytes{};
    bytes[0] = 'S'; bytes[1] = 'V'; bytes[2] = 'R'; bytes[3] = '1';
    auto put = [&](size_t offset, size_t n, uint64_t value) {
        for (size_t i = 0; i < n; ++i) bytes[offset + i] = uint8_t(value >> (8 * i));
    };
    put(4, 2, 1); put(6, 2, uint16_t(h.kind)); put(8, 4, h.payload_size);
    put(16, 8, h.instance); put(24, 8, h.job); put(32, 8, h.sequence);
    return bytes;
}
inline bool matches(const Header & reply, Kind expected, uint64_t instance, uint64_t job, uint64_t sequence) {
    return (reply.kind == expected || reply.kind == Kind::error) && reply.instance == instance &&
           reply.job == job && reply.sequence == sequence;
}
} // namespace strata_frontend::vision_protocol
