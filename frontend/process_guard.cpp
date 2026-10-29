// Clean-exec trampoline: Linux parent-death protection, installed before native/GPU code.
// Usage: process-guard EXPECTED_PARENT_PID EXECUTABLE ARGV0 [ARG...]
// No shell; environment is the launcher's explicit, filtered environment.
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <sys/prctl.h>
#include <unistd.h>

int main(int argc, char ** argv) {
    if (argc < 4) return 125;
    char * end = nullptr;
    errno = 0;
    long expected = std::strtol(argv[1], &end, 10);
    if (errno || end == argv[1] || *end || expected <= 0 || expected > INT_MAX) return 125;
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) return 125;
    // Parent may have died before prctl. Never exec in that case.
    // PID 1 is valid for a container supervisor: Linux kills all remaining members
    // when that PID namespace's init exits, so reparenting-to-1 cannot hide its death.
    // Guard/target stay in the spawning parent's PID namespace.
    if (::getppid() != expected) return 125;
    ::execv(argv[2], argv + 3);
    // Fixed classification only: no executable arguments/environment in diagnostics.
    return 126;
}
