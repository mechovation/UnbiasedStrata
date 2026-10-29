// White-box lifecycle fixtures: include the implementation without changing production visibility or adding
// fault switches. Built separately by context-lifecycle.sh against a CPU-only frontend build's static libraries.
#include "../strata_context.cpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <spawn.h>
#include <stdexcept>

// Test-only link seam: simulate unsupported chunk kinds using upstream's real image fixture. No invented
// mtmd serialization, production fault switch or dependency changes are needed.
static bool override_media_type = false;
static mtmd_input_chunk_type media_type = MTMD_INPUT_CHUNK_TYPE_AUDIO;
static const mtmd_input_chunk * only_override_chunk = nullptr;
static bool override_media_count = false;
static size_t media_count = 0;
static std::map<const mtmd_input_chunk *, std::string> fixture_ids;
static unsigned picture_file_attempts = 0;
extern "C" int __real_mkstemp(char *);
extern "C" int __wrap_mkstemp(char * name) {
    constexpr char prefix[] = "/tmp/unbiased-strata-pictures-";
    if (std::strncmp(name, prefix, sizeof(prefix) - 1) == 0) ++picture_file_attempts;
    return __real_mkstemp(name);
}
extern "C" mtmd_input_chunk_type __real_mtmd_input_chunk_get_type(const mtmd_input_chunk *);
extern "C" mtmd_input_chunk_type __wrap_mtmd_input_chunk_get_type(const mtmd_input_chunk * chunk) {
    return override_media_type && (!only_override_chunk || chunk == only_override_chunk)
        ? media_type : __real_mtmd_input_chunk_get_type(chunk);
}
extern "C" size_t __real_mtmd_input_chunk_get_n_tokens(const mtmd_input_chunk *);
extern "C" size_t __wrap_mtmd_input_chunk_get_n_tokens(const mtmd_input_chunk * chunk) {
    return override_media_count ? media_count : __real_mtmd_input_chunk_get_n_tokens(chunk);
}
extern "C" const char * __real_mtmd_input_chunk_get_id(const mtmd_input_chunk *);
extern "C" const char * __wrap_mtmd_input_chunk_get_id(const mtmd_input_chunk * chunk) {
    const auto it = fixture_ids.find(chunk);
    return it == fixture_ids.end() ? __real_mtmd_input_chunk_get_id(chunk) : it->second.c_str();
}

namespace {
void require(bool condition, const char * message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

server_task task(strata_context_impl & ctx, int id, int count = 3, bool media = false) {
    server_task t(SERVER_TASK_TYPE_COMPLETION);
    t.id = id;
    t.tokens = server_tokens(llama_tokens(count, media ? LLAMA_TOKEN_NULL : 100), media);
    t.params.n_predict = 4;
    ctx.queue_results.add_waiting_task_id(id);
    return t;
}

server_task_result_ptr result(strata_context_impl & ctx, int id) {
    return ctx.queue_results.recv_with_timeout({id}, 0);
}

void no_success_accounting(const strata_context_impl & ctx) {
    require(ctx.metrics.prompt.count == 0 && ctx.metrics.predict.count == 0,
            "failure/cancellation contributed successful metrics");
}

void start(strata_context_impl & ctx, const std::string & fake) {
    std::string err;
    require(ctx.engine.start(fake, {fake}, err), err.c_str());
    require(ctx.engine.wait_ready(err, 3), err.c_str());
    ctx.n_ctx = 64;
}

std::string owned_file(Slot & slot) {
    char name[] = "/tmp/us2-slot-file-XXXXXX";
    const int fd = ::mkstemp(name);
    require(fd >= 0, "mkstemp");
    ::close(fd);
    slot.images_path = name;
    return name;
}

void expect_error(strata_context_impl & ctx, int id, error_type type, const char * message) {
    auto res = result(ctx, id);
    const auto * error = dynamic_cast<server_task_result_error *>(res.get());
    require(error && error->err_type == type && error->err_msg == message, "unexpected error result");
    require(!result(ctx, id), "duplicate terminal result");
    require(!ctx.slot.active(), "failed task retained slot");
    no_success_accounting(ctx);
}

void idempotent_cleanup() {
    strata_context_impl ctx;
    ctx.slot.task = std::make_unique<server_task>(task(ctx, 1));
    const auto path = owned_file(ctx.slot);
    ctx.queue_tasks.defer(task(ctx, 2));
    ctx.queue_tasks.defer(task(ctx, 3));
    ctx.finish_slot();
    require(::access(path.c_str(), F_OK) != 0, "owned picture file leaked");
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 1, "cleanup did not promote exactly one");
    ctx.finish_slot();
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 1, "duplicate cleanup promoted again");
    require(!result(ctx, 1), "cleanup manufactured result");
    no_success_accounting(ctx);
}

