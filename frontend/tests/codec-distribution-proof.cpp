// CPU-only candidate artifact + real compatible LGPL replacement proof.
// No decoder changes, model weights, backend initialization, HTTP or GPU access.
#include "../vision_runtime.hpp"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "vendor/stb/stb_image.h"
#include <iostream>
#include <sys/wait.h>
extern char ** environ;
namespace vr = strata_frontend::vision_runtime;
namespace vi = strata_frontend::vision_image;
namespace fs = std::filesystem;
static void check(bool ok) { if (!ok) throw std::runtime_error("distribution check failed"); }
static std::string digest(const fs::path & p) { return vr::hex(vr::hash_file(p.string(), fs::file_size(p))); }
static bool inside(const fs::path & p, const fs::path & dir) {
    const auto rel = fs::canonical(p).lexically_relative(fs::canonical(dir));
    return !rel.empty() && !rel.is_absolute() && *rel.begin() != "..";
}
static void manifest(const fs::path & bundle) {
    std::ifstream in(bundle / "BUNDLE.sha256"); check(bool(in));
    std::string line; size_t count = 0; std::set<std::string> listed;
    while (std::getline(in, line)) {
        check(line.size() > 66 && line.substr(64, 2) == "  ");
        const auto name = line.substr(66); const auto file = bundle / name;
        check(listed.insert(name).second && inside(file, bundle));
        check(digest(file) == line.substr(0, 64)); ++count;
    }
    check(in.eof() && count >= 25);
    for (const auto * name : {"bin/unbiased-strata", "bin/strata-vision-reader", "bin/strata-process-guard",
            "share/APPLICATION-LICENSE", "share/vision-codecs/NOTICE.md", "share/vision-codecs/licenses/LGPL-2.1.txt",
            "share/vision-codecs/licenses/libexif12.copyright", "share/vision-codecs/licenses/libjpeg-turbo8.copyright",
            "share/vision-codecs/source/libexif_0.6.24.orig.tar.gz",
            "share/vision-codecs/source/libexif_0.6.24-1ubuntu0.24.04.1.debian.tar.xz",
            "share/vision-codecs/source/libexif_0.6.24-1ubuntu0.24.04.1.dsc"}) check(listed.count(name));
    for (const auto & p : fs::recursive_directory_iterator(bundle))
        if (!p.is_directory() && p.path() != bundle / "BUNDLE.sha256")
            check(listed.count(p.path().lexically_relative(bundle).string()));
}
int main(int argc, char ** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--clean-config") {
            const char * config = std::getenv("STRATA_READER_CONFIG"); check(config);
            std::vector<std::string> env; for (char ** e = environ; *e; ++e) env.emplace_back(*e);
            const auto runtime = vr::Runtime::reader_config(config, env);
            std::cout << "PR6_CLEAN_CONFIG_PASS generation=" << vr::hex(runtime.config.generation) << '\n';
            return 0;
        }
        check(argc == 5); // BUNDLE EXPECTED_EXIF_DIRECTORY CPU_PROFILE WORKSPACE
        const auto bundle = fs::canonical(argv[1]); const auto exif_dir = fs::canonical(argv[2]);
        manifest(bundle);
        std::string exif_hash;
        const auto libraries = vi::libraries(); check(libraries.size() == 6);
        for (const auto & lib : libraries) {
            check(inside(lib.path, lib.role == "exif" ? exif_dir : bundle / "lib"));
            if (lib.role == "exif") exif_hash = digest(lib.path);
        }
        check(!exif_hash.empty());
        const auto workspace = fs::canonical(argv[4]);
        const auto model = workspace / "model.gguf", projector = workspace / "projector.gguf", profile = workspace / "profile.json";
        auto m = gguf_init_empty(); check(m);
        gguf_set_val_str(m, "general.architecture", "qwen4exp");
        gguf_set_val_u32(m, "qwen4exp.embedding_length", 2560);
        const char * tokens[] = {"a", "b"}; gguf_set_arr_str(m, "tokenizer.ggml.tokens", tokens, 2);
        check(gguf_write_to_file(m, model.c_str(), true)); gguf_free(m);
        auto p = gguf_init_empty(); check(p);
        gguf_set_val_str(p, "clip.projector_type", "qwen3vl_merger");
        gguf_set_val_u32(p, "clip.vision.projection_dim", 2560);
        gguf_set_val_u32(p, "clip.vision.patch_size", 16);
        check(gguf_write_to_file(p, projector.c_str(), true)); gguf_free(p);
        auto settings = vr::parse(vr::read_small(argv[3]));
        const char * search = std::getenv("LD_LIBRARY_PATH"); check(search);
        settings["reader_environment"]["LD_LIBRARY_PATH"] = search;
        { std::ofstream out(profile); out << settings.dump(); check(out.good()); }
        const auto runtime = vr::Runtime::load(profile.string(), model.string(), projector.string(), "/usr/bin/true", {}, 64, 4, false);
        vr::Json invariant;
        const std::string prefix = "STRATA_READER_CONFIG=";
        for (const auto & value : runtime.reader_environment)
            if (value.starts_with(prefix)) invariant = vr::parse(value.substr(prefix.size()));
        check(invariant.is_object()); invariant.erase("generation");
        for (auto & lib : invariant["reader_policy"]["image_libraries"])
            if (lib["role"] == "exif") lib["sha256"] = std::string(64, '0');
        const auto invariant_hash = vr::hex(vr::hash(invariant.dump()));
        // A real exec with ONLY the runtime's approved reader environment verifies
        // ordinary loader replacement and parent/clean-reader canonical agreement.
        auto env = runtime.reader_environment; std::vector<char *> pointers;
        for (auto & value : env) pointers.push_back(value.data());
        pointers.push_back(nullptr);
        const auto self = fs::canonical("/proc/self/exe").string();
        const auto child = ::fork(); check(child >= 0);
        if (!child) {
            char mode[] = "--clean-config"; char * args[] = {const_cast<char *>(self.c_str()), mode, nullptr};
            ::execve(self.c_str(), args, pointers.data()); ::_exit(127);
        }
        int status = 0; check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        std::cout << "PR6_DISTRIBUTION_PASS manifest=pass loaded_libraries=6 clean_config=pass exif_sha256=" << exif_hash
                  << " generation=" << vr::hex(runtime.config.generation) << " replacement_invariant=" << invariant_hash << " gpu=none\n";
        return 0;
    } catch (...) { std::cerr << "PR6 distribution verification failed\n"; return 1; }
}
