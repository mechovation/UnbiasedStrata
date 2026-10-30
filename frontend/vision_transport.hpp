// Bounded AF_UNIX/SOCK_STREAM framing, not a recipe/payload validator.
// One owning supervisor thread; no concurrent send/receive or descriptor close.
#pragma once
#include "vision_protocol.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <new>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace strata_frontend::vision_protocol {
using Clock = std::chrono::steady_clock;
constexpr size_t max_frame_fds = 4; // wire bound, not a production resource budget
// Rights accompany exactly the first byte; no ancillary data allowed on subsequent bytes.
struct Frame {
    Header header{};
    std::vector<uint8_t> payload;
    std::array<int, max_frame_fds> fds{{-1, -1, -1, -1}};
    size_t fd_count = 0;
    Frame() = default;
    Frame(const Frame &) = delete;
    Frame & operator=(const Frame &) = delete;
    Frame(Frame && other) noexcept { *this = std::move(other); }
    Frame & operator=(Frame && other) noexcept {
        if (this != &other) {
            clear(); header = other.header; payload = std::move(other.payload);
            fds = other.fds; fd_count = other.fd_count; other.fd_count = 0;
        }
        return *this;
    }
    ~Frame() { clear(); }
    void clear() {
        for (size_t i = 0; i < std::min(fd_count, fds.size()); ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
            fds[i] = -1;
        }
        fd_count = 0;
    }
};
enum class IoResult { ok, cancelled, deadline, eof, protocol, system, resource };
struct Expected {
    Kind kind;
    uint64_t instance, job, sequence;
    size_t fd_count;
};

// Optional owner-only observation of another service component during reader IO.
// Must be bounded, nonblocking, noexcept and non-reentrant; must not touch this
// channel or destroy its owner. Borrowed context outlives the channel. false
// poisons the operation just like an IO failure, never authorizes cleanup.
struct HealthProbe {
    bool (*check)(void *) noexcept = nullptr;
    void * context = nullptr;
};
class Channel {
public:
    // Takes ownership. Only connected Unix stream sockets are supported.
    explicit Channel(int socket, HealthProbe probe = {}) : fd_(socket), probe_(probe) {}
    Channel(const Channel &) = delete;
    Channel & operator=(const Channel &) = delete;
    ~Channel() { close(); }
    bool open() const { return fd_ >= 0; }

