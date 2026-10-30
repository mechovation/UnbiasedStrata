#include "vision_reader.hpp"
#include "vision_artifact.hpp"
#include "vision_commands.hpp"
#include "vision_transport.hpp"
#include "vision_image.hpp"
#include "mtmd.h"
#include "vendor/hash/hash.h"
#include "vendor/stb/stb_image.h" // declarations; implementation is in pinned libmtmd
#include <memory>
#include <algorithm>
#include <string>
#include <vector>

namespace strata_frontend::vision_reader {
namespace vp = vision_protocol;
using Kind = vp::Kind;
std::string source_id(const vision_payload::Digest & digest) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string id; id.reserve(64);
    for (auto b : digest) { id += hex[b >> 4]; id += hex[b & 15]; }
    return id;
}
namespace {
using Chunks = std::unique_ptr<mtmd_input_chunks, decltype(&mtmd_input_chunks_free)>;
struct Record {
    vision_payload::Digest source{};
    uint32_t occurrence = 0;
    std::string id;
    uint64_t bytes = 0, handle = 0, rgb = 0, recipe_bytes = 0;
    Chunks chunks{nullptr, mtmd_input_chunks_free};
    const mtmd_input_chunk * image = nullptr;
    vision_metadata::Shape shape{};
    bool encoded = false, released = false;
};
class Reader {
    mtmd_context * ctx_;
    const vision_payload::Config & config_;
    const vision_payload::Limits & caps_;
    Limits limits_;
    uint64_t instance_, job_ = 0, sequence_ = 0, next_handle_ = 0, rgb_ = 0, recipes_ = 0;
    bool encoding_ = false, releasing_ = false;
    uint32_t next_encode_ = 0, prepare_error_ = 1, last_prepare_ = UINT32_MAX;
    vision_payload::InputError input_error_ = vision_payload::InputError::none;
    vp::Channel channel_;
    const std::atomic<bool> never_{false};
    std::vector<Record> records_;
    vp::Clock::time_point until() const {
        auto now = vp::Clock::now();
        return limits_.io > vp::Clock::time_point::max() - now ? vp::Clock::time_point::max() : now + limits_.io;
    }
    bool send(Kind kind, uint64_t job, uint64_t sequence, const vision_payload::Binding & binding = {},
              const vision_commands::Prepared & prepared = {}, uint32_t error = 0,
              vision_payload::InputError input_error = vision_payload::InputError::none) {
        vision_commands::Payload payload;
        if (vision_commands::build(kind, config_, caps_, binding, payload, prepared, error, input_error) != vision_commands::Result::ok) return false;
        return channel_.send_borrowed({kind, uint32_t(payload.bytes.size()), instance_, job, sequence},
            payload.bytes.data(), payload.bytes.size(), nullptr, 0, caps_.payload_bytes, until(), never_) == vp::IoResult::ok;
    }
    bool prepare(Record & record, int fd, vision_commands::Prepared & prepared,
                 std::string & prefix, std::string & suffix, std::string & metadata) {
        prepare_error_ = 4; input_error_ = vision_payload::InputError::none;
        if (vision_artifact::sealed_size(fd, record.bytes, caps_.source_bytes) != vision_artifact::Result::ok) return false;
        std::vector<unsigned char> source(size_t(record.bytes));
        for (size_t at = 0; at < source.size();) {
            auto n = ::pread(fd, source.data() + at, std::min<size_t>(65536, source.size() - at), off_t(at));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            at += size_t(n);
        }
        if (hash_sha256_hex(source.data(), source.size()) != record.id) return false;
        if (rgb_ > limits_.retained_rgb_bytes || limits_.source_pixels > UINT64_MAX / vision_image::workspace_per_pixel) return false;
        auto image = vision_image::decode(source.data(), source.size(), {limits_.source_dimension, limits_.source_pixels,
            limits_.retained_rgb_bytes - rgb_, limits_.source_pixels * vision_image::workspace_per_pixel});
        if (image.status == vision_image::Status::invalid_webp_header)
            input_error_ = vision_payload::InputError::webp_header;
        prepare_error_ = image.status == vision_image::Status::resource ? 2 :
            image.status == vision_image::Status::unreadable ? 1 : 3;
        if (image.status != vision_image::Status::ok) return false;
        record.rgb = image.rgb.size();
        std::unique_ptr<mtmd_bitmap, decltype(&mtmd_bitmap_free)> bitmap(
            mtmd_bitmap_init(int(image.width), int(image.height), image.rgb.data()), mtmd_bitmap_free);
        if (!bitmap) return false;
        std::vector<unsigned char>().swap(image.rgb);
        mtmd_bitmap_set_id(bitmap.get(), record.id.c_str());
        const mtmd_bitmap * images[] = {bitmap.get()};
        record.chunks.reset(mtmd_input_chunks_init());
        if (!record.chunks) return false;
        prepare_error_ = 3;
        const char * marker = mtmd_get_marker(ctx_);
        mtmd_input_text text{marker, std::strlen(marker), false, true};
        if (mtmd_tokenize(ctx_, record.chunks.get(), &text, images, 1) != 0) return false;
        prepare_error_ = 1;
        uint32_t text_left = caps_.text_tokens;
        for (size_t i = 0; i < mtmd_input_chunks_size(record.chunks.get()); ++i) {
            auto chunk = mtmd_input_chunks_get(record.chunks.get(), i);
            if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
                if (record.image) return false;
                record.image = chunk;
            } else if (mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_TEXT) {
                size_t n = 0; auto ids = mtmd_input_chunk_get_tokens_text(chunk, &n);
                if (n > text_left) return false;
                text_left -= uint32_t(n);
                auto & run = record.image ? suffix : prefix;
                for (size_t j = 0; j < n; ++j) {
                    if (ids[j] < 0 || uint32_t(ids[j]) >= config_.vocabulary_size) return false;
                    for (unsigned b = 0; b < 4; ++b) run += char(uint32_t(ids[j]) >> (8 * b));
                }
            } else return false;
        }
        if (!record.image) return false;
        size_t bytes = 0;
        if (mtmd_input_chunk_save(record.image, nullptr, 0, &bytes) != 0 || !bytes || bytes > caps_.metadata.bytes) return false;
        // Bound aggregate logical recipe bytes before serialization allocation.
        prepare_error_ = 2;
        uint64_t left = limits_.recipe_bytes - recipes_;
        for (size_t n : {prefix.size(), suffix.size(), bytes}) { if (n > left) return false; left -= n; }
        record.recipe_bytes = limits_.recipe_bytes - recipes_ - left;
        metadata.resize(bytes);
        if (mtmd_input_chunk_save(record.image, metadata.data(), bytes, &bytes) != 0 || bytes != metadata.size() ||
            !vision_metadata::preflight(metadata, record.id, caps_.metadata, record.shape) ||
            mtmd_input_chunk_get_n_tokens(record.image) != record.shape.cells ||
            mtmd_input_chunk_get_n_pos(record.image) != int32_t(record.shape.positions)) return false;
        prepared = {record.handle, record.shape.x, record.shape.y, prefix, suffix, metadata};
        return true;
    }
    bool encode(Record & record, int fd) {
        uint64_t bytes = 0;
        if (record.encoded || !record.image || !vision_payload::rows(record.shape.x, record.shape.y, config_.width, caps_.row_bytes, bytes)) return false;
        struct stat st{};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || uint64_t(st.st_size) != bytes) return false;
        const int seals = ::fcntl(fd, F_GET_SEALS);
        const int required = F_SEAL_GROW | F_SEAL_SHRINK;
        if (seals < 0 || (seals & required) != required || (seals & (F_SEAL_WRITE | F_SEAL_SEAL))) return false;
        if (mtmd_encode_chunk(ctx_, record.image) != 0) return false;
        auto * rows = reinterpret_cast<const unsigned char *>(mtmd_get_output_embd(ctx_));
        if (!rows) return false;
        // No writable mmap survives the reply; parent seals and scans the output.
        for (uint64_t at = 0; at < bytes;) {
            auto n = ::pwrite(fd, rows + at, size_t(std::min<uint64_t>(65536, bytes - at)), off_t(at));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            at += uint64_t(n);
        }
        record.encoded = true; return true;
    }
