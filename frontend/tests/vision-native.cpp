// CPU integration: actual reader + safe parent composition + SVE1 + supervisor.
// The legacy in-process frontend below is a parity ORACLE only, never the new path.
#include "../strata_context.cpp"
#include "../vision_reader.hpp"
#include "../vision_composition.hpp"
#include "../vision_engine_adapter.hpp"
#include "vision-recipe.hpp"
#include "hash.h"
#include "stb_image.h"
#include <cassert>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>

using namespace strata_frontend;
namespace vs = vision_supervisor;
namespace io = vision_job_io;
namespace vc = vision_control;
namespace ea = vision_engine_adapter;
namespace eh = vision_engine_handoff;
namespace comp = vision_composition;
using namespace std::chrono_literals;
static std::atomic<bool> never{false};
static std::string counter;
extern "C" int32_t __real_mtmd_encode_chunk(mtmd_context *, const mtmd_input_chunk *);
extern "C" int32_t __wrap_mtmd_encode_chunk(mtmd_context * ctx, const mtmd_input_chunk * chunk) {
    const char * path = std::getenv("PR4_NATIVE_COUNTER");
    if (path) { std::ofstream f(path, std::ios::app); f << 'E'; f.flush(); assert(f.good()); }
    const int32_t result = __real_mtmd_encode_chunk(ctx, chunk);
    const char * hold = std::getenv("PR4_NATIVE_ENCODE_HOLD");
    while (hold && ::access(hold,F_OK) == 0) std::this_thread::sleep_for(10ms);
    return result;
}
extern "C" unsigned char * __real_stbi_load_from_memory(const unsigned char *, int, int *, int *, int *, int);
extern "C" unsigned char * __wrap_stbi_load_from_memory(const unsigned char * data, int bytes, int * x, int * y, int * channels, int wanted) {
    const char * path = std::getenv("PR4_NATIVE_COUNTER");
    if (path) { std::ofstream f(path, std::ios::app); f << 'D'; f.flush(); assert(f.good()); }
    const char * hold = std::getenv("PR5_NATIVE_DECODE_HOLD");
    while (hold && ::access(hold,F_OK) == 0) std::this_thread::sleep_for(10ms);
    return __real_stbi_load_from_memory(data, bytes, x, y, channels, wanted);
}
extern "C" char * __real_mkdtemp(char *);
extern "C" char * __wrap_mkdtemp(char * pattern) {
    if (std::string_view(pattern).find("/tmp/unbiased-strata-vision-") == 0) {
        const char * inherited = std::getenv("PR4_NATIVE_COUNTER");
        const std::string path = inherited ? inherited : counter;
        if (!path.empty()) { std::ofstream f(path,std::ios::app); f << 'F'; }
    }
    return __real_mkdtemp(pattern);
}
static auto deadline() { return eh::Clock::now() + 180s; }
template<class F> static void await(F f) {
    auto until = deadline();
    while (!f()) { assert(eh::Clock::now() < until); std::this_thread::sleep_for(2ms); }
}
static vision_payload::Config configuration(const llama_model * model, const char * projector) {
    auto * gg = gguf_init_from_file(projector, {true, nullptr}); assert(gg);
    auto family = gguf_find_key(gg, "clip.projector_type"); assert(family >= 0);
    assert(std::string(gguf_get_val_str(gg, family)) == "qwen3vl_merger");
    auto merge = gguf_find_key(gg, "clip.vision.spatial_merge_size");
    assert(merge < 0 || gguf_get_val_u32(gg, merge) == 2);
    auto patch = gguf_find_key(gg, "clip.vision.patch_size"); assert(patch >= 0);
    auto width = gguf_find_key(gg, "clip.vision.projection_dim"); assert(width >= 0);
    vision_payload::Config c{};
    c.width = gguf_get_val_u32(gg, width); c.pixel_stride = 2 * gguf_get_val_u32(gg, patch);
    c.vocabulary_size = llama_vocab_n_tokens(llama_model_get_vocab(model));
    char arch[64]{}, embedding[32]{};
    assert(llama_model_meta_val_str(model, "general.architecture", arch, sizeof(arch)) > 0);
    assert(llama_model_meta_val_str(model, (std::string(arch) + ".embedding_length").c_str(), embedding, sizeof(embedding)) > 0);
    assert(std::stoul(embedding) == c.width);
    // Injected CPU qualification identity, NOT a production provenance digest.
    c.generation.fill(0x74); gguf_free(gg); return c;
}
static vision_payload::Limits limits(const vision_payload::Config & c) {
    return {16384, 4, 64, 10 * 1024 * 1024, 16 * 1024 * 1024, {4096, 64, 4096, 16384, c.pixel_stride, 16 * 1024 * 1024}};
}
static raw_buffer bmp(unsigned seed) {
    const unsigned w = 64, h = 64, stride = w * 3;
    raw_buffer out(54 + h * stride);
    auto number = [&](unsigned at, unsigned value, unsigned bytes) { for (unsigned b = 0; b < bytes; ++b) out[at + b] = value >> (8*b); };
    out[0] = 'B'; out[1] = 'M'; number(2, out.size(), 4); number(10, 54, 4); number(14, 40, 4);
    number(18, w, 4); number(22, h, 4); number(26, 1, 2); number(28, 24, 2);
    for (size_t i = 54; i < out.size(); ++i) out[i] = (i + seed) % 251;
    return out;
}
static vision_payload::Digest digest(const raw_buffer & b) {
    auto hex = hash_sha256_hex(b.data(), b.size()); vision_payload::Digest result{};
    for (size_t i = 0; i < result.size(); ++i) result[i] = std::stoul(hex.substr(2*i, 2), nullptr, 16);
    return result;
}
struct Wake final : vc::Wake { std::atomic<bool> pending{false}; void post() noexcept override { pending = true; } };
struct Events {
    std::shared_ptr<Wake> wake = std::make_shared<Wake>();
    std::shared_ptr<vc::Inbox> inbox = std::make_shared<vc::Inbox>(8, wake);
    std::vector<vc::Event> events;
    vc::Event take(vc::Event::Kind kind, uint64_t task) {
        std::optional<vc::Event> found;
        await([&] {
            if (wake->pending.exchange(false)) while (auto e = inbox->pop()) events.push_back(*e);
            for (auto it = events.begin(); it != events.end(); ++it) if (it->kind == kind && it->task == task) {
                found = *it; events.erase(it); return true;
            }
            return false;
        }); return *found;
    }
};
struct Output final : ea::Sink {
    std::atomic<unsigned> tokens{0};
    bool info(std::string_view) noexcept override { return true; }
    bool line(eh::Identity, const EngineLine & line) noexcept override { if (line.kind == EngineLine::TOKEN) ++tokens; return true; }
};
struct CpuOnlySafety final : vs::Safety {
    std::shared_ptr<ea::Adapter> adapter;
    bool leased = false, engine = false;
    std::atomic<bool> revoked{false};
    bool acquire(uint64_t) noexcept override { assert(!leased && !revoked); leased = true; return true; }
    void service_failed() noexcept override { revoked = true; }
    bool engine_stopped() noexcept override { return adapter->stopped(); }
    bool certify(uint64_t, bool, bool) noexcept override { assert(!engine); return true; }
    void release(uint64_t) noexcept override { assert(leased && !engine); leased = false; }
    bool to_engine(eh::Identity) noexcept override { assert(leased && !engine); if (revoked) return false; engine = true; return true; }
    bool to_reader(eh::Identity) noexcept override { assert(engine); if (!adapter->idle() && !adapter->stopped()) return false; engine = false; return true; }
};
static size_t calls(char kind) {
    std::ifstream f(counter); std::string contents(std::istreambuf_iterator<char>(f), {});
    return std::count(contents.begin(), contents.end(), kind);
}
static size_t encodes() { return calls('E'); }
static int native_engine(const char * gate = nullptr) {
    std::cout << "READY 32768 stop\n" << std::flush;
    std::string line; unsigned requests = 0;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT") return 0;
        std::istringstream stream(line); std::string command, path, keys, pad, tokens; unsigned maximum;
        stream >> command >> maximum;
        assert(maximum == 1);
        const bool text_only = command == "GEN";
        if (text_only) {
            stream >> tokens; assert(!tokens.empty());
            std::string extra; assert(!(stream >> extra)); // no image options or path
            assert(gate);
            std::ifstream expected(std::string(gate)+"/expected-text"); std::string ids;
            assert(std::getline(expected,ids) && ids == tokens); // exact parent token oracle
            if (const char * path = std::getenv("PR4_NATIVE_COUNTER")) { std::ofstream f(path,std::ios::app); f << 'X'; }
        } else {
        stream >> keys >> pad >> path >> tokens;
        assert(command == "GENI" && keys.substr(0,9) == "img_keys=" && pad.substr(0,10) == "image_pad=");
        std::ifstream file(path, std::ios::binary); assert(file);
        unsigned images = 0; std::vector<char> first;
        while (file.peek() != EOF) {
            int32_t header[5]{}; file.read(reinterpret_cast<char *>(header), sizeof(header));
            assert(file && header[0] == 0x31455653 && header[1] == header[2]*header[3] && header[4] > 0);
            std::vector<float> rows(size_t(header[1]) * header[4]); file.read(reinterpret_cast<char *>(rows.data()), rows.size()*4); assert(file);
            for (float value : rows) assert(std::isfinite(value));
            if (images == 0) first.assign(reinterpret_cast<char *>(rows.data()), reinterpret_cast<char *>(rows.data()+rows.size()));
            if (images == 1) assert(first != std::vector<char>(reinterpret_cast<char *>(rows.data()), reinterpret_cast<char *>(rows.data()+rows.size())));
            if (images == 2) assert(first == std::vector<char>(reinterpret_cast<char *>(rows.data()), reinterpret_cast<char *>(rows.data()+rows.size())));
            ++images;
        }
        assert(images == 3);
        if (const char * capture = std::getenv("PR5_NATIVE_SIGNATURE")) {
            std::ifstream sve(path,std::ios::binary);
            std::string bytes = keys + "|" + pad + "|" + tokens + "|";
            bytes.append(std::istreambuf_iterator<char>(sve),{});
            std::ofstream signature(capture,std::ios::app);
            signature << hash_sha256_hex(reinterpret_cast<const uint8_t *>(bytes.data()),bytes.size()) << '\n';
            assert(signature.good()); // exact IDs, keys, ordered SVE1 header/row bytes
        }
        }
        if (const char * path = std::getenv("PR4_NATIVE_COUNTER")) { std::ofstream f(path,std::ios::app); f << 'G'; f.flush(); assert(f.good()); }
        if (gate && !requests++) {
            { std::ofstream entered(std::string(gate)+"/entered"); entered << '1'; }
            while (::access((std::string(gate)+"/release").c_str(),F_OK) != 0) std::this_thread::sleep_for(2ms);
            assert(std::ifstream(path,std::ios::binary)); // file still owned at terminal drain
        }
        const auto prompt = 1 + std::count(tokens.begin(), tokens.end(), ',');
        const bool cancel = gate && ::access((std::string(gate)+"/cancel").c_str(),F_OK) == 0;
        const bool soft = gate && ::access((std::string(gate)+"/soft").c_str(),F_OK) == 0;
        if (cancel || soft) {
            if (soft) std::cout << "T 1\n" << std::flush;
            else { std::ofstream active(std::string(gate)+"/active"); active << '1'; }
            std::string stop; assert(std::getline(std::cin,stop) && stop == "STOP");
            if (const char * path = std::getenv("PR4_NATIVE_COUNTER")) { std::ofstream f(path,std::ios::app); f << 'S'; }
            if (cancel) {
                std::cout << "T 1\nPP 9 9\n" << std::flush;
                { std::ofstream draining(std::string(gate)+"/draining"); draining << '1'; }
                while (::access((std::string(gate)+"/drain-release").c_str(),F_OK) != 0) std::this_thread::sleep_for(2ms);
            }
            assert(text_only || std::ifstream(path,std::ios::binary));
            std::cout << "DONE 1 " << prompt << " 0 0 cancel\n" << std::flush;
        } else std::cout << "T 1\nDONE 1 " << prompt << " 0 0 stop\n" << std::flush;
    }
    return 1;
}
#include "vision-native-queue.inc"
#include "vision-native-cache.inc"
#include "pr6-reader-codecs.inc"
int main(int argc, char ** argv) {
    if ((argc == 2 || argc == 3) && std::string(argv[1]) == "engine") return native_engine(argc == 3 ? argv[2] : nullptr);
    const bool reader = argc == 6 && std::string(argv[1]) == "reader";
    const bool codecs = argc == 5 && std::string(argv[1]) == "--codecs";
    assert(reader || codecs || argc == 4); // model, projector, guard
    const char * model_path = argv[reader || codecs ? 2 : 1], * projector_path = argv[reader || codecs ? 3 : 2];
    assert(!ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU));
    auto mp = llama_model_default_params(); mp.vocab_only = true; mp.n_gpu_layers = 0;
    auto * model = llama_model_load_from_file(model_path, mp); assert(model);
    const auto config = configuration(model, projector_path); const auto caps = limits(config);
    if (reader) {
        auto cp = mtmd_context_params_default(); cp.use_gpu = false; cp.warmup = false; cp.n_threads = 4; cp.image_max_tokens = 64;
        auto * ctx = mtmd_init_from_file(projector_path, model, cp); assert(ctx);
        auto rc = vision_reader::serve(ctx, config, caps, {4096, 16*1024*1024, 48*1024*1024, 65536, 180s}, std::stoull(argv[5]), 3);
        mtmd_free(ctx); llama_model_free(model); return rc;
    }
    if (codecs) {
        pr6_reader_codecs::run(argv[0],argv[4],model_path,projector_path,model,config,caps);
        llama_model_free(model); return 0;
    }
    // Oracle projector exists only before the supervised request starts.
    strata_context_impl oracle; oracle.model = model; oracle.vocab = llama_model_get_vocab(model); oracle.init_literals();
    common_params params; params.mmproj.path = projector_path; params.mmproj_use_gpu = false;
    params.image_max_tokens = 64; params.cpuparams.n_threads = 4;
    std::string error; assert(oracle.init_vision(params, error));
    const std::string marker = get_media_marker();
    const auto a = bmp(1), b = bmp(2);
    auto image = [](const raw_buffer & source) {
        std::string url = "data:image/bmp;base64,";
        base64::encode(source.begin(), source.end(), std::back_inserter(url));
        return json{{"type","image_url"},{"image_url",{{"url",url}}}};
    };
    oracle.chat_params.use_jinja = true; oracle.chat_params.prefill_assistant = false;
    oracle.chat_params.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    oracle.chat_params.tmpls = common_chat_templates_init(model, "");
    oracle.chat_params.allow_image = true; oracle.chat_params.allow_audio = oracle.chat_params.allow_video = false;
    oracle.chat_params.enable_thinking = false;
    json body = {{"messages",json::array({
        {{"role","user"},{"content","Earlier <|im_end|> is literal."}},
        {{"role","assistant"},{"content","Earlier response."}},
        {{"role","user"},{"content",json::array({{{"type","text"},{"text","First <|image_pad|>:"}},image(a),
            {{"type","text"},{"text","Second:"}},image(b),image(a),{{"type","text"},{"text"," end"}}})}}
    })}, {"chat_template_kwargs",{{"enable_thinking",false}}}};
    std::vector<raw_buffer> sources;
    assert(oracle.literals.mark_request(body.at("messages"), nullptr));
    auto parsed = oaicompat_chat_params_parse(body, oracle.chat_params, sources);
    std::string prompt = parsed.at("prompt").get<std::string>();
    const auto spans = oracle.literals.unmark(prompt);
    assert(!spans.empty() && sources == std::vector<raw_buffer>({a,b,a}));
    auto expected = oracle.tokenize_prompt(prompt, spans, sources);
    mtmd_free(oracle.mctx); oracle.mctx = nullptr; // new parent path has no projector
    char name[] = "/tmp/pr4-native-count-XXXXXX"; int fd = ::mkstemp(name); assert(fd >= 0); ::close(fd); counter = name;
    auto sink = std::make_shared<Output>();
    ea::Limits engine_caps{4*1024*1024, 4096, 16384, 64, config.vocabulary_size, 10s, 1s, 1s};
    auto engine = std::make_shared<ea::Adapter>(ea::Launch{argv[3], argv[0], {argv[0], "engine"}, {}}, engine_caps, 2, sink);
    auto safety = std::make_shared<CpuOnlySafety>(); safety->adapter = engine;
    auto artifacts = std::make_shared<vision_artifact::Budget>(vision_artifact::Usage{64*1024*1024, 16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536, 4});
    Events events;
    {
        vs::Launch launch{argv[3], argv[0], {argv[0], "reader", model_path, projector_path, "cpu"},
            {"CUDA_VISIBLE_DEVICES=-1", "NVIDIA_VISIBLE_DEVICES=void", "PR4_NATIVE_COUNTER="+counter}, 100, config, caps};
        vs::Supervisor supervisor(launch, {180s, 1s, 1s, 5s, 50ms, 600s, 3}, 2, safety, events.inbox, std::make_shared<vs::Reaper>(), engine);
        assert(supervisor.start(1)); events.take(vc::Event::Kind::ready, 1);
        auto submit = [&](uint64_t job, std::shared_ptr<vc::Request> control) {
            auto s = std::make_unique<io::Submission>(); s->job = job; s->control = control; s->artifacts = artifacts; s->recipes = recipes;
            s->limits = {65536, 16384, 32768}; s->snapshot_bytes = 65536; s->prepare = 180s; s->encode = 180s; s->cleanup = 5s;
            for (size_t i = 0; i < sources.size(); ++i) {
                vision_artifact::Artifact::Ptr source; auto hash = digest(sources[i]);
                assert(vision_artifact::Artifact::source(*artifacts, {0, job, uint32_t(i), hash},
                    {reinterpret_cast<const char *>(sources[i].data()), sources[i].size()}, caps.source_bytes, deadline(), never, source) == vision_artifact::Result::ok);
                s->inputs.push_back({source, vision_reader::source_id(hash)});
            }
            return s;
        };
        std::vector<std::string> ids; for (const auto & source : sources) ids.push_back(vision_reader::source_id(digest(source)));
        for (unsigned request = 0; request < 4; ++request) {
            const io::Identity id{20+request, supervisor.instance(), 11+request};
            auto control = std::make_shared<vc::Request>(deadline()); auto submission = submit(id.job, control);
            assert(supervisor.prepare(id.task, submission)); events.take(vc::Event::Kind::prepared, id.task);
            auto prepared = supervisor.prepared(); assert(prepared);
            auto composed = comp::compose(oracle.vocab, prompt, spans, marker, *prepared, ids, config, caps, 32768,
                [&](const std::string & text) { llama_tokens tokens; oracle.tokenize_plain(text, tokens); return tokens; });
            vision_recipe::compare(expected, composed);
            if (request == 0) {
                io::Prepared corrupt; corrupt.recipes = prepared->recipes; corrupt.recipes[0].metadata.pop_back();
                bool refused = false;
                try { (void)comp::compose(oracle.vocab, prompt, spans, marker, corrupt, ids, config, caps, 32768,
                    [&](const std::string & text) { llama_tokens tokens; oracle.tokenize_plain(text, tokens); return tokens; }); }
                catch (const std::runtime_error &) { refused = true; }
                assert(refused && encodes() == 0);
            }
            const int64_t context = request == 1 ? composed.size() + ENGINE_CONTEXT_RESERVE : 32768;
            const int64_t room = context - int64_t(composed.size()) - ENGINE_CONTEXT_RESERVE;
            if (room <= 0) { // actual full-cell context refusal, before native encode
                assert(room == 0 && supervisor.admit(id, false)); events.take(vc::Event::Kind::completed, id.task);
            } else {
                const auto before = encodes(); assert(supervisor.admit(id, true));
                if (request == 2) { // interrupt actual native encode; safe terminal then explicit recovery
                    await([&] { return encodes() > before; }); control->cancel(vc::Reason::disconnect);
                    events.take(vc::Event::Kind::failed, id.task);
                } else {
                    events.take(vc::Event::Kind::encoded, id.task); auto encoded = supervisor.encoded(); assert(encoded);
                    assert(!comp::File::create(composed, *encoded, oracle.image_pad_id, config.vocabulary_size, 1, deadline(), never));
                    auto file = comp::File::create(composed, *encoded, oracle.image_pad_id, config.vocabulary_size, 16*1024*1024, deadline(), never); assert(file);
                    const auto path = file->path();
                    const std::string header = "GENI 1 img_keys=" + file->keys + " image_pad=" + std::to_string(oracle.image_pad_id) + " " + path;
                    server_task task; task.params.sampling.temp = 0;
                    std::vector<uint64_t> engine_keys;
                    for (const auto & source : sources) {
                        const auto bytes = vision_cache::identity_bytes({config,digest(source)});
                        const auto key = PictureCache::engine_key(hash_sha256_hex(reinterpret_cast<const uint8_t *>(bytes.data()),bytes.size()));
                        assert(key != PictureCache::engine_key(hash_sha256_hex(source.data(),source.size())));
                        engine_keys.push_back(key);
                    }
                    assert(engine_keys[0] == engine_keys[2] && engine_keys[0] != engine_keys[1]);
                    assert(header == oracle.gen_header(task, 1, path, engine_keys));
                    auto resources = ea::Request::create({id.task,id.instance,id.job,1}, header, file->ids, engine_caps, file); assert(resources);
                    assert(supervisor.generate(id, resources, 10s, 5s)); resources.reset(); file.reset();
                    events.take(vc::Event::Kind::engine_submitted, id.task);
                    events.take(vc::Event::Kind::job_terminal, id.task);
                    assert(supervisor.terminal()->outcome == vision_job::Outcome::success && sink->tokens == (request == 0 ? 1u : 2u));
                    assert(::access(path.c_str(), F_OK) != 0 && encodes() == before + 3);
                }
            }
            if (request == 1 || request == 2) events.take(vc::Event::Kind::job_terminal, id.task);
            auto terminal = supervisor.terminal(); assert(terminal);
            assert(terminal->outcome == (request == 1 ? vision_job::Outcome::refused : request == 2 ? vision_job::Outcome::cancelled : vision_job::Outcome::success));
            if (request == 1) assert(encodes() == 3);
            assert(supervisor.acknowledge(id));
            if (request == 2) {
                await([&] { return supervisor.state() == vs::State::backoff; });
                std::this_thread::sleep_for(60ms); assert(supervisor.start(2)); events.take(vc::Event::Kind::ready, 2);
                assert(supervisor.instance() == 101); // no automatic failed-request retry
            }
        }
        // A valid BMP header declaring excessive dimensions must be refused
        // before the decoder, bitmap allocation, preprocessing or encode.
        auto oversized = bmp(3); const uint32_t width = 5000;
        for (unsigned b = 0; b < 4; ++b) oversized[18+b] = width >> (8*b);
        int x = 0, y = 0, channels = 0;
        assert(stbi_info_from_memory(oversized.data(), oversized.size(), &x, &y, &channels) && x == 5000);
        const auto before_decode = calls('D'), before_encode = encodes();
        const io::Identity rejected_id{30, supervisor.instance(), 30};
        auto rejected_control = std::make_shared<vc::Request>(deadline()); auto rejected = submit(30, rejected_control);
        auto hash = digest(oversized); vision_artifact::Artifact::Ptr source;
        assert(vision_artifact::Artifact::source(*artifacts, {0,30,0,hash},
            {reinterpret_cast<const char *>(oversized.data()), oversized.size()}, caps.source_bytes, deadline(), never, source) == vision_artifact::Result::ok);
        rejected->inputs[0] = {source, vision_reader::source_id(hash)}; source.reset();
        assert(supervisor.prepare(rejected_id.task, rejected)); events.take(vc::Event::Kind::failed, rejected_id.task);
        events.take(vc::Event::Kind::job_terminal, rejected_id.task);
        assert(supervisor.terminal()->outcome == vision_job::Outcome::failure && supervisor.terminal()->error_class == 2 && supervisor.acknowledge(rejected_id));
        assert(calls('D') == before_decode && encodes() == before_encode && sink->tokens == 2);
    }
    events.take(vc::Event::Kind::stopped, 0);
    assert(engine->stopped() && !safety->revoked && !safety->leased);
    assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0);
    queue_scenario(argv[0],argv[3],model_path,projector_path,config,caps,oracle.vocab,oracle.image_pad_id,prompt,spans,sources,oracle.chat_params);
    queue_cache_scenario(argv[0],argv[3],model_path,projector_path,config,caps,oracle.vocab,oracle.image_pad_id,prompt,spans,sources);
    ::unlink(counter.c_str()); oracle.chat_params.tmpls.reset(); oracle.model = nullptr; llama_model_free(model);
    std::cout << "PR4_NATIVE_PASS real_reader=1 real_rows=1 prompt_parity=1 sve1_geni=1 context_refusal=1 encode_cancel=1 explicit_recovery=1 predecode_refusal=1 gpu=none queue=integrated\n";
}
