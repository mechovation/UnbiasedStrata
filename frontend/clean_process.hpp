// Linux/glibc clean spawn foundation. Not yet wired into Engine or the vision reader.
// Caller owns returned PID and descriptors, including bounded termination/reaping.
#pragma once
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace strata_frontend {
struct SpawnedProcess {
    pid_t pid = -1;
    int input = -1;
    int output = -1;
};

namespace clean_process_detail {
struct Fd {
    int value = -1;
    Fd() = default;
    Fd(const Fd &) = delete;
    Fd & operator=(const Fd &) = delete;
    ~Fd() { if (value >= 0) ::close(value); }
    int release() { int result = value; value = -1; return result; }
};
inline bool pipe(Fd & read_end, Fd & write_end) {
    int ends[2];
    if (::pipe2(ends, O_CLOEXEC) != 0) return false;
    read_end.value = ends[0];
    write_end.value = ends[1];
    for (Fd * fd : {&read_end, &write_end}) {
        if (fd->value < 3) {
            int high = ::fcntl(fd->value, F_DUPFD_CLOEXEC, 3);
            if (high < 0) return false;
            ::close(fd->value);
            fd->value = high;
        }
    }
    return true;
}
} // namespace clean_process_detail

// No shell, PATH search or inherited environment. stderr_fd must be an open descriptor.
// Do not pass secrets in argv. Use protected_spawn for parent-death protection.
// Optional control socket is mapped to FD 3; all other descriptors >=3 are closed.
inline bool clean_spawn(const std::string & executable,
                        const std::vector<std::string> & arguments,
                        const std::vector<std::string> & environment,
                        int stderr_fd, SpawnedProcess & result, std::string & error,
                        int control_fd = -1) {
    auto fail = [&](int code) { error = std::string("clean spawn: ") + std::strerror(code); return false; };
    if (result.pid != -1 || result.input != -1 || result.output != -1 ||
        executable.empty() || executable.find('\0') != std::string::npos || arguments.empty()) return fail(EINVAL);
    for (const auto & arg : arguments) if (arg.find('\0') != std::string::npos) return fail(EINVAL);
    for (const auto & entry : environment) {
        const auto equals = entry.find('=');
        if (equals == std::string::npos || equals == 0 || entry.find('\0') != std::string::npos) return fail(EINVAL);
    }
    using clean_process_detail::Fd;
    // Validate all borrowed descriptors before allocating any: otherwise a closed control
    // FD could be reused by the stderr duplicate and silently map the wrong resource.
    // Caller must keep borrowed descriptors open and stable throughout this call.
    if (::fcntl(stderr_fd, F_GETFD) < 0 ||
        (control_fd != -1 && ::fcntl(control_fd, F_GETFD) < 0)) return fail(errno);
    Fd diagnostic;
    diagnostic.value = ::fcntl(stderr_fd, F_DUPFD_CLOEXEC, 3);
    if (diagnostic.value < 0) return fail(errno);
    Fd control;
    if (control_fd != -1) {
        control.value = ::fcntl(control_fd, F_DUPFD_CLOEXEC, 3);
        if (control.value < 0) return fail(errno);
    }
    Fd child_in, parent_in, parent_out, child_out;
    if (!clean_process_detail::pipe(child_in, parent_in) ||
        !clean_process_detail::pipe(parent_out, child_out)) return fail(errno);

    std::vector<char *> argv, envp;
    for (const auto & arg : arguments) argv.push_back(const_cast<char *>(arg.c_str()));
    for (const auto & entry : environment) envp.push_back(const_cast<char *>(entry.c_str()));
    argv.push_back(nullptr);
    envp.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int rc = ::posix_spawn_file_actions_init(&actions);
    if (rc) return fail(rc);
    struct ActionsGuard {
        posix_spawn_file_actions_t * p;
        ~ActionsGuard() { ::posix_spawn_file_actions_destroy(p); }
    } actions_guard{&actions};
    if (!(rc = ::posix_spawn_file_actions_adddup2(&actions, child_in.value, 0)) &&
        !(rc = ::posix_spawn_file_actions_adddup2(&actions, child_out.value, 1)) &&
        !(rc = ::posix_spawn_file_actions_adddup2(&actions, diagnostic.value, 2))) {
        if (control.value >= 0) rc = ::posix_spawn_file_actions_adddup2(&actions, control.value, 3);
        if (!rc) rc = ::posix_spawn_file_actions_addclosefrom_np(&actions, control.value >= 0 ? 4 : 3);
    }
    if (rc) return fail(rc);
    posix_spawnattr_t attributes;
    rc = ::posix_spawnattr_init(&attributes);
    if (rc) return fail(rc);
    struct AttributesGuard {
        posix_spawnattr_t * p;
        ~AttributesGuard() { ::posix_spawnattr_destroy(p); }
    } attributes_guard{&attributes};
    sigset_t mask, defaults;
    ::sigemptyset(&mask);
    ::sigfillset(&defaults);
    ::sigdelset(&defaults, SIGKILL);
    ::sigdelset(&defaults, SIGSTOP);
    if ((rc = ::posix_spawnattr_setsigmask(&attributes, &mask)) ||
        (rc = ::posix_spawnattr_setsigdefault(&attributes, &defaults)) ||
        (rc = ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF))) return fail(rc);
    pid_t pid = -1;
    rc = ::posix_spawn(&pid, executable.c_str(), &actions, &attributes, argv.data(), envp.data());
    if (rc) return fail(rc);
    result.pid = pid;
    result.input = parent_in.release();
    result.output = parent_out.release();
    error.clear();
    return true;
}
// The spawning thread must remain alive for the child's lifetime: Linux PDEATHSIG
// observes death of that thread, not just process exit. Targets must not change credentials
// or clear PDEATHSIG. Trampoline/target exec failures are child exit codes 125/126,
// not synchronous spawn errors; startup must wait for READY or reap failure.
inline bool protected_spawn(const std::string & guard, const std::string & executable,
                            const std::vector<std::string> & arguments,
                            const std::vector<std::string> & environment,
                            int stderr_fd, SpawnedProcess & result, std::string & error,
                            int control_fd = -1) {
    if (arguments.empty() || executable.empty() || executable.find('\0') != std::string::npos) {
        error = "protected spawn: invalid target";
        return false;
    }
    std::vector<std::string> guarded{guard, std::to_string(::getpid()), executable};
    guarded.insert(guarded.end(), arguments.begin(), arguments.end());
    return clean_spawn(guard, guarded, environment, stderr_fd, result, error, control_fd);
}
} // namespace strata_frontend
