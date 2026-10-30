// Focused excluded CPU test: real reader PREPARE/ENCODE blocked inside native
// decode/encode, with an independently dying resident existing-protocol engine.
// Requires the same static CPU libraries/assets as vision-native.sh; no serving/GPU.
#include "../vision_reader.hpp"
#include "../vision_engine_adapter.hpp"
#include "hash.h"
#include "mtmd.h"
#include "ggml-backend.h"
#include "gguf.h"
#include "stb_image.h"
#include <cassert>
#include <dirent.h>
#include <fstream>
#include <iostream>

using namespace strata_frontend;
namespace vs = vision_supervisor;
namespace io = vision_job_io;
namespace vc = vision_control;
namespace ea = vision_engine_adapter;
namespace eh = vision_engine_handoff;
namespace art = vision_artifact;
using namespace std::chrono_literals;
static std::string reader_gate, reader_phase;
static std::atomic<bool> never{false};
static auto deadline() { return eh::Clock::now() + 60s; }
template<class F> static void await(F f) {
    const auto until = deadline();
    while (!f()) { assert(eh::Clock::now() < until); std::this_thread::sleep_for(1ms); }
}
static void hold(const char * phase) {
    if (reader_phase != phase) return;
    { std::ofstream marker(reader_gate); marker << phase; assert(marker); }
    for (;;) ::pause(); // owner must contain/reap, never release on pipe EOF alone
}
extern "C" int32_t __real_mtmd_encode_chunk(mtmd_context *, const mtmd_input_chunk *);
extern "C" int32_t __wrap_mtmd_encode_chunk(mtmd_context * ctx, const mtmd_input_chunk * chunk) {
    const int32_t result = __real_mtmd_encode_chunk(ctx, chunk);
    assert(result == 0); hold("encode"); return result;
}
extern "C" unsigned char * __real_stbi_load_from_memory(const unsigned char *, int, int *, int *, int *, int);
extern "C" unsigned char * __wrap_stbi_load_from_memory(const unsigned char * data, int bytes, int * x, int * y, int * channels, int wanted) {
    auto * result = __real_stbi_load_from_memory(data, bytes, x, y, channels, wanted);
    assert(result); hold("prepare"); return result;
}
static int fd_count() {
    DIR * d = ::opendir("/proc/self/fd"); assert(d);
    int count = 0; while (::readdir(d)) ++count; ::closedir(d); return count;
}
static vision_payload::Config configuration(const llama_model * model, const char * projector) {
    auto * gg = gguf_init_from_file(projector, {true, nullptr}); assert(gg);
    auto family = gguf_find_key(gg, "clip.projector_type"); assert(family >= 0);
    assert(std::string(gguf_get_val_str(gg, family)) == "qwen3vl_merger");
    auto patch = gguf_find_key(gg, "clip.vision.patch_size"); assert(patch >= 0);
    auto width = gguf_find_key(gg, "clip.vision.projection_dim"); assert(width >= 0);
    vision_payload::Config c{};
    c.width = gguf_get_val_u32(gg, width); c.pixel_stride = 2 * gguf_get_val_u32(gg, patch);
    c.vocabulary_size = llama_vocab_n_tokens(llama_model_get_vocab(model));
    c.generation.fill(0x74); gguf_free(gg); return c; // synthetic CPU identity only
}
static vision_payload::Limits limits(const vision_payload::Config & c) {
    return {16384, 4, 64, 10 * 1024 * 1024, 16 * 1024 * 1024,
            {4096, 64, 4096, 16384, c.pixel_stride, 16 * 1024 * 1024}};
}
static std::string bmp() {
    const unsigned w = 64, h = 64, stride = w * 3;
    std::string out(54 + h * stride, '\0');
    auto number = [&](unsigned at, unsigned value, unsigned bytes) {
        for (unsigned b = 0; b < bytes; ++b) out[at + b] = value >> (8*b);
    };
    out[0] = 'B'; out[1] = 'M'; number(2, out.size(), 4); number(10, 54, 4); number(14, 40, 4);
    number(18, w, 4); number(22, h, 4); number(26, 1, 2); number(28, 24, 2);
    for (size_t i = 54; i < out.size(); ++i) out[i] = i % 251;
    return out;
}
struct Wake final : vc::Wake { std::atomic<bool> pending{false}; void post() noexcept override { pending = true; } };
struct Events {
    std::shared_ptr<Wake> wake = std::make_shared<Wake>();
    std::shared_ptr<vc::Inbox> inbox = std::make_shared<vc::Inbox>(8, wake);
    std::vector<vc::Event> events;
    void pump() { if (wake->pending.exchange(false)) while (auto e = inbox->pop()) events.push_back(*e); }
    void take(vc::Event::Kind kind, uint64_t task) {
        await([&] {
            pump();
            for (auto it = events.begin(); it != events.end(); ++it) if (it->kind == kind && it->task == task) {
                events.erase(it); return true;
            }
            return false;
        });
    }
};
struct Output final : ea::Sink {
    bool info(std::string_view) noexcept override { return true; }
    bool line(eh::Identity, const EngineLine &) noexcept override { return true; }
};
struct CpuOnlySafety final : vs::Safety {
    std::shared_ptr<ea::Adapter> adapter;
    bool leased = false, engine = false; eh::Identity identity{};
    std::atomic<bool> revoked{false};
    std::atomic<unsigned> releases{0};
    bool acquire(uint64_t) noexcept override { assert(!leased && !revoked); leased = true; return true; }
    void service_failed() noexcept override { revoked = true; }
    bool engine_stopped() noexcept override { return adapter->stopped(); }
    bool certify(uint64_t, bool, bool service) noexcept override {
        assert(!engine); if (service) assert(revoked && adapter->stopped()); return true;
    }
    void release(uint64_t) noexcept override { assert(leased && !engine); leased = false; ++releases; }
    bool to_engine(eh::Identity id) noexcept override {
        assert(leased && !engine); if (revoked) return false; engine = true; identity = id; return true;
    }
    bool to_reader(eh::Identity id) noexcept override {
        assert(engine && id == identity); if (!adapter->idle() && !adapter->stopped()) return false;
        engine = false; return true;
    }
};
static int engine(const char * pid_path) {
    ::signal(SIGTERM, SIG_IGN);
    { std::ofstream pid(pid_path); pid << ::getpid(); assert(pid); }
    std::cout << "READY 32768 stop\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT") return 0;
        assert(line == "GEN 1 1");
        std::cout << "DONE 0 1 0 0 stop\n" << std::flush;
    }
    return 0;
}
static void scenario(const char * executable, const char * guard, const char * model, const char * projector,
                     vision_payload::Config config, const std::string & phase) {
    const auto baseline = fd_count();
    char path[] = "/tmp/pr4-real-reader-health-XXXXXX"; assert(::mkdtemp(path));
    const std::string directory(path), gate = directory + "/reader", pid_path = directory + "/engine";
    auto sink = std::make_shared<Output>();
    ea::Limits engine_limits{65536, 4096, 16384, 64, config.vocabulary_size, 10s, 20ms, 20ms};
    auto adapter = std::make_shared<ea::Adapter>(ea::Launch{guard, executable, {executable, "engine", pid_path}, {}}, engine_limits, 2, sink);
    auto safety = std::make_shared<CpuOnlySafety>(); safety->adapter = adapter;
    auto artifacts = std::make_shared<art::Budget>(art::Usage{64*1024*1024, 16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536, 4});
    Events events;
    {
        vs::Launch reader{guard, executable, {executable, "reader", model, projector, gate, phase},
                          {"CUDA_VISIBLE_DEVICES=-1", "NVIDIA_VISIBLE_DEVICES=void"}, 100, config, limits(config)};
        vs::Supervisor supervisor(std::move(reader), {60s, 20ms, 20ms, 2s, 20ms, 180s, 3}, 2, safety,
                                  events.inbox, std::make_shared<vs::Reaper>(), adapter);
        assert(supervisor.start(1)); events.take(vc::Event::Kind::ready, 1);
        auto submission = [&](uint64_t job, std::shared_ptr<vc::Request> control) {
            auto s = std::make_unique<io::Submission>(); s->job = job; s->control = std::move(control);
            s->artifacts = artifacts; s->recipes = recipes; s->limits = {65536, 16384, 32768};
            s->snapshot_bytes = 65536; s->prepare = 30s; s->encode = 30s; s->cleanup = 1s; return s;
        };
        // Warm the resident adapter on this supervisor's authorized owner using a
        // text-only Job, without exercising the reader's blocked image stage yet.
        const io::Identity first{20, supervisor.instance(), 11};
        auto initial = submission(first.job, std::make_shared<vc::Request>(deadline())); initial->text_only = true;
        assert(supervisor.prepare(first.task, initial)); events.take(vc::Event::Kind::prepared, first.task);
        assert(supervisor.admit(first, true)); events.take(vc::Event::Kind::encoded, first.task);
        auto request = ea::Request::create({first.task, first.instance, first.job, 1}, "GEN 1", {1}, engine_limits);
        assert(supervisor.generate(first, request, 10s, 1s)); events.take(vc::Event::Kind::engine_submitted, first.task);
        events.take(vc::Event::Kind::job_terminal, first.task);
        assert(supervisor.terminal()->outcome == vision_job::Outcome::success && supervisor.acknowledge(first));
        assert(adapter->idle() && !adapter->stopped() && !safety->revoked);
        pid_t engine_pid = -1; { std::ifstream pid(pid_path); pid >> engine_pid; assert(pid && engine_pid > 0); }

        const io::Identity second{21, supervisor.instance(), 12};
        auto control = std::make_shared<vc::Request>(deadline()); auto input = submission(second.job, control);
        const auto bytes = bmp(); const auto hex = hash_sha256_hex(bytes.data(), bytes.size()); vision_payload::Digest hash{};
        for (size_t i = 0; i < hash.size(); ++i) hash[i] = std::stoul(hex.substr(2*i, 2), nullptr, 16);
        art::Artifact::Ptr source;
        assert(art::Artifact::source(*artifacts, {0, second.job, 0, hash}, bytes, limits(config).source_bytes,
                                    deadline(), never, source) == art::Result::ok);
        input->inputs.push_back({std::move(source), vision_reader::source_id(hash)});
        assert(supervisor.prepare(second.task, input));
        if (phase == "encode") {
            events.take(vc::Event::Kind::prepared, second.task);
            assert(supervisor.admit(second, true));
        }
        await([&] { std::ifstream marker(gate); std::string got; marker >> got; return got == phase; });
        assert(!supervisor.terminal() && !control->flag() && artifacts->usage().artifacts > 0);
        std::vector<vc::Inbox::Reservation> full;
        while (auto reservation = events.inbox->reserve()) full.push_back(std::move(reservation));
        const auto failed_at = eh::Clock::now(); assert(::kill(engine_pid, SIGKILL) == 0);
        await([&] { return control->flag().load(); }); assert(safety->revoked);
        events.take(vc::Event::Kind::job_terminal, second.task);
        assert(eh::Clock::now() - failed_at < 1s); // not PREPARE/ENCODE's 30s deadline
        events.take(vc::Event::Kind::failed, second.task); events.take(vc::Event::Kind::stopped, 0);
        assert(supervisor.state() == vs::State::exited && adapter->stopped() && safety->releases == 1);
        assert(supervisor.terminal()->outcome == vision_job::Outcome::cancelled);
        assert(artifacts->usage().artifacts == 0 && recipes->usage().snapshots == 0);
        assert(supervisor.acknowledge(second) && !supervisor.start(99) && supervisor.attempts() == 1);
        full.clear(); events.pump(); assert(events.events.empty());
    }
    adapter.reset(); safety.reset(); events.inbox->stop_accepting(); assert(events.inbox->settled());
    assert(::unlink(gate.c_str()) == 0 && ::unlink(pid_path.c_str()) == 0 && ::rmdir(directory.c_str()) == 0);
    assert(fd_count() == baseline);
}
int main(int argc, char ** argv) {
    if (argc == 3 && std::string(argv[1]) == "engine") return engine(argv[2]);
    const bool reader = argc == 7 && std::string(argv[1]) == "reader";
    assert(reader || argc == 4); // existing MODEL_GGUF PROJECTOR_GGUF PROCESS_GUARD
    const char * model_path = argv[reader ? 2 : 1], * projector = argv[reader ? 3 : 2];
    assert(!ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU));
    auto mp = llama_model_default_params(); mp.vocab_only = true; mp.n_gpu_layers = 0;
    auto * model = llama_model_load_from_file(model_path, mp); assert(model);
    const auto config = configuration(model, projector);
    if (reader) {
        ::signal(SIGTERM, SIG_IGN); reader_gate = argv[4]; reader_phase = argv[5];
        auto cp = mtmd_context_params_default(); cp.use_gpu = false; cp.warmup = false; cp.n_threads = 4; cp.image_max_tokens = 64;
        auto * ctx = mtmd_init_from_file(projector, model, cp); assert(ctx);
        const int rc = vision_reader::serve(ctx, config, limits(config),
            {4096, 16*1024*1024, 48*1024*1024, 65536, 60s}, std::stoull(argv[6]), 3);
        mtmd_free(ctx); llama_model_free(model); return rc;
    }
    for (const auto * phase : {"prepare", "encode"}) scenario(argv[0], argv[3], model_path, projector, config, phase);
    llama_model_free(model);
    std::cout << "vision real reader blocked IO engine-death observation: PASS (2 CPU-only scenarios)\n";
}
