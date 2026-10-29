// Single-owner, bounded child reaping. No concurrent waitpid/kill users are permitted.
#pragma once
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace strata_frontend {
using ProcessClock = std::chrono::steady_clock;
enum class ReapResult { reaped, timeout, error };
inline ReapResult reap_until(pid_t & pid, ProcessClock::time_point deadline, int & status) {
    if (pid <= 0) return ReapResult::error;
    for (;;) {
        pid_t rc = ::waitpid(pid, &status, WNOHANG);
        if (rc == pid) { pid = -1; return ReapResult::reaped; }
        // ECHILD is not proof that *this owner* established quiescence.
        if (rc < 0 && errno != EINTR) return ReapResult::error;
        auto now = ProcessClock::now();
        if (now >= deadline) return ReapResult::timeout;
        std::this_thread::sleep_until(std::min(deadline, now + std::chrono::milliseconds(5)));
    }
}
// Caller retains positive PID on error/timeout and must fail closed, not spawn a replacement.
// Reaping only proves process exit, not recovery from a GPU/driver fault.
inline ReapResult terminate_until(pid_t & pid, ProcessClock::time_point term_deadline,
                                  ProcessClock::time_point kill_deadline, int & status) {
    auto result = reap_until(pid, ProcessClock::now(), status);
    if (result != ReapResult::timeout) return result;
    if (::kill(pid, SIGTERM) != 0 && errno != ESRCH) return ReapResult::error;
    result = reap_until(pid, term_deadline, status);
    if (result != ReapResult::timeout) return result;
    if (::kill(pid, SIGKILL) != 0 && errno != ESRCH) return ReapResult::error;
    return reap_until(pid, kill_deadline, status);
}
} // namespace strata_frontend
