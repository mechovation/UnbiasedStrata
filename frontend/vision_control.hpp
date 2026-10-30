// CPU-only PR4 request/inbox foundations; not connected to the serving queue.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace strata_frontend::vision_control {
using Clock = std::chrono::steady_clock;
enum class Reason { none, disconnect, deadline, cancelled };
// Shared by owned request envelopes and HTTP observers, never borrows HTTP state.
// Cleanup must use a separate service-shutdown flag, not this sticky request flag.
class Request {
public:
    explicit Request(Clock::time_point deadline) : deadline_(deadline) {}
    void cancel(Reason reason) noexcept {
        if (reason == Reason::none) return;
        Reason expected = Reason::none;
        reason_.compare_exchange_strong(expected, reason);
        stopped_.store(true);
    }
    bool observe(Clock::time_point now = Clock::now()) noexcept {
        if (now >= deadline_) cancel(Reason::deadline);
        return stopped_.load();
    }
    Reason reason() const noexcept { return reason_.load(); }
    const std::atomic<bool> & flag() const noexcept { return stopped_; }
    Clock::time_point deadline() const noexcept { return deadline_; }
private:
    const Clock::time_point deadline_;
    std::atomic<Reason> reason_{Reason::none};
    std::atomic<bool> stopped_{false};
};

// Small notifications only. Owned recipes/artifacts stay in an independently
// bounded sidecar until the queue consumes the matching identity.
struct Event {
    uint64_t task = 0, instance = 0, job = 0, sequence = 0;
    enum class Kind { ready, completed, failed, unavailable, stopped, prepared, encoded, engine_submitted, job_terminal } kind = Kind::failed;
};
// Wake sink is owned by the inbox state. post() MUST be nonblocking, noexcept,
// non-reentrant and deliver a queue wake reliably (or fail the service closed).
// It must not synchronously drain this inbox or mutate a slot. The queue must
// outlive the sink, including reservations that outlive the Inbox facade.
struct Wake {
    virtual ~Wake() = default;
    virtual void post() noexcept = 0;
};
class Inbox {
    struct State {
        State(size_t capacity, std::shared_ptr<Wake> sink) : ring(capacity), wake(std::move(sink)) {}
        std::mutex mutex;
        std::vector<Event> ring;
        std::shared_ptr<Wake> wake;
        size_t head = 0, count = 0, reserved = 0;
        bool accepting = true, wake_pending = false;
    };
public:
    class Reservation {
    public:
        Reservation() = default;
        Reservation(const Reservation &) = delete;
        Reservation & operator=(const Reservation &) = delete;
        Reservation(Reservation && other) noexcept : state_(std::move(other.state_)) {}
        Reservation & operator=(Reservation && other) noexcept {
            if (this != &other) { reset(); state_ = std::move(other.state_); }
            return *this;
        }
        ~Reservation() { reset(); }
        explicit operator bool() const noexcept { return bool(state_); }
        // Exactly one completion per reservation; no allocation on this path.
        bool publish(Event event) {
            if (!state_) return false;
            auto state = std::move(state_);
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->reserved;
            state->ring[(state->head + state->count) % state->ring.size()] = event;
            ++state->count;
            if (!state->wake_pending) {
                state->wake_pending = true;
                state->wake->post();
            }
            return true;
        }
        void reset() {
            if (!state_) return;
            auto state = std::move(state_);
            std::lock_guard<std::mutex> lock(state->mutex);
            --state->reserved;
        }
    private:
        friend class Inbox;
        explicit Reservation(std::shared_ptr<State> state) : state_(std::move(state)) {}
        std::shared_ptr<State> state_;
    };
    Inbox(size_t capacity, std::shared_ptr<Wake> wake) {
        if (!capacity || !wake) throw std::invalid_argument("inbox requires capacity and wake sink");
        state_ = std::make_shared<State>(capacity, std::move(wake));
    }
    Inbox(const Inbox &) = delete;
    Inbox & operator=(const Inbox &) = delete;
    Reservation reserve() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->accepting || state_->reserved == state_->ring.size() - state_->count) return {};
        ++state_->reserved;
        return Reservation(state_);
    }
    // Single queue consumer. Call until empty BEFORE the idle-slot early return.
    // Clear wake_pending only when observing empty under the producer mutex:
    // a racing producer is either consumed or posts a fresh wake.
    std::optional<Event> pop() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->count) { state_->wake_pending = false; return {}; }
        Event result = state_->ring[state_->head];
        state_->head = (state_->head + 1) % state_->ring.size();
        --state_->count;
        return result;
    }
    // Stop admission first; outstanding operations retain guaranteed event space.
    // Keep draining until joined producers have returned/published reservations.
    void stop_accepting() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
    }
    bool settled() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return !state_->accepting && !state_->reserved && !state_->count;
    }
private:
    std::shared_ptr<State> state_;
};
} // namespace strata_frontend::vision_control
