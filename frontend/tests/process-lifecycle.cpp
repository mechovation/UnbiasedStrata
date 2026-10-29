#include "../clean_process.hpp"
#include "../process_lifecycle.hpp"
#include <cassert>
#include <iostream>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>

using namespace strata_frontend;
using namespace std::chrono_literals;
static std::string self;
static std::string guard;
static void close_pipes(SpawnedProcess & p) {
    if (p.input >= 0) ::close(p.input);
    if (p.output >= 0) ::close(p.output);
    p.input = p.output = -1;
}
static char read_byte(int fd) {
    pollfd p{fd, POLLIN, 0};
    assert(::poll(&p, 1, 2000) == 1);
    char byte = 0;
    assert(::read(fd, &byte, 1) == 1);
    return byte;
}
static SpawnedProcess spawn(const std::string & mode, int control = -1) {
    SpawnedProcess child;
    std::string error;
    assert(protected_spawn(guard, self, {self, mode, guard}, {}, 2, child, error, control));
    return child;
}
int main(int argc, char ** argv) {
    assert(argc >= 3);
    self = argv[0]; guard = argv[2];
    const std::string mode = argv[1];
    if (mode == "hang" || mode == "eof" || mode == "socket") {
        int death_signal = 0;
        assert(::prctl(PR_GET_PDEATHSIG, &death_signal) == 0 && death_signal == SIGKILL);
        if (mode == "socket") {
            assert(::fcntl(3, F_GETFD) >= 0);
            assert(::write(3, "S", 1) == 1);
            return 0;
        }
        if (mode == "hang") ::signal(SIGTERM, SIG_IGN);
        assert(::write(1, "R", 1) == 1);
        if (mode == "eof") {
            char c;
            assert(::read(0, &c, 1) == 0);
            return 0;
        }
        for (;;) ::pause();
    }
    if (mode == "delayed-guard") {
        // Hold before guard exec/PR_SET_PDEATHSIG until the parent actually exits.
        const std::string expected_parent = std::to_string(::getppid());
        assert(::write(1, "B", 1) == 1);
        char c;
        assert(::read(0, &c, 1) == 0);
        ::execl(guard.c_str(), guard.c_str(), expected_parent.c_str(), self.c_str(),
                self.c_str(), "hang", guard.c_str(), static_cast<char *>(nullptr));
        return 127;
    }
    if (mode == "parent" || mode == "early-parent") {
        SpawnedProcess child;
        if (mode == "parent") child = spawn("hang");
        else {
            std::string error;
            assert(clean_spawn(self, {self, "delayed-guard", guard}, {}, 2, child, error));
        }
        assert(read_byte(child.output) == (mode == "parent" ? 'R' : 'B'));
        std::cout << child.pid << std::endl;
        for (;;) ::pause();
    }
    assert(mode == "test");
    assert(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);
    int status = 0;
    auto child = spawn("hang");
    assert(read_byte(child.output) == 'R');
    auto start = ProcessClock::now();
    assert(terminate_until(child.pid, start + 30ms, start + 2s, status) == ReapResult::reaped);
    assert(child.pid == -1 && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    close_pipes(child);

    child = spawn("eof");
    assert(read_byte(child.output) == 'R');
    ::close(child.input); child.input = -1;
    assert(reap_until(child.pid, ProcessClock::now() + 2s, status) == ReapResult::reaped);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close_pipes(child);

    int sockets[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    child = spawn("socket", sockets[1]);
    ::close(sockets[1]);
    assert(read_byte(sockets[0]) == 'S');
    ::close(sockets[0]);
    assert(reap_until(child.pid, ProcessClock::now() + 2s, status) == ReapResult::reaped);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close_pipes(child);

    // An invalid borrowed control FD must fail before stderr duplication can reuse it.
    int invalid_control = ::fcntl(2, F_DUPFD_CLOEXEC, 3);
    assert(invalid_control >= 0); ::close(invalid_control);
    SpawnedProcess invalid;
    std::string invalid_error;
    assert(!protected_spawn(guard, self, {self, "socket", guard}, {}, 2,
                            invalid, invalid_error, invalid_control));
    assert(invalid.pid == -1);

    // A mismatched expected parent must prevent target exec.
    std::string error;
    SpawnedProcess wrong;
    assert(clean_spawn(guard, {guard, "2147483647", self, self, "hang", guard}, {}, 2, wrong, error));
    assert(reap_until(wrong.pid, ProcessClock::now() + 2s, status) == ReapResult::reaped);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 125);
    close_pipes(wrong);

    // Linux ties PDEATHSIG to the spawning thread, even while the process remains alive.
    std::thread transient_owner([&] {
        child = spawn("hang");
        assert(read_byte(child.output) == 'R');
    });
    transient_owner.join();
    assert(reap_until(child.pid, ProcessClock::now() + 2s, status) == ReapResult::reaped);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    close_pipes(child);

    // Partial startup rollback: a live first child is cleaned up after second target exec fails.
    child = spawn("hang");
    assert(read_byte(child.output) == 'R');
    SpawnedProcess missing;
    assert(protected_spawn(guard, "/no/pr4/target", {"target"}, {}, 2, missing, error));
    assert(reap_until(missing.pid, ProcessClock::now() + 2s, status) == ReapResult::reaped);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 126);
    close_pipes(missing);
    start = ProcessClock::now();
    assert(terminate_until(child.pid, start, start + 2s, status) == ReapResult::reaped);
    close_pipes(child);

    // Cover both death before guard installation and death during a blocked target.
    for (const std::string parent_mode : {"early-parent", "parent"}) {
        SpawnedProcess parent;
        assert(protected_spawn(guard, self, {self, parent_mode, guard}, {}, 2, parent, error));
        std::string pid_text;
        for (char byte; (byte = read_byte(parent.output)) != '\n';) pid_text += byte;
        pid_t orphan = std::stoi(pid_text);
        assert(orphan > 1);
        assert(::kill(parent.pid, SIGKILL) == 0);
        assert(reap_until(parent.pid, ProcessClock::now() + 2s, status) == ReapResult::reaped);
        close_pipes(parent);
        // Adoption by this test subreaper makes child exit observable without kill(pid,0) ambiguity.
        assert(reap_until(orphan, ProcessClock::now() + 2s, status) == ReapResult::reaped);
        if (parent_mode == "parent") assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        else assert(WIFEXITED(status) && WEXITSTATUS(status) == 125);
    }
    assert(::waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    std::cout << "process lifecycle: PASS\n";
}
