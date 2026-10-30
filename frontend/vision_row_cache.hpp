// PR4 LATE row cache: service-owned immutable artifacts, never recipes/handles.
// Logical artifact bytes/FDs stay charged to the service Budget through the last
// held reference, including after LRU eviction. No PREPARE bypass or singleflight.
#pragma once
#include "vision_artifact.hpp"
#include <list>

namespace strata_frontend::vision_row_cache {
class Cache {
public:
    struct Key {
        vision_payload::Config config;
        vision_payload::Digest source;
        vision_metadata::Shape shape;
        uint64_t bytes;
        bool operator==(const Key & b) const {
            return config.generation == b.config.generation && config.device == b.config.device &&
                config.width == b.config.width && config.pixel_stride == b.config.pixel_stride &&
                config.vocabulary_size == b.config.vocabulary_size && source == b.source && bytes == b.bytes &&
                shape.x == b.shape.x && shape.y == b.shape.y && shape.cells == b.shape.cells && shape.positions == b.shape.positions;
        }
    };
    struct Entry {
        const Key key;
        const vision_artifact::Artifact::Ptr rows;
    };
    using Held = std::shared_ptr<const Entry>;
    struct Stats { uint64_t hits = 0, misses = 0, evictions = 0; };
    Cache(size_t entries, uint64_t bytes, std::shared_ptr<vision_artifact::Budget> budget)
        : entries_(entries), bytes_(bytes), budget_(std::move(budget)) {}
    bool uses(const std::shared_ptr<vision_artifact::Budget> & budget) const { return budget_ == budget; }
    Held find(const Key & key) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = lru_.begin(); it != lru_.end(); ++it) if ((*it)->key == key) {
            auto held = *it; lru_.splice(lru_.end(), lru_, it); ++stats_.hits; return held;
        }
        ++stats_.misses; return {};
    }
    // Best effort only: oversize/allocation failure does not fail a real ENCODE.
    void put(const Key & key, vision_artifact::Artifact::Ptr rows) {
        if (!entries_ || key.bytes > bytes_ || !rows || !rows->validated_rows() ||
            rows->tag().source != key.source || rows->bytes() != key.bytes) return;
        std::atomic<bool> never{false};
        if (rows->finish_rows(rows->tag(), key.shape.x, key.shape.y, key.config.width,
            vision_artifact::Clock::time_point::max(), never) != vision_artifact::Result::ok) return;
        try {
            auto entry = std::make_shared<const Entry>(Entry{key, std::move(rows)});
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = lru_.begin(); it != lru_.end(); ++it) if ((*it)->key == key) {
                resident_ -= (*it)->key.bytes; lru_.erase(it); break;
            }
            // Subtraction-based admission also handles a UINT64_MAX byte cap.
            while (!lru_.empty() && (lru_.size() >= entries_ || key.bytes > bytes_ - resident_)) evict_locked();
            lru_.push_back(std::move(entry)); resident_ += key.bytes;
        } catch (const std::bad_alloc &) {} // cache is optional
    }
    // Reclaim only evictable ownership on artifact admission pressure. A held
    // entry still consumes the SAME hard Budget; no early ledger refund.
    bool evict_one() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lru_.empty()) return false;
        evict_locked(); return true;
    }
    Stats stats() const { std::lock_guard<std::mutex> lock(mutex_); return stats_; }
    size_t size() const { std::lock_guard<std::mutex> lock(mutex_); return lru_.size(); }
private:
    const size_t entries_;
    const uint64_t bytes_;
    const std::shared_ptr<vision_artifact::Budget> budget_;
    mutable std::mutex mutex_;
    std::list<Held> lru_;
    uint64_t resident_ = 0;
    Stats stats_;
    void evict_locked() { resident_ -= lru_.front()->key.bytes; lru_.pop_front(); ++stats_.evictions; }
};
// Copy into NEW parent-tagged storage, never expose another job's artifact as
// this job's output. The Job then seals/scans/binds the copy before publishing.
inline vision_artifact::Result copy(const Cache::Held & held, const Cache::Key & expected,
    vision_artifact::Artifact & output, vision_artifact::Clock::time_point until, const std::atomic<bool> & cancel) {
    using namespace vision_artifact;
    if (!held || !(held->key == expected) || !held->rows || !held->rows->validated_rows() ||
        held->rows->tag().source != expected.source || output.tag().source != expected.source ||
        output.bytes() != expected.bytes || !output.writable_shape(expected.shape.x, expected.shape.y, expected.config.width) ||
        sealed_size(held->rows->fd(), expected.bytes, expected.bytes) != Result::ok) return Result::invalid;
    char buffer[16384];
    for (uint64_t offset = 0; offset < expected.bytes;) {
        auto rc = checkpoint(until, cancel); if (rc != Result::ok) return rc;
        auto n = ::pread(held->rows->fd(), buffer, size_t(std::min<uint64_t>(sizeof(buffer), expected.bytes - offset)), off_t(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return Result::system;
        for (size_t done = 0; done < size_t(n);) {
            rc = checkpoint(until, cancel); if (rc != Result::ok) return rc;
            auto wrote = ::pwrite(output.fd(), buffer + done, size_t(n) - done, off_t(offset + done));
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) return Result::system;
            done += size_t(wrote);
        }
        offset += uint64_t(n);
    }
    return checkpoint(until, cancel);
}
} // namespace strata_frontend::vision_row_cache
