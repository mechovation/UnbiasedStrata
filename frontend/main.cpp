// UnbiasedStrata: the front end's program. Mirrors llama.cpp's tools/server/server.cpp without router mode, MCP,
// server-side tools, the web UI and model downloads.
//
//   unbiased-strata --engine build/bin/strata-poly -m model.gguf [llama-server options] -- <engine options>
//
// Options before `--` are llama-server's (common_params_parse): -m/--model gives the GGUF the vocabulary and chat
// template are read from, --host/--port/--api-key/--threads-http the listener, --temp/--top-k/... the sampling
// defaults, --reasoning-format/--chat-template-kwargs/--jinja the chat handling. Options after `--` go to the engine
// as they are; `--serve` is added. The engine's READY line gives the context size.

#include "strata_context.hpp"

#include "arg.h"
#include "common.h"
#include "llama.h"
#include "log.h"
#include "server-common.h"
#include "server-stream.h"

#include <atomic>
#include <clocale>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

volatile sig_atomic_t shutdown_signal = 0;
void signal_handler(int signal) {
    if (shutdown_signal) ::_exit(1);
    shutdown_signal = signal; // no locks, allocation, or native IO in a signal handler
}

void usage() {
    std::fprintf(stderr,
        "usage: unbiased-strata --engine <strata|strata-poly|path> -m <model.gguf> [llama-server options] -- <engine options>\n"
        "\n"
        "  --engine PATH          the engine binary; a bare name is looked up beside this program\n"
        "  -m, --model FILE       the GGUF the vocabulary and chat template are read from (no tensors are loaded)\n"
        "  --host, --port, --api-key, --threads-http, --temp, --top-k, --top-p, --min-p, --reasoning-format,\n"
        "  --chat-template-kwargs, --alias ...   llama-server options (see llama-server --help)\n"
        "  --mmproj FILE          the model's vision projector: image_url parts are encoded in this process and\n"
        "                         handed to the engine (Qwen family); on the CPU (--threads N) unless --mmproj-gpu\n"
        "  --mmproj-gpu           encode on the card: about 1 GB of VRAM plus work buffers, taken before the engine\n"
        "                         sizes its expert cache only if the encoder loads first; for cards with room\n"
        "  --native-vision FILE   opt-in supervised reader/engine candidate; explicit JSON resource profile required\n"
        "                         requires --mmproj, --image-max-tokens and companion reader/guard executables\n"
        "  --picture-cache N      keep the encoded rows of the last N pictures (default 64, up to 1 GiB) so a chat\n"
        "                         that resends its pictures every turn encodes each once; 0 turns it off\n"
        "  --                     everything after it is passed to the engine; --serve is added\n"
        "\n"
        "example (Qwen3.6-35B-A3B from its GGUF):\n"
        "  unbiased-strata --engine strata-poly -m Qwen3.6-35B-A3B-UD-Q3_K_M.gguf --port 8080 \\\n"
        "      -- --model Qwen3.6-35B-A3B-UD-Q3_K_M.gguf --max-context 32768\n");
}

std::string self_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) {
        return ".";
    }
    buf[n] = 0;
    std::string p(buf);
    const size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool file_exists(const std::string & p) {
    return ::access(p.c_str(), X_OK) == 0;
}

} // namespace