void early_failures(const std::string & fake) {
    strata_context_impl ctx;
    start(ctx, fake);
    // Synthetic media token cells intentionally have no encodable chunks/projector. This is a branch-order
    // oracle, not real-projector qualification: baseline returns the picture-preparation error instead of 400.
    ctx.queue_tasks.defer(task(ctx, 2));
    ctx.queue_tasks.defer(task(ctx, 3));
    ctx.launch(task(ctx, 1, 56, true));
    auto res = result(ctx, 1);
    auto * error = dynamic_cast<server_task_result_error *>(res.get());
    require(error && error->err_type == ERROR_TYPE_EXCEED_CONTEXT_SIZE,
            "media context admission ran after picture preparation");
    require(error->n_prompt_tokens == 56 && error->n_ctx == 64, "context metadata changed");
    require(!result(ctx, 1) && !ctx.slot.active(), "media refusal not terminal exactly once");
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 1, "media refusal stranded follower");
    require(ctx.slot.images_path.empty(), "media refusal retained picture path");
    ctx.launch(task(ctx, 4, 0));
    expect_error(ctx, 4, ERROR_TYPE_INVALID_REQUEST, "the prompt is empty");
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 0, "empty rejection stranded follower");

    ctx.n_ctx = 4096;
    ctx.queue_tasks.defer(task(ctx, 6));
    // A supported upstream image chunk (not just orphan null cells) must reach picture preparation.
    mtmd::input_chunks chunks(mtmd_test_create_input_chunks());
    auto image = task(ctx, 5, 0);
    image.tokens.has_mtmd = true;
    image.tokens.push_back(chunks[1]);
    image.tokens.push_back(100);
    ctx.launch(std::move(image));
    expect_error(ctx, 5, ERROR_TYPE_SERVER, "images need the server started with --mmproj");
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 0, "picture-preparation failure stranded follower");
    ctx.engine.quit();
    ctx.queue_tasks.defer(task(ctx, 8));
    ctx.launch(task(ctx, 7));
    expect_error(ctx, 7, ERROR_TYPE_UNAVAILABLE, "the engine is not running");
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 0, "unavailable rejection stranded follower");
}

void failed_writes(const std::string & fake, bool stop) {
    ::setenv("FAKE_CLOSE_INPUT", "1", 1);
    {
        strata_context_impl ctx;
        start(ctx, fake); // READY is emitted only after the child has closed stdin: no timing race.
        ctx.queue_tasks.defer(task(ctx, 2));
        if (stop) {
            ctx.slot.task = std::make_unique<server_task>(task(ctx, 1));
            ctx.slot.started = true;
            const auto path = owned_file(ctx.slot);
            ctx.cancel_current();
            require(!ctx.slot.active(), "failed STOP retained slot");
            require(::access(path.c_str(), F_OK) != 0, "failed STOP leaked picture file");
            require(!result(ctx, 1), "cancelled request received result");
        } else {
            ctx.launch(task(ctx, 1));
            expect_error(ctx, 1, ERROR_TYPE_UNAVAILABLE, "the engine is not running");
        }
        require(!ctx.engine.alive(), "failed write did not mark engine unavailable");
        require(ctx.queue_tasks.queue_tasks_deferred_size() == 0, "failed write stranded follower");
        ctx.launch(task(ctx, 3)); // Must reject, never write a successor GEN to the unavailable engine.
        expect_error(ctx, 3, ERROR_TYPE_UNAVAILABLE, "the engine is not running");
    }
    ::unsetenv("FAKE_CLOSE_INPUT");
}

