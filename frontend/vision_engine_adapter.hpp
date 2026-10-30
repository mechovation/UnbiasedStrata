// PR4 bounded clean-launch adapter for the existing native engine line protocol.
// Used by the profile-selected native Supervisor; legacy Engine remains the default.
#pragma once
#include "vision_supervisor.hpp"
#include "engine.hpp" // EngineLine data only; never invoke the legacy fork launcher
#include <cmath>
#include <charconv>
#include <locale>
#include <sstream>

namespace strata_frontend::vision_engine_adapter {
namespace eh = vision_engine_handoff;
using Clock = eh::Clock;
struct Limits {
    size_t command_bytes, line_bytes, startup_bytes, lines_per_poll;
    uint32_t vocabulary;
    Clock::duration ready, quit, term;
};
struct Launch {
    std::string guard, executable;
    std::vector<std::string> argv, environment; // caller-reviewed explicit allowlist
};
struct Sink {
    virtual ~Sink() = default;
    // Owned, bounded, nonblocking, noexcept, non-reentrant. false fails closed;
    // reserve output capacity before generate. Never mutate main-queue slots here.
    virtual bool line(eh::Identity, const EngineLine &) noexcept = 0;
    // Bounded startup INFO payload (without the prefix), private adapter metadata,
    // not an HTTP/log error. false fails startup closed.
    virtual bool info(std::string_view) noexcept = 0;
};
class Request final : public eh::Resources {
public:
    static std::shared_ptr<const Request> create(eh::Identity id, std::string_view header,
        const std::vector<int32_t> & ids, const Limits & caps, std::shared_ptr<const eh::Resources> files = {}) {
        if (!id.task || !id.instance || !id.job || !id.sequence || ids.empty() ||
            ids.size() > size_t(INT32_MAX) || header.size() > caps.command_bytes ||
            (header.substr(0, 4) != "GEN " && header.substr(0, 5) != "GENI ")) return {};
        for (unsigned char c : header) if (c < 32 || c > 126) return {};
        const size_t begin = header.find(' ') + 1;
        const size_t end = header.find(' ', begin);
        const auto number = header.substr(begin, end == std::string_view::npos ? header.size() - begin : end - begin);
        uint64_t maximum = 0;
        auto parsed = std::from_chars(number.data(), number.data() + number.size(), maximum);
        if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || !maximum || maximum > INT32_MAX ||
            (header.substr(0, 5) == "GENI " && !files)) return {};
        // Upper bound before allocation: ten decimal digits + separator per ID.
        if (caps.command_bytes < 2 || header.size() > caps.command_bytes - 2 ||
            ids.size() > (caps.command_bytes - 2 - header.size()) / 11) return {};
        for (int32_t token : ids) if (token < 0 || uint32_t(token) >= caps.vocabulary) return {};
        try {
            auto request = std::shared_ptr<Request>(new Request(id, ids.size(), maximum, std::move(files)));
            request->command_.assign(header); request->command_ += ' ';
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) request->command_ += ',';
                request->command_ += std::to_string(ids[i]);
            }
            request->command_ += '\n'; return request;
        } catch (...) { return {}; }
    }
    Request(const Request &) = delete;
    Request & operator=(const Request &) = delete;
    eh::Identity identity() const { return id_; }
    std::string_view command() const { return command_; }
    uint64_t prompt() const { return prompt_; }
    uint64_t maximum() const { return maximum_; }
