// Included by vision-supervisor-job.cpp: native CPU reader + native fake engine.
// The line fixture is not a production Engine adapter or a real SVE1 writer.
#include <sys/prctl.h>
namespace eh = vision_engine_handoff;
static bool fixture_send(int fd, const std::string & value) {
    return ::send(fd, value.data(), value.size(), MSG_NOSIGNAL) == ssize_t(value.size());
}
static std::string fixture_line(int fd) {
    std::string result;
    while (result.size() < 1024) {
        char c = 0; if (::recv(fd, &c, 1, 0) != 1) return {};
        if (c == '\n') return result;
        result += c;
    }
    return {};
}
static int fake_engine(const std::string & mode) {
    ::signal(SIGTERM, SIG_IGN);
    if (mode == "engine-start-cancel" || mode == "engine-start-shutdown") { signal_gate("starting"); blocked(); }
    assert(fixture_send(3, "READY\n"));
    const unsigned jobs = mode == "engine-two" ? 2 : 1;
    for (unsigned n = 0; n < jobs; ++n) {
        const auto line = fixture_line(3); assert(line.rfind("GENI ", 0) == 0);
        const auto path = line.substr(5);
        auto check_file = [&] { std::ifstream input(path); std::string text; input >> text; assert(text == "owned-fixture"); };
        check_file(); signal_gate("engine");
        if (mode == "engine-eof") return 0;
        if (mode == "engine-uncertain" || mode == "engine-stop-fail" || mode == "engine-drain-timeout") blocked();
        const bool cancel = mode == "engine-cancel" || mode == "engine-full" || mode == "engine-shutdown" ||
                            mode == "engine-deadline" || mode == "engine-active-death" || mode == "engine-reader-eof" || mode == "engine-stop";
        if (cancel) {
            assert(fixture_line(3) == "STOP"); signal_gate("stop");
            assert(fixture_send(3, "T 7\nPP 1 2\n")); // residual records must be drained, not treated as completion
        }
        const std::string resume = std::string(std::getenv("PR4_TEST_GATE")) + ".resume";
        wait_for([&] { return ::access(resume.c_str(), F_OK) == 0; });
        check_file(); assert(::unlink(resume.c_str()) == 0);
        assert(fixture_send(3, mode == "engine-error" ? "ERR fixture\n" : "DONE\n"));
    }
    // Remain resident until the persistent owner explicitly terminates/reaps us.
    blocked();
}
struct EngineSafety final : vs::Safety {
    std::atomic<bool> leased{false}, engine_turn{false}, revoked{false}, proof{true}, allow_engine{true};
    std::atomic<bool> drained{false}, reaped{true};
    std::atomic<unsigned> transfers{0}, handbacks{0}, releases{0};
    eh::Identity current{}; // persistent owner only
    std::shared_ptr<control::Request> cancel_on_transfer;
    bool acquire(uint64_t) noexcept override { assert(!leased && !revoked); leased = true; return true; }
    void service_failed() noexcept override { revoked = true; }
    bool engine_stopped() noexcept override { return reaped; }
    bool certify(uint64_t, bool, bool service) noexcept override {
        assert(!engine_turn); if (service) assert(revoked && reaped); return true;
    }
    void release(uint64_t) noexcept override { assert(leased && !engine_turn); leased = false; ++releases; }
    bool to_engine(eh::Identity id) noexcept override {
        assert(leased && !engine_turn); if (!allow_engine || revoked) return false;
        current = id; engine_turn = true; drained = false; ++transfers;
        if (cancel_on_transfer) cancel_on_transfer->cancel(control::Reason::disconnect);
        return true;
    }
    bool to_reader(eh::Identity id) noexcept override {
        assert(id == current && leased && engine_turn);
        if (!proof || (!drained && !reaped)) return false;
        engine_turn = false; ++handbacks; return true; // does NOT clear revocation
    }
};
struct FixtureFile final : eh::Resources {
    std::string path;
    explicit FixtureFile(std::string filename) : path(std::move(filename)) {
        std::ofstream output(path); output << "owned-fixture"; assert(output);
    }
    ~FixtureFile() { assert(::unlink(path.c_str()) == 0); }
};
struct NativeEngine final : eh::Engine {
    std::string executable, guard, mode, gate;
    std::shared_ptr<EngineSafety> safety;
    SpawnedProcess child;
    clean_process_detail::Fd socket;
    std::thread::id owner{};
    std::string incoming;
    std::atomic<unsigned> starts{0}, stops{0}, contains{0}, shutdowns{0};
    void on_owner() {
        if (owner == std::thread::id{}) owner = std::this_thread::get_id();
        assert(owner == std::this_thread::get_id());
    }
    ~NativeEngine() { assert(child.pid <= 0); }
    std::optional<std::string> next_line() {
        for (;;) {
            auto newline = incoming.find('\n');
            if (newline != std::string::npos) {
                auto line = incoming.substr(0, newline); incoming.erase(0, newline + 1); return line;
            }
            char buffer[128]; const auto n = ::recv(socket.value, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (n > 0) { incoming.append(buffer, size_t(n)); assert(incoming.size() <= 1024); }
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return {};
            else return "EOF";
        }
    }
    eh::Start start(eh::Identity, const eh::Resources & owned, wire::Clock::time_point until,
                    const std::atomic<bool> & cancel) noexcept override {
        on_owner(); assert(safety->engine_turn); ++starts;
        const auto & resources = dynamic_cast<const vs::EngineResources &>(owned);
        assert(resources.encoded && resources.encoded->rows.size() == 2);
        for (const auto & row : resources.encoded->rows) assert(row->validated_rows());
        const auto & file = dynamic_cast<const FixtureFile &>(*resources.request);
        if (mode == "engine-not-sent" || mode == "engine-unsent-proof-permanent") return eh::Start::not_sent;
        if (child.pid <= 0) {
            int fds[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
            socket.value = fds[0]; clean_process_detail::Fd target; target.value = fds[1];
            std::string error;
            assert(protected_spawn(guard, executable, {executable, "engine-fake", mode},
                                   {"PR4_TEST_GATE=" + gate}, 2, child, error, target.value));
            safety->reaped = false;
            ::close(child.input); child.input = -1; ::close(child.output); child.output = -1;
            bool ready = false;
            while (wire::Clock::now() < until && !cancel.load()) {
                if (auto line = next_line()) { assert(*line == "READY"); ready = true; break; }
                std::this_thread::sleep_for(1ms);
            }
            // If startup failed, an owned process exists: force containment, not
            // a definitely-unsent result which would pretend the process is safe.
            if (!ready) return eh::Start::uncertain;
        }
        assert(fixture_send(socket.value, "GENI " + file.path + "\n"));
        return mode == "engine-uncertain" ? eh::Start::uncertain : eh::Start::sent;
    }
    bool stop(eh::Identity, wire::Clock::time_point) noexcept override {
        on_owner(); assert(safety->engine_turn); ++stops;
        if (mode == "engine-stop-fail") return false;
        return fixture_send(socket.value, "STOP\n");
    }
    eh::Observation poll(eh::Identity, wire::Clock::time_point) noexcept override {
        on_owner(); assert(safety->engine_turn);
        // Bounded records per tick, independent of request deadline/progress.
        for (unsigned i = 0; i < 8; ++i) {
            auto line = next_line(); if (!line) return eh::Observation::pending;
            if (*line == "DONE" || line->rfind("ERR ", 0) == 0) {
                safety->drained = true;
                return *line == "DONE" ? eh::Observation::succeeded : eh::Observation::failed;
            }
            if (*line == "EOF") return eh::Observation::broken;
            assert(line->rfind("T ", 0) == 0 || line->rfind("PP ", 0) == 0);
        }
        return eh::Observation::pending;
    }
    bool reap(wire::Clock::time_point until) {
        if (child.pid > 0 && terminate_until(child.pid, std::min(until, wire::Clock::now() + 10ms), until, status_)
            != ReapResult::reaped) return false;
        safety->reaped = true; return true;
    }
    bool contain(eh::Identity, wire::Clock::time_point until) noexcept override {
        on_owner(); assert(safety->engine_turn); ++contains; return reap(until);
    }
    bool shutdown(wire::Clock::time_point until) noexcept override {
        on_owner(); assert(!safety->engine_turn); ++shutdowns;
        const bool safe = reap(until); if (safe && socket.value >= 0) { ::close(socket.value); socket.value = -1; }
        return safe;
    }
private:
    int status_ = 0;
};
static std::weak_ptr<FixtureFile> fatal_resources;
static EngineSafety * fatal_safety = nullptr;
static art::Budget * fatal_budget = nullptr;
static int fatal_report = -1;
static void engine_scenario(const char * executable, const char * guard, const std::string & mode) {
    const int baseline = fd_count();
    Gate gate; Events events;
    auto safety = std::make_shared<EngineSafety>();
    auto engine = std::make_shared<NativeEngine>();
    engine->executable = executable; engine->guard = guard; engine->mode = mode; engine->gate = gate.file; engine->safety = safety;
    auto budget = std::make_shared<art::Budget>(art::Usage{65536, 16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536, 8});
    const bool service_fault = mode == "engine-uncertain" || mode == "engine-eof" || mode == "engine-stop-fail" || mode == "engine-drain-timeout" || mode == "engine-start-cancel" || mode == "engine-start-shutdown";
    {
        vs::Launch launch{guard, executable, {executable, "fake", mode}, {"PR4_TEST_GATE=" + gate.file}, 100, config, caps};
        vs::Supervisor supervisor(std::move(launch), lifecycle, 2, safety, events.inbox, std::make_shared<vs::Reaper>(), engine);
        assert(supervisor.start(1)); events.take(control::Event::Kind::ready, 1);
        const unsigned jobs = mode == "engine-two" ? 2 : 1;
        for (unsigned n = 0; n < jobs; ++n) {
            const io::Identity id{20 + n, supervisor.instance(), 11 + n};
            auto control = std::make_shared<control::Request>(deadline());
            auto input = submission(id.job, control, budget, recipes);
            assert(supervisor.prepare(id.task, input)); events.take(control::Event::Kind::prepared, id.task);
            auto file = std::make_shared<FixtureFile>(gate.file + ".fixture");
            std::weak_ptr<FixtureFile> held = file;
            assert(!supervisor.generate(id, file, 2s, 300ms)); // no engine before ENCODE
            assert(supervisor.admit(id, true)); events.take(control::Event::Kind::encoded, id.task);
            for (auto wrong : {io::Identity{99,id.instance,id.job}, io::Identity{id.task,99,id.job}, io::Identity{id.task,id.instance,99}})
                assert(!supervisor.generate(wrong, file, 2s, 300ms));
            if (mode == "engine-denied") safety->allow_engine = false;
            if (mode == "engine-transfer-cancel") safety->cancel_on_transfer = control;
            const bool permanent = mode == "engine-proof-permanent" || mode == "engine-unsent-proof-permanent";
            if (mode == "engine-proof" || permanent) safety->proof = false;
            if (permanent) {
                fatal_resources = file; fatal_safety = safety.get(); fatal_budget = budget.get();
                assert(::write(fatal_report, gate.directory.data(), gate.directory.size()) == ssize_t(gate.directory.size()));
            }
            assert(supervisor.generate(id, file, 2s, 300ms));
            assert(!supervisor.generate(id, file, 2s, 300ms)); file.reset();
            if (mode == "engine-start-cancel" || mode == "engine-start-shutdown") {
                gate.await("starting"); assert(!held.expired() && !supervisor.terminal());
                if (mode == "engine-start-shutdown") supervisor.shutdown();
                else control->cancel(control::Reason::disconnect);
            }
            events.take(control::Event::Kind::engine_submitted, id.task);
            if (mode != "engine-denied" && mode != "engine-not-sent" && mode != "engine-unsent-proof-permanent" &&
                mode != "engine-uncertain" && mode != "engine-eof" && mode != "engine-start-cancel" &&
                mode != "engine-start-shutdown" && mode != "engine-transfer-cancel") {
                gate.await("engine");
                assert(!held.expired() && !supervisor.terminal());
                assert(budget->usage().artifacts == 4 && safety->engine_turn);
                auto next = submission(id.job + 1, control, budget, recipes);
                assert(!supervisor.prepare(99, next)); next.reset();
                std::vector<control::Inbox::Reservation> full;
                if (mode == "engine-full") while (auto reservation = events.inbox->reserve()) full.push_back(std::move(reservation));
                if (mode == "engine-stop") { assert(supervisor.stop(88)); }
                else if (mode == "engine-shutdown") supervisor.shutdown();
                else if (mode == "engine-active-death") supervisor.engine_died();
                else if (mode == "engine-reader-eof") { std::ofstream output(gate.file + ".reader-exit"); output << "exit"; assert(output); }
                else if (mode == "engine-deadline") control->observe(control->deadline());
                else if (mode == "engine-cancel" || mode == "engine-full" || mode == "engine-stop-fail" || mode == "engine-drain-timeout")
                    control->cancel(control::Reason::disconnect);
                if (mode == "engine-cancel" || mode == "engine-full" || mode == "engine-shutdown" || mode == "engine-deadline" || mode == "engine-active-death" || mode == "engine-reader-eof" || mode == "engine-stop") {
                    gate.await("stop"); assert(!held.expired() && !supervisor.terminal());
                    assert(budget->usage().artifacts == 4 && safety->engine_turn); // residual output is NOT drain
                }
                if (!service_fault) gate.resume();
                if (mode == "engine-proof") {
                    wait_for([&] { return supervisor.state() == vs::State::safety_blocked; });
                    assert(!held.expired() && !supervisor.terminal() && safety->engine_turn);
                    assert(budget->usage().artifacts == 4 && !supervisor.acknowledge(id));
                    safety->proof = true;
                }
                full.clear();
            }
            events.take(control::Event::Kind::job_terminal, id.task);
            auto terminal = supervisor.terminal(); assert(terminal && terminal->identity.job == id.job);
            const bool cancelled = mode == "engine-cancel" || mode == "engine-full" || mode == "engine-shutdown" ||
                mode == "engine-deadline" || mode == "engine-stop-fail" || mode == "engine-drain-timeout" || service_fault ||
                mode == "engine-active-death" || mode == "engine-reader-eof" || mode == "engine-stop" || mode == "engine-transfer-cancel";
            const bool failure = mode == "engine-error" || mode == "engine-not-sent" || mode == "engine-denied";
            assert(terminal->outcome == (cancelled ? job::Outcome::cancelled : failure ? job::Outcome::failure : job::Outcome::success));
            assert(held.expired() && !safety->engine_turn && budget->usage().artifacts == 0);
            assert(recipes->usage().bytes == 0);
            auto successor = submission(id.job + 1, control, budget, recipes);
            assert(!supervisor.prepare(99, successor)); successor.reset(); // terminal acknowledgement is still owed
            if (mode == "engine-reader-eof") events.take(control::Event::Kind::failed, 0);
            if (mode == "engine-stop") events.take(control::Event::Kind::stopped, 88);
            assert(supervisor.acknowledge(id) && !supervisor.acknowledge(id));
        }
    }
    events.take(control::Event::Kind::stopped, 0); events.empty();
    assert(safety->reaped && !safety->leased && engine->shutdowns > 0);
    if (service_fault) assert(safety->revoked && engine->contains > 0);
    if (mode == "engine-not-sent" || mode == "engine-denied" || mode == "engine-transfer-cancel" ||
        mode == "engine-start-cancel" || mode == "engine-start-shutdown") assert(engine->stops == 0);
    if (mode == "engine-transfer-cancel") assert(engine->starts == 0);
    if (mode == "engine-cancel" || mode == "engine-full" || mode == "engine-shutdown" || mode == "engine-deadline" ||
        mode == "engine-active-death" || mode == "engine-reader-eof" || mode == "engine-stop") assert(engine->stops == 1);
    if (mode == "engine-active-death") assert(safety->revoked);
    assert(fd_count() == baseline);
    events.inbox->stop_accepting(); assert(events.inbox->settled());
}
static void permanent_engine_proof(const char * executable, const char * guard, const char * mode) {
    // No threads exist here. Subreaping verifies PDEATHSIG of the remaining reader;
    // the owner must already have contained/reaped the engine before fail-stop.
    assert(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    int report[2]; assert(::pipe2(report, O_CLOEXEC) == 0);
    const pid_t pid = ::fork(); assert(pid >= 0);
    if (!pid) {
        ::close(report[0]); fatal_report = report[1];
        std::set_terminate([] {
            const bool retained = fatal_safety && fatal_safety->engine_turn && fatal_safety->revoked &&
                fatal_safety->reaped && fatal_budget && fatal_budget->usage().artifacts == 4 && !fatal_resources.expired();
            _exit(retained ? 73 : 74);
        });
        engine_scenario(executable, guard, mode); _exit(75);
    }
    ::close(report[1]);
    int status = 0;
    wait_for([&] { return ::waitpid(pid, &status, WNOHANG) == pid; });
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 73);
    char path[256]; const auto length = ::read(report[0], path, sizeof(path)); assert(length > 0);
    ::close(report[0]); const std::string directory(path, size_t(length));
    wait_for([&] {
        const pid_t child = ::waitpid(-1, &status, WNOHANG);
        if (child > 0) { assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL); return false; }
        return child == -1 && errno == ECHILD;
    });
    // _exit intentionally did not destroy resources before containment was proven.
    assert(::access((directory + "/gate.fixture").c_str(), F_OK) == 0);
    for (const auto * suffix : {"/gate.fixture", "/gate", "/gate.resume"}) ::unlink((directory + suffix).c_str());
    assert(::rmdir(directory.c_str()) == 0);
    assert(::prctl(PR_SET_CHILD_SUBREAPER, 0) == 0);
}