void late_cancel_and_accounting() {
    strata_context_impl ctx;
    ctx.slot.task = std::make_unique<server_task>(task(ctx, 10, 0));
    // Simulate repeated/late cancellation of completed A after successor C has acquired the slot.
    for (int repeat = 0; repeat < 2; ++repeat) {
        server_task cancel(SERVER_TASK_TYPE_CANCEL);
        cancel.id_target = 9;
        ctx.process_new_task(std::move(cancel), false);
        require(ctx.slot.active() && ctx.slot.task->id == 10 && !ctx.slot.cancelled,
                "late cancellation affected successor");
    }
    ctx.slot.stats.update_prompt_start();
    ctx.slot.stats.update_prompt_last();
    ctx.slot.stats.n_gen = 1;
    ctx.slot.stats.update_gen_last();
    EngineLine done;
    done.kind = EngineLine::DONE;
    done.prompt_n = 0;
    done.finish = "stop";
    ctx.on_done(done);
    auto final = result(ctx, 10);
    require(final && !final->is_error() && final->is_stop(), "missing successful terminal result");
    require(ctx.metrics.predict.count == 1, "success accounting changed");
    ctx.finish_slot();
    require(ctx.metrics.predict.count == 1 && !result(ctx, 10), "cleanup double-counted success");
}

void terminal_cleanup() {
    for (const std::string mode : {"err", "mismatch", "nonfinite", "cancel", "eof"}) {
        strata_context_impl ctx;
        ctx.slot.task = std::make_unique<server_task>(task(ctx, 1));
        ctx.slot.stats.update_prompt_start();
        const auto path = owned_file(ctx.slot);
        ctx.queue_tasks.defer(task(ctx, 2));
        ctx.queue_tasks.defer(task(ctx, 3));
        EngineLine line;
        line.kind = mode == "err" ? EngineLine::ERR : mode == "eof" ? EngineLine::EOF_ : EngineLine::DONE;
        line.prompt_n = mode == "mismatch" ? 4 : 3;
        line.finish = mode == "nonfinite" ? "nonfinite" : "stop";
        ctx.slot.cancelled = mode == "cancel";
        ctx.handle_line(line);
        require(!ctx.slot.active() && ::access(path.c_str(), F_OK) != 0, "terminal leaked task/file");
        require(ctx.queue_tasks.queue_tasks_deferred_size() == 1, "terminal did not promote exactly one");
        auto res = result(ctx, 1);
        require(mode == "cancel" ? !res : res && res->is_error(), "wrong terminal result");
        require(!result(ctx, 1), "duplicate terminal error");
        no_success_accounting(ctx);
        ctx.finish_slot();
        require(ctx.queue_tasks.queue_tasks_deferred_size() == 1, "terminal cleanup not idempotent");
        require(ctx.engine_dead == (mode == "eof"), "engine-death contract changed");
    }
}

void iterative_chain() {
    strata_context_impl ctx; // Unavailable engine gives a fast, deterministic early rejection for every task.
    constexpr int count = 2000;
    ctx.slot.task = std::make_unique<server_task>(task(ctx, 0));
    for (int id = 1; id <= count; ++id) {
        ctx.queue_tasks.defer(task(ctx, id));
    }
    int processed = 0;
    ctx.queue_tasks.on_new_task([&](server_task && t, bool yielding) {
        require(t.id == ++processed, "chain promotion out of order or duplicated");
        ctx.process_new_task(std::move(t), yielding);
        if (processed == count) {
            ctx.queue_tasks.terminate();
        }
        return true;
    });
    ctx.queue_tasks.on_update_slots([] {});
    ctx.finish_slot();
    ctx.queue_tasks.start_loop(); // Test runner supplies an external wall timeout as well.
    require(processed == count && !ctx.slot.active(), "large iterative chain stalled");
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 0, "large chain retained deferred work");
    for (int id = 1; id <= count; ++id) {
        expect_error(ctx, id, ERROR_TYPE_UNAVAILABLE, "the engine is not running");
    }
}
// PR2 fixtures. This pointer is only a capability sentinel: cache hits/text dispatch must never dereference it.
// Any real projector execution is deliberately outside this no-model test.
mtmd_context * capability_sentinel() {
    static char sentinel;
    return reinterpret_cast<mtmd_context *>(&sentinel);
}