public:
    Reader(mtmd_context * ctx, const vision_payload::Config & config, const vision_payload::Limits & caps,
           Limits limits, uint64_t instance, int socket)
        : ctx_(ctx), config_(config), caps_(caps), limits_(limits), instance_(instance), channel_(socket) {}
    int run() {
        if (!ctx_ || !instance_ || !limits_.source_dimension || !limits_.source_pixels || !limits_.retained_rgb_bytes ||
            !limits_.recipe_bytes || limits_.io <= vp::Clock::duration::zero() || caps_.metadata.id_bytes < 64 ||
            caps_.source_bytes > 10 * 1024 * 1024 || !mtmd_support_vision(ctx_) || !mtmd_decode_use_mrope(ctx_) ||
            !send(Kind::ready, 0, 0)) return 1;
        records_.reserve(caps_.occurrences);
        for (;;) {
            vp::Frame frame;
            if (channel_.receive_command(frame, instance_, caps_.payload_bytes, vp::Clock::time_point::max(), never_) != vp::IoResult::ok) return 1;
            const auto & h = frame.header;
            auto payload = std::string_view(reinterpret_cast<const char *>(frame.payload.data()), frame.payload.size());
            vision_payload::View view;
            if (h.kind == Kind::quit) return vision_payload::validate(h.kind, payload, frame.fd_count, config_, caps_, {}, view) ? 0 : 1;
            if (h.job != job_) {
                if (h.kind != Kind::prepare || h.job <= job_) return 1;
                for (const auto & record : records_) if (!record.released) return 1;
                records_.clear(); job_ = h.job; sequence_ = 0; encoding_ = releasing_ = false; next_encode_ = 0; last_prepare_ = UINT32_MAX;
            }
            if (sequence_ == UINT64_MAX || h.sequence != sequence_ + 1) return 1;
            sequence_ = h.sequence;
            // Extract only the fixed prefix to locate a record; the complete
            // schema is still validated against owned registry data below.
            vision_payload::Cursor cursor(payload);
            uint32_t occurrence = 0;
            vision_payload::Digest digest{};
            std::string_view raw;
            if (!cursor.equal(config_.generation) || !cursor.u32(occurrence) || !cursor.bytes(32, raw)) return 1;
            std::memcpy(digest.data(), raw.data(), digest.size());
            if (h.kind == Kind::prepare) {
                Record record; record.source = digest; record.id = source_id(digest); record.occurrence = occurrence;
                // PR5 complete hits/duplicates do not create reader handles.
                // Actual PREPAREs remain strictly ordered, but may be sparse.
                if (encoding_ || releasing_ || (last_prepare_ != UINT32_MAX && occurrence <= last_prepare_) || occurrence >= caps_.occurrences ||
                    !cursor.u64(record.bytes) || next_handle_ == UINT64_MAX) return 1;
                record.handle = ++next_handle_;
                vision_payload::Binding binding{occurrence, digest, record.id, record.bytes, 0, 0, 0};
                if (!vision_payload::validate(h.kind, payload, frame.fd_count, config_, caps_, binding, view)) return 1;
                last_prepare_ = occurrence;
                std::string prefix, suffix, metadata;
                vision_commands::Prepared prepared;
                if (!prepare(record, frame.fds[0], prepared, prefix, suffix, metadata)) {
                    if (input_error_ != vision_payload::InputError::none) {
                        // Header-only typed rejection has created no bitmap/chunks/GPU work.
                        // No handle for this occurrence. Parent may omit a tool
                        // occurrence or finalize; earlier handles remain owned.
                        if (!send(Kind::input_rejected, job_, sequence_, binding, {}, 0, input_error_)) return 1;
                        continue;
                    }
                    (void)send(Kind::error, job_, sequence_, {}, {}, prepare_error_); return 1;
                }
                rgb_ += record.rgb; recipes_ += record.recipe_bytes;
                if (!send(Kind::prepared, job_, sequence_, binding, prepared)) return 1;
                records_.push_back(std::move(record));
            } else {
                const auto found = std::find_if(records_.begin(),records_.end(),
                    [&](const Record & r) { return r.occurrence == occurrence; });
                if (found == records_.end()) return 1;
                auto & record = *found;
                vision_payload::Binding binding{occurrence, record.source, record.id, record.bytes, record.handle, record.shape.x, record.shape.y};
                if (!vision_payload::validate(h.kind, payload, frame.fd_count, config_, caps_, binding, view)) return 1;
                if (h.kind == Kind::encode) {
                    encoding_ = true;
                    if (releasing_ || occurrence < next_encode_ || record.released || !encode(record, frame.fds[0])) {
                        (void)send(Kind::error, job_, sequence_, {}, {}, 3); return 1;
                    }
                    next_encode_ = occurrence + 1; // earlier admitted occurrences may be late-cache hits
                    if (!send(Kind::encoded, job_, sequence_, binding)) return 1;
                } else if (h.kind == Kind::release) {
                    releasing_ = true;
                    if (!record.released) {
                        record.chunks.reset(); record.image = nullptr; record.released = true;
                        rgb_ -= record.rgb; recipes_ -= record.recipe_bytes;
                    }
                    if (!send(Kind::released, job_, sequence_, binding)) return 1;
                } else return 1;
            }
        }
    }
};
} // namespace
int serve(mtmd_context * ctx, const vision_payload::Config & config, const vision_payload::Limits & caps,
          Limits limits, uint64_t instance, int socket) {
    try { return Reader(ctx, config, caps, limits, instance, socket).run(); }
    catch (...) { return 1; } // native abort/blocked work is contained by the parent
}
} // namespace strata_frontend::vision_reader
