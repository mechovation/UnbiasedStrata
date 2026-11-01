#include "../vision_payload.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace vp = strata_frontend::vision_payload;
namespace vm = strata_frontend::vision_metadata;
using strata_frontend::vision_protocol::Kind;
static void u32(std::string & s, uint32_t n) { for (unsigned i = 0; i < 4; ++i) s += char(n >> (8 * i)); }
static void u64(std::string & s, uint64_t n) { for (unsigned i = 0; i < 8; ++i) s += char(n >> (8 * i)); }
template<size_t N> static void bytes(std::string & s, const std::array<uint8_t, N> & b) { s.append(reinterpret_cast<const char *>(b.data()), N); }
// Mutation only; accepted metadata always comes from the real public mtmd serializer fixtures.
static void corrupt(std::string & s, size_t offset, size_t width, uint64_t value) {
    assert(offset + width <= s.size());
    for (size_t i = 0; i < width; ++i) s[offset + i] = char(value >> (8 * i));
}
int main() {
    vp::Config config{};
    config.generation.fill(17); config.device.fill(18);
    config.width = 8; config.pixel_stride = 32; config.vocabulary_size = 1024;
    vp::Limits limits{16384, 8, 16, 10 * 1024 * 1024, 1024 * 1024, {4096, 64, 4096, 16384, 32, 16 * 1024 * 1024}};
    vp::Binding binding{2, {}, "pr4-fixture", 512, 99, 7, 7}; binding.source.fill(19);
    size_t negatives = 0;
    auto reject = [&](Kind kind, const std::string & p, size_t fd_count = 0) {
        vp::View unchanged; unchanged.handle = 1234;
        assert(!vp::validate(kind, p, fd_count, config, limits, binding, unchanged));
        assert(unchanged.handle == 1234); ++negatives;
    };
    auto common = [&] { std::string p; bytes(p, config.generation); return p; };
    auto occurrence = [&] { auto p = common(); u32(p, binding.occurrence); bytes(p, binding.source); return p; };
    std::vector<std::pair<Kind, std::string>> valid;
    auto ready = common(); u32(ready, 1); u32(ready, config.width); u32(ready, config.pixel_stride);
    u32(ready, config.vocabulary_size); bytes(ready, config.device); valid.push_back({Kind::ready, ready});
    valid.push_back({Kind::quit, common()});
    auto error = common(); u32(error, 3); valid.push_back({Kind::error, error});
    for (Kind kind : {Kind::release, Kind::released}) {
        auto p = occurrence(); u64(p, binding.handle); valid.push_back({kind, p});
    }
    auto prepare = occurrence(); u64(prepare, binding.source_bytes); valid.push_back({Kind::prepare, prepare});
    auto rejected = prepare; u32(rejected,uint32_t(vp::InputError::webp_header));
    valid.push_back({Kind::input_rejected,rejected});
    for (uint32_t reason : {0u,2u,UINT32_MAX}) {
        auto bad = rejected; corrupt(bad,76,4,reason); reject(Kind::input_rejected,bad);
    }
    for (auto mutation : {std::array<uint64_t,3>{32,4,3}, {36,1,0}, {68,8,511}}) {
        auto bad = rejected; corrupt(bad,mutation[0],mutation[1],mutation[2]); reject(Kind::input_rejected,bad);
    }
    for (Kind kind : {Kind::encode, Kind::encoded}) {
        auto p = occurrence(); u64(p, binding.handle); u32(p, 7); u32(p, 7); u32(p, config.width);
        u64(p, 7 * 7 * config.width * 4); valid.push_back({kind, p});
    }
    for (unsigned fixture = 0; fixture < 10; ++fixture) {
        std::ifstream file("frontend/tests/data/mtmd-metadata/qwen-still-" + std::to_string(fixture) + ".bin", std::ios::binary);
        assert(file); std::string blob((std::istreambuf_iterator<char>(file)), {});
        vm::Shape shape{};
        assert(vm::preflight(blob, binding.metadata_id, limits.metadata, shape));
        for (size_t n = 0; n < blob.size(); ++n) {
            vm::Shape unchanged{1, 2, 3, 4};
            assert(!vm::preflight(std::string_view(blob).substr(0, n), binding.metadata_id, limits.metadata, unchanged));
            assert(unchanged.x == 1 && unchanged.cells == 3); ++negatives;
        }
        assert(!vm::preflight(blob + "x", binding.metadata_id, limits.metadata, shape)); ++negatives;
        assert(!vm::preflight(blob, "wrong-id", limits.metadata, shape)); ++negatives;
        // v2 offsets for the 11-byte real fixture ID: nested lengths, types, canonical flags, dimensions.
        for (auto mutation : {std::array<uint64_t,3>{0,8,3}, {8,4,0}, {12,8,UINT64_MAX}, {20,1,2},
                {21,4,UINT32_MAX}, {25,4,0}, {29,4,0}, {33,4,1}, {37,4,0}, {37,4,1},
                {41,8,UINT64_MAX}, {60,1,1}, {61,8,UINT64_MAX}, {61,8,2}, {69,1,1}, {70,1,1},
                {71,4,1}, {75,4,UINT32_MAX}, {79,4,0}, {83,1,1}}) {
            auto bad = blob; corrupt(bad, mutation[0], mutation[1], mutation[2]);
            assert(!vm::preflight(bad, binding.metadata_id, limits.metadata, shape)); ++negatives;
        }
        auto cap = limits.metadata; cap.bytes = blob.size() - 1;
        assert(!vm::preflight(blob, binding.metadata_id, cap, shape)); ++negatives;
        cap = limits.metadata; cap.pixels = 1;
        assert(!vm::preflight(blob, binding.metadata_id, cap, shape)); ++negatives;
        cap = limits.metadata; cap.grid_cells = 1;
        assert(!vm::preflight(blob, binding.metadata_id, cap, shape)); ++negatives;
        cap = limits.metadata; cap.pixel_stride = 16;
        assert(!vm::preflight(blob, binding.metadata_id, cap, shape)); ++negatives;
        assert(vm::preflight(blob, binding.metadata_id, limits.metadata, shape));
        auto prepared = occurrence(); u64(prepared, 99); u32(prepared, shape.x); u32(prepared, shape.y); u32(prepared, config.width);
        u32(prepared, 2); u32(prepared, 0); u32(prepared, 42); // synthetic valid vocabulary IDs, not claimed wrapper parity
        u32(prepared, 1); u32(prepared, 1023); u32(prepared, blob.size()); prepared += blob;
        valid.push_back({Kind::prepared, prepared});
        auto bad = prepared; corrupt(bad, 88, 4, UINT32_MAX); reject(Kind::prepared, bad); // prefix count
        bad = prepared; corrupt(bad, 92, 4, UINT32_MAX); reject(Kind::prepared, bad); // sentinel
        bad = prepared; corrupt(bad, 92, 4, config.vocabulary_size); reject(Kind::prepared, bad);
        bad = prepared; corrupt(bad, 108, 4, UINT32_MAX); reject(Kind::prepared, bad); // metadata length
        bad = prepared; corrupt(bad, 76, 4, shape.x + 1); reject(Kind::prepared, bad); // declared/metadata shape mismatch
        auto small = limits; small.text_tokens = 2;
        vp::View out;
        assert(!vp::validate(Kind::prepared, prepared, 0, config, small, binding, out)); ++negatives;
    }
    for (const auto & item : valid) {
        const size_t fds = item.first == Kind::prepare || item.first == Kind::encode ? 1 : 0;
        vp::View out;
        assert(vp::validate(item.first, item.second, fds, config, limits, binding, out));
        reject(item.first, item.second, fds ? 0 : 1);
        for (size_t n = 0; n < item.second.size(); ++n) reject(item.first, item.second.substr(0, n), fds);
        reject(item.first, item.second + "trailing", fds);
        auto bad = item.second; bad[0] ^= 1; reject(item.first, bad, fds);
    }
    auto bad = ready; bad.back() ^= 1; reject(Kind::ready, bad); // wrong device
    bad = prepare; corrupt(bad, 32, 4, 3); reject(Kind::prepare, bad, 1);
    bad = prepare; bad[36] ^= 1; reject(Kind::prepare, bad, 1);
    bad = prepare; corrupt(bad, 68, 8, 511); reject(Kind::prepare, bad, 1);
    bad = error; corrupt(bad, 32, 4, 0); reject(Kind::error, bad);
    reject(static_cast<Kind>(65535), prepare);
    uint64_t result = 0;
    assert(!vp::rows(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT64_MAX, result));
    assert(!vp::rows(INT32_MAX, 1, UINT32_MAX, UINT64_MAX, result));
    vm::Shape unchanged{1, 2, 3, 4};
    auto hostile_limits = limits.metadata; hostile_limits.pixel_stride = UINT32_MAX;
    assert(!vm::grid_shape(4096, 1, hostile_limits, unchanged));
    assert(unchanged.x == 1);
    for (const auto & item : valid) if (item.first == Kind::encode || item.first == Kind::encoded) {
        auto tiny = limits; tiny.metadata.pixels = 1;
        vp::View out;
        assert(!vp::validate(item.first, item.second, item.first == Kind::encode ? 1 : 0, config, tiny, binding, out));
        ++negatives;
    }
    assert(!vp::rows(7, 7, 8, 1567, result));
    assert(vp::rows(7, 7, 8, 1568, result) && result == 1568);
    std::cout << "vision payload/metadata: PASS real_fixtures=10 rejected=" << negatives << '\n';
}