int main(int argc, char ** argv) {
    std::setlocale(LC_NUMERIC, "C");
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    // split: ours (--engine), llama-server's, and after `--` the engine's
    std::string engine_exe, native_profile;
    std::vector<std::string> engine_args;
    std::vector<char *> llama_argv;
    bool mmproj_gpu = false; // the vision encoder on the card: only where the engine leaves room (a 48 GB card)
    long picture_cache = 64; // --picture-cache N: encoded pictures kept for the turns that resend them
    llama_argv.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--") {
            for (++i; i < argc; ++i) {
                engine_args.push_back(argv[i]);
            }
            break;
        }
        if (a == "--engine") {
            if (i + 1 >= argc) { usage(); return 2; }
            engine_exe = argv[++i];
            continue;
        }
        if (a.rfind("--engine=", 0) == 0) {
            engine_exe = a.substr(9);
            continue;
        }
        if (a == "--native-vision") {
            if (i + 1 >= argc) { usage(); return 2; }
            native_profile = argv[++i]; continue;
        }
        if (a == "--mmproj-gpu") {
            mmproj_gpu = true;
            continue;
        }
        if (a == "--picture-cache") {
            if (i + 1 >= argc) { usage(); return 2; }
            picture_cache = std::strtol(argv[++i], nullptr, 10);
            if (picture_cache < 0) { usage(); return 2; }
            continue;
        }
        llama_argv.push_back(argv[i]);
    }
    if (engine_exe.empty()) {
        usage();
        return 2;
    }
    if (engine_exe.find('/') == std::string::npos) {
        const std::string beside = self_dir() + "/" + engine_exe;
        if (file_exists(beside)) {
            engine_exe = beside;
        }
    }
    if (!file_exists(engine_exe)) {
        std::fprintf(stderr, "unbiased-strata: engine not found or not executable: %s\n", engine_exe.c_str());
        return 2;
    }

    // Argument registration also enumerates backends, so mask before parsing,
    // not just before llama_backend_init(). The parent never owns GPU work.
    if (!native_profile.empty()) {
        if (::setenv("CUDA_VISIBLE_DEVICES","-1",1) || ::setenv("CUDA_DEVICE_ORDER","PCI_BUS_ID",1)) return 2;
        // Expired/denied safety proof requires immediate whole-service fail-stop.
        // Do not invoke ggml's terminate hook: it may fork a debugger, retain
        // unbounded diagnostics or fault while trying to print a backtrace.
        std::set_terminate([] { std::_Exit(70); });
    }

    common_params params;
    common_init();
    server_stream_session_manager_start();

    if (!common_params_parse((int) llama_argv.size(), llama_argv.data(), params, LLAMA_EXAMPLE_SERVER)) {
        return 1;
    }
    // llama-server encodes images on the GPU unless --no-mmproj-offload; here the card is the engine's, so the
    // encoder runs on the CPU unless --mmproj-gpu asks for the card (upstream PolyStrata's choice as well)
    params.mmproj_use_gpu = mmproj_gpu;
    if (params.model.path.empty()) {
        std::fprintf(stderr, "unbiased-strata: -m <model.gguf> is required (the vocabulary and chat template come from it)\n");
        return 2;
    }
    auto model_name = params.model.get_name();
    if (params.model_alias.empty() && !model_name.empty()) {
        params.model_alias.insert(model_name);
    }

    if (!native_profile.empty()) {
        if (params.n_threads_http < 1) {
            std::fprintf(stderr,"unbiased-strata: native mode requires an explicit --threads-http limit\n"); return 2;
        }
        // Only supervised children receive the approved GPU UUID through their
        // independently constructed runtime environments; keep the parent masked.
    }
    llama_backend_init();
    struct BackendGuard { ~BackendGuard() { llama_backend_free(); } } backend_guard;
    llama_numa_init(params.numa);

    strata_context ctx_server;
    if (!native_profile.empty()) {
        const auto guard = self_dir() + "/strata-process-guard", reader = self_dir() + "/strata-vision-reader";
        if (!file_exists(guard) || !file_exists(reader)) {
            std::fprintf(stderr,"unbiased-strata: native companion executables are missing\n"); return 2;
        }
        ctx_server.configure_native(native_profile,guard,reader);
    }
    ctx_server.set_picture_cache((size_t) picture_cache, (size_t) 1 << 30);
    server_http_context ctx_http;
    if (!ctx_http.init(params)) {
        SRV_ERR("%s", "failed to initialize HTTP server\n");
        return 1;
    }

    // routes first (handlers are not called until is_ready), then the engine (forked before any thread exists),
    // then the HTTP thread, which answers /health with 503 while the engine loads
    ctx_server.init_routes(ctx_http);

    std::string err;
    if (!ctx_server.start_engine(params, engine_exe, engine_args, err)) {
        SRV_ERR("could not start the engine: %s\n", err.c_str());
        return 1;
    }

    auto clean_up = [&]() {
        SRV_INF("%s", "cleaning up before exit...\n");
        server_stream_session_manager_stop();
        ctx_http.stop();
        ctx_server.terminate();
    };

    if (!ctx_http.start()) {
        clean_up();
        SRV_ERR("%s", "exiting due to HTTP server error\n");
        return 1;
    }

    if (!ctx_server.load(params, err)) {
        SRV_ERR("%s\n", err.c_str());
        clean_up();
        if (ctx_http.thread.joinable()) {
            ctx_http.thread.join();
        }
        return 1;
    }
    ctx_http.is_ready.store(true);
    SRV_INF("listening on %s, context %lld tokens\n", ctx_http.listening_address.c_str(), ctx_server.n_ctx());

    struct sigaction sigint_action;
    sigint_action.sa_handler = signal_handler;
    sigemptyset(&sigint_action.sa_mask);
    sigint_action.sa_flags = 0;
    sigaction(SIGINT,  &sigint_action, NULL);
    sigaction(SIGTERM, &sigint_action, NULL);

    // the engine's exit ends this process too, with a non-zero code, so a unit with Restart=on-failure brings both
    // back. A replacement engine is not forked from this multithreaded process (UnbiasedStrata 1 main.cpp:873).
    std::atomic<bool> engine_died{false};
    std::atomic<bool> stop_supervisor{false};
    std::thread supervisor([&]() {
        std::string why;
        while (!stop_supervisor) {
            if (shutdown_signal) { ctx_server.terminate(); return; }
            if (ctx_server.engine_exited(why)) {
                SRV_ERR("%s; shutting down so the service manager can restart\n", why.c_str());
                engine_died = true;
                ctx_server.terminate();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    ctx_server.start_loop(); // blocks until terminate()

    stop_supervisor = true;
    supervisor.join();
    { std::string why; if (ctx_server.engine_exited(why)) engine_died = true; }
    clean_up();
    if (ctx_http.thread.joinable()) {
        ctx_http.thread.join();
    }
    return engine_died ? 1 : 0;
}
