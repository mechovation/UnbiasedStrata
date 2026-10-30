// Standalone native reader: INSTANCE is the only argument, control socket is FD 3.
#include "vision_runtime.hpp"
#include "mtmd.h"
#include "llama.h"
#include "ggml-backend.h"
#include <cstdio>
#include <cstdlib>

extern char ** environ;
namespace {
void quiet(enum ggml_log_level, const char *, void *) {}
int run(int argc, char ** argv) {
    using namespace strata_frontend;
    namespace vr = vision_runtime;
    vr::require(argc == 2);
    uint64_t instance = 0; std::string_view arg(argv[1]);
    auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), instance);
    vr::require(parsed.ec == std::errc{} && parsed.ptr == arg.data() + arg.size() && instance &&
                arg == std::to_string(instance));
    struct stat st{}; vr::require(::fstat(3, &st) == 0 && S_ISSOCK(st.st_mode));
    const char * value = std::getenv("STRATA_READER_CONFIG");
    vr::require(value && ::strnlen(value, vr::json_cap + 1) <= vr::json_cap);
    const std::string json(value);
    std::vector<std::string> actual;
    size_t bytes = 0;
    for (char ** at = environ; at && *at; ++at) {
        auto n = ::strnlen(*at, vr::json_cap + 4097);
        vr::require(n <= vr::json_cap + 4096 && bytes <= 131072 - n && actual.size() < 32);
        actual.emplace_back(*at, n); bytes += n;
    }
    // No inherited credentials survive, even on validation failure. Only explicit
    // validated settings are reinstalled before the first backend initialization.
    vr::require(::clearenv() == 0);
    auto runtime = vr::Runtime::reader_config(json, actual);
    for (const auto & entry : runtime.reader_environment) {
        auto equal = entry.find('='); vr::require(equal != std::string::npos);
        if (entry.substr(0, equal) != "STRATA_READER_CONFIG")
            vr::require(::setenv(entry.substr(0, equal).c_str(), entry.c_str() + equal + 1, 1) == 0);
    }
    // Native library diagnostics are not a public error surface. Parent still
    // bounds/contains stderr (including aborts) independently of these callbacks.
    llama_log_set(quiet, nullptr); mtmd_log_set(quiet, nullptr); ggml_log_set(quiet, nullptr);
    ggml_backend_load_all(); llama_backend_init();
    struct Backend { ~Backend() { llama_backend_free(); } } backend;
    ggml_backend_dev_t device = nullptr;
    if (runtime.gpu) {
        size_t gpu_count = 0;
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            auto d = ggml_backend_dev_get(i);
            if (ggml_backend_dev_type(d) != GGML_BACKEND_DEVICE_TYPE_GPU) continue;
            ++gpu_count;
            auto reg = ggml_backend_dev_backend_reg(d);
            vr::require(std::string_view(ggml_backend_reg_name(reg)) == "CUDA"); device = d;
        }
        vr::require(gpu_count == 1 && device);
    }
    auto mp = llama_model_default_params(); mp.vocab_only = true; mp.n_gpu_layers = 0;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
        llama_model_load_from_file(runtime.model_path.c_str(), mp), llama_model_free);
    vr::require(bool(model));
    // vocab_only deliberately skips embedding hyperparameters at this pin;
    // Runtime already verified width against original GGUF metadata before load.
    vr::require(llama_vocab_n_tokens(llama_model_get_vocab(model.get())) == int32_t(runtime.config.vocabulary_size));
    auto cp = mtmd_context_params_default();
    cp.use_gpu = runtime.gpu; cp.device = device; cp.print_timings = false;
    // Preserve the legacy GPU graph/work-buffer reservation before engine sizing.
    // At this pin warmup reserves graphs/buffers; it does NOT run an encoder.
    cp.warmup = runtime.gpu;
    cp.n_threads = int(runtime.threads); cp.image_min_tokens = int(runtime.image_min_tokens);
    cp.image_max_tokens = int(runtime.image_tokens); cp.batch_max_tokens = int(runtime.batch_max_tokens);
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_AUTO;
    std::unique_ptr<mtmd_context, decltype(&mtmd_free)> ctx(
        mtmd_init_from_file(runtime.projector_path.c_str(), model.get(), cp), mtmd_free);
    vr::require(ctx && mtmd_support_vision(ctx.get()) && !mtmd_support_audio(ctx.get()) && mtmd_decode_use_mrope(ctx.get()));
    return vision_reader::serve(ctx.get(), runtime.config, runtime.wire_limits, runtime.reader_limits, instance, 3);
}
} // namespace
int main(int argc, char ** argv) {
    try { return run(argc, argv); }
    catch (...) { std::fputs("native vision reader configuration/startup failed\n", stderr); return 1; }
}
