// Standalone CPU-only HTTP qualification probe. Build: c++ -std=c++17 -O2 -Wall -Wextra -Werror ... -o pr4-http-engine
// All controls are files in --fixture-dir; no test environment crosses the clean exec boundary.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::string dir, input;
unsigned number = 0;
bool exists(const std::string &name) { return ::access((dir + "/" + name).c_str(), F_OK) == 0; }
std::string setting(const std::string &name, std::string fallback = "") {
    std::ifstream f(dir + "/" + name); std::string s; return std::getline(f, s) ? s : fallback;
}
std::string quote(const std::string &s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << c;
        else if (c < 32) o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
        else o << c;
    }
    return o.str() + '"';
}
void event(const std::string &kind, const std::string &fields = "") {
    std::ofstream f(dir + "/events.jsonl", std::ios::app);
    f << "{\"event\":" << quote(kind) << ",\"pid\":" << getpid() << ",\"request\":" << number
      << ",\"ns\":" << Clock::now().time_since_epoch().count() << fields << "}\n";
    f.flush(); if (!f) throw std::runtime_error("fixture event write failed");
}
void say(const std::string &s) { std::cout << s << '\n' << std::flush; }
// Unbuffered fd IO: poll must not miss commands already buffered by std::getline.
bool line(std::string &out, int milliseconds) {
    auto until = Clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        auto end = input.find('\n');
        if (end != std::string::npos) { out = input.substr(0, end); input.erase(0, end + 1); event("command", ",\"line\":" + quote(out)); return true; }
        pollfd p{STDIN_FILENO, POLLIN, 0};
        int remaining = std::max(0, int(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count()));
        int r = ::poll(&p, 1, remaining);
        if (r < 0) throw std::runtime_error("poll failed");
        if (!r) return false;
        char b[8192]; auto n = ::read(STDIN_FILENO, b, sizeof b);
        if (n <= 0) { event("eof"); std::exit(0); }
        input.append(b, size_t(n));
        if (input.size() > 262144) throw std::runtime_error("command too large");
    } while (Clock::now() <= until);
    return false;
}
void startup_wait() {
    auto until = Clock::now() + std::chrono::seconds(90);
    while ((exists("startup-hold") || exists("startup-silent")) && !exists("startup-release")) {
        if (Clock::now() > until) throw std::runtime_error("startup gate expired");
        std::string s; if (line(s, 10) && s == "QUIT") std::exit(0);
    }
}
uint32_t u32(const unsigned char *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
struct Row { uint32_t cells; std::string hash; };
std::vector<Row> read_rows(const std::string &path) {
    std::ifstream f(path, std::ios::binary); if (!f) throw std::runtime_error("missing SVE1 file");
    std::vector<Row> rows;
    while (f.peek() != EOF) {
        unsigned char h[20]; f.read(reinterpret_cast<char *>(h), sizeof h);
        if (!f || u32(h) != 0x31455653) throw std::runtime_error("bad SVE1 header");
        uint32_t cells = u32(h+4), x = u32(h+8), y = u32(h+12), width = u32(h+16);
        if (!cells || !x || !y || uint64_t(x)*y != cells || !width || uint64_t(cells)*width > 4194304)
            throw std::runtime_error("unbounded SVE1 dimensions");
        uint64_t hash = 1469598103934665603ULL;
        for (uint64_t i = 0; i < uint64_t(cells)*width; ++i) {
            unsigned char bytes[4]; f.read(reinterpret_cast<char *>(bytes), 4);
            if (!f) throw std::runtime_error("truncated SVE1 rows");
            uint32_t bits = u32(bytes); float value; std::memcpy(&value, &bits, 4);
            if (!std::isfinite(value)) throw std::runtime_error("nonfinite SVE1 row");
            for (auto b : bytes) { hash ^= b; hash *= 1099511628211ULL; }
        }
        std::ostringstream hex; hex << std::hex << std::setw(16) << std::setfill('0') << hash;
        rows.push_back({cells, hex.str()}); if (rows.size() > 4) throw std::runtime_error("too many SVE1 rows");
    }
    if (rows.empty()) throw std::runtime_error("empty SVE1 file");
    return rows;
}
bool wait_generation(const std::string &suffix, bool token_sent) {
    bool cancelled = false;
    auto until = Clock::now() + std::chrono::seconds(90);
    while ((exists("hold-"+suffix) && !exists("release-"+suffix)) ||
           (cancelled && exists("withhold-done-"+suffix) && !exists("done-release-"+suffix))) {
        if (Clock::now() > until) throw std::runtime_error("generation gate expired");
        std::string cmd;
        if (!line(cmd, 10)) continue;
        if (cmd == "QUIT") std::exit(0);
        if (cmd == "STOP") {
            if (!cancelled) {
                cancelled = true; if (!token_sent) say("T 1"); say("PP 1 1 1 1000"); event("residual");
            }
            // A STOP releases the pre-terminal generation gate, but NEVER the DONE gate.
            std::ofstream release(dir + "/release-" + suffix); release << '1';
        } else {
            event("ordering-violation", ",\"line\":" + quote(cmd));
            throw std::runtime_error("successor command before terminal drain");
        }
    }
    return cancelled;
}
int run() {
    event("startup", ",\"ppid\":" + std::to_string(getppid()));
    startup_wait();
    if (exists("startup-fail")) { say("ERR fixture startup failure"); return 31; }
    say("READY " + setting("context", "512") + " stop"); event("ready");
    std::string cmd;
    for (;;) {
        if (!line(cmd, 1000)) continue;
        if (cmd == "QUIT") return 0;
        if (cmd == "STOP") { event("idle-stop"); continue; }
        ++number;
        std::istringstream in(cmd); std::vector<std::string> fields; std::string field;
        while (in >> field) fields.push_back(field);
        if (fields.size() < 3 || (fields[0] != "GEN" && fields[0] != "GENI")) throw std::runtime_error("invalid GEN");
        std::string keys, path; int pad = -1;
        for (size_t i = 2; i + 1 < fields.size(); ++i) {
            if (fields[i].rfind("img_keys=", 0) == 0) keys = fields[i].substr(9);
            else if (fields[i].rfind("image_pad=", 0) == 0) pad = std::stoi(fields[i].substr(10));
            else if (fields[i].find('=') == std::string::npos) {
                if (!path.empty()) throw std::runtime_error("multiple picture paths");
                path = fields[i];
            }
        }
        std::vector<int> ids; std::istringstream tokens(fields.back());
        while (std::getline(tokens, field, ',')) { int id = std::stoi(field); if (id < 0) throw std::runtime_error("negative token"); ids.push_back(id); }
        if (ids.empty()) throw std::runtime_error("empty prompt");
        std::vector<Row> rows;
        if (fields[0] == "GENI") {
            if (path.empty() || pad < 0 || keys.empty()) throw std::runtime_error("missing image metadata");
            rows = read_rows(path);
            if (size_t(1 + std::count(keys.begin(), keys.end(), ',')) != rows.size()) throw std::runtime_error("key occurrence mismatch");
            std::vector<uint32_t> runs;
            for (size_t i = 0; i < ids.size();) {
                if (ids[i] != pad) { ++i; continue; }
                uint32_t count = 0;
                while (i < ids.size() && ids[i] == pad) { ++count; ++i; }
                runs.push_back(count);
            }
            if (runs.size() != rows.size()) throw std::runtime_error("image_pad occurrence mismatch");
            for (size_t i = 0; i < runs.size(); ++i)
                if (runs[i] != rows[i].cells) throw std::runtime_error("ordered image_pad cells mismatch");
        } else if (!path.empty() || !keys.empty() || pad >= 0) throw std::runtime_error("picture metadata in text GEN");
        std::string hashes = "[", cells = "[";
        for (const auto &r : rows) { if (hashes.size() > 1) { hashes += ','; cells += ','; } hashes += quote(r.hash); cells += std::to_string(r.cells); }
        event("gen", ",\"kind\":"+quote(fields[0])+",\"prompt\":"+std::to_string(ids.size())+
            ",\"keys\":"+quote(keys)+",\"pad\":"+std::to_string(pad)+",\"path\":"+quote(path)+
            ",\"hashes\":"+hashes+"],\"cells\":"+cells+"]");
        auto mode = setting("mode-" + std::to_string(number), "ok");
        if (mode == "death") { event("death"); return 32; }
        if (mode == "err") { say("ERR fixture native refusal"); event("error"); continue; }
        if (mode == "malformed") { say("DONE not-a-count"); event("malformed"); continue; }
        say("PP " + std::to_string(ids.size()) + " " + std::to_string(ids.size()) + " 1 1000");
        const bool early_token = mode == "normal-stop";
        if (early_token) say("T 1");
        bool cancel = wait_generation(std::to_string(number), early_token);
        if (!cancel && !early_token) say("T 1"); // cancellation already emitted the one bounded residual token
        if (!path.empty() && ::access(path.c_str(), R_OK)) throw std::runtime_error("SVE1 removed before DONE");
        event("done", ",\"cancel\":" + std::string(cancel ? "true" : "false"));
        say("DONE 1 " + std::to_string(ids.size()) + " 1 1 " + (cancel ? "cancel" : "stop") + " 0 0 0");
    }
}
}
int main(int argc, char **argv) {
    try {
        bool serve = false;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--serve") serve = true;
            else if (a == "--fixture-dir" && i+1 < argc) dir = argv[++i];
            else throw std::runtime_error("usage: --serve --fixture-dir DIR");
        }
        if (!serve || dir.empty()) throw std::runtime_error("missing fixture-dir or --serve");
        return run();
    } catch (const std::exception &e) {
        if (!dir.empty()) { try { event("probe-failure", ",\"message\":" + quote(e.what())); } catch (...) {} }
        std::cerr << "pr4-http-engine: " << e.what() << '\n'; return 33;
    }
}
