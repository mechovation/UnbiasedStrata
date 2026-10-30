// Explicit PR4 launch contract. Header-only; link the installed OpenSSL libcrypto.
// Reader argv is {absolute_reader_executable, INSTANCE}; FD 3 is the control socket.
// Never pass environ to either launcher: use the owned environment vectors below.
#pragma once
#include "vision_queue.hpp"
#include "vision_reader.hpp"
#include "gguf.h"
#include "nlohmann/json.hpp"
#include <openssl/evp.h>
#include <sys/resource.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <linux/magic.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <charconv>

namespace strata_frontend::vision_runtime {
using Json = nlohmann::ordered_json;
inline constexpr const char * approved_gpu = "GPU-d14daf49-45c8-58c5-cf61-5a40342672af";
inline constexpr const char * pinned_abi = "44072a19fb94d0bed63d381d42f6237373f91d54/mtmd-v2/qwen3vl-merge2/pr4-runtime-v1";
inline constexpr size_t json_cap = 65536;
[[noreturn]] inline void invalid() { throw std::runtime_error("invalid native vision configuration"); }
inline void require(bool ok) { if (!ok) invalid(); }

// No JSON exceptions (which may quote environment values) escape this boundary.
inline Json parse(std::string_view text) {
    require(!text.empty() && text.size() <= json_cap);
    try {
        std::vector<std::set<std::string>> keys;
        auto callback = [&](int depth, Json::parse_event_t event, Json & value) {
            require(depth <= 12);
            if (event == Json::parse_event_t::object_start) keys.emplace_back();
            if (event == Json::parse_event_t::key) require(!keys.empty() && keys.back().insert(value.get<std::string>()).second);
            if (event == Json::parse_event_t::object_end) { require(!keys.empty()); keys.pop_back(); }
            return true;
        };
        return Json::parse(text.begin(), text.end(), callback);
    } catch (...) { invalid(); }
}
inline void fields(const Json & j, std::initializer_list<const char *> names) {
    require(j.is_object() && j.size() == names.size());
    for (auto name : names) require(j.contains(name));
}
inline uint64_t number(const Json & j, const char * name, uint64_t maximum = INT64_MAX, uint64_t minimum = 1) {
    require(j.contains(name) && j[name].is_number_unsigned());
    const auto n = j[name].get<uint64_t>(); require(n >= minimum && n <= maximum); return n;
}
inline std::string text(const Json & j, const char * name) {
    require(j.contains(name) && j[name].is_string());
    auto s = j[name].get<std::string>(); require(!s.empty() && s.size() <= 4096);
    for (unsigned char c : s) require(c >= 32 && c != 127);
    return s;
}
inline std::string read_small(const std::string & path, size_t cap = json_cap) {
    std::ifstream file(path, std::ios::binary); require(bool(file));
    std::string result; char block[4096];
    while (file) { file.read(block, sizeof(block)); auto n = size_t(file.gcount()); require(n <= cap - result.size()); result.append(block, n); }
    require(file.eof()); return result;
}
inline std::string canonical_path(const std::string & path) {
    require(!path.empty() && path.size() <= 4096);
    for (unsigned char c : path) require(c >= 32 && c != 127);
    std::error_code ec; auto p = std::filesystem::canonical(path, ec); require(!ec);
    require(std::filesystem::is_regular_file(p, ec) && !ec);
    auto result = p.string(); require(result.size() <= 4096);
    for (unsigned char c : result) require(c >= 32 && c != 127);
    return result;
}
class Sha256 {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx_{EVP_MD_CTX_new(), EVP_MD_CTX_free};
public:
    Sha256() { require(ctx_ && EVP_DigestInit_ex(ctx_.get(), EVP_sha256(), nullptr) == 1); }
    void add(const void * p, size_t n) { require(EVP_DigestUpdate(ctx_.get(), p, n) == 1); }
    vision_payload::Digest finish() {
        vision_payload::Digest d{}; unsigned n = 0;
        require(EVP_DigestFinal_ex(ctx_.get(), d.data(), &n) == 1 && n == d.size()); return d;
    }
};
inline std::string hex(const vision_payload::Digest & d) {
    static constexpr char digits[] = "0123456789abcdef"; std::string s;
    for (auto b : d) { s += digits[b >> 4]; s += digits[b & 15]; } return s;
}
inline vision_payload::Digest hash(std::string_view s) { Sha256 h; h.add(s.data(), s.size()); return h.finish(); }
inline vision_payload::Digest hash_file(const std::string & path, uint64_t bytes) {
    std::ifstream f(path, std::ios::binary); require(bool(f)); Sha256 h; char block[65536];
    while (bytes) {
        size_t n = size_t(std::min<uint64_t>(bytes, sizeof(block)));
        f.read(block, n); require(size_t(f.gcount()) == n); h.add(block, n); bytes -= n;
    }
    return h.finish();
}
inline std::string lower(std::string s) { for (auto & c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return s; }
inline void validate_engine_args(const std::vector<std::string> & args) {
    require(args.size() <= 256); size_t bytes = 0;
    for (const auto & arg : args) {
        require(!arg.empty() && arg.size() <= 4096 && bytes <= 32768 - arg.size()); bytes += arg.size();
        for (unsigned char c : arg) require(c >= 32 && c != 127);
        const auto s = lower(arg.substr(0, arg.find('=')));
        // Reviewed numeric cache policies are not prompts or credential tokens.
        // Preserve the deployed residency policy without allowing similarly named
        // unreviewed options through the sensitive/device denylist.
        const bool cache_policy = s == "--prompt-cache" || s == "--conversation-cache-min-tokens";
        // Native --gpu overrides CUDA_VISIBLE_DEVICES. No device/split/RPC escape.
        for (const char * forbidden : {"gpu", "device", "split", "rpc", "api-key", "token", "password", "prompt", "proxy", "url"})
            require(cache_policy || s.empty() || s[0] != '-' || s.find(forbidden) == std::string::npos);
        for (const char * forbidden : {"-ngl", "-mg", "-sm", "-ts", "-dev", "-p", "-f", "--file", "--hf-repo", "--hf-file", "--host", "--port"})
            require(s != forbidden);
    }
}
inline void validate_environment(const Json & env) {
    require(env.is_object() && env.size() <= 20);
    for (auto it = env.begin(); it != env.end(); ++it) {
        auto v = text(env, it.key().c_str()); const auto & k = it.key();
        if (k == "LD_LIBRARY_PATH") {
            require(v.size() <= 4096); size_t at = 0;
            do { auto end = v.find(':', at); auto part = v.substr(at, end - at);
                require(!part.empty() && part[0] == '/' && part.find("..") == std::string::npos && part.find('$') == std::string::npos);
                if (end == std::string::npos) break;
                at = end + 1;
            } while (true);
        } else if (k == "CUDA_MODULE_LOADING") require(v == "LAZY" || v == "EAGER");
        else {
            uint64_t maximum = 0, minimum = 0;
            if (k == "OMP_NUM_THREADS" || k == "STRATA_IO_THREADS" || k == "STRATA_FETCH_THREADS" ||
                k == "STRATA_CKPT_THREADS" || k == "STRATA_STAGER_THREADS") { maximum = 256; minimum = 1; }
            else if (k == "STRATA_IO_PREFETCH_DEPTH") { maximum = 64; minimum = 1; }
            else if (k == "STRATA_PF_FUSED" || k == "STRATA_PREFILL_CPU_SHARE" || k == "STRATA_PLE_PROBE" ||
                     k == "STRATA_IO_PREFETCH" || k == "STRATA_IO_PF_AHEAD") maximum = 1;
            else if (k == "STRATA_RESIDENT_HEADROOM_GIB") maximum = 128;
            else invalid(); // no wildcards, PATH, credentials, proxies, GPU overrides
            uint64_t n = 0; auto r = std::from_chars(v.data(), v.data() + v.size(), n);
            require(r.ec == std::errc{} && r.ptr == v.data() + v.size() && n >= minimum && n <= maximum);
        }
    }
}

struct Runtime {
    vision_payload::Config config{};
    vision_payload::Limits wire_limits{};
    vision_reader::Limits reader_limits{};
    vision_queue::Limits queue_limits{};
    vision_supervisor::Limits lifecycle{};
    vision_engine_adapter::Limits engine_limits{};
    std::vector<std::string> engine_environment, reader_environment;
    uint64_t artifact_bytes = 0, recipe_bytes = 0;
    size_t artifact_fds = 0, recipe_snapshots = 0;
    std::string model_path, projector_path, engine_executable;
    std::vector<std::string> engine_args;
    uint32_t image_tokens = 0, threads = 0, image_min_tokens = 0, batch_max_tokens = 0;
    bool gpu = false;

    // Reads cgroup v2 limits in the current cgroup namespace. "max", absent
    // controllers and limits exceeding the explicitly reviewed ceiling fail closed.
    void validate_containment() const {
        for (auto entry : {std::pair<const char *, const char *>{"memory.max", "cgroup_memory_max"}, {"pids.max", "cgroup_pids_max"}}) {
            auto value = read_small(std::string("/sys/fs/cgroup/") + entry.first, 64);
            while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) value.pop_back();
            uint64_t n = 0; auto r = std::from_chars(value.data(), value.data() + value.size(), n);
            require(r.ec == std::errc{} && r.ptr == value.data() + value.size() && n && n <= number(profile_, entry.second));
        }
        // All job directories are in a bounded, memory-accounted candidate tmpfs.
        // This also bounds abandoned files across SIGKILL/service restart.
        struct statfs fs{}; struct statvfs space{};
        require(::statfs("/tmp", &fs) == 0 && fs.f_type == TMPFS_MAGIC && ::statvfs("/tmp", &space) == 0 && space.f_frsize &&
                uint64_t(space.f_blocks) <= queue_limits.disk_bytes / uint64_t(space.f_frsize));
    }
    static Runtime load(const std::string & profile_path, const std::string & model, const std::string & projector,
                        const std::string & engine, const std::vector<std::string> & args,
                        uint32_t image_tokens, uint32_t threads, bool gpu) {
        try {
            auto p = parse(read_small(profile_path)); Runtime r = settings(p);
            r.model_path = canonical_path(model); r.projector_path = canonical_path(projector);
            r.engine_executable = canonical_path(engine); r.engine_args = args;
            require(::access(r.engine_executable.c_str(), X_OK) == 0);
            r.image_tokens = image_tokens; r.threads = threads; r.gpu = gpu;
            r.validate_operations(); r.validate_containment(); r.resolve(); return r;
        } catch (...) { invalid(); }
    }
    // Parses/copies the sole immutable JSON value before clearenv; caller passes
    // a snapshot of the actual environment for exact verification (no credentials).
    static Runtime reader_config(std::string_view json, const std::vector<std::string> & actual_environment) {
        try {
            const auto j = parse(json);
            fields(j, {"profile", "model", "projector", "engine", "engine_args", "image_tokens", "threads", "gpu", "abi", "reader_policy", "engine_environment", "reader_environment", "family", "device", "model_sha256", "projector_sha256", "engine_sha256", "width", "stride", "vocabulary", "generation"});
            Runtime r = settings(j["profile"]);
            // Set limits BEFORE GGUF parsing, hashing or any backend initialization.
            r.apply_reader_rlimits();
            r.model_path = text(j, "model"); r.projector_path = text(j, "projector"); r.engine_executable = text(j, "engine");
            require(j["engine_args"].is_array());
            for (const auto & arg : j["engine_args"]) { require(arg.is_string()); r.engine_args.push_back(arg.get<std::string>()); }
            r.image_tokens = uint32_t(number(j, "image_tokens", INT32_MAX));
            r.threads = uint32_t(number(j, "threads", 256));
            require(j["gpu"].is_boolean()); r.gpu = j["gpu"].get<bool>();
            r.validate_operations();
            auto expected = r.environment(r.profile_["reader_environment"], true), actual = actual_environment;
            expected.push_back("STRATA_READER_CONFIG=" + std::string(json));
            std::sort(expected.begin(), expected.end()); std::sort(actual.begin(), actual.end());
            require(expected == actual); r.validate_containment();
            require(canonical_path(r.model_path) == r.model_path && canonical_path(r.projector_path) == r.projector_path &&
                    canonical_path(r.engine_executable) == r.engine_executable);
            r.resolve();
            // Canonical bytes, every resolved setting/path/hash and no extra fields.
            require(r.reader_json_ == json);
            return r;
        } catch (...) { invalid(); }
    }
    // Exposed for focused schema tests; does not inspect a model or activate a path.
    static Runtime settings(const Json & p) {
        fields(p, {"version", "gpu_uuid", "engine_environment", "reader_environment",
            "payload_bytes", "occurrences", "text_tokens", "source_bytes", "row_bytes",
            "metadata_bytes", "metadata_id_bytes", "grid_cells", "pixel_dimension", "pixels",
            "source_dimension", "source_pixels", "retained_rgb_bytes", "reader_recipe_bytes", "reader_io_ms",
            "pending_count", "pending_bytes", "output_records", "inbox_events", "prompt_cells", "disk_bytes", "snapshot_bytes",
            "job_recipe_bytes", "job_image_cells", "job_positions", "total_ms", "prepare_ms", "encode_ms", "cleanup_ms", "start_ms",
            "ready_ms", "quit_ms", "term_ms", "kill_ms", "backoff_ms", "restart_window_ms", "attempts",
            "engine_command_bytes", "engine_line_bytes", "engine_startup_bytes", "engine_lines_per_poll", "engine_ready_ms", "engine_quit_ms", "engine_term_ms",
            "artifact_bytes", "artifact_fds", "recipe_bytes", "recipe_snapshots",
            "reader_as_bytes", "reader_nofile", "reader_fsize_bytes", "reader_core_bytes", "cgroup_memory_max", "cgroup_pids_max",
            "parent_as_bytes", "parent_nofile", "parent_core_bytes", "engine_as_bytes", "engine_nofile", "engine_fsize_bytes", "engine_core_bytes",
            "image_min_tokens", "batch_max_tokens"});
        require(number(p, "version", 1) == 1 && text(p, "gpu_uuid") == approved_gpu);
        validate_environment(p["engine_environment"]); validate_environment(p["reader_environment"]);
        Runtime r; r.profile_ = p;
        auto n = [&](const char * name, uint64_t cap = INT64_MAX) { return number(p, name, cap); };
        auto u = [&](const char * name) { return uint32_t(n(name, INT32_MAX)); };
        auto z = [&](const char * name) { return size_t(n(name, std::min<uint64_t>(SIZE_MAX, INT64_MAX))); };
        auto ms = [&](const char * name) { return std::chrono::milliseconds(n(name, 86400000)); };
        r.wire_limits = {u("payload_bytes"), u("occurrences"), u("text_tokens"), n("source_bytes", 10485760), n("row_bytes"),
            {z("metadata_bytes"), z("metadata_id_bytes"), u("grid_cells"), u("pixel_dimension"), 0, n("pixels")}};
        r.reader_limits = {u("source_dimension"), n("source_pixels"), n("retained_rgb_bytes"), n("reader_recipe_bytes"), ms("reader_io_ms")};
        r.queue_limits = {z("pending_count"), z("pending_bytes"), z("output_records"), z("inbox_events"), u("prompt_cells"), n("disk_bytes"), n("snapshot_bytes"),
            {n("job_recipe_bytes"), n("job_image_cells"), n("job_positions")}, ms("total_ms"), ms("prepare_ms"), ms("encode_ms"), ms("cleanup_ms"), ms("start_ms")};
        r.lifecycle = {ms("ready_ms"), ms("quit_ms"), ms("term_ms"), ms("kill_ms"), ms("backoff_ms"), ms("restart_window_ms"), u("attempts")};
        r.engine_limits = {z("engine_command_bytes"), z("engine_line_bytes"), z("engine_startup_bytes"), z("engine_lines_per_poll"), 0,
            ms("engine_ready_ms"), ms("engine_quit_ms"), ms("engine_term_ms")};
        r.artifact_bytes = n("artifact_bytes"); r.artifact_fds = z("artifact_fds");
        r.recipe_bytes = n("recipe_bytes"); r.recipe_snapshots = z("recipe_snapshots");
        n("reader_as_bytes", std::numeric_limits<rlim_t>::max() - 1); n("reader_nofile", 1048576);
        n("reader_fsize_bytes", std::numeric_limits<rlim_t>::max() - 1);
        require(number(p, "reader_core_bytes", 0, 0) == 0);
        n("cgroup_memory_max"); n("cgroup_pids_max", INT32_MAX);
        n("parent_as_bytes", std::numeric_limits<rlim_t>::max() - 1); n("parent_nofile", 1048576);
        n("engine_as_bytes", std::numeric_limits<rlim_t>::max() - 1); n("engine_nofile", 1048576);
        n("engine_fsize_bytes", std::numeric_limits<rlim_t>::max() - 1);
        require(number(p, "parent_core_bytes", 0, 0) == 0 && number(p, "engine_core_bytes", 0, 0) == 0);
        r.image_min_tokens = u("image_min_tokens"); r.batch_max_tokens = u("batch_max_tokens");
        require(r.queue_limits.job.image_cells <= INT32_MAX && r.queue_limits.job.positions <= INT32_MAX);
        require(r.wire_limits.payload_bytes >= 80 && r.wire_limits.metadata.id_bytes >= 64 &&
                r.wire_limits.metadata.bytes <= r.wire_limits.payload_bytes && r.wire_limits.metadata.grid_cells <= INT32_MAX &&
                r.engine_limits.line_bytes >= 32 && r.engine_limits.startup_bytes >= r.engine_limits.line_bytes &&
                r.queue_limits.inbox_events >= 4 && n("reader_nofile") >= 16 && n("parent_nofile") >= 32 && n("engine_nofile") >= 16);
        const auto memory = n("cgroup_memory_max");
        require(r.queue_limits.pending_count <= n("cgroup_pids_max") && r.artifact_fds <= n("parent_nofile") &&
                r.queue_limits.output_records <= memory / sizeof(vision_queue::Output::Record) &&
                r.queue_limits.inbox_events <= memory / sizeof(vision_control::Event));
        uint64_t remaining = memory;
        for (auto bytes : {r.artifact_bytes,uint64_t(r.queue_limits.pending_bytes),r.recipe_bytes,uint64_t(r.wire_limits.payload_bytes),
                uint64_t(r.queue_limits.output_records)*sizeof(vision_queue::Output::Record),
                uint64_t(r.queue_limits.inbox_events)*sizeof(vision_control::Event)}) {
            require(bytes <= remaining); remaining -= bytes;
        }
        return r;
    }
    void apply_parent_rlimits() const {
        for (auto entry : {std::pair<int,const char *>{RLIMIT_AS,"parent_as_bytes"}, {RLIMIT_NOFILE,"parent_nofile"}, {RLIMIT_CORE,"parent_core_bytes"}}) {
            const rlim_t n = rlim_t(number(profile_,entry.second,std::numeric_limits<rlim_t>::max()-1,0));
            const rlimit limit{n,n}; require(::setrlimit(entry.first,&limit) == 0);
        }
    }
    struct Inspection {
        uint32_t width, stride, vocabulary;
        std::string family;
        vision_payload::Digest model_hash, projector_hash;
    };
    // Metadata-only qualification seam, also usable by offline CPU tests. Does not
    // initialize llama/mtmd, load weights, grant containment or launch authority.
    static Inspection inspect_artifacts(const std::string & model, const std::string & projector) {
        using Gguf = std::unique_ptr<gguf_context, decltype(&gguf_free)>;
        Gguf m(gguf_init_from_file(model.c_str(), {true, nullptr}), gguf_free);
        Gguf p(gguf_init_from_file(projector.c_str(), {true, nullptr}), gguf_free);
        require(m && p);
        const auto family = gg_string(m.get(), "general.architecture");
        require(family == "qwen3vl" || family == "qwen3vlmoe" || family == "qwen35" || family == "qwen35moe" || family == "qwen4exp");
                require(gg_string(p.get(), "clip.projector_type") == "qwen3vl_merger");
        const auto merge_key = gguf_find_key(p.get(), "clip.vision.spatial_merge_size");
        // The pinned qwen3vl_merger implementation fixes merge=2 when absent.
        require(merge_key < 0 || gg_u32(p.get(), "clip.vision.spatial_merge_size") == 2);
        const auto patch = gg_u32(p.get(), "clip.vision.patch_size"); require(patch <= uint32_t(INT32_MAX) / 2);
        const auto width = gg_u32(p.get(), "clip.vision.projection_dim");
        require(width == gg_u32(m.get(), family + ".embedding_length"));
        const auto vocab = gguf_find_key(m.get(), "tokenizer.ggml.tokens");
        require(vocab >= 0 && gguf_get_kv_type(m.get(), vocab) == GGUF_TYPE_ARRAY && gguf_get_arr_type(m.get(), vocab) == GGUF_TYPE_STRING);
        const auto count = gguf_get_arr_n(m.get(), vocab); require(count && count <= INT32_MAX);
        // gguf_get_meta_size measures reserialization, which can normalize tensor
        // dimensions. Bind the ORIGINAL metadata extent, including its padding;
        // never omit original descriptor bytes or read model tensor data.
        const auto serialized_meta_bytes = gguf_get_meta_size(m.get());
        const auto meta_bytes = gguf_get_data_offset(m.get());
        require(serialized_meta_bytes && meta_bytes && meta_bytes <= std::filesystem::file_size(model));
        const auto projector_bytes = std::filesystem::file_size(projector); require(projector_bytes);
        return {width, patch * 2, uint32_t(count), family, hash_file(model, meta_bytes), hash_file(projector, projector_bytes)};
    }
private:
    Json profile_;
    std::string reader_json_;
    void apply_reader_rlimits() const {
        for (auto entry : {std::pair<int, const char *>{RLIMIT_AS, "reader_as_bytes"}, {RLIMIT_NOFILE, "reader_nofile"},
                {RLIMIT_FSIZE, "reader_fsize_bytes"}, {RLIMIT_CORE, "reader_core_bytes"}}) {
            rlim_t n = rlim_t(number(profile_, entry.second, std::numeric_limits<rlim_t>::max() - 1, 0));
            rlimit limit{n, n}; require(::setrlimit(entry.first, &limit) == 0);
        }
    }
    void validate_operations() const {
        require(image_tokens && image_tokens <= INT32_MAX && image_tokens <= wire_limits.metadata.grid_cells &&
                threads && threads <= 256 && number(profile_, "image_min_tokens") <= image_tokens);
        validate_engine_args(engine_args);
    }
    static std::string gg_string(const gguf_context * g, const char * name) {
        auto k = gguf_find_key(g, name); require(k >= 0 && gguf_get_kv_type(g, k) == GGUF_TYPE_STRING);
        return gguf_get_val_str(g, k);
    }
    static uint32_t gg_u32(const gguf_context * g, const std::string & name) {
        auto k = gguf_find_key(g, name.c_str()); require(k >= 0 && gguf_get_kv_type(g, k) == GGUF_TYPE_UINT32);
        auto n = gguf_get_val_u32(g, k); require(n && n <= INT32_MAX); return n;
    }
    std::vector<std::string> environment(const Json & env, bool reader) const {
        std::vector<std::string> out;
        for (auto it = env.begin(); it != env.end(); ++it) out.push_back(it.key() + "=" + it.value().get<std::string>());
        out.push_back(std::string("CUDA_VISIBLE_DEVICES=") + (gpu ? approved_gpu : "-1"));
        out.push_back(std::string("NVIDIA_VISIBLE_DEVICES=") + (gpu ? approved_gpu : "void"));
        out.push_back("CUDA_DEVICE_ORDER=PCI_BUS_ID");
        out.push_back("HIP_VISIBLE_DEVICES=-1"); out.push_back("ROCR_VISIBLE_DEVICES=-1");
        const std::string prefix = reader ? "reader_" : "engine_";
        for (const auto & field : {std::pair<const char *, const char *>{"STRATA_PROCESS_AS_BYTES","as_bytes"},
                {"STRATA_PROCESS_NOFILE","nofile"}, {"STRATA_PROCESS_FSIZE_BYTES","fsize_bytes"}, {"STRATA_PROCESS_CORE_BYTES","core_bytes"}})
            out.push_back(std::string(field.first) + "=" + std::to_string(number(profile_,(prefix+field.second).c_str(),INT64_MAX,0)));
        return out;
    }
    void resolve() {
        const auto inspected = inspect_artifacts(model_path, projector_path);
        config.width = inspected.width; config.pixel_stride = inspected.stride;
        wire_limits.metadata.pixel_stride = inspected.stride;
        config.vocabulary_size = inspected.vocabulary; engine_limits.vocabulary = inspected.vocabulary;
        if (gpu) {
            const std::string digits = "d14daf4945c858c5cf615a40342672af";
            for (size_t i = 0; i < config.device.size(); ++i) {
                unsigned n = 0; auto result = std::from_chars(digits.data() + 2*i, digits.data() + 2*i + 2, n, 16);
                require(result.ec == std::errc{}); config.device[i] = uint8_t(n);
            }
        }
        Json resolved = {{"profile", profile_}, {"model", model_path}, {"projector", projector_path}, {"engine", engine_executable},
            {"engine_args", engine_args}, {"image_tokens", image_tokens}, {"threads", threads}, {"gpu", gpu},
            {"abi", std::string(pinned_abi) + "/ptr" + std::to_string(sizeof(void *)) + "/size" + std::to_string(sizeof(size_t)) +
                (uint16_t(1) == *reinterpret_cast<const uint8_t *>(&endian_probe_) ? "/le" : "/be")},
            {"reader_policy", {{"vocab_only", true}, {"n_gpu_layers", 0}, {"warmup", gpu}, {"print_timings", false},
                {"flash_attention", "auto"}, {"control_fd", 3}, {"vision_only", true}, {"merge", 2}}},
            {"engine_environment", environment(profile_["engine_environment"], false)}, {"reader_environment", environment(profile_["reader_environment"], true)},
            {"family", inspected.family}, {"device", gpu ? approved_gpu : "cpu"},
            {"model_sha256", hex(inspected.model_hash)}, {"projector_sha256", hex(inspected.projector_hash)},
            {"engine_sha256", hex(hash_file(engine_executable, std::filesystem::file_size(engine_executable))) },
            {"width", config.width}, {"stride", config.pixel_stride}, {"vocabulary", config.vocabulary_size}};
        config.generation = hash(resolved.dump()); resolved["generation"] = hex(config.generation);
        reader_json_ = resolved.dump(); require(reader_json_.size() <= json_cap);
        engine_environment = environment(profile_["engine_environment"], false);
        reader_environment = environment(profile_["reader_environment"], true);
        reader_environment.push_back("STRATA_READER_CONFIG=" + reader_json_);
    }
    inline static constexpr uint16_t endian_probe_ = 1;
};
} // namespace strata_frontend::vision_runtime