    IoResult receive(Frame & out, const Expected & expected, uint32_t payload_cap,
                     Clock::time_point deadline, const std::atomic<bool> & cancel) {
        return receive_impl(out, &expected, 0, payload_cap, deadline, cancel);
    }
    // Reader-side command boundary: fixed command kinds/FD counts and instance
    // are checked before payload allocation. Job/sequence ordering is enforced
    // by the reader registry, never inferred from an untrusted payload.
    IoResult receive_command(Frame & out, uint64_t instance, uint32_t payload_cap,
                             Clock::time_point deadline, const std::atomic<bool> & cancel) {
        return receive_impl(out, nullptr, instance, payload_cap, deadline, cancel);
    }
private:
    IoResult receive_impl(Frame & out, const Expected * expected, uint64_t instance, uint32_t payload_cap,
                          Clock::time_point deadline, const std::atomic<bool> & cancel) {
        if (expected && expected->fd_count > max_frame_fds) return fail(IoResult::protocol);
        Frame pending;
        auto header = std::array<uint8_t, header_size>{};
        auto rc = read_bytes(header.data(), 1, &pending, deadline, cancel);
        if (rc != IoResult::ok) return fail(rc);
        rc = read_bytes(header.data() + 1, header.size() - 1, nullptr, deadline, cancel);
        if (rc != IoResult::ok) return fail(rc);
        if (!parse_header(header.data(), header.size(), payload_cap, pending.header)) return fail(IoResult::protocol);
        if (expected) {
            if (!matches(pending.header, expected->kind, expected->instance, expected->job, expected->sequence) ||
                pending.fd_count != (pending.header.kind == Kind::error ? 0 : expected->fd_count)) return fail(IoResult::protocol);
        } else {
            const auto kind = pending.header.kind;
            if (!instance || pending.header.instance != instance ||
                (kind != Kind::prepare && kind != Kind::encode && kind != Kind::release && kind != Kind::quit) ||
                pending.fd_count != (kind == Kind::prepare || kind == Kind::encode ? 1u : 0u)) return fail(IoResult::protocol);
        }
        try { pending.payload.resize(pending.header.payload_size); }
        catch (const std::bad_alloc &) { return fail(IoResult::resource); }
        rc = read_bytes(pending.payload.data(), pending.payload.size(), nullptr, deadline, cancel);
        if (rc != IoResult::ok) return fail(rc);
        rc = check(deadline, cancel);
        if (rc != IoResult::ok) return fail(rc);
        out = std::move(pending);
        return IoResult::ok;
    }

public:
    // Frame owns outgoing FDs; send never transfers local ownership.
    IoResult send(const Frame & frame, uint32_t payload_cap, Clock::time_point deadline,
                  const std::atomic<bool> & cancel) {
        return send_borrowed(frame.header, frame.payload.data(), frame.payload.size(), frame.fds.data(), frame.fd_count,
                             payload_cap, deadline, cancel);
    }
    // Borrowed buffers/FDs must stay stable until return. No copies, adoption or unaccounted
    // parent-side dup; SCM_RIGHTS creates only the receiving process's reference.
    IoResult send_borrowed(const Header & h, const uint8_t * payload, size_t size, const int * fds, size_t fd_count,
                           uint32_t payload_cap, Clock::time_point deadline, const std::atomic<bool> & cancel) {
        auto header = encode_header(h);
        Header validated{};
        if (fd_count > max_frame_fds || (fd_count && !fds) || (size && !payload) || size != h.payload_size ||
            !parse_header(header.data(), header.size(), payload_cap, validated)) return fail(IoResult::protocol);
        for (size_t i = 0; i < fd_count; ++i) if (fds[i] < 0) return fail(IoResult::protocol);
        Rights rights{fds, fd_count};
        auto rc = write_bytes(header.data(), 1, &rights, deadline, cancel);
        if (rc == IoResult::ok) rc = write_bytes(header.data() + 1, header.size() - 1, nullptr, deadline, cancel);
        if (rc == IoResult::ok) rc = write_bytes(payload, size, nullptr, deadline, cancel);
        if (rc == IoResult::ok) rc = check(deadline, cancel);
        return rc == IoResult::ok ? rc : fail(rc);
    }
private:
    struct Rights { const int * fds; size_t count; };
    int fd_;
    const HealthProbe probe_;
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    // Any partial operation failure poisons the stream. Never resynchronize on attacker bytes.
    IoResult fail(IoResult result) { close(); return result; }
    IoResult check(Clock::time_point deadline, const std::atomic<bool> & cancel) const {
        if (probe_.check && !probe_.check(probe_.context)) return IoResult::system;
        if (cancel.load()) return IoResult::cancelled;
        if (Clock::now() >= deadline) return IoResult::deadline;
        return fd_ < 0 ? IoResult::system : IoResult::ok;
    }
    IoResult wait(short events, Clock::time_point deadline, const std::atomic<bool> & cancel) {
        for (;;) {
            auto rc = check(deadline, cancel);
            if (rc != IoResult::ok) return rc;
            auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            int ms = int(std::max<int64_t>(0, std::min<int64_t>(remaining, 10)));
            pollfd p{fd_, events, 0};
            int n = ::poll(&p, 1, ms);
            const int poll_error = errno;
            // Observe health/cancellation/deadline after EVERY bounded poll too,
            // including readable/error returns and EINTR. Progress cannot hide a
            // failed engine or extend either the 10ms poll or absolute deadline.
            rc = check(deadline, cancel);
            if (rc != IoResult::ok) return rc;
            if (n > 0) return (p.revents & POLLNVAL) ? IoResult::system : IoResult::ok;
            if (n < 0 && poll_error != EINTR) return IoResult::system;
        }
    }
    IoResult read_bytes(uint8_t * bytes, size_t size, Frame * rights,
                        Clock::time_point deadline, const std::atomic<bool> & cancel) {
        while (size) {
            auto rc = check(deadline, cancel);
            if (rc != IoResult::ok) return rc;
            alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * max_frame_fds)]{};
            iovec iov{bytes, size};
            msghdr msg{};
            msg.msg_iov = &iov; msg.msg_iovlen = 1;
            msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
            ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) return IoResult::system;
                rc = wait(POLLIN, deadline, cancel);
                if (rc != IoResult::ok) return rc;
                continue;
            }
            bool bad = (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0;
            for (cmsghdr * c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS || c->cmsg_len < CMSG_LEN(0)) {
                    bad = true; continue;
                }
                size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                if ((c->cmsg_len - CMSG_LEN(0)) % sizeof(int)) bad = true;
                for (size_t i = 0; i < count; ++i) {
                    int received;
                    std::memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                    if (rights && rights->fd_count < max_frame_fds) rights->fds[rights->fd_count++] = received;
                    else { ::close(received); bad = true; }
                }
            }
            if (bad) return IoResult::protocol;
            if (n == 0) return IoResult::eof;
            bytes += n; size -= size_t(n);
        }
        return IoResult::ok;
    }
    IoResult write_bytes(const uint8_t * bytes, size_t size, const Rights * rights,
                         Clock::time_point deadline, const std::atomic<bool> & cancel) {
        while (size) {
            auto rc = check(deadline, cancel);
            if (rc != IoResult::ok) return rc;
            alignas(cmsghdr) char ancillary[CMSG_SPACE(sizeof(int) * max_frame_fds)]{};
            iovec iov{const_cast<uint8_t *>(bytes), size};
            msghdr msg{};
            msg.msg_iov = &iov; msg.msg_iovlen = 1;
            if (rights && rights->count) {
                msg.msg_control = ancillary;
                msg.msg_controllen = CMSG_SPACE(rights->count * sizeof(int));
                cmsghdr * c = CMSG_FIRSTHDR(&msg);
                c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS;
                c->cmsg_len = CMSG_LEN(rights->count * sizeof(int));
                std::memcpy(CMSG_DATA(c), rights->fds, rights->count * sizeof(int));
            }
            ssize_t n = ::sendmsg(fd_, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN && errno != EWOULDBLOCK) return IoResult::system;
                rc = wait(POLLOUT, deadline, cancel);
                if (rc != IoResult::ok) return rc;
                continue;
            }
            if (n == 0) return IoResult::system;
            bytes += n; size -= size_t(n); rights = nullptr;
        }
        return IoResult::ok;
    }
};
} // namespace strata_frontend::vision_protocol
