#include "../clean_process.hpp"
#include "../process_lifecycle.hpp"
#include "../vision_transport.hpp"
#include <cassert>
#include <dirent.h>
#include <iostream>
#include <sys/mman.h>
#include <thread>

using namespace strata_frontend;
using namespace strata_frontend::vision_protocol;
using namespace std::chrono_literals;
static std::atomic<bool> cancel{false};
static constexpr uint32_t cap = 1024;
static Expected expected{Kind::prepared, 7, 11, 13, 0};
static Frame frame(size_t size = 0) {
    Frame f;
    f.header = {Kind::prepared, uint32_t(size), 7, 11, 13};
    f.payload.resize(size, 42);
    return f;
}
static int count_fds() {
    DIR * d = ::opendir("/proc/self/fd"); assert(d);
    int n = 0; while (::readdir(d)) ++n;
    ::closedir(d); return n;
}
static void pair(int (&fds)[2]) { assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0); }
static void raw_send(int fd, const void * data, size_t size, const int * rights = nullptr, size_t count = 0) {
    alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * 8)]{};
    assert(count <= 8);
    iovec iov{const_cast<void *>(data), size};
    msghdr msg{}; msg.msg_iov = &iov; msg.msg_iovlen = 1;
    if (count) {
        msg.msg_control = ancillary; msg.msg_controllen = CMSG_SPACE(count * sizeof(int));
        auto c = CMSG_FIRSTHDR(&msg); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(count * sizeof(int));
        std::memcpy(CMSG_DATA(c), rights, count * sizeof(int));
    }
    assert(::sendmsg(fd, &msg, MSG_NOSIGNAL) == ssize_t(size));
}
static int fake(const std::string & mode) {
    if (mode == "abort") ::_exit(17);
    if (mode == "silence" || mode == "blocked") { for (;;) ::pause(); }
    if (mode == "partial") {
        auto h = encode_header(frame().header);
        raw_send(3, h.data(), 9);
        return 0;
    }
    if (mode == "slow") {
        auto h = encode_header(frame().header);
        for (auto byte : h) { raw_send(3, &byte, 1); std::this_thread::sleep_for(20ms); }
        return 0;
    }
    assert(mode == "good");
    Channel channel(3);
    auto f = frame(16);
    f.fds[0] = ::memfd_create("pr4-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    assert(f.fds[0] >= 0); f.fd_count = 1;
    assert(::write(f.fds[0], "source", 6) == 6);
    assert(::fcntl(f.fds[0], F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) == 0);
    assert(channel.send(f, cap, Clock::now() + 2s, cancel) == IoResult::ok);
    return 0;
}
int main(int argc, char ** argv) {
    assert(argc == 3);
    if (std::string(argv[1]) != "test") return fake(argv[1]);
    const int baseline = count_fds();
    for (const std::string mode : {"good", "partial", "abort", "silence", "slow", "blocked"}) {
        int sockets[2]; pair(sockets);
        SpawnedProcess child; std::string error;
        assert(protected_spawn(argv[2], argv[0], {argv[0], mode, argv[2]}, {}, 2, child, error, sockets[1]));
        ::close(sockets[1]);
        {
            Channel channel(sockets[0]);
            Frame out;
            if (mode == "blocked") {
                int small = 1024;
                assert(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
                auto large = frame(1024 * 1024);
                assert(channel.send(large, 1024 * 1024, Clock::now() + 120ms, cancel) == IoResult::deadline);
            } else {
                auto want = expected; if (mode == "good") want.fd_count = 1;
                auto result = channel.receive(out, want, cap, Clock::now() + (mode == "good" ? 2s : 120ms), cancel);
                if (mode == "good") {
                    assert(result == IoResult::ok && out.payload == std::vector<uint8_t>(16, 42));
                    assert(out.fd_count == 1 && (::fcntl(out.fds[0], F_GETFD) & FD_CLOEXEC));
                    assert((::fcntl(out.fds[0], F_GET_SEALS) & F_SEAL_WRITE) != 0);
                    assert(::pwrite(out.fds[0], "x", 1, 0) == -1 && errno == EPERM);
                } else if (mode == "partial" || mode == "abort") assert(result == IoResult::eof);
                else assert(result == IoResult::deadline);
            }
            if (mode != "good") assert(!channel.open());
        }
        int status;
        auto now = ProcessClock::now();
        assert(terminate_until(child.pid, now + 50ms, now + 2s, status) == ReapResult::reaped);
        ::close(child.input); ::close(child.output);
        assert(count_fds() == baseline);
    }
    // Invalid headers/identity are rejected before body allocation, despite declaring a body.
    for (int scenario = 0; scenario < 7; ++scenario) {
        int sockets[2]; pair(sockets);
        {
            Channel channel(sockets[0]);
            auto f = frame();
            if (scenario == 0) f.header.instance++;
            if (scenario == 1) f.header.job++;
            if (scenario == 2) f.header.sequence++;
            if (scenario == 3) f.header.kind = Kind::encoded;
            if (scenario == 4) f.header.payload_size = cap + 1;
            auto h = encode_header(f.header);
            if (scenario == 5) h[12] = 1;
            if (scenario == 6) h[6] = 255;
            raw_send(sockets[1], h.data(), h.size());
            Frame out;
            assert(channel.receive(out, expected, cap, Clock::now() + 1s, cancel) == IoResult::protocol);
            assert(!channel.open() && out.fd_count == 0 && out.payload.empty());
        }
        ::close(sockets[1]); assert(count_fds() == baseline);
    }
    // Truncated ancillary data, unexpected rights, missing rights, and rights after first byte.
    for (int scenario = 0; scenario < 4; ++scenario) {
        int sockets[2]; pair(sockets);
        int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC); assert(source >= 0);
        int rights[8]; for (int & fd : rights) fd = source;
        {
            Channel channel(sockets[0]);
            auto h = encode_header(frame().header);
            if (scenario == 3) {
                raw_send(sockets[1], h.data(), 1);
                raw_send(sockets[1], h.data() + 1, h.size() - 1, rights, 1);
            } else raw_send(sockets[1], h.data(), h.size(), rights, scenario == 0 ? 8 : scenario == 1 ? 1 : 0);
            Frame out;
            auto want = expected; if (scenario == 2) want.fd_count = 1;
            assert(channel.receive(out, want, cap, Clock::now() + 1s, cancel) == IoResult::protocol);
        }
        ::close(source); ::close(sockets[1]); assert(count_fds() == baseline);
    }
    // Received rights are released on partial-header/body EOF; closed-peer sends cannot SIGPIPE.
    for (bool partial_body : {false, true}) {
        int sockets[2]; pair(sockets);
        int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC); assert(source >= 0);
        {
            Channel channel(sockets[0]);
            auto h = encode_header(frame(partial_body ? 16 : 0).header);
            raw_send(sockets[1], h.data(), partial_body ? h.size() : 1, &source, 1);
            ::close(sockets[1]);
            auto want = expected; want.fd_count = 1;
            Frame out;
            assert(channel.receive(out, want, cap, Clock::now() + 1s, cancel) == IoResult::eof);
            assert(!out.fd_count);
        }
        ::close(source); assert(count_fds() == baseline);
    }
    {
        int sockets[2]; pair(sockets);
        Channel channel(sockets[0]); ::close(sockets[1]);
        auto f = frame();
        assert(channel.send(f, cap, Clock::now() + 1s, cancel) == IoResult::system);
    }
    assert(count_fds() == baseline);
    // Cancellation interrupts both blocked reads and writes, independent of incoming traffic.
    for (bool writing : {false, true}) {
        int sockets[2]; pair(sockets);
        {
            Channel channel(sockets[0]);
            const auto started = Clock::now();
            std::thread canceller([] { std::this_thread::sleep_for(30ms); cancel = true; });
            if (writing) {
                auto large = frame(1024 * 1024);
                assert(channel.send(large, 1024 * 1024, Clock::now() + 2s, cancel) == IoResult::cancelled);
            } else {
                Frame out;
                assert(channel.receive(out, expected, cap, Clock::now() + 2s, cancel) == IoResult::cancelled);
            }
            assert(Clock::now() - started < 1s);
            canceller.join(); cancel = false;
            assert(!channel.open());
        }
        ::close(sockets[1]); assert(count_fds() == baseline);
    }
    // An owner-only service health probe interrupts blocked reader IO even when
    // the reader never replies and request cancellation/deadline remain unset.
    // Probe failure poisons writes too; no partial frame can be retried.
    for (bool writing : {false, true}) {
        struct Observation {
            std::thread::id owner = std::this_thread::get_id();
            Clock::time_point fail_at = Clock::now() + 35ms;
            unsigned calls = 0;
            static bool check(void * context) noexcept {
                auto & self = *static_cast<Observation *>(context);
                assert(std::this_thread::get_id() == self.owner);
                ++self.calls;
                return Clock::now() < self.fail_at;
            }
        } observation;
        int sockets[2]; pair(sockets);
        {
            Channel channel(sockets[0], {&Observation::check, &observation});
            const auto started = Clock::now();
            Frame out = frame(4);
            if (writing) {
                int small = 1024;
                assert(::setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
                auto large = frame(1024 * 1024);
                assert(channel.send(large, 1024 * 1024, Clock::now() + 2s, cancel) == IoResult::system);
            } else {
                // Received rights stay private and are closed when a health
                // failure interrupts the rest of an otherwise valid header.
                int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC); assert(source >= 0);
                auto h = encode_header(frame().header);
                raw_send(sockets[1], h.data(), 1, &source, 1); ::close(source);
                auto want = expected; want.fd_count = 1;
                assert(channel.receive(out, want, cap, Clock::now() + 2s, cancel) == IoResult::system);
                assert(out.payload == std::vector<uint8_t>(4, 42) && !out.fd_count);
            }
            assert(!channel.open() && !cancel && observation.calls >= 4);
            assert(Clock::now() - started < 1s);
        }
        ::close(sockets[1]); assert(count_fds() == baseline);
    }
    // Reject malformed local FD counts without out-of-bounds cleanup or closing unrelated FDs.
    {
        int sockets[2]; pair(sockets);
        Channel channel(sockets[0]);
        {
            auto malformed = frame();
            malformed.fd_count = max_frame_fds + 1;
            assert(channel.send(malformed, cap, Clock::now() + 1s, cancel) == IoResult::protocol);
        }
        assert(::fcntl(sockets[1], F_GETFD) >= 0);
        ::close(sockets[1]);
    }
    assert(count_fds() == baseline);
    // Back-to-back frames are not over-read; duplicates are rejected by the next expected sequence.
    {
        int sockets[2]; pair(sockets);
        Channel sender(sockets[0]), receiver(sockets[1]);
        auto f = frame(4);
        assert(sender.send(f, cap, Clock::now() + 1s, cancel) == IoResult::ok);
        assert(sender.send(f, cap, Clock::now() + 1s, cancel) == IoResult::ok);
        Frame out;
        assert(receiver.receive(out, expected, cap, Clock::now() + 1s, cancel) == IoResult::ok);
        auto next = expected; ++next.sequence;
        assert(receiver.receive(out, next, cap, Clock::now() + 1s, cancel) == IoResult::protocol);
        assert(out.payload.size() == 4); // failed receive leaves previous output untouched
    }
    assert(count_fds() == baseline);
    // Reader command admission rejects reply kinds, stale instance, oversized
    // declarations and wrong descriptor counts before waiting for any body.
    for (int scenario = 0; scenario < 7; ++scenario) {
        int sockets[2]; pair(sockets);
        int source = ::open("/dev/null", O_RDONLY | O_CLOEXEC); assert(source >= 0);
        {
            Channel receiver(sockets[0]);
            Header command{Kind::prepare, 76, 7, 11, 1};
            if (scenario == 0) command.kind = Kind::prepared;
            if (scenario == 1) ++command.instance;
            if (scenario == 2) command.payload_size = cap + 1;
            if (scenario == 4) command.kind = Kind::release;
            if (scenario == 5) { command.kind = Kind::quit; command.job = command.sequence = 0; command.payload_size = 0; }
            if (scenario == 6) { command.kind = Kind::release; command.payload_size = 0; }
            auto h = encode_header(command);
            raw_send(sockets[1], h.data(), h.size(), &source, scenario == 3 || scenario == 5 || scenario == 6 ? 0 : 1);
            Frame out;
            assert(receiver.receive_command(out, 7, cap, Clock::now() + 1s, cancel) ==
                (scenario >= 5 ? IoResult::ok : IoResult::protocol));
            if (scenario < 5) assert(!receiver.open() && out.payload.empty() && !out.fd_count);
        }
        ::close(source); ::close(sockets[1]); assert(count_fds() == baseline);
    }
    std::cout << "vision transport: PASS\n";
}