private:
    const eh::Identity id_;
    const uint64_t prompt_, maximum_;
    std::shared_ptr<const eh::Resources> files_; // caller-owned SVE1 lifetime, never a child-selected path
    std::string command_;
    Request(eh::Identity id, uint64_t prompt, uint64_t maximum, std::shared_ptr<const eh::Resources> files)
        : id_(id), prompt_(prompt), maximum_(maximum), files_(std::move(files)) {}
};
class Adapter final : public eh::Engine {
public:
    Adapter(Launch launch, Limits limits, int diagnostic_fd, std::shared_ptr<Sink> sink)
        : launch_(std::move(launch)), limits_(limits), sink_(std::move(sink)) {
        if (!sink_ || launch_.guard.empty() || launch_.executable.empty() || launch_.argv.empty() ||
            !limits.command_bytes || limits.line_bytes < 32 || limits.startup_bytes < limits.line_bytes ||
            !limits.lines_per_poll || !limits.vocabulary || limits.vocabulary > INT32_MAX ||
            !vision_job_io::duration_valid(limits.ready) || !vision_job_io::duration_valid(limits.quit) ||
            !vision_job_io::duration_valid(limits.term)) throw std::invalid_argument("invalid engine adapter limits");
        diagnostic_.value = ::fcntl(diagnostic_fd, F_DUPFD_CLOEXEC, 3);
        if (diagnostic_.value < 0) throw std::runtime_error("engine diagnostic descriptor");
        buffer_.reserve(limits_.line_bytes); // bounded single-line storage; no stdout read-ahead
    }
    ~Adapter() { if (child_.pid > 0) std::terminate(); }
    // Only these scalar observations may cross threads. Safety must independently
    // qualify GPU quiescence; an idle/reaped CPU test child alone is NOT that proof.
    bool idle() const noexcept { return idle_.load(); }
    bool stopped() const noexcept { return stopped_.load(); }
    uint64_t context() const noexcept { return context_.load(); }
    bool can_stop() const noexcept { return can_stop_.load(); }
    pid_t observed_pid() const noexcept { return observed_pid_.load(); }
    // Owner must hold the shared engine turn, including initial load/READY. This
    // never retries a failed launch; any ambiguous startup requires containment.
    bool initialize(Clock::time_point until, const std::atomic<bool> & cancel) noexcept override {
        own();
        if (broken_ || active_ || cancel.load() || Clock::now() >= until) return false;
        try {
            if (!loaded_ && !load(std::min(until, after(limits_.ready)), cancel)) {
                (void) fault_start(); return false;
            }
            if (!healthy()) return false;
            return !cancel.load() && Clock::now() < until;
        } catch (...) { (void) fault_start(); return false; }
    }
    eh::Start start(eh::Identity id, const eh::Resources & resources, Clock::time_point until,
                    const std::atomic<bool> & cancel) noexcept override {
        own();
        const auto * envelope = dynamic_cast<const vision_supervisor::EngineResources *>(&resources);
        const auto * request = envelope ? dynamic_cast<const Request *>(envelope->request.get()) : nullptr;
        if (broken_ || active_) return eh::Start::uncertain;
        if (!request || !(request->identity() == id)) return eh::Start::not_sent;
        try {
            if (cancel.load() || Clock::now() >= until) return eh::Start::not_sent;
            if (!initialize(until, cancel)) {
                if (!broken_ && (cancel.load() || Clock::now() >= until)) return eh::Start::not_sent;
                return fault_start();
            }
            if (request->prompt() >= context_ || request->maximum() > context_ - request->prompt())
                return eh::Start::not_sent; // preserve admission before GEN, never silently clamp
            if (cancel.load() || Clock::now() >= until) return eh::Start::not_sent;
            id_ = id; prompt_ = request->prompt(); maximum_ = request->maximum(); emitted_ = 0;
            active_ = true; idle_ = false; // possible consumption precedes native write
            if (!send(request->command(), until, cancel)) return fault_start();
            return eh::Start::sent;
        } catch (...) { return fault_start(); }
    }
    bool stop(eh::Identity id, Clock::time_point until) noexcept override {
        own(); if (!(id == id_) || !active_ || broken_ || !can_stop_) return false;
        return send("STOP\n", until, never_);
    }
    eh::Observation poll(eh::Identity id, Clock::time_point until) noexcept override {
        own(); if (!(id == id_) || !active_ || broken_) return eh::Observation::broken;
        try {
            // Nonblocking and bounded work per owner tick, including progress floods.
            for (size_t i = 0; i < limits_.lines_per_poll; ++i) {
                if (Clock::now() >= until) return eh::Observation::pending;
                std::string line;
                const int rc = read_line(line, until);
                if (!rc) return eh::Observation::pending;
                if (rc < 0) return fault();
                EngineLine parsed;
                const auto result = parse(line, parsed);
                if (result == eh::Observation::broken) return fault();
                if (parsed.kind == EngineLine::TOKEN) {
                    if (emitted_ == maximum_) return fault(); // refuse before sink/output growth
                    ++emitted_;
                }
                if (!sink_->line(id_, parsed)) return fault();
                if (result != eh::Observation::pending) {
                    active_ = false; idle_ = true; return result;
                }
            }
            return eh::Observation::pending;
        } catch (...) { return fault(); }
    }
    // Owner-only idle health check; detects exit, duplicate terminals and unsolicited
    // bytes before another request. No engine startup/restart here. The legacy
    // protocol has NO response IDs: a malicious/delayed duplicate after a later
    // GEN cannot be distinguished. This boundary requires a trusted exactly-once
    // native engine, unlike the identity-bearing untrusted reader protocol.
    bool healthy() noexcept override {
        own(); if (broken_) return false;
        if (!loaded_) return child_.pid <= 0;
        if (active_) return true;
        int status = 0;
        if (reap_until(child_.pid, Clock::now(), status) != ReapResult::timeout) { broken_ = true; return false; }
        pollfd fd{child_.output, POLLIN, 0};
        const int rc = ::poll(&fd, 1, 0);
        // No replies are outstanding while idle. Even a partial trailing line is
        // unexpected; do not let a duplicate terminal complete a successor.
        if (!buffer_.empty() || rc > 0 || (rc < 0 && errno != EINTR)) { broken_ = true; return false; }
        return true;
    }
    bool contain(eh::Identity, Clock::time_point until) noexcept override { own(); broken_ = true; return terminate(until); }
    bool shutdown(Clock::time_point until) noexcept override {
        own();
        if (child_.pid > 0 && !broken_ && !active_) {
            const auto quit = std::min(until, after(limits_.quit));
            if (send("QUIT\n", quit, never_)) { int status = 0; (void) reap_until(child_.pid, quit, status); }
        }
        return terminate(until);
    }
private:
    Launch launch_;
    const Limits limits_;
    std::shared_ptr<Sink> sink_;
    clean_process_detail::Fd diagnostic_;
    SpawnedProcess child_;
    std::thread::id owner_{};
    std::string buffer_;
    bool loaded_ = false, active_ = false, broken_ = false;
    std::atomic<bool> idle_{true}, stopped_{true}, can_stop_{false};
    std::atomic<uint64_t> context_{0};
    std::atomic<pid_t> observed_pid_{-1};
    const std::atomic<bool> never_{false};
    eh::Identity id_{};
    uint64_t prompt_ = 0, maximum_ = 0, emitted_ = 0;
    void own() noexcept {
        if (owner_ == std::thread::id{}) owner_ = std::this_thread::get_id();
        if (owner_ != std::this_thread::get_id()) std::terminate();
    }
    static Clock::time_point after(Clock::duration d) { return vision_job_io::after(d); }
    eh::Start fault_start() noexcept { broken_ = true; idle_ = false; return eh::Start::uncertain; }
    eh::Observation fault() noexcept { broken_ = true; idle_ = false; return eh::Observation::broken; }
    static bool wait(int fd, short events, Clock::time_point until, const std::atomic<bool> & cancel) noexcept {
        while (!cancel.load() && Clock::now() < until) {
            pollfd p{fd, events, 0};
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
            const int rc = ::poll(&p, 1, int(std::min<int64_t>(10, std::max<int64_t>(0, left))));
            if (rc > 0) return (p.revents & events) != 0;
            if (rc < 0 && errno != EINTR) return false;
        }
        return false;
    }
    // Thread-local SIGPIPE containment for pipe writes. Never install a process-wide
    // ignore handler, consume an already-pending SIGPIPE, or alter another thread.
    bool send(std::string_view value, Clock::time_point until, const std::atomic<bool> & cancel) noexcept {
        sigset_t set, old, pending; ::sigemptyset(&set); ::sigaddset(&set, SIGPIPE);
        if (::pthread_sigmask(SIG_BLOCK, &set, &old) != 0) return false;
        if (::sigpending(&pending) != 0) { ::pthread_sigmask(SIG_SETMASK, &old, nullptr); return false; }
        const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;
        bool ok = true, pipe_error = false;
        size_t position = 0;
        while (position < value.size()) {
            if (cancel.load() || Clock::now() >= until || child_.input < 0) { ok = false; break; }
            const ssize_t n = ::write(child_.input, value.data() + position, std::min<size_t>(4096, value.size() - position));
            if (n > 0) position += size_t(n);
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!wait(child_.input, POLLOUT, until, cancel)) { ok = false; break; }
            } else { pipe_error = n < 0 && errno == EPIPE; ok = false; break; }
        }
        if (pipe_error && !already_pending) { timespec zero{}; while (::sigtimedwait(&set, nullptr, &zero) < 0 && errno == EINTR) {} }
        if (::pthread_sigmask(SIG_SETMASK, &old, nullptr) != 0) std::terminate();
        return ok;
    }
    // Reads at most one bounded line per call. Bytewise nonblocking reads avoid
    // hidden read-ahead/leftover terminal ownership; caps bound work per call.
    int read_line(std::string & output, Clock::time_point until) {
        for (size_t work = 0; work <= limits_.line_bytes; ++work) {
            if (work % 64 == 0 && Clock::now() >= until) return 0;
            char c = 0; const ssize_t n = ::read(child_.output, &c, 1);
            if (n == 1) {
                if (c == '\n') {
                    if (!buffer_.empty() && buffer_.back() == '\r') buffer_.pop_back();
                    output.swap(buffer_); buffer_.clear(); return 1;
                }
                if (!c || buffer_.size() >= limits_.line_bytes) return -1;
                buffer_ += c;
            } else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            else return -1; // partial EOF is not a terminal boundary
        }
        return 0;
    }
    bool load(Clock::time_point until, const std::atomic<bool> & cancel) {
        std::string error;
        if (!protected_spawn(launch_.guard, launch_.executable, launch_.argv, launch_.environment,
                             diagnostic_.value, child_, error)) return false;
        observed_pid_ = child_.pid;
        stopped_ = false; idle_ = false;
        for (int fd : {child_.input, child_.output}) {
            const int flags = ::fcntl(fd, F_GETFL);
            if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
        }
        size_t bytes = 0;
        while (!cancel.load() && Clock::now() < until) {
            std::string line;
            const int rc = read_line(line, until);
            if (rc < 0) return false;
            if (!rc) { if (!wait(child_.output, POLLIN, until, cancel)) return false; continue; }
            if (line.size() + 1 > limits_.startup_bytes - bytes) return false;
            bytes += line.size() + 1;
            if (line.rfind("READY ", 0) == 0) {
                std::istringstream input(line); input.imbue(std::locale::classic());
                std::string ready, capability, extra; uint64_t context = 0;
                if (!(input >> ready >> context) || context == 0 || context > INT32_MAX) return false;
                if (input >> capability) { if (capability != "stop" || (input >> extra)) return false; }
                context_ = context; can_stop_ = capability == "stop"; loaded_ = true; idle_ = true; return true;
            }
            if (line.rfind("ERR", 0) == 0 || line.rfind("FATAL", 0) == 0 || line.rfind("READY", 0) == 0) return false;
            if (line.rfind("INFO ", 0) == 0 && !sink_->info(std::string_view(line).substr(5))) return false;
            // Legacy startup permits chatter/INFO. Bound total bytes and time;
            // never forward child diagnostics into an HTTP error or log them here.
        }
        return false;
    }
    static bool decimal(std::string_view text, uint64_t & value) {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= INT64_MAX;
    }
    static bool statistics(std::istream & input, EngineLine & out) {
        size_t count = 0; bool keys = false, chunk = false, streamed = false;
        std::string token;
        while (input >> token) {
            const auto equals = token.find('='); uint64_t integer = 0;
            if (equals != std::string::npos) {
                keys = true;
                if (!decimal(std::string_view(token).substr(equals + 1), integer)) return false;
                const auto name = token.substr(0, equals);
                if (name == "chunk" && !chunk) chunk = true;
                else if (name == "experts_streamed" && !streamed) streamed = true;
                else return false;
            } else {
                if (keys || count >= 15) return false;
                if (count == 7) { // native file-MB counter is the sole floating statistic
                    std::istringstream number(token); number.imbue(std::locale::classic()); double value = 0;
                    if (!(number >> value) || !std::isfinite(value) || value < 0) return false;
                    number >> std::ws; if (!number.eof()) return false;
                } else if (!decimal(token, integer)) return false;
                if (count == 0) out.draft_accepted = int64_t(integer);
                if (count == 1) out.draft_offered = int64_t(integer);
                if (count == 2) out.reused = int64_t(integer);
                ++count;
            }
        }
        // Supported append-only native revisions: base counters, hit/lookups,
        // storage tiers, prompt-read, offload and optional lookup-chain counters.
        return (count == 0 && !keys) || count == 3 || count == 5 || count == 8 || count == 9 || count == 10 || count == 15;
    }
    eh::Observation parse(const std::string & line, EngineLine & out) const {
        std::istringstream input(line); input.imbue(std::locale::classic());
        std::string kind; input >> kind;
        auto end = [&] { input >> std::ws; return input.eof(); };
        if (kind == "T") {
            int64_t token = -1;
            if (!(input >> token) || token < 0 || uint64_t(token) >= limits_.vocabulary || !end()) return eh::Observation::broken;
            out.kind = EngineLine::TOKEN; out.token = int32_t(token);
        } else if (kind == "DONE") {
            if (!(input >> out.produced >> out.prompt_n >> out.prompt_ms >> out.decode_ms >> out.finish) ||
                out.produced < 0 || out.prompt_n < 0 || !std::isfinite(out.prompt_ms) || !std::isfinite(out.decode_ms) ||
                out.prompt_ms < 0 || out.decode_ms < 0) return eh::Observation::broken;
            if (!statistics(input, out) || uint64_t(out.produced) != emitted_) return eh::Observation::broken;
            out.kind = EngineLine::DONE;
            if (out.finish != "stop" && out.finish != "length" && out.finish != "cancel" && out.finish != "nonfinite")
                return eh::Observation::broken;
            if (uint64_t(out.prompt_n) != prompt_ || uint64_t(out.produced) > maximum_ || out.finish == "nonfinite")
                return eh::Observation::failed;
            return out.finish == "cancel" ? eh::Observation::stopped : eh::Observation::succeeded;
        } else if (kind == "ERR") {
            out.kind = EngineLine::ERR; out.message = "engine refused request"; return eh::Observation::failed;
        } else if (kind == "FATAL") return eh::Observation::broken;
        else if (kind == "PP") {
            if (!(input >> out.pp_done >> out.pp_total) || out.pp_done < 0 || out.pp_total < 0) return eh::Observation::broken;
            if (!end()) {
                double elapsed = 0, rate = 0;
                if (!(input >> elapsed >> rate) || !std::isfinite(elapsed) || !std::isfinite(rate) ||
                    elapsed < 0 || rate < 0 || !end()) return eh::Observation::broken;
            }
            out.kind = EngineLine::PP;
        } else if (kind == "RESUME" || kind == "REUSED") {
            if (!(input >> out.n) || out.n < 0 || !end()) return eh::Observation::broken;
            out.kind = kind == "RESUME" ? EngineLine::RESUME : EngineLine::REUSED;
        } else if (kind == "INFO" || kind == "WARN") out.kind = EngineLine::INFO;
        else return eh::Observation::broken;
        return eh::Observation::pending;
    }
    bool terminate(Clock::time_point until) noexcept {
        int status = 0;
        if (child_.pid > 0 && terminate_until(child_.pid, std::min(until, after(limits_.term)), until, status) != ReapResult::reaped)
            return false;
        for (int * fd : {&child_.input, &child_.output}) { if (*fd >= 0) ::close(*fd); *fd = -1; }
        active_ = false; loaded_ = false; stopped_ = true; idle_ = true; return true;
    }
};
} // namespace strata_frontend::vision_engine_adapter
