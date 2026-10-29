// PR4 parent-owned memfd artifacts. No paths supplied by the reader, no GPU work.
// Caps cover backing bytes and parent-owned artifact FDs, not decoder/cache heap memory.
#pragma once
#include "vision_payload.hpp"
#include <atomic>
#include <chrono>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace strata_frontend::vision_artifact {
using Clock = std::chrono::steady_clock;
enum class Result { ok, invalid, budget, system, cancelled, deadline, unsealed, nonfinite };
struct Tag {
    uint64_t instance, job;
    uint32_t occurrence;
    vision_payload::Digest source;
    bool operator==(const Tag & b) const {
        return instance == b.instance && job == b.job && occurrence == b.occurrence && source == b.source;
    }
};
struct Usage { uint64_t bytes; size_t artifacts; };
class Artifact;
class Budget {
    struct State {
        std::mutex mutex;
        const Usage cap;
        Usage used{0, 0};
        explicit State(Usage c) : cap(c) {}
    };
    std::shared_ptr<State> state_;
    friend class Artifact;
public:
    explicit Budget(Usage cap) : state_(std::make_shared<State>(cap)) {}
    Usage usage() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->used; }
};
inline Result checkpoint(Clock::time_point deadline, const std::atomic<bool> & cancel) {
    if (cancel.load()) return Result::cancelled;
    return Clock::now() >= deadline ? Result::deadline : Result::ok;
}
inline Result sealed_size(int fd, uint64_t bytes, uint64_t cap) {
    if (!bytes || bytes > cap || bytes > uint64_t(std::numeric_limits<off_t>::max())) return Result::invalid;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || uint64_t(st.st_size) != bytes) return Result::invalid;
    int seals = ::fcntl(fd, F_GET_SEALS);
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    return seals >= 0 && (seals & required) == required ? Result::ok : Result::unsealed;
}
class Artifact {
public:
    using Ptr = std::shared_ptr<Artifact>;
    enum class Kind { source, rows };
    Artifact(const Artifact &) = delete;
    Artifact & operator=(const Artifact &) = delete;
    ~Artifact() {
        if (fd_ >= 0) ::close(fd_);
        if (charged_) {
            std::lock_guard<std::mutex> lock(budget_->mutex);
            budget_->used.bytes -= bytes_; --budget_->used.artifacts;
        }
    }
    // Borrow only; never close/adopt this FD. Transmission duplicates a reader-side reference.
    // Keep the Artifact alive until RELEASE acknowledgement or confirmed reader termination.
    int fd() const { return fd_; }
    uint64_t bytes() const { return bytes_; }
    const Tag & tag() const { return tag_; }
    Kind kind() const { return kind_; }
    bool validated_rows() const { return validated_; }
    bool writable_shape(uint32_t x, uint32_t y, uint32_t width) const {
        int seals = ::fcntl(fd_, F_GET_SEALS);
        return kind_ == Kind::rows && !validated_ && x_ == x && y_ == y && width_ == width &&
               seals >= 0 && !(seals & (F_SEAL_WRITE | F_SEAL_SEAL));
    }

