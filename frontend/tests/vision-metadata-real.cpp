// CPU-only real-tokenizer qualification. No encoder, engine, HTTP, or fabricated metadata.
#include "../vision_metadata.hpp"
#include "../vision_payload.hpp"
#include "mtmd.h"
#include "ggml-backend.h"
#include "gguf.h"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

namespace vm = strata_frontend::vision_metadata;
namespace vp = strata_frontend::vision_payload;
static void u32(std::string & s, uint32_t n) { for (unsigned i = 0; i < 4; ++i) s += char(n >> (i * 8)); }
static void u64(std::string & s, uint64_t n) { for (unsigned i = 0; i < 8; ++i) s += char(n >> (i * 8)); }
extern "C" int32_t __wrap_mtmd_encode(mtmd_context *, const mtmd_image_tokens *) { std::abort(); }
extern "C" int32_t __wrap_mtmd_encode_chunk(mtmd_context *, const mtmd_input_chunk *) { std::abort(); }
extern "C" int32_t __wrap_mtmd_batch_encode(mtmd_batch *) { std::abort(); }
int main(int argc, char ** argv) {
    assert(argc == 4); // model, projector, output directory (existing)
    assert(!ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU));
    gguf_context * gg = gguf_init_from_file(argv[2], {true, nullptr}); assert(gg);
    auto family = gguf_find_key(gg, "clip.projector_type");
    auto patch = gguf_find_key(gg, "clip.vision.patch_size");
    assert(family >= 0 && patch >= 0);
    assert(std::string(gguf_get_val_str(gg, family)) == "qwen3vl_merger");
    auto merge_key = gguf_find_key(gg, "clip.vision.spatial_merge_size");
    uint32_t merge = merge_key < 0 ? 2 : gguf_get_val_u32(gg, merge_key);
    uint32_t patch_size = gguf_get_val_u32(gg, patch);
    assert(merge == 2 && patch_size > 0 && patch_size <= uint32_t(INT32_MAX) / merge);
    uint32_t stride = patch_size * merge;
    auto width_key = gguf_find_key(gg, "clip.vision.projection_dim"); assert(width_key >= 0);
    uint32_t projector_width = gguf_get_val_u32(gg, width_key);
    assert(projector_width > 0 && projector_width <= uint32_t(INT32_MAX));
    gguf_free(gg);
    auto mp = llama_model_default_params(); mp.vocab_only = true; mp.n_gpu_layers = 0;
    llama_model * model = llama_model_load_from_file(argv[1], mp); assert(model);
    // Vocab-only models have no initialized hparams: mirror the production metadata width check.
    char arch[64]{}, width_text[32]{};
    assert(llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch)) > 0);
    assert(llama_model_meta_val_str(model, (std::string(arch) + ".embedding_length").c_str(), width_text, sizeof(width_text)) > 0);
    char * end = nullptr;
    assert(std::strtoull(width_text, &end, 10) == projector_width && end && !*end);
    size_t tested = 0;
    for (int budget : {64, 1024}) {
        auto params = mtmd_context_params_default();
        params.use_gpu = false; params.warmup = false; params.n_threads = 4; params.image_max_tokens = budget;
        mtmd_context * ctx = mtmd_init_from_file(argv[2], model, params); assert(ctx);
        for (auto size : {std::pair{224,224}, {448,112}, {112,448}, {333,197}, {2048,1024}}) {
            std::vector<unsigned char> rgb(size.first * size.second * 3);
            for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = i % 251;
            auto bitmap = mtmd_bitmap_init(size.first, size.second, rgb.data()); assert(bitmap);
            mtmd_bitmap_set_id(bitmap, "pr4-fixture");
            const mtmd_bitmap * images[] = {bitmap};
            auto chunks = mtmd_input_chunks_init();
            const char * marker = mtmd_default_marker();
            mtmd_input_text text{marker, std::strlen(marker), false, true};
            assert(mtmd_tokenize(ctx, chunks, &text, images, 1) == 0);
            std::vector<llama_token> prefix, suffix;
            bool past_image = false;
            for (size_t i = 0; i < mtmd_input_chunks_size(chunks); ++i) {
                auto part = mtmd_input_chunks_get(chunks, i);
                if (mtmd_input_chunk_get_type(part) == MTMD_INPUT_CHUNK_TYPE_IMAGE) { past_image = true; continue; }
                assert(mtmd_input_chunk_get_type(part) == MTMD_INPUT_CHUNK_TYPE_TEXT);
                size_t count = 0;
                auto ids = mtmd_input_chunk_get_tokens_text(part, &count);
                auto & run = past_image ? suffix : prefix;
                if (count) run.insert(run.end(), ids, ids + count);
            }
            size_t images_seen = 0;
            for (size_t i = 0; i < mtmd_input_chunks_size(chunks); ++i) {
                const auto chunk = mtmd_input_chunks_get(chunks, i);
                if (mtmd_input_chunk_get_type(chunk) != MTMD_INPUT_CHUNK_TYPE_IMAGE) continue;
                ++images_seen;
                size_t bytes = 0;
                assert(mtmd_input_chunk_save(chunk, nullptr, 0, &bytes) == 0);
                std::string blob(bytes, '\0');
                assert(mtmd_input_chunk_save(chunk, blob.data(), blob.size(), &bytes) == 0);
                vm::Limits limits{4096, 64, 4096, 16384, stride, 16 * 1024 * 1024}; // test limits only
                vm::Shape shape{};
                assert(vm::preflight(blob, "pr4-fixture", limits, shape));
                assert(shape.cells == mtmd_input_chunk_get_n_tokens(chunk));
                assert(shape.positions == uint32_t(mtmd_input_chunk_get_n_pos(chunk)));
                // Only invoke loader after preflight; compare loaded public shape/positions.
                auto loaded = mtmd_input_chunk_load(blob.data(), blob.size()); assert(loaded);
                assert(mtmd_input_chunk_get_n_tokens(loaded) == shape.cells);
                assert(std::string(mtmd_input_chunk_get_id(loaded)) == "pr4-fixture");
                assert(mtmd_input_chunk_get_n_pos(loaded) == int32_t(shape.positions));
                const auto image = mtmd_input_chunk_get_tokens_image(loaded);
                for (uint32_t cell = 0; cell < shape.cells; ++cell) {
                    auto p = mtmd_image_tokens_get_decoder_pos(image, 0, cell);
                    assert(p.t == 0 && p.x == cell % shape.x && p.y == cell / shape.x && p.z == 0);
                }
                mtmd_input_chunk_free(loaded);
                // Qualify the single-image PREPARED schema with actual tokenizer wrapper IDs.
                vp::Config config{};
                config.width = projector_width; config.pixel_stride = stride;
                config.vocabulary_size = llama_vocab_n_tokens(llama_model_get_vocab(model));
                vp::Limits caps{4096, 1, 64, 10 * 1024 * 1024, 256 * 1024 * 1024, limits};
                vp::Binding binding{0, {}, "pr4-fixture", rgb.size(), 0, 0, 0};
                std::string payload(32, '\0'); u32(payload, 0); payload.append(32, '\0'); u64(payload, 1);
                u32(payload, shape.x); u32(payload, shape.y); u32(payload, config.width);
                u32(payload, prefix.size()); for (auto id : prefix) u32(payload, uint32_t(id));
                u32(payload, suffix.size()); for (auto id : suffix) u32(payload, uint32_t(id));
                u32(payload, blob.size()); payload += blob;
                vp::View validated;
                assert(vp::validate(strata_frontend::vision_protocol::Kind::prepared, payload, 0, config, caps, binding, validated));
                std::ofstream out(std::string(argv[3]) + "/qwen-still-" + std::to_string(tested) + ".bin", std::ios::binary);
                out.write(blob.data(), blob.size()); assert(out.good());
                std::cout << "METADATA_FIXTURE " << tested << " bytes=" << bytes << " grid=" << shape.x << 'x' << shape.y
                          << " stride=" << stride << " budget=" << budget << '\n';
                ++tested;
            }
            assert(images_seen == 1);
            mtmd_input_chunks_free(chunks); mtmd_bitmap_free(bitmap);
        }
        mtmd_free(ctx);
    }
    llama_model_free(model);
    std::cout << "PR4_METADATA_REAL_PASS fixtures=" << tested << " gpu=none wrapped_encode_entries=not_reached\n";
}
