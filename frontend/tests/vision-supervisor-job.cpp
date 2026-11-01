// Native pre-engine Job integration through the reusable owner. No actual engine/GPU.
#include "../vision_supervisor.hpp"
#include <cassert>
#include <deque>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace strata_frontend;
namespace wire = vision_protocol;
namespace vs = vision_supervisor;
namespace io = vision_job_io;
namespace job = vision_job;
namespace art = vision_artifact;
namespace control = vision_control;
using namespace std::chrono_literals;
static vision_payload::Config config{{}, {}, 4, 32, 1024};
static vision_payload::Limits caps{16384, 4, 16, 1024, 65536, {4096, 64, 4096, 16384, 32, 16 * 1024 * 1024}};
static vs::Limits lifecycle{2s, 30ms, 20ms, 2s, 80ms, 10s, 3};
static std::atomic<bool> never{false};
static auto deadline() { return wire::Clock::now() + 5s; }
template<class F> static void wait_for(F predicate) {
    const auto until = deadline();
    while (!predicate()) { assert(wire::Clock::now() < until); std::this_thread::sleep_for(1ms); }
}
static int fd_count() {
    DIR * d = ::opendir("/proc/self/fd"); assert(d);
    int count = 0; while (::readdir(d)) ++count; ::closedir(d); return count;
}
struct Gate {
    std::string directory, file;
    Gate() {
        char path[] = "/tmp/pr4-supervisor-job-XXXXXX";
        assert(::mkdtemp(path)); directory = path; file = directory + "/gate";
    }
    ~Gate() { ::unlink(file.c_str()); ::unlink((file + ".resume").c_str()); ::rmdir(directory.c_str()); }
    void await(const std::string & value) {
        wait_for([&] { std::ifstream input(file); std::string got; input >> got; return got == value; });
    }
    void resume() { std::ofstream output(file + ".resume"); output << "resume"; assert(output); }
};
static void signal_gate(const char * phase) {
    const char * path = std::getenv("PR4_TEST_GATE"); assert(path);
    std::ofstream output(path); output << phase; assert(output);
}
[[noreturn]] static void blocked() { for (;;) ::pause(); }
static std::string metadata() {
    std::ifstream input("frontend/tests/data/mtmd-metadata/qwen-still-0.bin", std::ios::binary); assert(input);
    return {std::istreambuf_iterator<char>(input), {}};
}
static vision_payload::Binding binding(unsigned occurrence, uint64_t handle = 0) {
    return {occurrence, {}, "pr4-fixture", 6, handle, 7, 7};
}
static int fake(const std::string & mode, uint64_t instance) {
    ::signal(SIGTERM, SIG_IGN);
    const unsigned images = mode == "shared-shutdown" ? 1 : 2;
    wire::Channel channel(3);
    auto send = [&](wire::Kind kind, uint64_t id, uint64_t sequence, unsigned occurrence = 0, uint64_t handle = 0) {
        vision_commands::Payload payload;
        const auto meta = metadata();
        const std::string prefix(4, '\0'), suffix("\x2a\0\0\0", 4);
        vision_commands::Prepared prepared{handle, 7, 7, prefix, suffix, meta};
        assert(vision_commands::build(kind, config, caps, binding(occurrence, handle), payload, prepared) == vision_commands::Result::ok);
        if (kind == wire::Kind::prepared && (mode == "metadata" || mode == "proof-blocked")) payload.bytes.back() = 1;
        if (kind == wire::Kind::released && mode == "release-stale") ++sequence;
        assert(channel.send_borrowed({kind, uint32_t(payload.bytes.size()), instance, id, sequence}, payload.bytes.data(),
            payload.bytes.size(), nullptr, 0, caps.payload_bytes, deadline(), never) == wire::IoResult::ok);
    };
    auto receive = [&](wire::Kind kind, uint64_t id, uint64_t seq, unsigned occurrence, uint64_t handle, size_t fds) {
        wire::Frame f;
        auto rc = channel.receive(f, {kind, instance, id, seq, fds}, caps.payload_bytes, deadline(), never);
        if (rc == wire::IoResult::eof) blocked(); // owner may force containment instead of QUIT
        assert(rc == wire::IoResult::ok && f.header.kind == kind);
        vision_payload::View view;
        assert(vision_payload::validate(kind, {reinterpret_cast<const char *>(f.payload.data()), f.payload.size()},
                                       f.fd_count, config, caps, binding(occurrence, handle), view));
        return f;
    };
    send(wire::Kind::ready, 0, 0);
    const auto count = mode == "recipe-budget" ? 3u :
        (mode == "two-jobs" || mode == "engine-two" || mode.rfind("engine-health-", 0) == 0) ? 2u : 1u;
    for (unsigned request = 0; request < count; ++request) {
        const uint64_t id = 11 + request;
        uint64_t sequence = 0;
        std::array<wire::Frame, 2> sources;
        for (unsigned i = 0; i < images; ++i) {
            sources[i] = receive(wire::Kind::prepare, id, ++sequence, i, 0, 1);
            assert(art::sealed_size(sources[i].fds[0], 6, caps.source_bytes) == art::Result::ok);
            if (mode == "prepare-cancel" || mode == "prepare-deadline" || mode == "prepare-timeout" ||
                (request == 1 && mode == "engine-health-prepare")) {
                signal_gate("prepare"); blocked();
            }
            send(wire::Kind::prepared, id, sequence, i, 1000 + id * 2 + (mode == "duplicate" ? 0 : i));
            if (mode == "metadata" || mode == "proof-blocked" || (mode == "duplicate" && i == 1)) blocked();
        }
        const bool refuse = mode == "refused" || mode == "sticky-refused" || mode == "release-timeout" ||
                            mode == "release-stale" || mode == "two-jobs" || mode == "recipe-budget";
        if (!refuse) {
            for (unsigned i = 0; i < images; ++i) {
                auto f = receive(wire::Kind::encode, id, ++sequence, i, 1000 + id * 2 + i, 1);
                constexpr size_t length = 7 * 7 * 4 * sizeof(float);
                struct stat status{}; assert(::fstat(f.fds[0], &status) == 0 && status.st_size == length);
                int seals = ::fcntl(f.fds[0], F_GET_SEALS);
                assert((seals & (F_SEAL_GROW | F_SEAL_SHRINK)) == (F_SEAL_GROW | F_SEAL_SHRINK));
                void * mapping = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, f.fds[0], 0);
                assert(mapping != MAP_FAILED);
                if (mode == "encode-cancel" || mode == "shutdown" || mode == "shared-shutdown" || mode == "engine-died" ||
                    (request == 1 && mode == "engine-health-encode")) {
                    signal_gate("encode"); blocked();
                }
                auto * values = static_cast<float *>(mapping);
                for (size_t n = 0; n < length / sizeof(float); ++n) values[n] = float(i + 1);
                if (mode == "nonfinite") { const uint32_t nan = 0x7fc00000; std::memcpy(mapping, &nan, 4); }
                if (mode != "mapping") assert(::munmap(mapping, length) == 0);
                send(wire::Kind::encoded, id, sequence, i, 1000 + id * 2 + i);
                if (mode == "mapping" || mode == "nonfinite") blocked();
            }
            if (mode == "engine-reader-eof") {
                const std::string exit_gate = std::string(std::getenv("PR4_TEST_GATE")) + ".reader-exit";
                wait_for([&] { return ::access(exit_gate.c_str(), F_OK) == 0; });
                assert(::unlink(exit_gate.c_str()) == 0); return 0;
            }
            if (mode.rfind("engine-", 0) != 0) blocked();
            // Engine scenarios must not receive RELEASE until native engine drain
            // and parent authority handback. The parent checks its retained files.
        }
        for (unsigned i = 0; i < images; ++i) {
            auto release = receive(wire::Kind::release, id, ++sequence, i, 1000 + id * 2 + i, 0);
            if (mode == "release-timeout") { signal_gate("release"); blocked(); }
            if (mode == "sticky-refused" && i == 0) {
                signal_gate("release");
                const std::string path = std::string(std::getenv("PR4_TEST_GATE")) + ".resume";
                wait_for([&] { return ::access(path.c_str(), F_OK) == 0; });
            }
            sources[i].clear(); send(wire::Kind::released, id, sequence, i, 1000 + id * 2 + i);
            if (mode == "release-stale") blocked();
        }
    }
    auto quit = receive(wire::Kind::quit, 0, 0, 0, 0, 0);
    return 0;
}
struct CpuOnlySafety final : vs::Safety {
    std::atomic<bool> leased{false}, proof{true}, revoked{false};
    std::atomic<unsigned> releases{0};
    bool acquire(uint64_t) noexcept override { assert(!leased && !revoked); leased = true; return true; }
    void service_failed() noexcept override { revoked = true; }
    bool engine_stopped() noexcept override { return true; } // no actual engine exists in this test
    bool certify(uint64_t, bool, bool service) noexcept override { if (service) assert(revoked); return proof; }
    void release(uint64_t) noexcept override { assert(leased); leased = false; ++releases; }
};
struct Sink final : control::Wake {
    std::atomic<unsigned> pending{0};
    void post() noexcept override { ++pending; }
};
struct Events {
    std::shared_ptr<Sink> sink = std::make_shared<Sink>();
    std::shared_ptr<control::Inbox> inbox = std::make_shared<control::Inbox>(6, sink);
    std::deque<control::Event> queued;
    void pump() {
        if (!sink->pending.exchange(0)) return;
        while (auto event = inbox->pop()) queued.push_back(*event);
    }
    control::Event take(control::Event::Kind kind, uint64_t task) {
        std::optional<control::Event> result;
        wait_for([&] {
            pump();
            for (auto it = queued.begin(); it != queued.end(); ++it) if (it->kind == kind && it->task == task) {
                result = *it; queued.erase(it); return true;
            }
            return false;
        });
        return *result;
    }
    void empty() { pump(); assert(queued.empty()); }
};
static std::unique_ptr<io::Submission> submission(uint64_t id, std::shared_ptr<control::Request> control,
                                                 std::shared_ptr<art::Budget> budget, std::shared_ptr<io::RecipeBudget> recipes) {
    auto result = std::make_unique<io::Submission>();
    result->job = id; result->control = std::move(control); result->artifacts = std::move(budget);
    result->recipes = std::move(recipes);
    result->limits = {8192, 4096, 4096}; result->snapshot_bytes = 8192;
    result->prepare = 2s; result->encode = 2s; result->cleanup = 2s;
    for (unsigned i = 0; i < 2; ++i) {
        art::Artifact::Ptr source;
        assert(art::Artifact::source(*result->artifacts, {0, id, i, {}}, "source", caps.source_bytes,
                                    deadline(), never, source) == art::Result::ok);
        result->inputs.push_back({std::move(source), "pr4-fixture"});
    }
    return result;
}
static void recipe_budget_tests() {
    io::RecipeBudget empty({0, 1});
    auto text = empty.reserve(0); assert(text && empty.usage().snapshots == 1 && !empty.reserve(0));
    text.reset(); assert(empty.usage().snapshots == 0 && empty.reserve(0));
    io::RecipeBudget bytes({8, 5});
    auto full = bytes.reserve(8); assert(full && !bytes.reserve(1));
    full.reset(); assert(bytes.usage().bytes == 0);
    io::RecipeBudget count({100, 1});
    auto one = count.reserve(1); assert(one && !count.reserve(1));
    one.reset(); assert(count.reserve(1));
    std::shared_ptr<const io::RecipeBudget::Charge> held;
    { io::RecipeBudget temporary({8, 1}); held = temporary.reserve(8); assert(held); }
    held.reset(); // accounting state survives its facade
    io::RecipeBudget concurrent({16, 2});
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&] {
        for (unsigned n = 0; n < 100; ++n) {
            auto charge = concurrent.reserve(8);
            auto used = concurrent.usage(); assert(used.bytes <= 16 && used.snapshots <= 2);
        }
    });
    for (auto & thread : threads) thread.join();
    assert(concurrent.usage().bytes == 0 && concurrent.usage().snapshots == 0);
}
static void scenario(const char * executable, const char * guard, const std::string & mode) {
    const auto baseline = fd_count();
    Gate gate; Events events; auto safety = std::make_shared<CpuOnlySafety>();
    auto budget = std::make_shared<art::Budget>(art::Usage{mode == "row-budget" ? 20u : 65536u, 8});
    const uint64_t snapshot_bytes = 2 * (metadata().size() + 8);
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{2 * snapshot_bytes, 2});
    std::vector<std::shared_ptr<const io::Prepared>> retained;
    std::shared_ptr<const io::Prepared> held_recipe;
    std::shared_ptr<const io::Encoded> held_rows;
    {
        vs::Launch launch{guard, executable, {executable, "fake", mode}, {"PR4_TEST_GATE=" + gate.file}, 100, config, caps};
        vs::Supervisor supervisor(std::move(launch), lifecycle, 2, safety, events.inbox);
        assert(supervisor.start(1)); events.take(control::Event::Kind::ready, 1);
        const unsigned jobs = mode == "recipe-budget" ? 3 : mode == "two-jobs" ? 2 : 1;
        for (unsigned request = 0; request < jobs; ++request) {
            auto control = std::make_shared<control::Request>(wire::Clock::now() + (mode == "prepare-deadline" ? 200ms : 10s));
            const io::Identity id{20 + request, supervisor.instance(), 11 + request};
            auto input = submission(id.job, control, budget, recipes);
            std::shared_ptr<control::Request> live_follower;
            vision_cache::Flights flights(1,2);
            vision_cache::Flights::Held producer_ticket, follower_ticket;
            if (mode == "shared-shutdown") {
                input->inputs.resize(1);
                input->cache = std::make_shared<vision_cache::Cache>(1,65536,budget,recipes);
                live_follower = std::make_shared<control::Request>(deadline());
                producer_ticket = flights.join({config,{}},control);
                follower_ticket = flights.join({config,{}},live_follower);
                input->flights = {producer_ticket};
                input->work_control = std::make_shared<vision_cache::Interest>(input->flights);
                input->encode = 30s; // shutdown must not wait for this phase deadline
            }
            if (mode == "snapshot-budget") input->snapshot_bytes = 1;
            if (mode == "prepare-timeout") input->prepare = 150ms;
            if (mode == "release-timeout") input->cleanup = 150ms;
            if (mode == "proof-blocked") safety->proof = false;
            assert(supervisor.prepare(id.task, input) && !input);
            const bool prepare_fails = mode == "prepare-cancel" || mode == "prepare-deadline" || mode == "prepare-timeout" ||
                mode == "metadata" || mode == "duplicate" || mode == "snapshot-budget" || mode == "proof-blocked" ||
                (mode == "recipe-budget" && request == 2);
            const bool refusal = mode == "refused" || mode == "sticky-refused" || mode == "release-timeout" ||
                mode == "release-stale" || mode == "two-jobs" || (mode == "recipe-budget" && request < 2);
            if (mode == "prepare-cancel") {
                gate.await("prepare");
                // Cancellation requires no free event capacity.
                std::vector<control::Inbox::Reservation> full;
                while (auto reservation = events.inbox->reserve()) full.push_back(std::move(reservation));
                control->cancel(control::Reason::disconnect);
                events.take(control::Event::Kind::job_terminal, id.task);
                full.clear();
            }
            if (prepare_fails) events.take(control::Event::Kind::failed, id.task);
            else {
                auto prepared = events.take(control::Event::Kind::prepared, id.task); assert(prepared.job == id.job);
                held_recipe = supervisor.prepared(); assert(held_recipe && held_recipe->recipes.size() == (mode == "shared-shutdown" ? 1u : 2u));
                assert(held_recipe->identity.instance == id.instance && held_recipe->recipes[0].metadata == metadata());
                if (mode == "recipe-budget") retained.push_back(held_recipe);
                auto wrong = id; ++wrong.job; assert(!supervisor.admit(wrong, true));
                wrong = id; ++wrong.instance; assert(!supervisor.admit(wrong, true));
                wrong = id; ++wrong.task; assert(!supervisor.admit(wrong, true));
                assert(supervisor.admit(id, !refusal));
                assert(!supervisor.admit(id, !refusal));
                if (mode == "sticky-refused") {
                    gate.await("release"); control->cancel(control::Reason::disconnect); gate.resume();
                }
                if (mode == "encode-cancel" || mode == "shutdown" || mode == "shared-shutdown" || mode == "engine-died") {
                    gate.await("encode");
                    if (mode == "shutdown" || mode == "shared-shutdown") {
                        const auto start = wire::Clock::now();
                        supervisor.shutdown();
                        if (live_follower) {
                            events.take(control::Event::Kind::job_terminal,id.task);
                            assert(wire::Clock::now() - start < 3s && !live_follower->observe());
                        }
                    }
                    else if (mode == "engine-died") supervisor.engine_died();
                    else control->cancel(control::Reason::disconnect);
                }
                if (mode == "rows") {
                    events.take(control::Event::Kind::encoded, id.task);
                    held_rows = supervisor.encoded(); assert(held_rows && held_rows->rows.size() == 2);
                    assert(safety->leased); // rows do NOT authorize an engine turn
                    for (size_t i = 0; i < held_rows->rows.size(); ++i) {
                        float value = 0;
                        const auto & row = held_rows->rows[i]; assert(row->validated_rows());
                        assert(::pread(row->fd(), &value, sizeof(value), 0) == sizeof(value) && value == float(i + 1));
                        assert(::pwrite(row->fd(), &value, sizeof(value), 0) == -1 && errno == EPERM);
                    }
                    control->cancel(control::Reason::disconnect);
                } else events.take(refusal && mode != "release-timeout" && mode != "release-stale" ?
                                   control::Event::Kind::completed : control::Event::Kind::failed, id.task);
            }
            if (mode == "proof-blocked") {
                assert(supervisor.state() == vs::State::safety_blocked && !supervisor.terminal());
                assert(budget->usage().bytes == 12 && safety->leased && !safety->releases);
                assert(!supervisor.acknowledge(id));
                safety->proof = true;
                wait_for([&] { return supervisor.stop(99); });
                events.take(control::Event::Kind::stopped, 99);
            }
            if (mode != "prepare-cancel" && mode != "shared-shutdown") events.take(control::Event::Kind::job_terminal, id.task);
            auto terminal = supervisor.terminal(); assert(terminal && terminal->identity.job == id.job);
            const bool cancelled = mode == "rows" || mode == "prepare-cancel" || mode == "prepare-deadline" ||
                mode == "encode-cancel" || mode == "sticky-refused" || mode == "shutdown" || mode == "shared-shutdown" || mode == "engine-died";
            const auto expected = cancelled ? job::Outcome::cancelled : refusal ? job::Outcome::refused : job::Outcome::failure;
            assert(terminal->outcome == expected);
            if (mode == "prepare-timeout" || mode == "release-timeout") assert(terminal->timed_out);
            if (cancelled) assert(control->flag().load());
            if (mode == "prepare-deadline") assert(control->reason() == control::Reason::deadline);
            assert(!supervisor.prepared() && !supervisor.encoded());
            assert(budget->usage().artifacts == (held_rows ? 2u : 0u));
            auto wrong = id; ++wrong.job; assert(!supervisor.acknowledge(wrong));
            assert(supervisor.acknowledge(id) && !supervisor.acknowledge(id));
            assert(!supervisor.admit(id, true) && !supervisor.terminal());
            // Reusing a retired ID is never accepted, including on the same live reader.
            auto reused = submission(id.job, control, budget, recipes);
            assert(!supervisor.prepare(id.task, reused) && reused); reused.reset();
            if (mode == "engine-died") assert(safety->revoked);
            if (mode == "recipe-budget") {
                const auto count = std::min<unsigned>(request + 1, 2);
                assert(recipes->usage().bytes == count * snapshot_bytes && recipes->usage().snapshots == count);
            }
        }
    }
    events.take(control::Event::Kind::stopped, 0); events.empty();
    held_rows.reset(); held_recipe.reset(); retained.clear();
    assert(recipes->usage().bytes == 0 && recipes->usage().snapshots == 0);
    assert(budget->usage().bytes == 0 && budget->usage().artifacts == 0 && fd_count() == baseline);
    events.inbox->stop_accepting(); assert(events.inbox->settled());
}
#include "input-rejection-tests.inc"
#include "vision-supervisor-engine.inc"
#include "vision-engine-adapter.inc"
int main(int argc, char ** argv) {
    if (argc == 4 && std::string(argv[1]) == "input-fake") return input_fake(argv[2],std::stoull(argv[3]));
    if (argc == 3 && std::string(argv[1]) == "adapter-fake") return adapter_fake(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "engine-fake") return fake_engine(argv[2]);
    if (argc == 4 && std::string(argv[1]) == "fake") return fake(argv[2], std::stoull(argv[3]));
    assert(argc == 3);
    recipe_budget_tests();
    for (const auto * mode : {"first","second","repeat","live-waiter","class-one","unknown","binding","fd",
                             "cancel-before","release-timeout","prepare-budget","release-stale","release-error","proof-blocked","shutdown"})
        input_scenario(argv[0],argv[2],mode);
    std::cout << "PR6_INPUT_REJECTION_PASS scenarios=15 typed_cleanup=1 same_reader=1 faults_fatal=1 live_waiter=1 gpu=none\n";
    for (const auto * mode : {"rows", "refused", "sticky-refused", "two-jobs", "prepare-cancel", "prepare-deadline",
                             "prepare-timeout", "metadata", "duplicate", "snapshot-budget", "proof-blocked", "encode-cancel",
                             "mapping", "nonfinite", "row-budget", "release-timeout", "release-stale", "shutdown", "shared-shutdown", "engine-died", "recipe-budget"})
        scenario(argv[0], argv[2], mode);
    std::cout << "vision supervisor Job IO: PASS (21 CPU-only scenarios, shared-reader shutdown bounded)\n";
    for (const auto * mode : {"engine-success", "engine-two", "engine-cancel", "engine-full", "engine-shutdown",
                             "engine-deadline", "engine-proof", "engine-not-sent", "engine-denied", "engine-uncertain",
                             "engine-error", "engine-eof", "engine-stop-fail", "engine-drain-timeout", "engine-active-death", "engine-reader-eof", "engine-stop",
                             "engine-start-cancel", "engine-start-shutdown", "engine-transfer-cancel"})
        engine_scenario(argv[0], argv[2], mode);
    permanent_engine_proof(argv[0], argv[2], "engine-proof-permanent");
    permanent_engine_proof(argv[0], argv[2], "engine-unsent-proof-permanent");
    std::cout << "vision supervisor engine integration: PASS (20 native CPU-only scenarios, 2 fail-stop cases)\n";
    request_builder_tests();
    for (const auto * mode : {"success", "text", "two", "error", "nonfinite", "mismatch", "bad-done", "fatal", "eof",
                             "partial-eof", "duplicate", "bad-token", "oversized", "ready-silent", "blocked-write",
                             "closed-input", "ready-bad", "no-stop", "burst", "cancel", "info-flood", "exec-failure", "sink-full",
                             "excess-tokens", "count-mismatch", "suffix-junk", "counter-junk", "stats-full", "pp-junk", "duplicate-key",
                             "progress-flood", "ready-cancel", "ready-shutdown", "ready-deadline", "context-refusal"})
        adapter_scenario(argv[0], argv[2], mode);
    std::cout << "vision native engine adapter: PASS (35 integrated CPU-only scenarios)\n";
    for (const auto * phase : {"prepare", "encode"})
        for (const auto * failure : {"death", "eof", "unsolicited"})
            adapter_reader_health(argv[0], argv[2], phase, failure);
    std::cout << "vision engine death during blocked reader IO: PASS (6 native CPU-only scenarios)\n";
}