struct CleanupSlot {
    strata_context_impl & ctx;
    ~CleanupSlot() { ctx.slot.release(); ctx.mctx = nullptr; }
};

void drain_probe(strata_context_impl & ctx, int prompt_n, bool handle = false) {
    const int64_t deadline = Engine::now_ms() + 3000;
    EngineLine line;
    while (Engine::now_ms() < deadline) {
        if (!ctx.engine.next_line(line, 50)) continue;
        require(line.kind != EngineLine::ERR && line.kind != EngineLine::EOF_, "probe rejected transport");
        if (handle) {
            require(ctx.slot.images_path.empty() || ::access(ctx.slot.images_path.c_str(), F_OK) == 0,
                    "picture file released before terminal handling");
        }
        if (line.kind == EngineLine::DONE) {
            require(line.prompt_n == prompt_n, "transport changed prompt cell count");
            if (handle) ctx.handle_line(line);
            return;
        }
        if (handle) ctx.handle_line(line);
    }
    throw std::runtime_error("probe terminal deadline");
}

std::string last_gen(const std::string & path) {
    std::ifstream f(path);
    std::string line, last;
    while (std::getline(f, line)) {
        const auto event = json::parse(line);
        if (event.at("event") == "command") {
            const auto command = event.at("line").get<std::string>();
            if (command.rfind("GEN ", 0) == 0 || command.rfind("GENI ", 0) == 0) last = command;
        }
    }
    return last;
}

std::string wire_ids(const llama_tokens & tokens) {
    std::string out;
    for (auto id : tokens) out += (out.empty() ? "" : ",") + std::to_string(id);
    return out;
}

void text_transport(const std::string & fake, const char * vocabulary) {
    strata_context_impl ctx;
    start(ctx, fake); // This fixture runs in a fresh exec, before logging/model-loading starts threads.
    CleanupSlot cleanup{ctx};
    ctx.n_ctx = 4096;
    auto params = llama_model_default_params();
    params.vocab_only = true;
    params.n_gpu_layers = 0;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
        llama_model_load_from_file(vocabulary, params), llama_model_free);
    require(model != nullptr, "vocabulary fixture unavailable");
    ctx.vocab = llama_model_get_vocab(model.get());
    ctx.literals = strata_frontend::Literals::from_texts({"<|im_end|>", "<think>", "<|image_pad|>"});
    for (bool literal : {false, true}) {
        for (bool stream : {false, true}) {
            const auto message = literal ? "tags <|im_end|> <think> <|image_pad|> are text" : "Say hello";
            std::string prompt = "<|im_start|>user\n" + ctx.literals.mark(message) +
                                 "<|im_end|>\n<|im_start|>assistant\n";
            auto plain = ctx.literals.unmark(prompt);
            ctx.mctx = nullptr;
            auto tokens = ctx.tokenize_prompt(prompt, plain, {});
            const auto expected = tokens.get_text_tokens();
            // Reproduce the flag mtmd sets when a projector is loaded, even though there is no media.
            tokens.has_mtmd = true;
            ctx.mctx = capability_sentinel();
            auto t = task(ctx, literal ? 21 : 20, 0);
            t.tokens = std::move(tokens);
            t.params.stream = stream;
            picture_file_attempts = 0;
            ctx.launch(std::move(t));
            require(picture_file_attempts == 0, "text attempted picture-file creation");
            require(ctx.slot.active() && ctx.slot.started, "text with mtmd capability failed launch");
            require(ctx.slot.images_path.empty() && ctx.slot.image_keys.empty(), "text created a picture file/key");
            require(ctx.mctx == capability_sentinel(), "text changed advertised projector capability");
            drain_probe(ctx, expected.size());
            const auto command = last_gen(std::getenv("FAKE_CAPTURE"));
            require(command.rfind("GEN ", 0) == 0, "text-only task used GENI");
            require(command.substr(command.rfind(' ') + 1) == wire_ids(expected), "text/literal/BOS/EOS IDs changed");
            ctx.finish_slot();
            ctx.mctx = nullptr;
        }
    }
}

