// Parent-only PR4 composition and SVE1 writing. Never decodes or encodes images.
#pragma once
#include "clean_process.hpp"
#include "picture_cache.hpp"
#include "vision_job_io.hpp"
#include "vision_engine_handoff.hpp"
#include "server-common.h"
#include "vendor/hash/hash.h"
#include <functional>
#include <stdexcept>

namespace strata_frontend::vision_composition {
inline void require(bool ok) { if (!ok) throw std::runtime_error("invalid vision composition"); }
// Plain callback is the existing recursive literal tokenizer, not parse_special=false.
// All inputs remain owned by the composing task; do this off the main queue.
inline server_tokens compose(const llama_vocab * vocab, const std::string & prompt,
    const std::vector<std::pair<size_t, size_t>> & plain, std::string_view marker,
    const vision_job_io::Prepared & prepared, const std::vector<std::string> & image_ids,
    const vision_payload::Config & config, const vision_payload::Limits & caps, uint32_t max_cells,
    const std::function<llama_tokens(const std::string &)> & literal) {
    require(vocab && uint32_t(llama_vocab_n_tokens(vocab)) == config.vocabulary_size && !marker.empty() &&
            prepared.recipes.size() == image_ids.size() &&
            image_ids.size() <= caps.occurrences && max_cells && max_cells <= uint32_t(INT32_MAX));
    server_tokens out(llama_tokens{}, !prepared.recipes.empty());
    uint64_t cells = 0, positions = 0;
    auto reserve = [&](uint64_t n, uint64_t p) {
        require(n <= max_cells - cells && p <= uint64_t(INT32_MAX) - positions); cells += n; positions += p;
    };
    auto token = [&](llama_token id) {
        require(id >= 0 && uint32_t(id) < config.vocabulary_size); reserve(1, 1); out.push_back(id);
    };
    auto run = [&](std::string_view bytes) {
        require(bytes.size() % 4 == 0);
        for (size_t i = 0; i < bytes.size(); i += 4)
            token(llama_token(vision_protocol::little(reinterpret_cast<const uint8_t *>(bytes.data() + i), 4)));
    };
    size_t next = 0;
    auto image = [&] {
        require(next < prepared.recipes.size());
        const auto & recipe = prepared.recipes[next];
        vision_metadata::Shape shape{};
        // Mandatory preflight BEFORE public load. Compare owned declarations too.
        require(vision_metadata::preflight(recipe.metadata, image_ids[next], caps.metadata, shape) &&
                shape.x == recipe.shape.x && shape.y == recipe.shape.y && shape.cells == recipe.shape.cells &&
                shape.positions == recipe.shape.positions && recipe.prefix_ids.size() / 4 <= caps.text_tokens &&
                recipe.suffix_ids.size() / 4 <= caps.text_tokens - recipe.prefix_ids.size() / 4);
        run(recipe.prefix_ids);
        reserve(shape.cells, shape.positions);
        std::unique_ptr<mtmd_input_chunk, decltype(&mtmd_input_chunk_free)> chunk(
            mtmd_input_chunk_load(recipe.metadata.data(), recipe.metadata.size()), mtmd_input_chunk_free);
        require(chunk && mtmd_input_chunk_get_type(chunk.get()) == MTMD_INPUT_CHUNK_TYPE_IMAGE &&
                mtmd_input_chunk_get_n_tokens(chunk.get()) == shape.cells &&
                mtmd_input_chunk_get_n_pos(chunk.get()) == int32_t(shape.positions) &&
                mtmd_input_chunk_get_id(chunk.get()) && image_ids[next] == mtmd_input_chunk_get_id(chunk.get()));
        auto image_tokens = mtmd_input_chunk_get_tokens_image(chunk.get()); require(image_tokens);
        for (uint32_t i = 0; i < shape.cells; ++i) {
            auto p = mtmd_image_tokens_get_decoder_pos(image_tokens, 0, i);
            require(p.t == 0 && p.x == i % shape.x && p.y == i / shape.x && p.z == 0);
        }
        out.push_back(chunk.get()); // metadata placeholder, NEVER used for encode
        run(recipe.suffix_ids); ++next;
    };
    const bool bos = llama_vocab_get_add_bos(vocab), eos = llama_vocab_get_add_eos(vocab);
    auto text = [&](const std::string & s) { for (auto id : common_tokenize(vocab, s, false, true)) token(id); };
    auto segment = [&](const std::string & s, bool add_bos, bool add_eos) {
        if (add_bos) token(llama_vocab_bos(vocab));
        size_t pos = 0;
        for (size_t at = prepared.recipes.empty() ? std::string::npos : s.find(marker);
             at != std::string::npos; at = s.find(marker, pos)) {
            text(s.substr(pos, at - pos)); image(); pos = at + marker.size();
        }
        text(s.substr(pos));
        if (add_eos) token(llama_vocab_eos(vocab));
    };
    if (plain.empty()) segment(prompt, bos, eos);
    else {
        require(bool(literal));
        if (bos) token(llama_vocab_bos(vocab));
        auto special = [&](const std::string & s) {
            if (!s.empty()) segment(s, false, eos && s.find(marker) != std::string::npos);
        };
        size_t pos = 0;
        for (const auto & span : plain) {
            require(span.first >= pos && span.second >= span.first && span.second <= prompt.size());
            special(prompt.substr(pos, span.first - pos));
            for (auto id : literal(prompt.substr(span.first, span.second - span.first))) token(id);
            pos = span.second;
        }
        special(prompt.substr(pos));
    }
    require(next == image_ids.size() && out.size() == cells && out.pos_next() == int32_t(positions));
    return out;
}

// Owns private native-engine-visible storage until terminal drain/containment.
// Disk bytes must be separately reserved by the service before calling create;
// disk_cap is a per-request bound, not an aggregate service ledger.
class File final : public vision_engine_handoff::Resources {
    std::string directory_, path_;
    File() = default;
public:
    std::vector<int32_t> ids;
    std::string keys;
    const std::string & path() const { return path_; }
    ~File() { if (!path_.empty()) ::unlink(path_.c_str()); if (!directory_.empty()) ::rmdir(directory_.c_str()); }
    static std::shared_ptr<const File> create(const server_tokens & prompt, const vision_job_io::Encoded & encoded,
        int32_t image_pad, uint32_t vocabulary, uint64_t disk_cap,
        vision_protocol::Clock::time_point until, const std::atomic<bool> & cancel) {
        try {
            require(image_pad >= 0 && uint32_t(image_pad) < vocabulary && !prompt.empty() &&
                    encoded.prepared && encoded.rows.size() == encoded.prepared->recipes.size());
            uint64_t bytes = 0;
            for (const auto & row : encoded.rows) {
                require(row && row->validated_rows() && bytes <= disk_cap && 20 <= disk_cap - bytes && row->bytes() <= disk_cap - bytes - 20);
                bytes += 20 + row->bytes();
            }
            auto file = std::shared_ptr<File>(new File);
            char name[] = "/tmp/unbiased-strata-vision-XXXXXX";
            // Allocate name capacity before creating anything on disk: even a
            // string bad_alloc after mkdtemp must not orphan its directory.
            file->directory_.reserve(sizeof(name)); file->path_.reserve(sizeof(name) + 16);
            require(::mkdtemp(name)); file->directory_ = name; file->path_ = file->directory_ + "/rows.sve";
            clean_process_detail::Fd fd;
            fd.value = ::open(file->path_.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            std::vector<uint64_t> keys;
            require(fd.value >= 0);
            auto write = [&](const void * data, size_t count) {
                auto p = static_cast<const char *>(data);
                while (count) {
                    require(vision_artifact::checkpoint(until, cancel) == vision_artifact::Result::ok);
                    auto n = ::write(fd.value, p, std::min<size_t>(count, 65536));
                    if (n < 0 && errno == EINTR) continue;
                    require(n > 0); p += n; count -= size_t(n);
                }
            };
            size_t occurrence = 0;
            file->ids.reserve(prompt.size());
            for (size_t at = 0; at < prompt.size();) {
                if (prompt[at] != LLAMA_TOKEN_NULL) {
                    require(prompt[at] >= 0 && uint32_t(prompt[at]) < vocabulary); file->ids.push_back(prompt[at++]); continue;
                }
                require(occurrence < encoded.rows.size());
                const auto & row = encoded.rows[occurrence];
                const auto & recipe = encoded.prepared->recipes[occurrence];
                const auto & identity = encoded.prepared->identity;
                auto chunk = prompt.find_chunk(at).get();
                require(chunk && mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE &&
                        mtmd_input_chunk_get_n_tokens(chunk) == recipe.shape.cells &&
                        row->tag().instance == identity.instance && row->tag().job == identity.job &&
                        (encoded.prepared->source_occurrences.empty() ? row->tag().occurrence == occurrence :
                         encoded.prepared->source_occurrences.size() == encoded.rows.size() &&
                         row->tag().occurrence == encoded.prepared->source_occurrences[occurrence]) &&
                        row->bytes() == recipe.row_bytes && recipe.shape.cells <= prompt.size() - at);
                size_t metadata_bytes = 0;
                require(mtmd_input_chunk_save(chunk, nullptr, 0, &metadata_bytes) == 0 && metadata_bytes == recipe.metadata.size());
                std::string metadata(metadata_bytes, '\0');
                require(mtmd_input_chunk_save(chunk, metadata.data(), metadata.size(), &metadata_bytes) == 0 && metadata == recipe.metadata);
                for (uint32_t i = 0; i < recipe.shape.cells; ++i) require(prompt[at + i] == LLAMA_TOKEN_NULL);
                require(recipe.shape.cells && row->bytes() / recipe.shape.cells / 4 <= uint64_t(INT32_MAX));
                const int32_t header[5] = {0x31455653, int32_t(recipe.shape.cells), int32_t(recipe.shape.x), int32_t(recipe.shape.y),
                    int32_t(row->bytes() / recipe.shape.cells / 4)};
                write(header, sizeof(header));
                char buffer[65536];
                for (uint64_t offset = 0; offset < row->bytes();) {
                    auto n = ::pread(row->fd(), buffer, size_t(std::min<uint64_t>(sizeof(buffer), row->bytes() - offset)), off_t(offset));
                    if (n < 0 && errno == EINTR) continue;
                    require(n > 0); write(buffer, size_t(n)); offset += uint64_t(n);
                }
                file->ids.insert(file->ids.end(), recipe.shape.cells, image_pad);
                const char * id = mtmd_input_chunk_get_id(chunk); require(id && *id);
                const auto identity_bytes = vision_cache::identity_bytes({encoded.prepared->config,row->tag().source});
                const auto identity_hash = hash_sha256_hex(reinterpret_cast<const uint8_t *>(identity_bytes.data()),identity_bytes.size());
                keys.push_back(PictureCache::engine_key(identity_hash));
                at += recipe.shape.cells; ++occurrence;
            }
            require(occurrence == encoded.rows.size() && file->ids.size() == prompt.size() && file->ids.back() != image_pad);
            file->keys = PictureCache::hex_list(keys);
            require(::fchmod(fd.value, 0400) == 0);
            const int raw_fd = fd.value; fd.value = -1;
            require(::close(raw_fd) == 0);
            return file;
        } catch (...) { return {}; }
    }
};
} // namespace strata_frontend::vision_composition
