// Development-workspace probe (never published, never a product binary).
//
// Question it answers: what does the native reader's opt-in encoder warm-up actually cost, and
// does one warm shape cover other shapes? It mirrors frontend/vision_reader_main.cpp's context
// setup (vocab-only model, mtmd with buffer warm-up) and then times synthetic encodes per shape,
// reporting NVML device memory around every step.
//
// Scope limit: this runs on the development GPU (RTX 3060, sm_86, 12 GiB). It is mechanism
// evidence only; it is not 5070 Ti qualification and never carries a slot/production claim.
#include "mtmd.h"
#include "llama.h"
#include "ggml-backend.h"
#include <nvml.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
void quiet(enum ggml_log_level, const char *, void *) {}

struct Vram {
    nvmlDevice_t device = nullptr;
    bool ok = false;
    Vram() {
        if (nvmlInit_v2() != NVML_SUCCESS) return;
        if (nvmlDeviceGetHandleByIndex_v2(0, &device) != NVML_SUCCESS) return;
        ok = true;
    }
    long long used_mib() const {
        if (!ok) return -1;
        nvmlMemory_t m{};
        return nvmlDeviceGetMemoryInfo(device, &m) == NVML_SUCCESS ? static_cast<long long>(m.used >> 20) : -1;
    }
    long long total_mib() const {
        if (!ok) return -1;
        nvmlMemory_t m{};
        return nvmlDeviceGetMemoryInfo(device, &m) == NVML_SUCCESS ? static_cast<long long>(m.total >> 20) : -1;
    }
};

double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

// One tokenize + encode of a synthetic uniform-grey bitmap, exactly the shape the reader's
// warm-up uses (mtmd scales the bitmap down to the configured image-token budget).
bool encode(mtmd_context * ctx, uint32_t w, uint32_t h, const char * event, int trial, const Vram & vram) {
    std::vector<unsigned char> rgb((size_t) w * h * 3, 128);
    const long long before = vram.used_mib();
    const auto started = std::chrono::steady_clock::now();
    mtmd_bitmap * bitmap = mtmd_bitmap_init(w, h, rgb.data());
    mtmd_input_chunks * chunks = mtmd_input_chunks_init();
    const std::string marker = mtmd_default_marker();
    mtmd_input_text text{marker.c_str(), marker.size(), false, true};
    const mtmd_bitmap * pictures[1] = { bitmap };
    int tokens = 0; bool done = false;
    if (bitmap && chunks && mtmd_tokenize(ctx, chunks, &text, pictures, 1) == 0) {
        done = true;
        for (size_t c = 0; c < mtmd_input_chunks_size(chunks); ++c) {
            const mtmd_input_chunk * chunk = mtmd_input_chunks_get(chunks, c);
            if (mtmd_input_chunk_get_type(chunk) != MTMD_INPUT_CHUNK_TYPE_IMAGE) continue;
            if (mtmd_encode_chunk(ctx, chunk) != 0) { done = false; break; }
            tokens = (int) mtmd_input_chunk_get_n_tokens(chunk);
        }
    }
    if (bitmap) mtmd_bitmap_free(bitmap);
    if (chunks) mtmd_input_chunks_free(chunks);
    std::printf("{\"event\":\"%s\",\"trial\":%d,\"width\":%u,\"height\":%u,\"tokens\":%d,\"ms\":%.0f,"
                "\"vram_used_mib_before\":%lld,\"vram_used_mib_after\":%lld,\"ok\":%s}\n",
                event, trial, w, h, tokens, ms_since(started), before, vram.used_mib(), done ? "true" : "false");
    std::fflush(stdout);
    return done;
}
} // namespace

