#pragma once

#include "server-common.h"
#include "mtmd.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Trusted internal Spike A representation, NOT a hardened IPC/file format.
// mtmd save/load preserves metadata only. Restored chunks are placeholders for
// KV/embedding-cache bookkeeping and MUST NEVER be passed to an encoder.
namespace vision_recipe {

struct Generation {
    std::string vocabulary, projector, preprocessing, family;
    bool operator==(const Generation & b) const {
        return vocabulary == b.vocabulary && projector == b.projector &&
               preprocessing == b.preprocessing && family == b.family;
    }
    bool operator!=(const Generation & b) const { return !(*this == b); }
};

struct Part {
    enum class Kind { text, image };
    Kind kind = Kind::text;
    std::vector<llama_token> text_ids;
    std::vector<char> metadata;
    std::string id;
    size_t token_count = 0;
    llama_pos position_count = 0;
    std::vector<mtmd_decoder_pos> decoder_positions; // base zero, every token
    int embedding_width = 0;
    size_t grid_x = 0, grid_y = 0;
};

struct Recipe {
    Generation generation;
    std::vector<Part> parts;
    bool has_mtmd = false;
    int embedding_width = 0;
};

namespace detail {
// Deliberately finite spike limits, not model/vocabulary validation.
constexpr size_t max_tokens = 1024 * 1024;
constexpr size_t max_metadata = 16 * 1024 * 1024;
constexpr size_t max_id = 4096;
constexpr int max_width = 65536;

inline void require(bool ok, const char * message) {
    if (!ok) throw std::runtime_error(std::string("vision recipe: ") + message);
}
inline void id(const std::string & s) {
    require(!s.empty() && s.size() <= max_id && s.find('\0') == std::string::npos, "invalid ID");
}
inline std::string id(const char * s) {
    require(s != nullptr, "null image ID");
    size_t n = 0;
    while (n <= max_id && s[n]) ++n;
    require(n > 0 && n <= max_id, "invalid image ID length");
    return std::string(s, n);
}
inline void generation(const Generation & g, int width) {
    require(g.family == "qwen-still-v1", "unsupported family");
    id(g.vocabulary); id(g.projector); id(g.preprocessing);
    require(width > 0 && width <= max_width, "invalid embedding width");
}
inline bool position_equal(const mtmd_decoder_pos & a, const mtmd_decoder_pos & b) {
    return a.t == b.t && a.x == b.x && a.y == b.y && a.z == b.z;
}
inline void counts(size_t n, llama_pos p, int width) {
    require(n > 0 && n <= max_tokens && p > 0 && size_t(p) <= max_tokens, "invalid image counts");
    require(width > 0 && width <= max_width, "invalid embedding width");
    require(n <= std::numeric_limits<size_t>::max() / size_t(width) / sizeof(float), "embedding size overflow");
}
inline void add(size_t & total, size_t n) {
    require(n <= max_tokens && total <= max_tokens - n, "total count overflow/limit");
    total += n;
}
inline std::vector<char> save(const mtmd_input_chunk * chunk) {
    size_t n = 0;
    require(mtmd_input_chunk_save(chunk, nullptr, 0, &n) == 0 && n > 0 && n <= max_metadata,
            "invalid serialized metadata size");
    std::vector<char> data(n);
    size_t actual = 0;
    require(mtmd_input_chunk_save(chunk, data.data(), data.size(), &actual) == 0 && actual == n,
            "metadata save failed");
    return data;
}

// These deprecated but public accessors are the only explicit grid accessors.
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
inline size_t grid_x(const mtmd_image_tokens * image) { return mtmd_image_tokens_get_nx(image); }
inline size_t grid_y(const mtmd_image_tokens * image) { return mtmd_image_tokens_get_ny(image); }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

inline Part image(const mtmd_input_chunk * chunk, int width) {
    require(chunk && mtmd_input_chunk_get_type(chunk) == MTMD_INPUT_CHUNK_TYPE_IMAGE, "not an image chunk");
    Part p;
    p.kind = Part::Kind::image;
    p.token_count = mtmd_input_chunk_get_n_tokens(chunk);
    p.position_count = mtmd_input_chunk_get_n_pos(chunk);
    p.embedding_width = width;
    counts(p.token_count, p.position_count, width);
    p.id = id(mtmd_input_chunk_get_id(chunk));
    const auto * image = mtmd_input_chunk_get_tokens_image(chunk);
    require(image != nullptr, "null image metadata");
    require(mtmd_image_tokens_get_n_tokens(image) == p.token_count &&
            mtmd_image_tokens_get_n_pos(image) == p.position_count &&
            id(mtmd_image_tokens_get_id(image)) == p.id, "inconsistent image metadata");
    p.grid_x = grid_x(image); p.grid_y = grid_y(image);
    require(p.grid_x > 0 && p.grid_x <= max_tokens && p.grid_y > 0 && p.grid_y <= max_tokens,
            "invalid grid");
    require(p.grid_x <= max_tokens / p.grid_y && p.grid_x * p.grid_y == p.token_count &&
            size_t(p.position_count) == (p.grid_x > p.grid_y ? p.grid_x : p.grid_y),
            "unsupported non-still Qwen grid layout");
    for (size_t i = 0; i < p.token_count; ++i) {
        auto pos = mtmd_image_tokens_get_decoder_pos(image, 0, i);
        require(pos.t == 0 && pos.x == i % p.grid_x && pos.y == i / p.grid_x && pos.z == 0,
                "unsupported non-still Qwen decoder layout");
        require(pos.t <= max_tokens && pos.x <= max_tokens && pos.y <= max_tokens && pos.z <= max_tokens,
                "decoder position out of bounds");
        p.decoder_positions.push_back(pos);
    }
    p.metadata = save(chunk);
    return p;
}
inline void equal_image(const Part & a, const Part & b) {
    require(a.id == b.id && a.token_count == b.token_count && a.position_count == b.position_count &&
            a.embedding_width == b.embedding_width && a.grid_x == b.grid_x && a.grid_y == b.grid_y &&
            a.metadata == b.metadata && a.decoder_positions.size() == b.decoder_positions.size(),
            "image metadata/grid mismatch");
    for (size_t i = 0; i < a.decoder_positions.size(); ++i)
        require(position_equal(a.decoder_positions[i], b.decoder_positions[i]), "decoder position mismatch");
}
} // namespace detail

inline Recipe capture(const server_tokens & tokens, const Generation & generation, int width) {
    detail::generation(generation, width);
    detail::require(tokens.size() <= detail::max_tokens, "too many tokens");
    Recipe recipe;
    recipe.generation = generation;
    recipe.has_mtmd = tokens.has_mtmd;
    recipe.embedding_width = width;
    size_t positions = 0;
    for (size_t i = 0; i < tokens.size();) {
        if (tokens[i] != LLAMA_TOKEN_NULL) {
            Part p;
            do {
                detail::require(tokens[i] >= 0, "invalid text token ID");
                p.text_ids.push_back(tokens[i++]);
            } while (i < tokens.size() && tokens[i] != LLAMA_TOKEN_NULL);
            detail::add(positions, p.text_ids.size());
            recipe.parts.push_back(std::move(p));
        } else {
            detail::require(tokens.has_mtmd, "image without has_mtmd");
            const auto & chunk = tokens.find_chunk(i);
            Part p = detail::image(chunk.get(), width);
            detail::require(p.token_count <= tokens.size() - i, "image extends past tokens");
            const auto next = tokens.find_next_media_chunk(i);
            detail::require(!next.first || next.second >= i + p.token_count, "overlapping images");
            for (size_t j = 0; j < p.token_count; ++j)
                detail::require(tokens[i + j] == LLAMA_TOKEN_NULL, "non-placeholder image token");
            detail::add(positions, size_t(p.position_count));
            i += p.token_count;
            recipe.parts.push_back(std::move(p));
        }
    }
    return recipe;
}

inline server_tokens restore(const Recipe & recipe, const Generation & generation, int width) {
    detail::generation(generation, width);
    detail::require(recipe.generation == generation, "generation mismatch");
    detail::require(recipe.embedding_width == width, "recipe width mismatch");
    detail::require(recipe.parts.size() <= detail::max_tokens, "too many parts");
    server_tokens tokens;
    tokens.has_mtmd = recipe.has_mtmd;
    size_t total = 0, positions = 0;
    bool previous_text = false;
    for (const auto & p : recipe.parts) {
        if (p.kind == Part::Kind::text) {
            detail::require(!previous_text && !p.text_ids.empty(), "non-maximal/empty text run");
            detail::require(p.metadata.empty() && p.id.empty() && p.token_count == 0 && p.position_count == 0 &&
                            p.decoder_positions.empty() && p.embedding_width == 0 && p.grid_x == 0 && p.grid_y == 0,
                            "image fields in text part");
            detail::add(total, p.text_ids.size()); detail::add(positions, p.text_ids.size());
            for (auto t : p.text_ids) {
                detail::require(t != LLAMA_TOKEN_NULL && t >= 0, "invalid text token ID");
                tokens.push_back(t);
            }
            previous_text = true;
        } else {
            detail::require(p.kind == Part::Kind::image && tokens.has_mtmd && p.text_ids.empty(), "invalid image part");
            detail::counts(p.token_count, p.position_count, width);
            detail::id(p.id);
            detail::require(p.embedding_width == width && p.decoder_positions.size() == p.token_count,
                            "image width/position count mismatch");
            detail::require(!p.metadata.empty() && p.metadata.size() <= detail::max_metadata, "invalid metadata size");
            detail::add(total, p.token_count); detail::add(positions, size_t(p.position_count));
            std::unique_ptr<mtmd_input_chunk, decltype(&mtmd_input_chunk_free)> chunk(
                mtmd_input_chunk_load(p.metadata.data(), p.metadata.size()), mtmd_input_chunk_free);
            detail::require(bool(chunk), "metadata load failed");
            detail::equal_image(p, detail::image(chunk.get(), width));
            tokens.push_back(chunk.get()); // copies metadata placeholder, NEVER encodes
            previous_text = false;
        }
    }
    return tokens;
}

inline void compare(const server_tokens & a, const server_tokens & b) {
    detail::require(a.has_mtmd == b.has_mtmd && a.size() == b.size(), "token shape mismatch");
    // Capture also validates chunk extents, IDs, and counts before prefix queries.
    const Generation g{"parity", "parity", "parity", "qwen-still-v1"};
    const auto ra = capture(a, g, 1), rb = capture(b, g, 1);
    detail::require(ra.parts.size() == rb.parts.size(), "chunk layout mismatch");
    size_t start = 0;
    for (size_t i = 0; i < ra.parts.size(); ++i) {
        const auto & pa = ra.parts[i]; const auto & pb = rb.parts[i];
        detail::require(pa.kind == pb.kind && pa.text_ids == pb.text_ids, "chunk type/text mismatch");
        if (pa.kind == Part::Kind::image) {
            detail::equal_image(pa, pb);
            const auto * ia = mtmd_input_chunk_get_tokens_image(a.find_chunk(start).get());
            const auto * ib = mtmd_input_chunk_get_tokens_image(b.find_chunk(start).get());
            for (llama_pos base : {llama_pos(0), llama_pos(17), a.pos_next(static_cast<int64_t>(start))})
                for (size_t j = 0; j < pa.token_count; ++j)
                    detail::require(detail::position_equal(mtmd_image_tokens_get_decoder_pos(ia, base, j),
                                                          mtmd_image_tokens_get_decoder_pos(ib, base, j)),
                                    "decoder getter parity mismatch");
            start += pa.token_count;
        } else start += pa.text_ids.size();
    }
    for (size_t i = 0; i < a.size(); ++i) detail::require(a[i] == b[i], "token ID mismatch");
    detail::require(a.pos_next() == b.pos_next(), "final position mismatch");
    for (size_t i = 0; i <= a.size(); ++i) {
        const auto p = a.pos_next(static_cast<int64_t>(i));
        detail::require(p == b.pos_next(static_cast<int64_t>(i)), "prefix position mismatch");
        for (int delta : {-1, 0, 1}) {
            const int64_t boundary = int64_t(p) + delta;
            if (boundary < 0 || boundary > std::numeric_limits<llama_pos>::max()) continue;
            detail::require(a.size_up_to_pos(llama_pos(boundary)) == b.size_up_to_pos(llama_pos(boundary)),
                            "position boundary mismatch");
        }
    }
}

} // namespace vision_recipe
