#include "../vision_artifact.hpp"
#include "../vision_transport.hpp"
#include "../clean_process.hpp"
#include "../process_lifecycle.hpp"
#include <cassert>
#include <dirent.h>
#include <iostream>
#include <linux/falloc.h>
#include <sys/resource.h>
#include <thread>

namespace va = strata_frontend::vision_artifact;
namespace wire = strata_frontend::vision_protocol;
using namespace strata_frontend;
using namespace std::chrono_literals;
static std::atomic<bool> cancel{false};
static va::Tag tag{7, 11, 0, {}};
static auto deadline() { return va::Clock::now() + 2s; }
static int fd_count() {
    DIR * d = ::opendir("/proc/self/fd"); assert(d);
    int n = 0; while (::readdir(d)) ++n; ::closedir(d); return n;
}
static va::Artifact::Ptr rows(va::Budget & budget, uint32_t width = 4) {
    va::Artifact::Ptr out;
    assert(va::Artifact::rows(budget, tag, 1, 1, width, 65536, deadline(), cancel, out) == va::Result::ok);
    return out;
}
static int fake_reader() {
    wire::Channel channel(3);
    wire::Frame received;
    assert(channel.receive(received, {wire::Kind::encode, 7, 11, 1, 1}, 0, deadline(), cancel) == wire::IoResult::ok);
    void * writable = ::mmap(nullptr, 16, PROT_READ | PROT_WRITE, MAP_SHARED, received.fds[0], 0);
    assert(writable != MAP_FAILED);
    const float values[4] = {0, -0.0f, 1, -2}; std::memcpy(writable, values, sizeof(values));
    wire::Frame ack; ack.header = {wire::Kind::encoded, 0, 7, 11, 1};
    assert(channel.send(ack, 0, deadline(), cancel) == wire::IoResult::ok);
    char c; assert(::read(0, &c, 1) == 1 && c == 'U');
    assert(::munmap(writable, 16) == 0);
    assert(::write(1, "U", 1) == 1);
    assert(::read(0, &c, 1) == 1 && c == 'S');
    // Retaining the descriptor cannot mutate the sealed parent-owned rows.
    assert(::pwrite(received.fds[0], values, sizeof(values), 0) == -1 && errno == EPERM);
    return 0;
}
int main(int argc, char ** argv) {
    assert(argc == 3);
    if (std::string(argv[1]) == "fake") return fake_reader();
    const int baseline = fd_count();
    va::Budget budget({65536, 3});
    va::Artifact::Ptr source;
    assert(va::Artifact::source(budget, tag, "source", 6, deadline(), cancel, source) == va::Result::ok);
    assert(va::sealed_size(source->fd(), 6, 6) == va::Result::ok);
    assert(va::sealed_size(source->fd(), 5, 6) == va::Result::invalid);
    assert(::pwrite(source->fd(), "x", 1, 0) == -1 && errno == EPERM);
    assert(::ftruncate(source->fd(), 7) == -1 && errno == EPERM);
    va::Artifact::Ptr untouched = source;
    assert(va::Artifact::source(budget, tag, "too-big", 6, deadline(), cancel, untouched) == va::Result::invalid && untouched == source);
    cancel = true;
    assert(va::Artifact::source(budget, tag, "a", 1, deadline(), cancel, untouched) == va::Result::cancelled);
    cancel = false;
    assert(va::Artifact::source(budget, tag, "a", 1, va::Clock::now(), cancel, untouched) == va::Result::deadline);
    untouched.reset(); source.reset(); assert(budget.usage().bytes == 0);
    {
        auto output = rows(budget);
        assert(::ftruncate(output->fd(), 20) == -1 && errno == EPERM);
        assert(::ftruncate(output->fd(), 12) == -1 && errno == EPERM);
        float partial = 1; assert(::pwrite(output->fd(), &partial, 4, 0) == 4);
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::nonfinite);
        assert(!output->validated_rows()); // unwritten cells were poisoned, not sparse finite zeros
    }
    for (uint32_t invalid : {0x7f800000u, 0xff800000u, 0x7fc00001u}) {
        auto output = rows(budget, 1);
        assert(::pwrite(output->fd(), &invalid, 4, 0) == 4);
        assert(output->finish_rows(tag, 1, 1, 1, deadline(), cancel) == va::Result::nonfinite);
    }
    {
        auto output = rows(budget);
        uint32_t bits[4] = {0, 0x80000000u, 1, 0x7f7fffffu};
        assert(::pwrite(output->fd(), bits, sizeof(bits), 0) == sizeof(bits));
        for (int field = 0; field < 4; ++field) {
            auto wrong = tag;
            if (field == 0) ++wrong.job;
            if (field == 1) ++wrong.instance;
            if (field == 2) ++wrong.occurrence;
            if (field == 3) ++wrong.source[0];
            assert(output->finish_rows(wrong, 1, 1, 4, deadline(), cancel) == va::Result::invalid);
        }
        assert(output->finish_rows(tag, 1, 1, 3, deadline(), cancel) == va::Result::invalid);
        assert(output->finish_rows(tag, 1, 1, 4, va::Clock::now(), cancel) == va::Result::deadline);
        cancel = true;
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::cancelled);
        cancel = false;
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::ok);
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::ok);
    }
    {
        auto output = rows(budget);
        assert(::fcntl(output->fd(), F_ADD_SEALS, F_SEAL_SEAL) == 0); // malicious premature seal blocks WRITE seal
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::unsealed);
    }
    // Aggregate counters follow final shared ownership, including held/evicted rows.
    {
        va::Budget small({16, 1}); auto output = rows(small); auto held = output; output.reset();
        va::Artifact::Ptr refused;
        assert(va::Artifact::rows(small, tag, 1, 1, 1, 16, deadline(), cancel, refused) == va::Result::budget);
        assert(small.usage().bytes == 16 && small.usage().artifacts == 1);
        held.reset(); assert(small.usage().bytes == 0 && small.usage().artifacts == 0);
        assert(va::Artifact::rows(small, tag, 1, 1, 1, 16, deadline(), cancel, refused) == va::Result::ok);
    }
    // File-creation failure returns its reserved bytes/FD slot without altering the output.
    {
        struct rlimit saved{}; assert(::getrlimit(RLIMIT_NOFILE, &saved) == 0);
        auto exhausted = saved; exhausted.rlim_cur = 0;
        assert(::setrlimit(RLIMIT_NOFILE, &exhausted) == 0);
        va::Artifact::Ptr out;
        auto result = va::Artifact::rows(budget, tag, 1, 1, 4, 16, deadline(), cancel, out);
        assert(::setrlimit(RLIMIT_NOFILE, &saved) == 0);
        assert(result == va::Result::system && !out && budget.usage().bytes == 0);
    }
    // Poison detects untouched cells, not encoder correctness: hole punching is equivalent
    // to explicitly writing valid zero floats. It must remain impossible AFTER final sealing.
    {
        auto output = rows(budget);
        assert(::fallocate(output->fd(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 16) == 0);
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::ok);
        assert(::fallocate(output->fd(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 16) == -1 && errno == EPERM);
    }
    // Cancellation observed after sealing still prevents publication; later standalone
    // validation can safely rescan that immutable backing (a cancelled Job itself cannot retry).
    {
        constexpr uint32_t count = 8 * 1024 * 1024;
        va::Budget scan_budget({uint64_t(count) * 4, 1});
        va::Artifact::Ptr output;
        assert(va::Artifact::rows(scan_budget, tag, 1, 1, count, uint64_t(count) * 4, deadline(), cancel, output) == va::Result::ok);
        assert(::fallocate(output->fd(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, count * 4) == 0);
        std::atomic<bool> watching{false};
        std::thread observer([&] {
            watching = true;
            while (!(::fcntl(output->fd(), F_GET_SEALS) & F_SEAL_WRITE)) std::this_thread::yield();
            cancel = true;
        });
        while (!watching.load()) std::this_thread::yield();
        auto result = output->finish_rows(tag, 1, 1, count, deadline(), cancel);
        observer.join();
        assert(result == va::Result::cancelled && !output->validated_rows());
        cancel = false;
        assert(output->finish_rows(tag, 1, 1, count, deadline(), cancel) == va::Result::ok);
    }
    // Artifact ownership keeps the accounting state alive beyond the Budget facade.
    {
        va::Artifact::Ptr held;
        { va::Budget temporary({16, 1}); held = rows(temporary); }
        assert(::fcntl(held->fd(), F_GETFD) >= 0);
    }
    // A failed borrowed send closes only the channel, never the caller's artifact descriptor.
    {
        auto output = rows(budget);
        int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
        wire::Channel channel(sockets[0]); ::close(sockets[1]);
        wire::Header h{wire::Kind::encode, 0, 7, 11, 1}; int fd = output->fd();
        assert(channel.send_borrowed(h, nullptr, 0, &fd, 1, 0, deadline(), cancel) == wire::IoResult::system);
        assert(::fcntl(fd, F_GETFD) >= 0 && budget.usage().artifacts == 1);
    }
    // Parallel reservations stay bounded and unwind fully.
    {
        va::Budget shared({16, 1}); std::atomic<int> successes{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) threads.emplace_back([&] {
            for (int j = 0; j < 20; ++j) {
                va::Artifact::Ptr output;
                auto rc = va::Artifact::rows(shared, tag, 1, 1, 4, 16, deadline(), cancel, output);
                assert(rc == va::Result::ok || rc == va::Result::budget);
                if (rc == va::Result::ok) ++successes;
                auto usage = shared.usage(); assert(usage.bytes <= 16 && usage.artifacts <= 1);
            }
        });
        for (auto & t : threads) t.join();
        assert(successes > 0 && shared.usage().bytes == 0);
    }
    // A real clean-spawned fake reader retains a writable mapping after its completion reply.
    {
        auto output = rows(budget);
        int sockets[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
        SpawnedProcess child; std::string error;
        assert(protected_spawn(argv[2], argv[0], {argv[0], "fake", argv[2]}, {}, 2, child, error, sockets[1]));
        ::close(sockets[1]); wire::Channel channel(sockets[0]);
        wire::Header h{wire::Kind::encode, 0, 7, 11, 1}; int fd = output->fd();
        assert(channel.send_borrowed(h, nullptr, 0, &fd, 1, 0, deadline(), cancel) == wire::IoResult::ok);
        wire::Frame ack;
        assert(channel.receive(ack, {wire::Kind::encoded, 7, 11, 1, 0}, 0, deadline(), cancel) == wire::IoResult::ok);
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::unsealed);
        assert(::write(child.input, "U", 1) == 1);
        char byte; assert(::read(child.output, &byte, 1) == 1 && byte == 'U');
        assert(output->finish_rows(tag, 1, 1, 4, deadline(), cancel) == va::Result::ok);
        assert(output->validated_rows());
        assert(::write(child.input, "S", 1) == 1);
        int status;
        assert(reap_until(child.pid, deadline(), status) == ReapResult::reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        ::close(child.input); ::close(child.output);
        assert(::fcntl(output->fd(), F_GETFD) >= 0); // send_borrowed never adopted/closed the parent FD
    }
    assert(budget.usage().bytes == 0 && budget.usage().artifacts == 0 && fd_count() == baseline);
    std::cout << "vision artifact: PASS\n";
}