void image_transport(const std::string & fake, const char * vocabulary) {
    strata_context_impl ctx;
    start(ctx, fake);
    CleanupSlot cleanup{ctx};
    ctx.n_ctx = 4096;
    ctx.mctx = capability_sentinel();
    ctx.image_pad_id = 777;
    auto params = llama_model_default_params();
    params.vocab_only = true;
    params.n_gpu_layers = 0;
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
        llama_model_load_from_file(vocabulary, params), llama_model_free);
    require(model != nullptr, "vocabulary fixture unavailable");
    ctx.vocab = llama_model_get_vocab(model.get());
    mtmd::input_chunks chunks(mtmd_test_create_input_chunks());
    const int n_image = int(mtmd_input_chunk_get_n_tokens(chunks[1]));
    const std::string a = "11111111111111111111111111111111", b = "22222222222222222222222222222222";
    std::vector<strata_frontend::PictureCache::Rows> held;
    for (int which = 0; which < 2; ++which) {
        strata_frontend::PictureRows rows;
        rows.n_tok = n_image; rows.nx = n_image; rows.ny = 1; rows.width = 2;
        rows.embd.resize(size_t(n_image) * 2);
        for (size_t i = 0; i < rows.embd.size(); ++i) rows.embd[i] = float(i + which * 10000);
        held.push_back(ctx.pictures.put(which == 0 ? a : b, std::move(rows)));
    }
    auto t = task(ctx, 30, 0);
    t.tokens.has_mtmd = true;
    t.tokens.push_back(101);
    t.tokens.push_back(chunks[1]);
    t.tokens.push_back(202);
    t.tokens.push_back(chunks[1]);
    t.tokens.push_back(303);
    t.tokens.push_back(chunks[1]);
    t.tokens.push_back(404);
    fixture_ids[t.tokens.find_chunk(1).get()] = a;
    fixture_ids[t.tokens.find_chunk(n_image + 2).get()] = b;
    fixture_ids[t.tokens.find_chunk(2 * n_image + 3).get()] = a;
    picture_file_attempts = 0;
    ctx.launch(std::move(t));
    require(picture_file_attempts == 1, "image transport did not create exactly one picture file");
    require(ctx.slot.active() && ctx.slot.started && !ctx.slot.images_path.empty(), "images did not use picture transport");
    const auto path = ctx.slot.images_path;
    require(ctx.pictures.stats().hits == 3 && ctx.pictures.stats().misses == 0, "fixture attempted projector execution");
    require(ctx.slot.image_keys == std::vector<uint64_t>{
        strata_frontend::PictureCache::engine_key(a), strata_frontend::PictureCache::engine_key(b),
        strata_frontend::PictureCache::engine_key(a)}, "image key occurrence order changed");
    std::ifstream f(path, std::ios::binary);
    for (int occurrence = 0; occurrence < 3; ++occurrence) {
        int32_t header[5];
        f.read(reinterpret_cast<char *>(header), sizeof header);
        require(f && header[0] == 0x31455653 && header[1] == n_image && header[2] == n_image && header[3] == 1 && header[4] == 2,
                "ordered SVE1 header changed");
        std::vector<float> data(size_t(n_image) * 2);
        f.read(reinterpret_cast<char *>(data.data()), data.size() * sizeof(float));
        require(f && data == held[occurrence == 1 ? 1 : 0]->embd, "ordered SVE1 payload changed");
    }
    require(f.peek() == std::char_traits<char>::eof(), "extra image record/trailing data");
    auto follower = task(ctx, 31);
    follower.tokens.has_mtmd = true;
    ctx.process_new_task(std::move(follower), false);
    require(ctx.queue_tasks.queue_tasks_deferred_size() == 1, "text follower was not deferred");
    drain_probe(ctx, 3 * n_image + 4, true);
    const auto command = last_gen(std::getenv("FAKE_CAPTURE"));
    require(command.rfind("GENI ", 0) == 0, "image task did not use GENI");
    llama_tokens expected{101};
    expected.insert(expected.end(), n_image, 777); expected.push_back(202);
    expected.insert(expected.end(), n_image, 777); expected.push_back(303);
    expected.insert(expected.end(), n_image, 777); expected.push_back(404);
    require(command.substr(command.rfind(' ') + 1) == wire_ids(expected), "image/text occurrence order changed");
    require(command.find(" img_keys=1111111111111111,2222222222222222,1111111111111111 ") != std::string::npos,
            "GENI image-key order changed");
    require(!ctx.slot.active() && ::access(path.c_str(), F_OK) != 0, "image file survived terminal cleanup");
    auto final = result(ctx, 30);
    require(final && final->is_stop() && !final->is_error(), "image request did not finish normally");
    ctx.queue_tasks.on_new_task([&](server_task && next, bool yielding) {
        return ctx.process_new_task(std::move(next), yielding);
    });
    ctx.queue_tasks.on_update_slots([&] {
        ctx.update_slots();
        if (!ctx.slot.active()) ctx.queue_tasks.terminate();
    });
    ctx.queue_tasks.start_loop();
    final = result(ctx, 31);
    require(final && final->is_stop() && !final->is_error(), "text follower did not complete after image");
    require(last_gen(std::getenv("FAKE_CAPTURE")).rfind("GEN ", 0) == 0, "text after image used GENI");
    require(picture_file_attempts == 1, "text follower created another picture file");
    fixture_ids.clear();
    ctx.mctx = nullptr;
}

