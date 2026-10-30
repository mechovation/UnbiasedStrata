// Existing native line protocol through the real PR4 adapter AND Supervisor/Job.
#include "../vision_engine_adapter.hpp"
namespace ea = vision_engine_adapter;
static std::string stdin_line() {
    std::string line; char c = 0;
    while (::read(0, &c, 1) == 1) { if (c == '\n') return line; line += c; }
    return {};
}
static void stdout_line(const std::string & line) {
    const std::string bytes = line + '\n'; size_t at = 0;
    while (at < bytes.size()) { const auto n = ::write(1, bytes.data() + at, bytes.size() - at); assert(n > 0); at += size_t(n); }
}
static int adapter_fake(const std::string & mode) {
    ::signal(SIGTERM, SIG_IGN);
    assert(!std::getenv("HOME")); // explicit launch environment, no ambient inheritance
    if (mode == "ready-silent" || mode == "ready-cancel" || mode == "ready-shutdown" || mode == "ready-deadline") {
        signal_gate("startup"); blocked();
    }
    if (mode == "ready-bad") { stdout_line("READY invalid stop"); blocked(); }
    if (mode == "info-flood") { for (unsigned i = 0; i < 2000; ++i) stdout_line("INFO engine=fake"); blocked(); }
    if (mode == "closed-input") ::close(0);
    stdout_line("INFO engine=fake batch_slots=0");
    stdout_line(mode == "context-refusal" ? "READY 2 stop" : mode == "no-stop" ? "READY 262144" : "READY 262144 stop");
    if (mode.rfind("health-", 0) == 0) {
        std::ofstream pid(std::string(std::getenv("PR4_TEST_GATE")) + ".pid"); pid << ::getpid(); assert(pid);
    }
    if (mode == "blocked-write" || mode == "closed-input") blocked();
    for (;;) {
        const auto command = stdin_line();
        if (command == "QUIT" || command.empty()) return 0;
        assert(command.rfind("GEN ", 0) == 0 || command.rfind("GENI ", 0) == 0);
        std::vector<std::string> fields; std::istringstream input(command); std::string word;
        while (input >> word) fields.push_back(word);
        assert(fields.size() >= 3);
        const bool image = fields[0] == "GENI";
        auto check_file = [&] {
            if (image) { std::ifstream file(fields[fields.size() - 2]); std::string content; file >> content; assert(content == "owned-fixture"); }
        };
        check_file();
        { std::ofstream output(std::string(std::getenv("PR4_TEST_GATE")) + ".command"); output << command; assert(output); }
        signal_gate("generation");
        if (mode == "eof") return 3;
        if (mode == "partial-eof") { assert(::write(1, "DONE 1 3", 8) == 8); return 3; }
        if (mode == "fatal") { stdout_line("FATAL fixture"); blocked(); }
        if (mode == "oversized") { stdout_line(std::string(8192, 'X')); blocked(); }
        if (mode == "bad-token") { stdout_line("T -1"); blocked(); }
        if (mode == "pp-junk") { stdout_line("PP 1 3 FATAL failure"); blocked(); }
        if (mode == "progress-flood") { for (;;) stdout_line("PP 1 3 1.0 1000"); }
        if (mode == "no-stop") blocked();
        stdout_line("RESUME 0"); stdout_line("PP 3 3 1.0 1000"); stdout_line("REUSED 0");
        const unsigned tokens = mode == "burst" ? 20000 : mode == "excess-tokens" ? 4 : 3;
        for (unsigned i = 0; i < tokens; ++i) stdout_line("T " + std::to_string(i % 3 + 1));
        if (mode == "cancel") {
            assert(stdin_line() == "STOP"); stdout_line("T 7"); stdout_line("PP 1 3 1.0 1000"); signal_gate("draining");
        }
        if (mode == "sink-full") blocked();
        const std::string resume = std::string(std::getenv("PR4_TEST_GATE")) + ".resume";
        wait_for([&] { return ::access(resume.c_str(), F_OK) == 0; });
        check_file(); assert(::unlink(resume.c_str()) == 0);
        if (mode == "error") stdout_line("ERR private native diagnostics are not forwarded");
        else if (mode == "bad-done") stdout_line("DONE missing fields");
        else if (mode == "count-mismatch") stdout_line("DONE 2 3 12.5 30.0 stop 0 0 0");
        else if (mode == "suffix-junk") stdout_line("DONE 3 3 12.5 30.0 stop 0 0 0 FATAL failure");
        else if (mode == "counter-junk") stdout_line("DONE 3 3 12.5 30.0 stop 0 0 0junk");
        else if (mode == "duplicate-key") stdout_line("DONE 3 3 12.5 30.0 stop 0 0 0 chunk=1 chunk=1");
        else stdout_line("DONE " + std::to_string(mode == "cancel" ? tokens + 1 : tokens) + (mode == "mismatch" ? " 99" : " 3") + " 12.5 30.0 " +
                         (mode == "cancel" ? "cancel" : mode == "nonfinite" ? "nonfinite" : "stop") +
                         (mode == "stats-full" ? " 1 2 0 5 10 0 0 1.2 3 0 0 0 0 0 1 chunk=3 experts_streamed=0" : " 1 2 0 0 0 chunk=0 experts_streamed=0"));
        if (mode == "duplicate") { stdout_line("DONE 3 3 1 1 stop 0 0 0"); blocked(); }
        if (mode == "health-eof" || mode == "health-unsolicited") {
            const std::string fail = std::string(std::getenv("PR4_TEST_GATE")) + ".fail";
            wait_for([&] { return ::access(fail.c_str(), F_OK) == 0; });
            if (mode == "health-eof") assert(::close(1) == 0);
            else stdout_line("DONE 3 3 1 1 stop");
            blocked(); // EOF/unsolicited bytes are NOT independent stopped proof
        }
    }
}
struct AdapterSink final : ea::Sink {
    std::atomic<unsigned> tokens{0}, done{0}, errors{0}, progress{0};
    std::atomic<bool> got_info{false};
    bool refuse = false;
    bool info(std::string_view text) noexcept override {
        if (text == "engine=fake batch_slots=0") got_info = true;
        return true;
    }
    bool line(eh::Identity, const EngineLine & line) noexcept override {
        assert(line.raw.empty()); // no raw diagnostics copied to consumer/log paths
        if (line.kind == EngineLine::TOKEN) { ++tokens; return !refuse; }
        if (line.kind == EngineLine::DONE) { assert(line.prompt_ms == 12.5 && line.decode_ms == 30); ++done; }
        else if (line.kind == EngineLine::ERR) { assert(line.message == "engine refused request"); ++errors; }
        else ++progress;
        return true;
    }
};
struct AdapterSafety final : vs::Safety {
    std::shared_ptr<ea::Adapter> adapter;
    bool leased = false, engine = false; eh::Identity identity{}; // owner only
    std::atomic<bool> revoked{false};
    std::atomic<unsigned> certifications{0}, releases{0};
    bool acquire(uint64_t) noexcept override { assert(!leased && !revoked); leased = true; return true; }
    void service_failed() noexcept override { revoked = true; }
    bool engine_stopped() noexcept override { return adapter->stopped(); }
    bool certify(uint64_t, bool, bool service) noexcept override {
        assert(!engine); if (service) assert(adapter->stopped()); ++certifications; return true;
    }
    void release(uint64_t) noexcept override { assert(leased && !engine); leased = false; ++releases; }
    bool to_engine(eh::Identity id) noexcept override {
        assert(leased && !engine); if (revoked) return false; engine = true; identity = id; return true;
    }
    bool to_reader(eh::Identity id) noexcept override {
        assert(engine && id == identity);
        if (!adapter->idle() && !adapter->stopped()) return false;
        engine = false; return true;
    }
};
static ea::Limits adapter_caps{4 * 1024 * 1024, 4096, 16384, 512, 1024, 2s, 30ms, 20ms};
static void request_builder_tests() {
    const eh::Identity id{1,2,3,1};
    assert(ea::Request::create(id, "GEN 3 temperature=0.6", {1,2,3}, adapter_caps)->command() == "GEN 3 temperature=0.6 1,2,3\n");
    assert(!ea::Request::create(id, "GEN 3\nQUIT", {1}, adapter_caps));
    assert(!ea::Request::create(id, "GEN 0", {1}, adapter_caps));
    assert(!ea::Request::create(id, "GENI 3 /missing-owner", {1}, adapter_caps));
    assert(!ea::Request::create(id, "GEN 3", {-1}, adapter_caps));
    assert(!ea::Request::create(id, "GEN 3", {1024}, adapter_caps));
    auto small = adapter_caps; small.command_bytes = 10;
    assert(!ea::Request::create(id, "GEN 3", {1,2,3}, small));
}
// Establish a resident real adapter child with a successful GEN, then fail it
// while the SAME owner is stuck waiting for a different native reader process.
// No test call to engine_died(), no second waitpid owner, and no monitor thread.
static void adapter_reader_health(const char * executable, const char * guard,
                                  const std::string & phase, const std::string & failure) {
    const auto baseline = fd_count(); Gate engine_gate, reader_gate; Events events;
    auto sink = std::make_shared<AdapterSink>();
    ea::Launch engine_launch{guard, executable, {executable, "adapter-fake", "health-" + failure},
                             {"PR4_TEST_GATE=" + engine_gate.file}};
    auto adapter = std::make_shared<ea::Adapter>(std::move(engine_launch), adapter_caps, 2, sink);
    auto safety = std::make_shared<AdapterSafety>(); safety->adapter = adapter;
    auto budget = std::make_shared<art::Budget>(art::Usage{65536, 16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536, 8});
    {
        vs::Launch reader{guard, executable, {executable, "fake", "engine-health-" + phase},
                          {"PR4_TEST_GATE=" + reader_gate.file}, 100, config, caps};
        vs::Supervisor supervisor(std::move(reader), lifecycle, 2, safety, events.inbox, std::make_shared<vs::Reaper>(), adapter);
        assert(supervisor.start(1)); events.take(control::Event::Kind::ready, 1);
        const io::Identity first{20, supervisor.instance(), 11};
        auto first_control = std::make_shared<control::Request>(deadline());
        auto input = submission(first.job, first_control, budget, recipes);
        assert(supervisor.prepare(first.task, input)); events.take(control::Event::Kind::prepared, first.task);
        assert(supervisor.admit(first, true)); events.take(control::Event::Kind::encoded, first.task);
        auto request = ea::Request::create({first.task, first.instance, first.job, 1}, "GEN 3", {1,2,3}, adapter_caps);
        assert(supervisor.generate(first, request, 2s, 200ms));
        events.take(control::Event::Kind::engine_submitted, first.task);
        engine_gate.await("generation"); engine_gate.resume();
        events.take(control::Event::Kind::job_terminal, first.task);
        assert(supervisor.terminal()->outcome == job::Outcome::success);
        assert(supervisor.acknowledge(first));
        assert(adapter->idle() && !adapter->stopped() && !safety->revoked);
        pid_t engine_pid = -1;
        { std::ifstream pid(engine_gate.file + ".pid"); pid >> engine_pid; assert(pid && engine_pid > 0); }

        const io::Identity second{21, supervisor.instance(), 12};
        auto second_control = std::make_shared<control::Request>(deadline());
        input = submission(second.job, second_control, budget, recipes);
        // Deliberately much longer than the observation bound checked below.
        input->prepare = 4s; input->encode = 4s;
        assert(supervisor.prepare(second.task, input));
        if (phase == "encode") {
            events.take(control::Event::Kind::prepared, second.task);
            assert(supervisor.admit(second, true));
        }
        reader_gate.await(phase);
        assert(!supervisor.terminal() && budget->usage().artifacts >= 2 && !second_control->flag());
        std::vector<control::Inbox::Reservation> full;
        while (auto reservation = events.inbox->reserve()) full.push_back(std::move(reservation));
        const auto failed_at = wire::Clock::now();
        if (failure == "death") assert(::kill(engine_pid, SIGKILL) == 0);
        else { std::ofstream fail(engine_gate.file + ".fail"); fail << "fail"; assert(fail); }
        wait_for([&] { return second_control->flag().load(); });
        assert(safety->revoked); // service revocation precedes shutdown/cancel publication
        events.take(control::Event::Kind::job_terminal, second.task);
        assert(wire::Clock::now() - failed_at < 1s); // not the blocked reader's 4s deadline
        events.take(control::Event::Kind::failed, second.task);
        events.take(control::Event::Kind::stopped, 0);
        wait_for([&] { return supervisor.state() == vs::State::exited; });
        assert(adapter->stopped() && safety->certifications == 1 && safety->releases == 1);
        assert(supervisor.terminal()->outcome == job::Outcome::cancelled);
        assert(second_control->reason() == control::Reason::cancelled);
        assert(budget->usage().artifacts == 0 && recipes->usage().snapshots == 0);
        assert(supervisor.acknowledge(second));
        assert(!supervisor.start(99) && supervisor.attempts() == 1); // never recover engine death
        full.clear(); events.empty();
    }
    assert(adapter->stopped()); adapter.reset(); safety.reset();
    for (const auto * suffix : {".pid", ".fail", ".command"}) ::unlink((engine_gate.file + suffix).c_str());
    assert(fd_count() == baseline); events.inbox->stop_accepting(); assert(events.inbox->settled());
}
static void adapter_scenario(const char * executable, const char * guard, const std::string & mode) {
    const auto baseline = fd_count(); Gate gate; Events events;
    auto sink = std::make_shared<AdapterSink>(); sink->refuse = mode == "sink-full";
    auto limits = adapter_caps; if (mode == "ready-silent") limits.ready = 100ms;
    ea::Launch engine_launch{guard, mode == "exec-failure" ? "/no/pr4/engine" : executable,
                            {executable, "adapter-fake", mode}, {"PR4_TEST_GATE=" + gate.file}};
    auto adapter = std::make_shared<ea::Adapter>(std::move(engine_launch), limits, 2, sink);
    auto safety = std::make_shared<AdapterSafety>(); safety->adapter = adapter;
    auto budget = std::make_shared<art::Budget>(art::Usage{65536, 16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536, 8});
    const bool fault = mode == "ready-silent" || mode == "ready-bad" || mode == "info-flood" || mode == "exec-failure" ||
        mode == "blocked-write" || mode == "closed-input" || mode == "eof" || mode == "partial-eof" || mode == "fatal" ||
        mode == "oversized" || mode == "bad-token" || mode == "no-stop" || mode == "bad-done" || mode == "sink-full" ||
        mode == "excess-tokens" || mode == "count-mismatch" || mode == "suffix-junk" || mode == "counter-junk" ||
        mode == "pp-junk" || mode == "duplicate-key" || mode == "progress-flood" || mode == "ready-cancel" ||
        mode == "ready-shutdown" || mode == "ready-deadline";
    const bool immediate = mode == "ready-silent" || mode == "ready-bad" || mode == "info-flood" || mode == "exec-failure" ||
        mode == "blocked-write" || mode == "closed-input" || mode == "ready-cancel" || mode == "ready-shutdown" ||
        mode == "ready-deadline" || mode == "context-refusal";
    {
        vs::Launch reader{guard, executable, {executable, "fake", mode == "two" ? "engine-two" : "engine-adapter"}, {}, 100, config, caps};
        vs::Supervisor supervisor(std::move(reader), lifecycle, 2, safety, events.inbox, std::make_shared<vs::Reaper>(), adapter);
        assert(supervisor.start(1)); events.take(control::Event::Kind::ready, 1);
        const unsigned jobs = mode == "two" ? 2 : 1;
        for (unsigned n = 0; n < jobs; ++n) {
            auto control = std::make_shared<control::Request>(mode == "progress-flood" || mode == "ready-deadline" ? wire::Clock::now() + 300ms : deadline());
            const io::Identity id{20+n, supervisor.instance(), 11+n};
            auto input = submission(id.job, control, budget, recipes);
            assert(supervisor.prepare(id.task, input)); events.take(control::Event::Kind::prepared, id.task);
            assert(supervisor.admit(id, true)); events.take(control::Event::Kind::encoded, id.task);
            auto file = std::make_shared<FixtureFile>(gate.file + ".fixture"); std::weak_ptr<FixtureFile> held = file;
            const auto max_new = mode == "burst" ? 20000u : mode == "cancel" ? 8u : 3u;
            std::string header = mode == "text" ? "GEN " : "GENI ";
            header += std::to_string(max_new) + " temperature=0.6";
            if (mode != "text") header += " image_pad=42 " + file->path;
            auto request = ea::Request::create({id.task,id.instance,id.job,1}, header,
                mode == "blocked-write" ? std::vector<int32_t>(100000, 1) : std::vector<int32_t>{1,2,3}, limits, file);
            assert(request); const std::string command(request->command());
            assert(supervisor.generate(id, request, mode == "blocked-write" ? 120ms : 2s, 200ms));
            request.reset(); file.reset();
            if (mode == "ready-cancel" || mode == "ready-shutdown") {
                gate.await("startup"); assert(!held.expired() && !supervisor.terminal());
                if (mode == "ready-shutdown") supervisor.shutdown();
                else control->cancel(control::Reason::disconnect);
            }
            events.take(control::Event::Kind::engine_submitted, id.task);
            if (!immediate) {
                gate.await("generation");
                { std::ifstream captured(gate.file + ".command"); std::string line; std::getline(captured,line); assert(line + '\n' == command); }
                if (!fault || mode == "no-stop" || mode == "bad-done" || mode == "count-mismatch" ||
                    mode == "suffix-junk" || mode == "counter-junk" || mode == "duplicate-key") {
                    assert(!held.expired());
                    if (mode == "cancel" || mode == "no-stop") control->cancel(control::Reason::disconnect);
                    if (mode == "cancel") { gate.await("draining"); assert(!held.expired() && !supervisor.terminal()); }
                    if (mode != "no-stop") gate.resume();
                }
            }
            events.take(control::Event::Kind::job_terminal, id.task);
            const auto terminal = supervisor.terminal(); assert(terminal);
            if (fault || mode == "cancel") assert(terminal->outcome == job::Outcome::cancelled);
            else if (mode == "error" || mode == "nonfinite" || mode == "mismatch" || mode == "context-refusal") assert(terminal->outcome == job::Outcome::failure);
            // A queued duplicate can now be observed inside RELEASE, before
            // terminal cleanup, or on the next idle tick after a safe success.
            else if (mode == "duplicate") assert(terminal->outcome == job::Outcome::success || terminal->outcome == job::Outcome::cancelled);
            else assert(terminal->outcome == job::Outcome::success);
            assert(held.expired() && budget->usage().artifacts == 0 && recipes->usage().bytes == 0);
            assert(supervisor.acknowledge(id));
        }
        if (mode == "duplicate") wait_for([&] { return supervisor.state() == vs::State::exited; });
        if (!fault) assert(sink->got_info && adapter->context() == (mode == "context-refusal" ? 2u : 262144u));
    }
    events.take(control::Event::Kind::stopped, 0); events.empty();
    if (fault || mode == "duplicate") assert(safety->revoked);
    if (mode == "burst") assert(sink->tokens == 20000 && sink->done == 1);
    if (mode == "two") assert(sink->tokens == 6 && sink->done == 2);
    if (mode == "excess-tokens") assert(sink->tokens == 3 && sink->done == 0);
    if (mode == "count-mismatch" || mode == "suffix-junk" || mode == "counter-junk") assert(sink->done == 0);
    assert(adapter->stopped()); adapter.reset(); safety.reset();
    ::unlink((gate.file + ".command").c_str());
    assert(fd_count() == baseline); events.inbox->stop_accepting(); assert(events.inbox->settled());
}