int main(int argc, char ** argv) {
    std::string model, projector, shapes = "2048x2048,1920x1080,800x1200,64x64,2048x2048";
    bool buffer_warmup = true;
    for (int i = 1; i < argc; ++i) {
        std::string a(argv[i]);
        if (a == "--model" && i + 1 < argc) model = argv[++i];
        else if (a == "--projector" && i + 1 < argc) projector = argv[++i];
        else if (a == "--shapes" && i + 1 < argc) shapes = argv[++i];
        else if (a == "--no-buffer-warmup") buffer_warmup = false;
        else { std::fprintf(stderr, "usage: %s --model M --projector P [--shapes WxH,...] "
                                    "[--no-buffer-warmup]\n", argv[0]); return 2; }
    }
    if (model.empty() || projector.empty()) { std::fputs("missing --model/--projector\n", stderr); return 2; }

    llama_log_set(quiet, nullptr); mtmd_log_set(quiet, nullptr); ggml_log_set(quiet, nullptr);
    ggml_backend_load_all();
    llama_backend_init();
    Vram vram;
    ggml_backend_dev_t device = nullptr;
    for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
        auto d = ggml_backend_dev_get(i);
        if (ggml_backend_dev_type(d) == GGML_BACKEND_DEVICE_TYPE_GPU) { device = d; break; }
    }
    if (!device) { std::fputs("no GPU backend (refusing to fall back silently)\n", stderr); return 1; }
    std::printf("{\"event\":\"device\",\"name\":\"%s\",\"registry\":\"%s\",\"nvml_total_mib\":%lld,"
                "\"nvml_used_mib_before_start\":%lld}\n", ggml_backend_dev_name(device),
                ggml_backend_reg_name(ggml_backend_dev_backend_reg(device)), vram.total_mib(), vram.used_mib());
    std::fflush(stdout);

    auto mp = llama_model_default_params();
    mp.vocab_only = true; mp.n_gpu_layers = 0;
    const auto load_started = std::chrono::steady_clock::now();
    llama_model * model_p = llama_model_load_from_file(model.c_str(), mp);
    if (!model_p) { std::fputs("model load failed\n", stderr); return 1; }
    std::printf("{\"event\":\"model_vocab_only\",\"ms\":%.0f,\"vram_used_mib\":%lld}\n",
                ms_since(load_started), vram.used_mib());
    std::fflush(stdout);

    auto cp = mtmd_context_params_default();
    cp.use_gpu = true; cp.device = device; cp.print_timings = false;
    cp.warmup = buffer_warmup;                       // the reservation the reader already always sets
    cp.n_threads = 4; cp.image_min_tokens = 8; cp.image_max_tokens = 1024; cp.batch_max_tokens = 1024;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
    const long long before_init = vram.used_mib();
    const auto init_started = std::chrono::steady_clock::now();
    mtmd_context * ctx = mtmd_init_from_file(projector.c_str(), model_p, cp);
    std::printf("{\"event\":\"mtmd_init\",\"buffer_warmup\":%s,\"ms\":%.0f,"
                "\"vram_used_mib_before\":%lld,\"vram_used_mib_after\":%lld,\"ok\":%s}\n",
                buffer_warmup ? "true" : "false", ms_since(init_started), before_init, vram.used_mib(),
                ctx && mtmd_support_vision(ctx) ? "true" : "false");
    std::fflush(stdout);
    if (!ctx || !mtmd_support_vision(ctx)) return 1;

    int trial = 0;
    for (size_t at = 0; at < shapes.size();) {
        size_t comma = shapes.find(',', at);
        std::string one = shapes.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        at = comma == std::string::npos ? shapes.size() : comma + 1;
        auto x = one.find('x');
        if (x == std::string::npos) continue;
        uint32_t w = (uint32_t) std::strtoul(one.substr(0, x).c_str(), nullptr, 10);
        uint32_t h = (uint32_t) std::strtoul(one.substr(x + 1).c_str(), nullptr, 10);
        if (!w || !h) continue;
        // A shape seen for the second time is the warmed control for that shape.
        encode(ctx, w, h, "encode", trial++, vram);
    }

    mtmd_free(ctx);
    llama_model_free(model_p);
    llama_backend_free();
    return 0;
}