void unsupported_media(const std::string & fake) {
    strata_context_impl ctx;
    start(ctx, fake);
    CleanupSlot cleanup{ctx};
    ctx.n_ctx = 4096;
    ctx.mctx = capability_sentinel();
    mtmd::input_chunks chunks(mtmd_test_create_input_chunks());
    for (auto kind : {MTMD_INPUT_CHUNK_TYPE_AUDIO, MTMD_INPUT_CHUNK_TYPE_TEXT}) {
        auto t = task(ctx, 40, 0);
        t.tokens.has_mtmd = true;
        t.tokens.push_back(chunks[1]);
        t.tokens.push_back(100);
        ctx.queue_tasks.defer(task(ctx, 41));
        media_type = kind;
        override_media_type = true;
        picture_file_attempts = 0;
        ctx.launch(std::move(t));
        override_media_type = false;
        require(picture_file_attempts == 0, "unsupported media reached picture writer");
        expect_error(ctx, 40, ERROR_TYPE_NOT_SUPPORTED, "unsupported or missing media chunk; only pictures are supported");
        require(ctx.queue_tasks.queue_tasks_deferred_size() == 0, "unsupported media stranded follower");
    }
    for (bool flag : {false, true}) {
        auto t = task(ctx, 42, 3, true); // Orphan null cells, with or without the mtmd capability flag.
        t.tokens.has_mtmd = flag;
        picture_file_attempts = 0;
        ctx.launch(std::move(t));
        require(picture_file_attempts == 0, "orphan null cells reached picture writer");
        expect_error(ctx, 42, ERROR_TYPE_NOT_SUPPORTED, "unsupported or missing media chunk; only pictures are supported");
    }
    // A valid image followed by unsupported media must fail validation before any partial encode/write.
    auto mixed = task(ctx, 43, 0);
    mixed.tokens.has_mtmd = true;
    mixed.tokens.push_back(chunks[1]);
    mixed.tokens.push_back(chunks[1]);
    mixed.tokens.push_back(100);
    const size_t n = mtmd_input_chunk_get_n_tokens(chunks[1]);
    only_override_chunk = mixed.tokens.find_chunk(n).get();
    media_type = MTMD_INPUT_CHUNK_TYPE_AUDIO;
    override_media_type = true;
    picture_file_attempts = 0;
    ctx.launch(std::move(mixed));
    override_media_type = false;
    only_override_chunk = nullptr;
    require(picture_file_attempts == 0 && ctx.pictures.stats().misses == 0, "partial image work before unsupported media");
    expect_error(ctx, 43, ERROR_TYPE_NOT_SUPPORTED, "unsupported or missing media chunk; only pictures are supported");

    for (size_t count : {size_t(0), n + 2, n + 1}) {
        auto malformed = task(ctx, 44, 0);
        malformed.tokens.has_mtmd = true;
        malformed.tokens.push_back(chunks[1]);
        malformed.tokens.push_back(100);
        media_count = count;
        override_media_count = true;
        picture_file_attempts = 0;
        ctx.launch(std::move(malformed));
        override_media_count = false;
        require(picture_file_attempts == 0, "malformed image span reached picture writer");
        expect_error(ctx, 44, ERROR_TYPE_INVALID_REQUEST, "invalid picture token layout");
    }
    ctx.mctx = nullptr;
}

