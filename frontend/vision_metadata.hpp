// Pin-specific preflight for mtmd v2 Qwen3-VL still-image metadata.
// Never constructs serialization and never calls the mtmd loader. No allocations.
// Supported pin: 44072a19fb94d0bed63d381d42f6237373f91d54, same-host ABI only.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace strata_frontend::vision_metadata {
struct Limits {
    size_t bytes, id_bytes;
    uint32_t grid_cells, pixel_dimension, pixel_stride;
    uint64_t pixels;
};
struct Shape { uint32_t x, y, cells, positions; };
inline bool grid_shape(uint32_t x, uint32_t y, const Limits & limits, Shape & out) {
    const uint64_t cells = uint64_t(x) * y;
    const uint64_t px = uint64_t(x) * limits.pixel_stride, py = uint64_t(y) * limits.pixel_stride;
    if (!x || !y || !limits.pixel_stride || cells > limits.grid_cells || cells > uint64_t(INT32_MAX) ||
        px > uint64_t(INT32_MAX) || py > uint64_t(INT32_MAX) ||
        px > limits.pixel_dimension || py > limits.pixel_dimension || px * py > limits.pixels) return false;
    out = {x, y, uint32_t(cells), std::max(x, y)};
    return true;
}
// Host-endian memcpy matches the pinned serializer. No reinterpret casts/alignment assumptions.
class Cursor {
public:
    explicit Cursor(std::string_view bytes) : bytes_(bytes) {}
    template<class T> bool take(T & value) {
        if (sizeof(T) > bytes_.size()) return false;
        std::memcpy(&value, bytes_.data(), sizeof(T)); bytes_.remove_prefix(sizeof(T));
        return true;
    }
    bool equal(uint64_t count, std::string_view expected) {
        if (count != expected.size() || count > bytes_.size() || bytes_.substr(0, size_t(count)) != expected) return false;
        bytes_.remove_prefix(size_t(count)); return true;
    }
    bool empty() const { return bytes_.empty(); }
private:
    std::string_view bytes_;
};
inline bool preflight(std::string_view blob, std::string_view expected_id, const Limits & limits, Shape & out) {
    if (blob.empty() || blob.size() > limits.bytes || expected_id.empty() ||
        expected_id.size() > limits.id_bytes || expected_id.find('\0') != std::string_view::npos ||
        !limits.grid_cells || !limits.pixel_stride || !limits.pixel_dimension || !limits.pixels) return false;
    Cursor c(blob);
    uint64_t version, texts, id_size, batches;
    uint32_t type, x, y, position, image_index, temporal;
    uint8_t image, audio_batch, view_separator, newline, audio;
    int32_t pad, pixel_x, pixel_y;
    if (!c.take(version) || version != 2 || !c.take(type) || type != 1 ||
        !c.take(texts) || texts != 0 || !c.take(image) || image != 1 ||
        !c.take(x) || !c.take(y) || !c.take(position) || position != 1 ||
        !c.take(image_index) || image_index != 0 || !c.take(temporal) || temporal != 2 ||
        !c.take(id_size) || !c.equal(id_size, expected_id) ||
        !c.take(audio_batch) || audio_batch != 0 || !c.take(batches) || batches != 1 ||
        !c.take(view_separator) || view_separator != 0 || !c.take(newline) || newline != 0 ||
        !c.take(pad) || pad != 0 || !c.take(pixel_x) || !c.take(pixel_y) ||
        !c.take(audio) || audio != 0 || !c.empty()) return false;
    // Public mtmd getters use u32 products and signed llama_pos. Keep both safe.
    Shape pending{};
    if (!grid_shape(x, y, limits, pending) || pixel_x <= 0 || pixel_y <= 0 ||
        uint64_t(x) * limits.pixel_stride != uint32_t(pixel_x) ||
        uint64_t(y) * limits.pixel_stride != uint32_t(pixel_y)) return false;
    out = pending;
    return true;
}
} // namespace strata_frontend::vision_metadata
