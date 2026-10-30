#include "../vision_supervisor.hpp"
#include <cassert>
#include <dirent.h>
#include <iostream>
#include <deque>
#include <sys/prctl.h>

using namespace strata_frontend;
namespace vs = vision_supervisor;
namespace wire = vision_protocol;
namespace vc = vision_control;
using namespace std::chrono_literals;
static vision_payload::Config config{{}, {}, 4, 32, 1024};
static vision_payload::Limits caps{16384, 4, 16, 1024, 65536, {4096, 64, 4096, 16384, 32, 16 * 1024 * 1024}};
static vs::Limits limits{2s, 30ms, 20ms, 2s, 80ms, 10s, 3}; // injected test values only
static auto deadline() { return wire::Clock::now() + 5s; }
template<class F> static void wait_for(F predicate) {
    const auto until = deadline();
    while (!predicate()) {
        assert(wire::Clock::now() < until);
        std::this_thread::sleep_for(1ms);
    }
}
struct Sink final : vc::Wake {
    std::atomic<unsigned> pending{0};
    void post() noexcept override { ++pending; }
};
// Certification applies ONLY to these known CPU-only test executables. Not a
// GPU policy, and not evidence that waitpid establishes driver quiescence.
struct CpuOnlySafety : vs::Safety {
    std::atomic<bool> allow{true}, proof{true}, engine_proof{true}, revoked{false}, leased{false};
    std::atomic<unsigned> acquired{0}, released{0}, certifications{0};
    std::thread::id owner{};
    bool acquire(uint64_t instance) noexcept override {
        assert(instance);
        if (!allow || revoked) return false;
        bool expected = false;
        assert(leased.compare_exchange_strong(expected, true));
        if (owner == std::thread::id{}) owner = std::this_thread::get_id();
        assert(owner == std::this_thread::get_id());
        ++acquired; return true;
    }
    void service_failed() noexcept override { revoked = true; }
    bool engine_stopped() noexcept override { return engine_proof; }
    bool certify(uint64_t instance, bool, bool whole_service) noexcept override {
        assert(instance && leased && owner == std::this_thread::get_id());
        if (whole_service) assert(revoked && engine_proof);
        ++certifications; return proof;
    }
    void release(uint64_t) noexcept override {
        assert(leased && owner == std::this_thread::get_id());
        leased = false; ++released;
    }
};
struct FailReapOnce final : vs::Reaper {
    bool failed = false;
    ReapResult terminate(pid_t & pid, wire::Clock::time_point term, wire::Clock::time_point kill) noexcept override {
        assert(pid > 0);
        if (!failed) { failed = true; return ReapResult::timeout; }
        return Reaper::terminate(pid, term, kill);
    }
};
struct Events {
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    std::shared_ptr<vc::Inbox> inbox;
    std::deque<vc::Event> received;
    explicit Events(size_t capacity = 4) : inbox(std::make_shared<vc::Inbox>(capacity, sink)) {}
    void pump() {
        if (!sink->pending.exchange(0)) return;
        while (auto event = inbox->pop()) received.push_back(*event);
    }
    vc::Event take() {
        // Never poll pop() without a wake. Missing wake/rearming now times out.
        wait_for([&] { pump(); return !received.empty(); });
        auto event = received.front(); received.pop_front(); return event;
    }
    vc::Event next(uint64_t task, vc::Event::Kind kind) {
        auto event = take();
        assert(event.task == task && event.kind == kind && !event.job && !event.sequence);
        return event;
    }
    void empty() { pump(); assert(received.empty()); }
};
static int fd_count() {
    DIR * d = ::opendir("/proc/self/fd"); assert(d);
    int n = 0; while (::readdir(d)) ++n; ::closedir(d); return n;
}
static vs::Launch launch(const char * executable, const char * guard, const std::string & mode) {
    return {guard, executable, {executable, "fake", mode}, {"SUPERVISOR_TEST=1"}, 100, config, caps};
}
[[noreturn]] static void hang() { for (;;) ::pause(); }
static int fake(const std::string & mode, uint64_t instance) {
    ::signal(SIGTERM, SIG_IGN);
    assert(std::getenv("SUPERVISOR_TEST") && !std::getenv("HOME"));
    wire::Channel channel(3);
    if (mode == "silent" || (mode == "first-silent" && instance == 100)) hang();
    if (mode == "abort") return 7;
    vision_commands::Payload ready;
    assert(vision_commands::build(wire::Kind::ready, config, caps, {}, ready) == vision_commands::Result::ok);
    if (mode == "bad") ready.bytes[0] ^= 1;
    std::atomic<bool> cancel{false};
    assert(channel.send_borrowed({wire::Kind::ready, uint32_t(ready.bytes.size()), instance, 0, 0},
        ready.bytes.data(), ready.bytes.size(), nullptr, 0, caps.payload_bytes, deadline(), cancel) == wire::IoResult::ok);
    if (mode == "bad") hang();
    if (mode == "idle-exit" || mode == "idle-eof" || mode == "unsolicited") {
        std::this_thread::sleep_for(100ms);
        if (mode == "idle-exit") return 9;
        if (mode == "idle-eof") { ::shutdown(3, SHUT_RDWR); hang(); }
        assert(channel.send_borrowed({wire::Kind::ready, uint32_t(ready.bytes.size()), instance, 0, 0},
            ready.bytes.data(), ready.bytes.size(), nullptr, 0, caps.payload_bytes, deadline(), cancel) == wire::IoResult::ok);
        hang();
    }
    wire::Frame quit;
    auto rc = channel.receive(quit, {wire::Kind::quit, instance, 0, 0, 0}, caps.payload_bytes, deadline(), cancel);
    // Shutdown may intentionally close the channel instead of attempting QUIT.
    if (rc == wire::IoResult::eof) hang();
    assert(rc == wire::IoResult::ok);
    vision_payload::View view;
    assert(vision_payload::validate(wire::Kind::quit,
        {reinterpret_cast<const char *>(quit.payload.data()), quit.payload.size()}, quit.fd_count, config, caps, {}, view));
    if (mode == "ignore-quit") hang();
    return 0;
}
struct FailReapAlways final : vs::Reaper {
    ReapResult terminate(pid_t &, wire::Clock::time_point, wire::Clock::time_point) noexcept override {
        return ReapResult::timeout;
    }
};
struct FatalSafety final : CpuOnlySafety {
    void release(uint64_t) noexcept override { std::_Exit(87); } // releasing is a test failure
};
static int fatal_service(const char * executable, const char * guard, const std::string & mode) {
    std::set_terminate([] { std::_Exit(86); }); // no core dump; proves fail-service-closed branch
    Events e; auto safety = std::make_shared<FatalSafety>();
    std::shared_ptr<vs::Reaper> reaper = std::make_shared<vs::Reaper>();
    if (mode == "reap") reaper = std::make_shared<FailReapAlways>();
    if (mode == "proof") safety->proof = false;
    if (mode == "engine") safety->engine_proof = false;
    vs::Supervisor supervisor(launch(executable, guard, "good"), limits, 2, safety, e.inbox, reaper);
    if (mode == "engine") supervisor.engine_died(); // even with no reader, engine proof is mandatory
    else {
        assert(supervisor.start(1)); e.next(1, vc::Event::Kind::ready);
        supervisor.shutdown();
    }
    wait_for([&] { return supervisor.state() == vs::State::exited; });
    return 88; // must never report successful shutdown
}
static void tests(const char * executable, const char * guard) {
    const auto baseline = fd_count();
    // READY/QUIT, fresh identity, explicit stop; no replacement while turn denied.
    {
        Events e; auto safety = std::make_shared<CpuOnlySafety>(); safety->allow = false;
        {
            vs::Supervisor supervisor(launch(executable, guard, "good"), limits, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 0 && !safety->leased);
            safety->allow = true;
            assert(supervisor.start(2)); auto first = e.next(2, vc::Event::Kind::ready); e.empty();
            assert(first.instance == 100 && supervisor.state() == vs::State::ready && safety->leased);
            assert(supervisor.start(3)); e.next(3, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 1);
            assert(supervisor.stop(4)); e.next(4, vc::Event::Kind::stopped); e.empty();
            assert(!safety->leased);
            std::this_thread::sleep_for(100ms);
            assert(supervisor.start(5)); auto second = e.next(5, vc::Event::Kind::ready); e.empty();
            assert(second.instance == 101);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
        assert(safety->acquired == 2 && safety->released == 2);
        e.inbox->stop_accepting(); assert(e.inbox->settled());
    }
    // Startup rollback, backoff, fresh instance, no automatic restart or retry.
    {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        auto bounded = limits; bounded.ready = 100ms;
        {
            vs::Supervisor supervisor(launch(executable, guard, "first-silent"), bounded, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::failed); e.empty();
            assert(supervisor.state() == vs::State::backoff && safety->released == 1);
            assert(supervisor.start(2)); e.next(2, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 1);
            std::this_thread::sleep_for(100ms);
            assert(supervisor.attempts() == 1); // timers never autonomously spawn
            assert(supervisor.start(3)); auto event = e.next(3, vc::Event::Kind::ready); e.empty();
            assert(event.instance == 101 && supervisor.attempts() == 2);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Both count and elapsed restart-episode limits latch unavailable.
    for (bool window : {false, true}) {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        auto bounded = limits; bounded.ready = 100ms;
        bounded.attempts = window ? 3 : 1;
        bounded.restart_window = window ? 50ms : 10s;
        {
            vs::Supervisor supervisor(launch(executable, guard, "silent"), bounded, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::failed); e.empty();
            assert(supervisor.state() == vs::State::unavailable);
            std::this_thread::sleep_for(100ms);
            assert(supervisor.start(2)); e.next(2, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 1 && safety->released == 1);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Native READY/config/exec failures all roll back the turn and process.
    for (const auto * mode : {"bad", "abort", "missing-target", "missing-guard"}) {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        auto spec = launch(executable, guard, mode);
        if (std::string(mode) == "missing-target") spec.executable = "/nonexistent/pr4-reader";
        if (std::string(mode) == "missing-guard") spec.guard = "/nonexistent/pr4-guard";
        {
            vs::Supervisor supervisor(std::move(spec), limits, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::failed); e.empty();
            assert(supervisor.state() == vs::State::backoff && !safety->leased && safety->released == 1);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Reaping alone cannot release a GPU turn: explicit denied parent proof.
    {
        Events e; auto safety = std::make_shared<CpuOnlySafety>(); safety->proof = false;
        {
            vs::Supervisor supervisor(launch(executable, guard, "bad"), limits, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::failed); e.empty();
            assert(supervisor.state() == vs::State::safety_blocked && safety->leased && !safety->released);
            assert(supervisor.start(2)); e.next(2, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 1);
            safety->proof = true;
            assert(supervisor.stop(3)); e.next(3, vc::Event::Kind::stopped); e.empty();
            assert(!safety->leased);
            assert(supervisor.start(4)); e.next(4, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 1); // late proof cannot bypass cooldown
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Injected reap timeout retains PID/turn; no replacement, later bounded retry.
    {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        {
            vs::Supervisor supervisor(launch(executable, guard, "ignore-quit"), limits, 2, safety, e.inbox,
                                     std::make_shared<FailReapOnce>());
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::ready); e.empty();
            assert(supervisor.stop(2)); e.next(2, vc::Event::Kind::failed); e.empty();
            assert(supervisor.state() == vs::State::safety_blocked && safety->leased && !safety->certifications);
            assert(supervisor.start(3)); e.next(3, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.stop(4)); e.next(4, vc::Event::Kind::stopped); e.empty();
            assert(supervisor.attempts() == 1 && safety->released == 1);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Idle death/EOF/unsolicited frames use capacity reserved BEFORE launch.
    for (const auto * mode : {"idle-exit", "idle-eof", "unsolicited"}) {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        {
            vs::Supervisor supervisor(launch(executable, guard, mode), limits, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::ready); e.empty();
            e.next(0, vc::Event::Kind::failed); e.empty();
            assert(supervisor.state() == vs::State::backoff && safety->released == 1);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Shutdown/engine death bypasses a full command inbox and interrupts READY.
    for (bool engine_death : {false, true}) {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        {
            vs::Supervisor supervisor(launch(executable, guard, "silent"), limits, 2, safety, e.inbox);
            assert(supervisor.start(1));
            wait_for([&] { return supervisor.state() == vs::State::starting; });
            auto fill = e.inbox->reserve(); assert(fill && !e.inbox->reserve());
            const auto start = wire::Clock::now();
            if (engine_death) supervisor.engine_died(); else supervisor.shutdown();
            e.next(1, vc::Event::Kind::failed);
            e.next(0, vc::Event::Kind::stopped); e.empty();
            assert(wire::Clock::now() - start < 1s);
            assert(supervisor.state() == vs::State::exited && safety->released == 1);
            assert(!supervisor.start(2) && safety->revoked == engine_death);
        }
        e.inbox->stop_accepting(); assert(e.inbox->settled());
    }
    // Exhausted identity space never wraps/reuses an instance.
    {
        Events e; auto safety = std::make_shared<CpuOnlySafety>();
        auto spec = launch(executable, guard, "good"); spec.first_instance = UINT64_MAX;
        {
            vs::Supervisor supervisor(std::move(spec), limits, 2, safety, e.inbox);
            assert(supervisor.start(1)); assert(e.next(1, vc::Event::Kind::ready).instance == UINT64_MAX); e.empty();
            assert(supervisor.stop(2)); e.next(2, vc::Event::Kind::stopped); e.empty();
            assert(supervisor.start(3)); e.next(3, vc::Event::Kind::unavailable); e.empty();
            assert(supervisor.attempts() == 1 && supervisor.state() == vs::State::unavailable);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Every accepted command receives exactly one event even when submission
    // races shutdown at the mailbox/publication boundary. No child needed here.
    for (unsigned repetition = 0; repetition < 50; ++repetition) {
        Events e(256); auto safety = std::make_shared<CpuOnlySafety>();
        std::array<bool, 200> accepted{}, seen{};
        {
            vs::Supervisor supervisor(launch(executable, guard, "good"), limits, 2, safety, e.inbox);
            std::atomic<bool> go{false};
            std::array<std::thread, 2> submitters;
            for (unsigned p = 0; p < 2; ++p) submitters[p] = std::thread([&, p] {
                while (!go.load()) std::this_thread::yield();
                for (unsigned i = 0; i < 100; ++i) {
                    unsigned n = p * 100 + i;
                    accepted[n] = supervisor.stop(n + 1);
                    std::this_thread::yield();
                }
            });
            go = true;
            std::this_thread::sleep_for(50us);
            supervisor.shutdown();
            for (auto & t : submitters) t.join();
            for (;;) {
                auto event = e.take();
                if (!event.task) { assert(event.kind == vc::Event::Kind::stopped); break; }
                assert(event.task <= accepted.size() && accepted[event.task - 1] && !seen[event.task - 1]);
                assert(event.kind == vc::Event::Kind::stopped || event.kind == vc::Event::Kind::failed);
                seen[event.task - 1] = true;
            }
            assert(accepted == seen);
        }
        e.empty(); e.inbox->stop_accepting(); assert(e.inbox->settled());
    }
    // Ready-state health checks must run even under command traffic. Consumer is
    // wake-driven and permits the independent failure notification to interleave.
    {
        Events e(64); auto safety = std::make_shared<CpuOnlySafety>();
        auto bounded = limits; bounded.attempts = 1; // isolate health checks from valid recovery starts
        {
            vs::Supervisor supervisor(launch(executable, guard, "idle-exit"), bounded, 2, safety, e.inbox);
            assert(supervisor.start(1)); e.next(1, vc::Event::Kind::ready);
            std::atomic<bool> stop{false};
            std::atomic<unsigned> accepted{0};
            std::thread traffic([&] {
                const auto until = deadline();
                while (!stop.load()) {
                    assert(wire::Clock::now() < until);
                    if (supervisor.start(2)) ++accepted;
                    std::this_thread::yield();
                }
            });
            unsigned consumed = 0;
            for (;;) {
                auto event = e.take();
                if (!event.task) { assert(event.kind == vc::Event::Kind::failed); break; }
                assert(event.task == 2 && event.kind == vc::Event::Kind::unavailable); ++consumed;
            }
            stop = true; traffic.join();
            while (consumed < accepted.load()) {
                e.next(2, vc::Event::Kind::unavailable); ++consumed;
            }
            assert(consumed && safety->released == 1 && supervisor.attempts() == 1);
        }
        e.next(0, vc::Event::Kind::stopped); e.empty();
    }
    // Permanent failure must terminate the owning service, not return success or
    // release the turn. Subreaper observes actual PDEATHSIG cleanup of test readers.
    int previous_subreaper = 0;
    assert(::prctl(PR_GET_CHILD_SUBREAPER, &previous_subreaper) == 0);
    assert(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    for (const auto * mode : {"proof", "reap", "engine"}) {
        SpawnedProcess service; std::string error;
        assert(protected_spawn(guard, executable, {executable, "fatal", mode, guard}, {}, 2, service, error));
        ::close(service.input); ::close(service.output);
        int status;
        assert(reap_until(service.pid, deadline(), status) == ReapResult::reaped);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 86);
        if (std::string(mode) == "reap") {
            pid_t orphan = 0;
            wait_for([&] { orphan = ::waitpid(-1, &status, WNOHANG); return orphan > 0; });
            assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        }
        assert(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    }
    assert(::prctl(PR_SET_CHILD_SUBREAPER, previous_subreaper) == 0);
    assert(fd_count() == baseline);
}
int main(int argc, char ** argv) {
    if (argc == 4 && std::string(argv[1]) == "fatal") return fatal_service(argv[0], argv[3], argv[2]);
    // Launch appends instance to {exe, fake, mode}, so native child receives argc=4.
    if (argc == 4 && std::string(argv[1]) == "fake") return fake(argv[2], std::stoull(argv[3]));
    assert(argc == 3);
    tests(argv[0], argv[2]);
    std::cout << "vision lifecycle supervisor: PASS (CPU-only recovery/containment)\n";
}
