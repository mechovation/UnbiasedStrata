#include "../clean_process.hpp"
#include "../vision_protocol.hpp"
#include <cassert>
#include <cstdlib>
#include <dirent.h>
#include <iostream>
#include <poll.h>
#include <sys/wait.h>
#include <thread>
#include <atomic>

using namespace strata_frontend;
static int fd_count() {
    DIR * d = ::opendir("/proc/self/fd");
    assert(d);
    int n = 0;
    while (::readdir(d)) ++n;
    ::closedir(d);
    return n;
}
static int fake_reader() {
    assert(!::getenv("PR4_NOT_ALLOWED"));
    assert(std::string(::getenv("PR4_ALLOWED")) == "yes");
    for (int fd = 3; fd < 256; ++fd) assert(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    sigset_t mask;
    assert(::sigprocmask(SIG_SETMASK, nullptr, &mask) == 0);
    assert(!::sigismember(&mask, SIGUSR1));
    struct sigaction action{};
    assert(::sigaction(SIGPIPE, nullptr, &action) == 0 && action.sa_handler == SIG_DFL);
    // Minimal valid READY header, fragmented deliberately.
    std::array<uint8_t, 40> header{};
    header[0] = 'S'; header[1] = 'V'; header[2] = 'R'; header[3] = '1';
    header[4] = 1; header[6] = 1; header[16] = 7;
    for (auto byte : header) assert(::write(1, &byte, 1) == 1);
    char c;
    assert(::read(0, &c, 1) == 1 && c == 'Q');
    return 0;
}
static void reap(SpawnedProcess & child) {
    ::close(child.input); ::close(child.output);
    int status;
    assert(::waitpid(child.pid, &status, 0) == child.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(int argc, char ** argv) {
    if (argc == 2) return fake_reader();
    (void) argv;
    const std::string executable = "/proc/self/exe";
    ::setenv("PR4_NOT_ALLOWED", "dummy-not-a-secret", 1);
    int unrelated = ::open("/dev/null", O_RDONLY); // deliberately NOT CLOEXEC
    assert(unrelated >= 0);
    sigset_t blocked, original;
    ::sigemptyset(&blocked); ::sigaddset(&blocked, SIGUSR1);
    assert(::pthread_sigmask(SIG_BLOCK, &blocked, &original) == 0);
    auto old_pipe = ::signal(SIGPIPE, SIG_IGN);
    std::atomic<bool> stop{false};
    std::thread other([&] { while (!stop.load()) std::this_thread::yield(); });
    const int before = fd_count();
    for (int i = 0; i < 25; ++i) {
        SpawnedProcess child;
        std::string error;
        assert(clean_spawn(executable, {"fake-reader", "child"}, {"PR4_ALLOWED=yes"}, 2, child, error));
        std::array<uint8_t, 40> bytes{};
        size_t used = 0;
        while (used < bytes.size()) {
            pollfd p{child.output, POLLIN, 0};
            assert(::poll(&p, 1, 2000) == 1);
            ssize_t n = ::read(child.output, bytes.data() + used, bytes.size() - used);
            assert(n > 0); used += size_t(n);
        }
        using namespace vision_protocol;
        Header h{};
        assert(parse_header(bytes.data(), bytes.size(), 1024, h));
        assert(matches(h, Kind::ready, 7, 0, 0));
        assert(!matches(h, Kind::ready, 8, 0, 0));
        for (size_t length = 0; length < 40; ++length) assert(!parse_header(bytes.data(), length, 1024, h));
        assert(!parse_header(bytes.data(), 41, 1024, h));
        for (size_t offset : {size_t(0), size_t(4), size_t(6), size_t(12), size_t(24), size_t(32)}) {
            auto corrupt = bytes; corrupt[offset] = 255;
            assert(!parse_header(corrupt.data(), corrupt.size(), 1024, h));
        }
        auto oversized = bytes; oversized[10] = 1;
        assert(!parse_header(oversized.data(), oversized.size(), 1024, h));
        assert(::write(child.input, "Q", 1) == 1);
        reap(child);
        SpawnedProcess failed;
        assert(!clean_spawn("/nonexistent/pr4-reader", {"reader"}, {}, 2, failed, error));
        assert(failed.pid == -1 && failed.input == -1 && failed.output == -1);
        assert(!clean_spawn(executable, {}, {}, 2, failed, error));
        assert(!clean_spawn(executable, {"reader"}, {"invalid"}, 2, failed, error));
        assert(fd_count() == before);
    }
    stop = true; other.join();
    ::signal(SIGPIPE, old_pipe);
    assert(::pthread_sigmask(SIG_SETMASK, &original, nullptr) == 0);
    ::close(unrelated);
    // Closed stdin/stdout must not collide with newly allocated pipe endpoints.
    int saved_in = ::fcntl(0, F_DUPFD_CLOEXEC, 3);
    int saved_out = ::fcntl(1, F_DUPFD_CLOEXEC, 3);
    assert(saved_in >= 0 && saved_out >= 0);
    ::close(0); ::close(1);
    SpawnedProcess closed_stdio;
    std::string error;
    assert(clean_spawn(executable, {"fake-reader", "child"}, {"PR4_ALLOWED=yes"}, 2, closed_stdio, error));
    assert(::dup2(saved_in, 0) == 0 && ::dup2(saved_out, 1) == 1);
    ::close(saved_in); ::close(saved_out);
    assert(::write(closed_stdio.input, "Q", 1) == 1);
    std::array<char, 40> ready{};
    size_t received = 0;
    while (received < ready.size()) {
        ssize_t n = ::read(closed_stdio.output, ready.data() + received, ready.size() - received);
        assert(n > 0);
        received += size_t(n);
    }
    reap(closed_stdio);
    using namespace vision_protocol;
    Header reply{Kind::encoded, 0, 12, 34, 56};
    assert(matches(reply, Kind::encoded, 12, 34, 56));
    assert(!matches(reply, Kind::encoded, 12, 35, 56));
    assert(!matches(reply, Kind::encoded, 12, 34, 57));
    assert(!matches(reply, Kind::prepared, 12, 34, 56));
    reply.kind = Kind::input_rejected;
    const auto typed_header = encode_header(reply); Header typed{};
    assert(parse_header(typed_header.data(),typed_header.size(),1024,typed));
    assert(matches(reply,Kind::prepared,12,34,56));
    for (auto expected : {Kind::encoded,Kind::released,Kind::ready,Kind::quit})
        assert(!matches(reply,expected,12,34,56));
    reply.kind = static_cast<Kind>(11);
    const auto unknown_header = encode_header(reply);
    assert(!parse_header(unknown_header.data(),unknown_header.size(),1024,typed));
    std::cout << "clean-process/header foundations: PASS (25 threaded spawn cycles)\n";
}
