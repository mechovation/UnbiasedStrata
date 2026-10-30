// PR5 complete, configuration-scoped recipe/row cache. No worker handles or PIDs.
#pragma once
#include "vision_job.hpp"
#include "vision_row_cache.hpp"
#include "vision_recipe_budget.hpp"
#include "vision_control.hpp"
#include <map>

namespace strata_frontend::vision_cache {
struct Key {
    vision_payload::Config config;
    vision_payload::Digest source;
    bool operator==(const Key & b) const {
        return config.generation == b.config.generation && config.device == b.config.device &&
            config.width == b.config.width && config.pixel_stride == b.config.pixel_stride &&
            config.vocabulary_size == b.config.vocabulary_size && source == b.source;
    }
};
// Hash these canonical bytes with SHA-256 at the SVE1 writer boundary. This
// namespace also versions engine-visible keys, not just the parent LRU lookup.
inline std::string identity_bytes(const Key & key) {
    std::string bytes = "unbiased-strata/image/pr5-v1";
    bytes.append(reinterpret_cast<const char *>(key.config.generation.data()),key.config.generation.size());
    bytes.append(reinterpret_cast<const char *>(key.config.device.data()),key.config.device.size());
    for (auto n : {key.config.width,key.config.pixel_stride,key.config.vocabulary_size})
        for (unsigned b = 0; b < 4; ++b) bytes += char(n >> (8*b));
    bytes.append(reinterpret_cast<const char *>(key.source.data()),key.source.size());
    return bytes;
}
inline uint64_t recipe_bytes(const vision_job::Recipe & r) {
    return uint64_t(r.metadata.size()) + r.prefix_ids.size() + r.suffix_ids.size();
}
class Cache {
public:
    struct Entry {
        const Key key;
        const vision_job::Recipe recipe;
        const std::shared_ptr<const vision_job_io::RecipeBudget::Charge> charge;
        const vision_row_cache::Cache::Held rows;
    };
    using Held = std::shared_ptr<const Entry>;
    using Stats = vision_row_cache::Cache::Stats;
    Cache(size_t entries, uint64_t bytes, std::shared_ptr<vision_artifact::Budget> artifacts,
          std::shared_ptr<vision_job_io::RecipeBudget> recipes)
        : entries_(entries), bytes_(bytes), artifacts_(std::move(artifacts)), recipes_(std::move(recipes)) {}
    bool uses(const std::shared_ptr<vision_artifact::Budget> & a,
              const std::shared_ptr<vision_job_io::RecipeBudget> & r) const { return a == artifacts_ && r == recipes_; }
    Held find(const Key & key) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = lru_.begin(); it != lru_.end(); ++it) if ((*it)->key == key) {
            auto result = *it; lru_.splice(lru_.end(), lru_, it); ++stats_.hits; return result;
        }
        ++stats_.misses; return {};
    }
    // Only a validated ENCODE (or an already complete held entry) is publishable.
    // Best effort: cache pressure cannot make a successful encode fail. One newly
    // retained oversize entry is allowed by the SOFT cap, never by the hard ledgers.
    Held put(const Key & key, const vision_job::Recipe & recipe, vision_artifact::Artifact::Ptr rows) {
        if (!entries_ || !rows || !rows->validated_rows() || rows->tag().source != key.source || rows->bytes() != recipe.row_bytes) return {};
        std::atomic<bool> never{false};
        if (rows->finish_rows(rows->tag(), recipe.shape.x, recipe.shape.y, key.config.width,
            vision_artifact::Clock::time_point::max(), never) != vision_artifact::Result::ok) return {};
        const auto bytes = recipe_bytes(recipe);
        if (bytes > UINT64_MAX - rows->bytes()) return {};
        try {
            auto charge = recipes_->reserve(bytes);
            while (!charge && evict_one()) charge = recipes_->reserve(bytes);
            if (!charge) return {};
            auto row = std::make_shared<const vision_row_cache::Cache::Entry>(vision_row_cache::Cache::Entry{
                {key.config,key.source,recipe.shape,recipe.row_bytes},std::move(rows)});
            auto entry = std::make_shared<const Entry>(Entry{key,recipe,std::move(charge),std::move(row)});
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = lru_.begin(); it != lru_.end(); ++it) if ((*it)->key == key) {
                resident_ -= size(**it); lru_.erase(it); break;
            }
            const auto n = size(*entry);
            while (!lru_.empty() && (lru_.size() >= entries_ || n > bytes_ || n > bytes_ - std::min(bytes_,resident_))) evict_locked();
            lru_.push_back(entry); resident_ += n; return entry;
        } catch (const std::bad_alloc &) { return {}; }
    }
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
    const std::shared_ptr<vision_artifact::Budget> artifacts_;
    const std::shared_ptr<vision_job_io::RecipeBudget> recipes_;
    mutable std::mutex mutex_;
    std::list<Held> lru_;
    uint64_t resident_ = 0;
    Stats stats_;
    static uint64_t size(const Entry & e) { return e.recipe.row_bytes + recipe_bytes(e.recipe); }
    void evict_locked() { resident_ -= size(*lru_.front()); lru_.pop_front(); ++stats_.evictions; }
};