    static Result source(Budget & budget, Tag tag, std::string_view bytes, uint64_t per_source_cap,
                         Clock::time_point deadline, const std::atomic<bool> & cancel, Ptr & out) {
        if (bytes.empty() || bytes.size() > per_source_cap || bytes.size() > 10 * 1024 * 1024 || !tag.job) return Result::invalid;
        auto rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
        Ptr pending;
        rc = allocate(budget, tag, Kind::source, bytes.size(), pending); if (rc != Result::ok) return rc;
        size_t offset = 0;
        while (offset < bytes.size()) {
            rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
            size_t n = std::min<size_t>(16384, bytes.size() - offset);
            ssize_t wrote = ::pwrite(pending->fd_, bytes.data() + offset, n, off_t(offset));
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) return Result::system;
            offset += size_t(wrote);
        }
        if (::fcntl(pending->fd_, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL) != 0) return Result::unsealed;
        rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
        out = std::move(pending); return Result::ok;
    }
    static Result rows(Budget & budget, Tag tag, uint32_t x, uint32_t y, uint32_t width,
                       uint64_t row_cap, Clock::time_point deadline, const std::atomic<bool> & cancel, Ptr & out) {
        auto rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
        uint64_t bytes;
        if (!tag.job || !tag.instance || !vision_payload::rows(x, y, width, row_cap, bytes)) return Result::invalid;
        Ptr pending;
        rc = allocate(budget, tag, Kind::rows, bytes, pending); if (rc != Result::ok) return rc;
        pending->x_ = x; pending->y_ = y; pending->width_ = width;
        // A sparse zero-filled output would make unwritten cells look valid. Poison every
        // float before handoff so untouched cells fail the finite scan after completion.
        // This does not prove encoder provenance: a dishonest writer can overwrite with zeros
        // (including hole punching). Shape, completion and immutability are distinct from correctness.
        std::array<uint32_t, 4096> poison; poison.fill(0x7fc00000u);
        for (uint64_t offset = 0; offset < bytes;) {
            rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
            size_t count = size_t(std::min<uint64_t>(sizeof(poison), bytes - offset));
            size_t done = 0;
            while (done < count) {
                rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
                ssize_t n = ::pwrite(pending->fd_, reinterpret_cast<const char *>(poison.data()) + done, count - done, off_t(offset + done));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return Result::system;
                done += size_t(n);
            }
            offset += count;
        }
        rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
        out = std::move(pending); return Result::ok;
    }
    // Supervisor-only mutation; do not publish/share for consumption until this succeeds.
    Result finish_rows(const Tag & expected, uint32_t x, uint32_t y, uint32_t width,
                       Clock::time_point deadline, const std::atomic<bool> & cancel) {
        if (kind_ != Kind::rows || !(tag_ == expected) || x != x_ || y != y_ || width != width_) return Result::invalid;
        auto rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
        // F_SEAL_WRITE fails while ANY writable mapping survives, including in another process.
        if (sealed_size(fd_, bytes_, bytes_) != Result::ok &&
            ::fcntl(fd_, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_SEAL) != 0) return Result::unsealed;
        rc = sealed_size(fd_, bytes_, bytes_); if (rc != Result::ok) return rc;
        if (validated_) return Result::ok;
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "requires binary32 rows");
        std::array<uint32_t, 4096> words{}; // fixed scan storage, no row-sized copy or mmap
        for (uint64_t offset = 0; offset < bytes_;) {
            rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
            size_t wanted = size_t(std::min<uint64_t>(sizeof(words), bytes_ - offset)), got = 0;
            while (got < wanted) {
                rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
                ssize_t n = ::pread(fd_, reinterpret_cast<char *>(words.data()) + got, wanted - got, off_t(offset + got));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) return Result::system;
                got += size_t(n);
            }
            // Bit test remains valid even in a build with floating-point fast-math enabled.
            for (size_t i = 0; i < wanted / 4; ++i)
                if ((words[i] & 0x7f800000u) == 0x7f800000u) return Result::nonfinite;
            offset += wanted;
        }
        rc = checkpoint(deadline, cancel); if (rc != Result::ok) return rc;
        validated_ = true; return Result::ok;
    }
private:
    std::shared_ptr<Budget::State> budget_;
    const Tag tag_;
    const Kind kind_;
    const uint64_t bytes_;
    int fd_ = -1;
    uint32_t x_ = 0, y_ = 0, width_ = 0;
    bool charged_ = false, validated_ = false;
    Artifact(Budget & budget, Tag tag, Kind kind, uint64_t bytes)
        : budget_(budget.state_), tag_(tag), kind_(kind), bytes_(bytes) {}
    static Result allocate(Budget & budget, Tag tag, Kind kind, uint64_t bytes, Ptr & out) {
        if (bytes > uint64_t(std::numeric_limits<off_t>::max())) return Result::invalid;
        Ptr pending;
        try { pending.reset(new Artifact(budget, tag, kind, bytes)); }
        catch (const std::bad_alloc &) { return Result::budget; }
        {
            std::lock_guard<std::mutex> lock(pending->budget_->mutex);
            auto & state = *pending->budget_;
            if (state.used.artifacts >= state.cap.artifacts || bytes > state.cap.bytes - state.used.bytes) return Result::budget;
            state.used.bytes += bytes; ++state.used.artifacts; pending->charged_ = true;
        }
        pending->fd_ = ::memfd_create("strata-vision", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (pending->fd_ < 0 || ::ftruncate(pending->fd_, off_t(bytes)) != 0 ||
            ::fcntl(pending->fd_, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK) != 0) return Result::system;
        out = std::move(pending); return Result::ok;
    }
};
} // namespace strata_frontend::vision_artifact