void media_dispatch(const std::string & fake, const char * vocabulary, const std::string & mode) {
    char capture[] = "/tmp/us2-media-capture-XXXXXX";
    const int fd = ::mkstemp(capture);
    require(fd >= 0, "capture file creation");
    ::close(fd);
    ::setenv("FAKE_CAPTURE", capture, 1);
    if (mode == "text") {
        ::setenv("FAKE_REFUSE_GENI", "1", 1);
        text_transport(fake, vocabulary);
        ::unsetenv("FAKE_REFUSE_GENI");
    } else if (mode == "images") {
        image_transport(fake, vocabulary);
    } else {
        unsupported_media(fake);
    }
    ::unsetenv("FAKE_CAPTURE");
    ::unlink(capture);
}
} // namespace

int main(int argc, char ** argv) {
    if (argc != 3 && argc != 5) return 2;
    ::signal(SIGPIPE, SIG_IGN);
    // Each fixture gets a clean exec. In particular, Engine::start must not fork after a preceding fixture's
    // asynchronous logger, queue worker or model loading has started threads in this process.
    if (argc == 3) {
        int failures = 0;
        for (const char * name : {"cleanup", "early", "send", "stop", "late", "terminal", "chain", "text", "images", "unsupported"}) {
            char * args[] = {argv[0], argv[1], argv[2], const_cast<char *>("--case"), const_cast<char *>(name), nullptr};
            pid_t pid;
            require(::posix_spawn(&pid, argv[0], nullptr, nullptr, args, environ) == 0, "fixture clean spawn failed");
            int status;
            require(::waitpid(pid, &status, 0) == pid, "fixture wait failed");
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                std::fprintf(stderr, "FAIL fixture: %s\n", name);
                ++failures;
            }
        }
        if (!failures) std::puts("context lifecycle and media dispatch: all checks passed");
        return failures ? 1 : 0;
    }
    try {
        const std::string mode = argv[4];
        if (mode == "cleanup") idempotent_cleanup();
        else if (mode == "early") early_failures(argv[1]);
        else if (mode == "send" || mode == "stop") failed_writes(argv[1], mode == "stop");
        else if (mode == "late") late_cancel_and_accounting();
        else if (mode == "terminal") terminal_cleanup();
        else if (mode == "chain") iterative_chain();
        else if (mode == "text" || mode == "images" || mode == "unsupported") media_dispatch(argv[1], argv[2], mode);
        else return 2;
    } catch (const std::exception & e) {
        std::fprintf(stderr, "FAIL context lifecycle: %s\n", e.what());
        return 1;
    }
}
