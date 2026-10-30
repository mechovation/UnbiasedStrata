// Focused, no-model CPU tests. data/vision-runtime-cpu.json is the minimal complete
// example profile (no implicit numeric defaults). For actual CPU integration use
// a cgroup-v2 4-CPU/8-GiB/pids=256 test container, never production or a GPU.
// API: Runtime::load(profile, model, projector, engine, engine_args, 64, 4, false).
// Pass engine_environment and reader_environment verbatim to clean launchers;
// Supervisor reader argv is {absolute_reader_path}; it appends INSTANCE and maps FD3.
// Runtime::load itself checks containment; validate_containment() may recheck it.
#include "../vision_runtime.hpp"
#include <cassert>
#include <iostream>
#include <sys/wait.h>
using namespace strata_frontend::vision_runtime;
template<class F> void rejects(F f) {
    bool rejected = false;
    try { f(); } catch (const std::exception & e) {
        rejected = true;
        assert(std::string(e.what()) == "invalid native vision configuration");
    }
    assert(rejected);
}
static void artifact_tests() {
    char dir[] = "/tmp/pr4-runtime-artifacts-XXXXXX"; assert(::mkdtemp(dir));
    const std::string model = std::string(dir) + "/model.gguf", projector = std::string(dir) + "/projector.gguf";
    auto write_model = [&](const char * family, const char * name, uint32_t width = 2560) {
        auto g = gguf_init_empty(); assert(g);
        gguf_set_val_str(g, "general.architecture", family); gguf_set_val_str(g, "general.name", name);
        gguf_set_val_u32(g, (std::string(family) + ".embedding_length").c_str(), width);
        const char * tokens[] = {"a", "b"}; gguf_set_arr_str(g, "tokenizer.ggml.tokens", tokens, 2);
        assert(gguf_write_to_file(g, model.c_str(), true)); gguf_free(g);
    };
    auto write_projector = [&](const char * family, uint32_t width = 2560, uint32_t merge = 2, uint32_t patch = 16) {
        auto g = gguf_init_empty(); assert(g);
        gguf_set_val_str(g, "clip.projector_type", family); gguf_set_val_u32(g, "clip.vision.projection_dim", width);
        gguf_set_val_u32(g, "clip.vision.spatial_merge_size", merge); gguf_set_val_u32(g, "clip.vision.patch_size", patch);
        assert(gguf_write_to_file(g, projector.c_str(), true)); gguf_free(g);
    };
    write_model("qwen4exp", "Qwen3.8-Flash-Next"); write_projector("qwen3vl_merger");
    const auto baseline = Runtime::inspect_artifacts(model, projector);
    assert(baseline.width == 2560 && baseline.stride == 32 && baseline.vocabulary == 2 && baseline.family == "qwen4exp");
    // Tensor/data tails are intentionally excluded from model identity, included
    // completely for projector identity. The model tokenizer metadata IS bound.
    { std::ofstream f(model, std::ios::app | std::ios::binary); f << "not-loaded-weights"; }
    assert(Runtime::inspect_artifacts(model, projector).model_hash == baseline.model_hash);
    { std::ofstream f(projector, std::ios::app | std::ios::binary); f << "projector-tensor-tail"; }
    assert(Runtime::inspect_artifacts(model, projector).projector_hash != baseline.projector_hash);
    write_model("qwen4exp", "Qwen3.8-Flash-Next-metadata-changed");
    assert(Runtime::inspect_artifacts(model, projector).model_hash != baseline.model_hash);
    write_model("llama", "Qwen-looking-name"); rejects([&] { Runtime::inspect_artifacts(model, projector); });
    write_model("qwen4exp", "Qwen3.8-Flash-Next", 4096); rejects([&] { Runtime::inspect_artifacts(model, projector); });
    write_model("qwen4exp", "Qwen3.8-Flash-Next");
    write_projector("other"); rejects([&] { Runtime::inspect_artifacts(model, projector); });
    write_projector("qwen3vl_merger", 2560, 3); rejects([&] { Runtime::inspect_artifacts(model, projector); });
    write_projector("qwen3vl_merger", 2560, 2, UINT32_MAX); rejects([&] { Runtime::inspect_artifacts(model, projector); });
    write_projector("qwen3vl_merger");
    // Original GGUF tensor dimensions can be redundant. Reserializing normalizes
    // them: get_meta_size alone would omit the original descriptor's tail.
    std::string raw;
    auto integer = [&](uint64_t n, unsigned bytes) { for (unsigned i = 0; i < bytes; ++i) raw += char(n >> (8*i)); };
    auto string = [&](std::string_view s) { integer(s.size(), 8); raw += s; };
    integer(0x46554747, 4); integer(3, 4); integer(1, 8); integer(4, 8);
    string("general.architecture"); integer(GGUF_TYPE_STRING, 4); string("qwen4exp");
    string("general.name"); integer(GGUF_TYPE_STRING, 4); string("Qwen-test");
    string("qwen4exp.embedding_length"); integer(GGUF_TYPE_UINT32, 4); integer(2560, 4);
    string("tokenizer.ggml.tokens"); integer(GGUF_TYPE_ARRAY, 4); integer(GGUF_TYPE_STRING, 4); integer(2, 8); string("a"); string("b");
    string("weight"); integer(4, 4); integer(8, 8); integer(1, 8); integer(1, 8); integer(1, 8);
    const auto tensor_type_at = raw.size(); integer(GGML_TYPE_F32, 4); integer(0, 8);
    raw.resize((raw.size() + 31) / 32 * 32, '\0'); raw.resize(raw.size() + 32, '\0');
    auto save_raw = [&] { std::ofstream f(model, std::ios::binary); f.write(raw.data(), raw.size()); assert(f.good()); };
    save_raw();
    auto gg = gguf_init_from_file(model.c_str(), {true, nullptr}); assert(gg);
    assert(gguf_get_meta_size(gg) < gguf_get_data_offset(gg) && tensor_type_at >= gguf_get_meta_size(gg)); gguf_free(gg);
    const auto original = Runtime::inspect_artifacts(model, projector);
    raw[tensor_type_at] = char(GGML_TYPE_F16); save_raw();
    assert(Runtime::inspect_artifacts(model, projector).model_hash != original.model_hash);
    ::unlink(model.c_str()); ::unlink(projector.c_str()); ::rmdir(dir);
}
static int contained_reader_test(char ** argv) {
    using namespace strata_frontend;
    namespace vp = vision_protocol;
    // Explicit opt-in test seam; Runtime still requires real finite cgroups.
    auto r = vision_runtime::Runtime::load(argv[2], argv[3], argv[4], "/usr/bin/true", {}, 64, 4, false);
    assert(number(parse(read_small(argv[2])), "cgroup_memory_max") == 8589934592ULL);
    assert(number(parse(read_small(argv[2])), "cgroup_pids_max") == 256);
    auto cpu = read_small("/sys/fs/cgroup/cpu.max", 64);
    std::istringstream c(cpu); uint64_t quota = 0, period = 0;
    assert(c >> quota >> period && period && quota / period <= 4 && quota <= period * 4);
    int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    int diagnostic = ::open("/dev/null", O_WRONLY | O_CLOEXEC); assert(diagnostic >= 0);
    SpawnedProcess child; std::string error;
    assert(protected_spawn(argv[6], argv[5], {argv[5], "1"}, r.reader_environment, diagnostic, child, error, sockets[1]));
    ::close(sockets[1]); ::close(diagnostic);
    const std::atomic<bool> never{false};
    vp::Channel channel(sockets[0]); vp::Frame ready;
    auto until = vp::Clock::now() + std::chrono::seconds(150);
    const bool received = channel.receive(ready, {vp::Kind::ready, 1, 0, 0, 0}, r.wire_limits.payload_bytes, until, never) == vp::IoResult::ok;
    vision_payload::View view;
    const bool valid = received && vision_payload::validate(vp::Kind::ready,
        std::string_view(reinterpret_cast<const char *>(ready.payload.data()), ready.payload.size()), ready.fd_count, r.config, r.wire_limits, {}, view);
    bool quit_sent = false;
    if (valid) {
        vision_commands::Payload quit;
        assert(vision_commands::build(vp::Kind::quit, r.config, r.wire_limits, {}, quit) == vision_commands::Result::ok);
        quit_sent = channel.send_borrowed({vp::Kind::quit, uint32_t(quit.bytes.size()), 1, 0, 0}, quit.bytes.data(), quit.bytes.size(),
            nullptr, 0, r.wire_limits.payload_bytes, until, never) == vp::IoResult::ok;
    }
    ::close(child.input); ::close(child.output); int status = 0;
    auto reaped = reap_until(child.pid, vp::Clock::now() + std::chrono::seconds(5), status);
    if (reaped == ReapResult::timeout)
        reaped = terminate_until(child.pid, vp::Clock::now() + std::chrono::seconds(5), vp::Clock::now() + std::chrono::seconds(10), status);
    assert(reaped == ReapResult::reaped && valid && quit_sent && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::cout << "contained CPU reader READY/QUIT passed\n"; return 0;
}
int main(int argc, char ** argv) {
    if (argc == 7 && std::string_view(argv[1]) == "--contained-reader") return contained_reader_test(argv);
    if (argc == 4 && std::string_view(argv[1]) == "--inspect-only") {
        const auto a = Runtime::inspect_artifacts(argv[2], argv[3]);
        std::cout << "qualified family=" << a.family << " width=" << a.width << " stride=" << a.stride << " vocabulary=" << a.vocabulary << '\n';
        return 0; // metadata/hash inspection only; no weights/backend/decode/encode
    }
    assert(argc == 2); artifact_tests();
    auto fixture = parse(read_small(argv[1]));
    const auto r = Runtime::settings(fixture);
    assert(r.artifact_fds == 16 && r.reader_limits.source_dimension == 4096);
    assert(r.lifecycle.attempts == 2 && r.engine_limits.command_bytes == 131072);
    assert(r.image_min_tokens == 1 && r.batch_max_tokens == 64);
    assert(hex(hash("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Streaming hashes bind exact lengths, not just a prefix or text conversion.
    char path[] = "/tmp/pr4-runtime-hash-XXXXXX"; int fd = ::mkstemp(path); assert(fd >= 0);
    const std::string data(170001, '\xff'); assert(::write(fd, data.data(), data.size()) == ssize_t(data.size())); ::close(fd);
    assert(hash_file(path, data.size()) == hash(data));
    assert(hash_file(path, 65537) == hash(std::string_view(data.data(), 65537)));
    rejects([&] { hash_file(path, data.size() + 1); }); ::unlink(path);
    rejects([] { parse("{\"x\":1,\"x\":2}"); });
    rejects([] { parse("{\"a\":{\"x\":1,\"x\":2}}"); });
    rejects([] { parse(std::string(65537, ' ')); });
    rejects([] { parse("{\"password\":\"do-not-echo\","); });
    rejects([] { parse(std::string(20, '[') + "0" + std::string(20, ']')); });
    for (const auto & item : fixture.items()) {
        auto p = fixture; p.erase(item.key()); rejects([&] { Runtime::settings(p); });
    }
    auto changed = fixture; changed["extra"] = 1u; rejects([&] { Runtime::settings(changed); });
    for (const auto & item : fixture.items()) {
        if (!item.value().is_number_unsigned()) continue;
        auto p = fixture; p[item.key()] = -1; rejects([&] { Runtime::settings(p); });
        p[item.key()] = 1.5; rejects([&] { Runtime::settings(p); });
        p[item.key()] = "100"; rejects([&] { Runtime::settings(p); });
        p[item.key()] = UINT64_MAX; rejects([&] { Runtime::settings(p); });
        if (item.key().find("core_bytes") == std::string::npos) { p[item.key()] = 0u; rejects([&] { Runtime::settings(p); }); }
    }
    changed = fixture; changed["gpu_uuid"] = "GPU-RTX-3060"; rejects([&] { Runtime::settings(changed); });
    for (auto key : {"PATH", "HTTP_PROXY", "https_proxy", "OPENAI_API_KEY", "LD_PRELOAD", "CUDA_VISIBLE_DEVICES", "NVIDIA_VISIBLE_DEVICES", "STRATA_UNREVIEWED"}) {
        changed = fixture; changed["engine_environment"][key] = "do-not-echo";
        rejects([&] { Runtime::settings(changed); });
        changed = fixture; changed["reader_environment"][key] = "do-not-echo";
        rejects([&] { Runtime::settings(changed); });
    }
    validate_environment(Json{{"LD_LIBRARY_PATH", "/opt/reviewed/lib:/usr/lib"}, {"CUDA_MODULE_LOADING", "LAZY"},
        {"STRATA_IO_THREADS", "4"}, {"STRATA_RESIDENT_HEADROOM_GIB", "2"}});
    for (auto v : {".", "/safe:", ":/safe", "/safe:relative", "/safe/../other", "$HOME/lib"})
        rejects([&] { validate_environment(Json{{"LD_LIBRARY_PATH", v}}); });
    rejects([] { validate_environment(Json{{"OMP_NUM_THREADS", "999999999999999999999999"}}); });
    rejects([] { validate_environment(Json{{"STRATA_PREFILL_CPU_SHARE", "1;do-not-echo"}}); });
    validate_engine_args({"--native", "/models/qwen.gguf", "--max-context", "8192", "--vision",
        "--prompt-cache", "6", "--conversation-cache-min-tokens", "8000"});
    validate_engine_args({"--prompt-cache=6", "--conversation-cache-min-tokens=8000"});
    for (auto flag : {"--gpu", "--gpu=0", "--split", "--split-mode=none", "--device=0", "--tensor-split", "--main-gpu", "-mg", "--rpc", "--api-key", "--prompt", "--prompt-cache-file", "--conversation-cache-min-tokens-secret", "--model-url"})
        rejects([&] { validate_engine_args({flag, "do-not-echo"}); });
    rejects([] { validate_engine_args({std::string("--native\0bad", 12)}); });
    rejects([] { validate_engine_args({std::string(4097, 'x')}); });
    rejects([&] { Runtime::load(argv[1], "/not-a-model", "/not-a-projector", "/bin/true", {}, 64, 4, false); });
    // Limit setup is in the child only; failure must never change parent limits.
    rlimit before{}; assert(::getrlimit(RLIMIT_CORE, &before) == 0);
    auto child = ::fork(); assert(child >= 0);
    if (!child) {
        Json j = {{"profile", fixture}, {"model", "/missing"}, {"projector", "/missing"}, {"engine", "/bin/true"},
            {"engine_args", Json::array()}, {"image_tokens", 64u}, {"threads", 4u}, {"gpu", false}, {"abi", "bad"},
            {"reader_policy", Json::object()}, {"engine_environment", Json::array()}, {"reader_environment", Json::array()},
            {"family", "qwen4exp"}, {"device", "cpu"}, {"model_sha256", "bad"}, {"projector_sha256", "bad"}, {"engine_sha256", "bad"},
            {"width", 1u}, {"stride", 28u}, {"vocabulary", 1u}, {"generation", "bad"}};
        rejects([&] { Runtime::reader_config(j.dump(), {"API_KEY=do-not-echo"}); });
        rlimit core{}, as{}, nofile{}, fsize{};
        assert(::getrlimit(RLIMIT_CORE, &core) == 0 && core.rlim_cur == 0 && core.rlim_max == 0);
        assert(::getrlimit(RLIMIT_AS, &as) == 0 && as.rlim_cur == 8589934592ULL);
        assert(::getrlimit(RLIMIT_NOFILE, &nofile) == 0 && nofile.rlim_cur == 128);
        assert(::getrlimit(RLIMIT_FSIZE, &fsize) == 0 && fsize.rlim_cur == 16777216);
        ::_exit(0);
    }
    int status = 0; assert(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    rlimit after{}; assert(::getrlimit(RLIMIT_CORE, &after) == 0 && before.rlim_cur == after.rlim_cur && before.rlim_max == after.rlim_max);
    std::cout << "vision runtime schema/hash/environment/rlimit tests passed\n";
}
