// Aggregate immutable recipe accounting, including evicted but held cache entries.
#pragma once
#include <memory>
#include <mutex>
#include <cstdint>

namespace strata_frontend::vision_job_io {
class RecipeBudget {
public:
    struct Usage { uint64_t bytes; size_t snapshots; };
private:
    struct State {
        std::mutex mutex;
        Usage cap, used{0, 0};
        explicit State(Usage limits) : cap(limits) {}
    };
    std::shared_ptr<State> state_;
public:
    class Charge {
        friend class RecipeBudget;
        std::shared_ptr<State> state_;
        uint64_t bytes_;
        bool charged_ = false;
        Charge(std::shared_ptr<State> state, uint64_t bytes) : state_(std::move(state)), bytes_(bytes) {}
    public:
        Charge(const Charge &) = delete;
        Charge & operator=(const Charge &) = delete;
        ~Charge() {
            if (!charged_) return;
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->used.bytes -= bytes_; --state_->used.snapshots;
        }
    };
    explicit RecipeBudget(Usage cap) : state_(std::make_shared<State>(cap)) {}
    Usage usage() const { std::lock_guard<std::mutex> lock(state_->mutex); return state_->used; }
    std::shared_ptr<const Charge> reserve(uint64_t bytes) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (bytes > state_->cap.bytes - state_->used.bytes || state_->used.snapshots == state_->cap.snapshots) return {};
        try {
            auto charge = std::shared_ptr<Charge>(new Charge(state_, bytes));
            state_->used.bytes += bytes; ++state_->used.snapshots; charge->charged_ = true;
            return charge;
        } catch (const std::bad_alloc &) { return {}; }
    }
};
} // namespace strata_frontend::vision_job_io