// The serving queue already serializes producers. Register interests BEFORE queue
// admission so an HTTP disconnect cannot kill reader work needed by a deferred
// waiter. Complete results are pinned through all waiter tickets, even after LRU
// eviction. There is no second worker/GPU turn, retry loop or unbounded future map.
class Flights {
public:
    struct Flight {
        const Key key;
        std::mutex mutex;
        std::map<uint64_t,std::weak_ptr<vision_control::Request>> waiters;
        Cache::Held complete;
        explicit Flight(Key k) : key(k) {}
        bool live() {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto & w : waiters) if (auto c = w.second.lock(); c && !c->observe()) return true;
            return false;
        }
        Cache::Held result() { std::lock_guard<std::mutex> lock(mutex); return complete; }
        void publish(Cache::Held entry) {
            if (!entry || !(entry->key == key)) return;
            std::lock_guard<std::mutex> lock(mutex); complete = std::move(entry);
        }
    };
private:
    struct State {
        std::mutex mutex;
        const size_t flights, waiters;
        size_t used = 0;
        uint64_t next = 0;
        std::list<std::weak_ptr<Flight>> registry;
        State(size_t f, size_t w) : flights(f), waiters(w) {}
    };
    std::shared_ptr<State> state_;
public:
    struct Ticket {
        std::shared_ptr<Flight> flight;
        std::shared_ptr<State> state;
        uint64_t id = 0;
        ~Ticket() {
            if (!id) return;
            std::lock_guard<std::mutex> lock(state->mutex);
            { std::lock_guard<std::mutex> f(flight->mutex); flight->waiters.erase(id); }
            --state->used;
        }
    };
    using Held = std::shared_ptr<Ticket>;
    Flights(size_t flights, size_t waiters) : state_(std::make_shared<State>(flights,waiters)) {}
    Held join(const Key & key, const std::shared_ptr<vision_control::Request> & control) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!control || state_->used == state_->waiters || state_->next == UINT64_MAX) return {};
        std::shared_ptr<Flight> flight;
        for (auto it = state_->registry.begin(); it != state_->registry.end();) {
            auto f = it->lock();
            if (!f) it = state_->registry.erase(it);
            else { if (f->key == key) flight = f; ++it; }
        }
        if (!flight) {
            if (state_->registry.size() == state_->flights) return {};
            flight = std::make_shared<Flight>(key); state_->registry.push_back(flight);
        }
        auto ticket = std::make_shared<Ticket>(); ticket->flight = flight; ticket->state = state_;
        const auto id = ++state_->next;
        { std::lock_guard<std::mutex> f(flight->mutex); flight->waiters.emplace(id,control); }
        ++state_->used; ticket->id = id; return ticket;
    }
};
// Only reader work uses this control. Engine generation ALWAYS uses the original
// request control. Phase deadlines remain absolute and are never extended by join.
class Interest final : public vision_control::Request {
    const std::vector<Flights::Held> tickets_;
public:
    explicit Interest(std::vector<Flights::Held> tickets)
        : Request(vision_control::Clock::time_point::max()), tickets_(std::move(tickets)) {}
    bool observe(vision_control::Clock::time_point now = vision_control::Clock::now()) noexcept override {
        bool live = false;
        for (const auto & t : tickets_) live |= t && t->flight->live();
        if (!live) cancel(vision_control::Reason::cancelled);
        return Request::observe(now);
    }
};
} // namespace strata_frontend::vision_cache
