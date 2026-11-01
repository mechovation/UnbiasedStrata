// Full-wire CPU fake worker composition, not a production supervisor or mtmd encoder.
#include "../vision_commands.hpp"
#include "../vision_control.hpp"
#include "../vision_job.hpp"
#include "../clean_process.hpp"
#include "../process_lifecycle.hpp"
#include <cassert>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

using namespace strata_frontend;
namespace wire = vision_protocol;
namespace vp = vision_payload;
namespace vc = vision_commands;
namespace va = vision_artifact;
namespace vj = vision_job;
namespace control = vision_control;
using namespace std::chrono_literals;
static std::atomic<bool> service_stop{false};
static vp::Config config = [] {
    vp::Config c{{}, {}, 4, 32, 1024};
    c.generation.fill(0x37); c.device.fill(0x59); // synthetic identities, never a GPU selection
    return c;
}();
static vp::Limits caps{16384, 4, 16, 1024, 65536, {4096, 64, 4096, 16384, 32, 16 * 1024 * 1024}};
static auto deadline() { return wire::Clock::now() + 3s; }
static std::string metadata() {
    std::ifstream file("frontend/tests/data/mtmd-metadata/qwen-still-0.bin", std::ios::binary);
    assert(file); return {std::istreambuf_iterator<char>(file), {}};
}
static vp::Binding binding(unsigned occurrence, uint64_t handle = 0) {
    vp::Binding b{occurrence, {}, "pr4-fixture", 6, handle, 7, 7};
    b.source.fill(0x81); // synthetic digest label; these tests do not hash/decode source bytes
    return b;
}
static vc::Payload payload(wire::Kind kind, vp::Binding b = {}, const vc::Prepared & prepared = {}) {
    vc::Payload result;
    assert(vc::build(kind, config, caps, b, result, prepared, 3) == vc::Result::ok);
    return result;
}
static std::string_view bytes(const wire::Frame & f) {
    return {reinterpret_cast<const char *>(f.payload.data()), f.payload.size()};
}
static void send(wire::Channel & channel, wire::Kind kind, uint64_t job, uint64_t seq,
                 const vc::Payload & p, int fd = -1) {
    assert(p.fd_count == (fd >= 0 ? 1u : 0u));
    assert(channel.send_borrowed({kind, uint32_t(p.bytes.size()), 7, job, seq}, p.bytes.data(),
        p.bytes.size(), fd >= 0 ? &fd : nullptr, p.fd_count, caps.payload_bytes, deadline(), service_stop) == wire::IoResult::ok);
}
static wire::Frame receive(wire::Channel & channel, wire::Kind kind, uint64_t job, uint64_t seq,
                           vp::Binding b, size_t fds) {
    wire::Frame f;
    assert(channel.receive(f, {kind, 7, job, seq, fds}, caps.payload_bytes, deadline(), service_stop) == wire::IoResult::ok);
    assert(f.header.kind == kind);
    vp::View view;
    assert(vp::validate(kind, bytes(f), f.fd_count, config, caps, b, view));
    return f;
}
static void await_blocked(int fd) {
    const auto until = deadline();
    for (;;) {
        assert(wire::Clock::now() < until);
        pollfd pfd{fd, POLLIN, 0};
        int rc = ::poll(&pfd, 1, 10);
        assert(rc >= 0 || errno == EINTR);
        if (rc > 0) {
            char byte;
            assert((pfd.revents & POLLIN) && ::read(fd, &byte, 1) == 1 && byte == 'B');
            return;
        }
    }
}
[[noreturn]] static void blocked() {
    ::signal(SIGTERM, SIG_IGN);
    assert(::write(1, "B", 1) == 1);
    for (;;) ::pause();
}
static int fake(const std::string & mode) {
    wire::Channel channel(3);
    if (mode == "silent-ready") blocked();
    auto ready = payload(wire::Kind::ready);
    if (mode == "bad-ready") ready.bytes[0] ^= 1;
    send(channel, wire::Kind::ready, 0, 0, ready);
    if (mode == "bad-ready") blocked();
    const auto meta = metadata();
    const std::string prefix(4, '\0'), suffix("\x2a\0\0\0", 4);
    std::array<wire::Frame, 2> handles;
    uint64_t sequence = 0;
    for (unsigned i = 0; i < 2; ++i) {
        handles[i] = receive(channel, wire::Kind::prepare, 11, ++sequence, binding(i), 1);
        assert(va::sealed_size(handles[i].fds[0], 6, 1024) == va::Result::ok);
        char source[6];
        assert(::pread(handles[i].fds[0], source, 6, 0) == 6 && std::string(source, 6) == "source");
        if (mode == "blocked-prepare" || mode == "deadline-prepare" || mode == "request-deadline") blocked();
        if (mode == "abort-prepare") return 7;
        auto p = payload(wire::Kind::prepared, binding(i), {100 + i, 7, 7, prefix, suffix, meta});
        if (mode == "metadata") p.bytes.back() = 1; // unsupported audio flag in genuine saved metadata
        if (mode == "remote-error") p = payload(wire::Kind::error);
        send(channel, mode == "remote-error" ? wire::Kind::error : wire::Kind::prepared,
             11, sequence + (mode == "stale" ? 1 : 0), p);
        if (mode == "metadata" || mode == "stale" || mode == "remote-error") blocked();
    }
    if (mode != "refused") {
        for (unsigned i = 0; i < 2; ++i) {
            auto f = receive(channel, wire::Kind::encode, 11, ++sequence, binding(i, 100 + i), 1);
            constexpr size_t length = 7 * 7 * 4 * sizeof(float);
            struct stat stat{};
            assert(::fstat(f.fds[0], &stat) == 0 && stat.st_size == length);
            int seals = ::fcntl(f.fds[0], F_GET_SEALS);
            assert((seals & (F_SEAL_GROW | F_SEAL_SHRINK)) == (F_SEAL_GROW | F_SEAL_SHRINK));
            void * mapping = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, f.fds[0], 0);
            assert(mapping != MAP_FAILED);
            auto * floats = static_cast<float *>(mapping);
            for (size_t n = 0; n < length / sizeof(float); ++n) floats[n] = float(i + 1);
            if (mode == "blocked-encode") blocked();
            if (mode != "mapping") assert(::munmap(mapping, length) == 0);
            send(channel, wire::Kind::encoded, 11, sequence, payload(wire::Kind::encoded, binding(i, 100 + i)));
            if (mode == "mapping") blocked();
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        auto f = receive(channel, wire::Kind::release, 11, ++sequence, binding(i, 100 + i), 0);
        assert(handles[i].fd_count == 1);
        if (mode == "silent-release") blocked();
        handles[i].clear();
        send(channel, wire::Kind::released, 11, sequence + (mode == "stale-release" ? 1 : 0),
             payload(wire::Kind::released, binding(i, 100 + i)));
        if (mode == "stale-release") blocked();
    }
    auto quit = receive(channel, wire::Kind::quit, 0, 0, {}, 0);
    return 0;
}
struct Sink final : control::Wake {
    std::atomic<unsigned> wakes{0};
    void post() noexcept override { ++wakes; }
};
static int fd_count() {
    DIR * directory = ::opendir("/proc/self/fd"); assert(directory);
    int count = 0; while (::readdir(directory)) ++count;
    ::closedir(directory); return count;
}
static void builder_tests() {
    vc::Payload unchanged; unchanged.bytes = {17}; unchanged.fd_count = 3;
    const auto meta = metadata();
    const std::string token(4, '\0');
    auto b = binding(0, 100);
    vc::Prepared recipe{100, 7, 7, token, token, meta};
    for (auto kind : {wire::Kind::ready, wire::Kind::prepare, wire::Kind::prepared, wire::Kind::encode,
                      wire::Kind::encoded, wire::Kind::release, wire::Kind::released, wire::Kind::quit, wire::Kind::error}) {
        auto p = payload(kind, b, recipe);
        auto small = caps; small.payload_bytes = uint32_t(p.bytes.size() - 1);
        assert(vc::build(kind, config, small, b, unchanged, recipe, 3) == vc::Result::invalid);
        assert(unchanged.bytes == std::vector<uint8_t>{17} && unchanged.fd_count == 3);
    }
    recipe.prefix = "x";
    assert(vc::build(wire::Kind::prepared, config, caps, b, unchanged, recipe) == vc::Result::invalid);
    recipe.prefix = token;
    std::string bad_token(4, '\xff'); recipe.suffix = bad_token;
    assert(vc::build(wire::Kind::prepared, config, caps, b, unchanged, recipe) == vc::Result::invalid);
    b.grid_x = UINT32_MAX;
    assert(vc::build(wire::Kind::encode, config, caps, b, unchanged) == vc::Result::invalid);
    assert(vc::build(wire::Kind::error, config, caps, {}, unchanged, {}, 0) == vc::Result::invalid);
    assert(unchanged.bytes == std::vector<uint8_t>{17} && unchanged.fd_count == 3);
}
// One persistent spawning/IO owner executes all scenarios. No production engine,
// GPU authority or retry policy is simulated by a success boolean here.
static void scenario(const std::string & mode, const char * executable, const char * guard) {
    const int baseline = fd_count();
    auto sink = std::make_shared<Sink>();
    control::Inbox inbox(1, sink);
    auto completion = inbox.reserve(); assert(completion);
    std::optional<control::Request> request;
    std::atomic<bool> blocked_observed{false};
    std::thread observer;
    va::Budget budget({65536, 8});
    va::Artifact::Ptr held;
    {
        int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
        SpawnedProcess child; std::string error;
        assert(protected_spawn(guard, executable, {executable, "fake", mode}, {}, 2, child, error, sockets[1]));
        ::close(sockets[1]);
        wire::Channel channel(sockets[0]);
        wire::Frame ready;
        auto rc = channel.receive(ready, {wire::Kind::ready, 7, 0, 0, 0}, caps.payload_bytes,
            mode == "silent-ready" ? wire::Clock::now() + 100ms : deadline(), service_stop);
        vp::View view;
        bool failed = rc != wire::IoResult::ok || !vp::validate(wire::Kind::ready, bytes(ready), ready.fd_count, config, caps, {}, view);
        if (mode == "silent-ready") assert(rc == wire::IoResult::deadline);
        if (mode == "bad-ready") assert(rc == wire::IoResult::ok && failed);
        if (mode != "silent-ready" && mode != "bad-ready") assert(!failed);
        std::unique_ptr<vj::Job> job;
        if (!failed) {
            request.emplace(wire::Clock::now() + (mode == "request-deadline" ? 150ms : 10s));
            if (mode == "blocked-prepare" || mode == "blocked-encode") observer = std::thread([&] {
                const auto until = deadline();
                while (!blocked_observed.load()) {
                    assert(wire::Clock::now() < until);
                    std::this_thread::yield();
                }
                request->cancel(control::Reason::disconnect);
            });
            std::vector<vj::Input> inputs;
            std::array<int, 2> source_fds{}; // borrowed; Job owns sources before any send
            for (unsigned i = 0; i < 2; ++i) {
                va::Artifact::Ptr source;
                assert(va::Artifact::source(budget, {0, 11, i, binding(i).source}, "source", 1024, deadline(), service_stop, source) == va::Result::ok);
                source_fds[i] = source->fd();
                inputs.push_back({std::move(source), "pr4-fixture"});
            }
            job = vj::Job::create(config, caps, {8192, 4096, 4096}, 7, 11, std::move(inputs)); assert(job);
            auto exchange = [&](vj::Ticket ticket, int fd) {
                auto p = payload(ticket.command, binding(ticket.occurrence, ticket.handle));
                auto until = std::min(deadline(), request->deadline());
                if (mode == "deadline-prepare") until = wire::Clock::now() + 100ms;
                const bool cleanup = ticket.command == wire::Kind::release;
                if (cleanup) until = mode == "silent-release" ? wire::Clock::now() + 100ms : deadline();
                const auto & flag = cleanup ? service_stop : request->flag();
                assert(!request->observe() || cleanup || mode == "blocked-prepare" || mode == "blocked-encode");
                auto result = channel.send_borrowed({ticket.command, uint32_t(p.bytes.size()), 7, 11, ticket.sequence},
                    p.bytes.data(), p.bytes.size(), fd >= 0 ? &fd : nullptr, p.fd_count,
                    caps.payload_bytes, until, flag);
                if ((mode == "blocked-prepare" && ticket.command == wire::Kind::prepare) ||
                    (mode == "blocked-encode" && ticket.command == wire::Kind::encode)) {
                    await_blocked(child.output);
                    blocked_observed = true;
                }
                wire::Frame reply;
                const auto expected = ticket.command == wire::Kind::prepare ? wire::Kind::prepared :
                    ticket.command == wire::Kind::encode ? wire::Kind::encoded : wire::Kind::released;
                if (result == wire::IoResult::ok) result = channel.receive(reply,
                    {expected, 7, 11, ticket.sequence, 0}, caps.payload_bytes, until, flag);
                if (result != wire::IoResult::ok) {
                    assert(mode == "stale" || mode == "stale-release" || mode == "blocked-prepare" ||
                           mode == "blocked-encode" || mode == "deadline-prepare" || mode == "request-deadline" ||
                           mode == "silent-release" || mode == "abort-prepare");
                    if (mode == "stale" || mode == "stale-release") assert(result == wire::IoResult::protocol);
                    if (mode == "blocked-prepare" || mode == "blocked-encode") assert(result == wire::IoResult::cancelled);
                    if (mode == "deadline-prepare" || mode == "request-deadline" || mode == "silent-release")
                        assert(result == wire::IoResult::deadline);
                    if (mode == "abort-prepare") assert(result == wire::IoResult::eof);
                    if (request->observe()) job->cancel();
                    // Cleanup failures must force teardown even with sticky cancellation.
                    if (cleanup || job->phase() != vj::Phase::failed) assert(job->reader_failure());
                    return false;
                }
                auto accepted = job->accept(reply, until, request->flag());
                if (mode == "metadata") assert(accepted == vj::Reply::invalid);
                if (mode == "remote-error") assert(accepted == vj::Reply::remote_error);
                if (mode == "mapping" && ticket.command == wire::Kind::encode) assert(accepted == vj::Reply::artifact_error);
                return accepted == vj::Reply::accepted;
            };
            for (unsigned i = 0; i < 2 && !failed; ++i) {
                auto ticket = job->prepare(); assert(ticket);
                failed = !exchange(*ticket, source_fds[i]);
            }
            if (!failed) {
                assert(job->admit(mode != "refused"));
                if (mode != "refused") {
                    for (unsigned i = 0; i < 2 && !failed; ++i) {
                        va::Artifact::Ptr rows;
                        assert(va::Artifact::rows(budget, {7, 11, i, binding(i).source}, 7, 7, config.width, caps.row_bytes,
                            deadline(), request->flag(), rows) == va::Result::ok);
                        auto ticket = job->encode(rows); assert(ticket);
                        failed = !exchange(*ticket, rows->fd());
                        if (!failed) {
                            assert(job->rows(i));
                            float first;
                            assert(::pread(rows->fd(), &first, sizeof(first), 0) == sizeof(first) && first == float(i + 1));
                        }
                    }
                    if (!failed) {
                        held = job->rows(0);
                        assert(job->engine_started()); // state core only: no actual GEN/engine process
                        if (mode == "cancel-cleanup") {
                            request->cancel(control::Reason::disconnect); job->cancel();
                            assert(job->phase() == vj::Phase::draining && !job->release());
                        }
                        assert(job->engine_terminal(true));
                    }
                }
                if (!failed) while (auto ticket = job->release()) {
                    if (!exchange(*ticket, -1)) { failed = true; break; }
                }
            }
        }
        assert(failed == (mode != "good" && mode != "refused" && mode != "cancel-cleanup"));
        if (failed) {
            if (job) {
                assert(job->phase() == vj::Phase::failed && !job->take_terminal());
                assert(!job->reader_stopped(false) && budget.usage().bytes > 0);
            }
            int status;
            assert(terminate_until(child.pid, wire::Clock::now() + 20ms, deadline(), status) == ReapResult::reaped);
            // This known CPU-only fake has no descendants or GPU work. Reaping is
            // sufficient ONLY for this test's explicitly no-GPU containment boundary.
            if (job) {
                assert(job->reader_stopped(true));
                const bool cancelled = mode == "blocked-prepare" || mode == "blocked-encode" || mode == "request-deadline";
                if (mode == "request-deadline") assert(request->reason() == control::Reason::deadline && request->flag().load());
                assert(job->take_terminal() == (cancelled ? vj::Outcome::cancelled : vj::Outcome::failure));
                assert(!job->take_terminal());
            }
        } else {
            assert(job && job->phase() == vj::Phase::done);
            const auto outcome = mode == "refused" ? vj::Outcome::refused :
                mode == "cancel-cleanup" ? vj::Outcome::cancelled : vj::Outcome::success;
            assert(job->take_terminal() == outcome && !job->take_terminal());
            send(channel, wire::Kind::quit, 0, 0, payload(wire::Kind::quit));
            int status;
            assert(reap_until(child.pid, deadline(), status) == ReapResult::reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        }
        ::close(child.input); ::close(child.output);
        assert(completion.publish({1, 7, 11, 1, failed ? control::Event::Kind::failed : control::Event::Kind::completed}));
        assert(!completion.publish({}));
    }
    if (observer.joinable()) observer.join();
    assert(sink->wakes == 1 && inbox.pop() && !inbox.pop());
    inbox.stop_accepting(); assert(inbox.settled());
    assert(budget.usage().artifacts == (held ? 1u : 0u));
    held.reset();
    assert(budget.usage().bytes == 0 && fd_count() == baseline);
}
int main(int argc, char ** argv) {
    assert(argc == 3);
    if (std::string(argv[1]) == "fake") return fake(argv[2]);
    builder_tests();
    std::thread owner([&] {
        for (const auto * mode : {"good", "refused", "cancel-cleanup", "silent-ready", "bad-ready", "stale",
                                 "metadata", "mapping", "blocked-prepare", "deadline-prepare", "abort-prepare", "remote-error",
                                 "request-deadline", "blocked-encode", "silent-release", "stale-release"})
            scenario(mode, argv[0], argv[2]);
    });
    owner.join();
    std::cout << "vision full-wire fake worker: PASS (16 CPU-only scenarios)\n";
}
