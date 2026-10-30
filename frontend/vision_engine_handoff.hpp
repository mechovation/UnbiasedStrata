// PR4 single-owner engine handshake, integrated with Supervisor; not wired into serving.
#pragma once
#include "vision_control.hpp"
#include <algorithm>
#include <exception>
#include <stdexcept>

namespace strata_frontend::vision_engine_handoff {
using Clock = vision_control::Clock;
struct Identity {
    uint64_t task, instance, job, sequence;
    bool operator==(const Identity & b) const noexcept {
        return task == b.task && instance == b.instance && job == b.job && sequence == b.sequence;
    }
};
// Derived parent-owned envelope retains prompt, immutable rows AND engine-visible
// files. No borrowed slot/HTTP pointers. Its destruction may remove those files.
struct Resources { virtual ~Resources() = default; };
enum class Start { not_sent, sent, uncertain }; // partial writes are uncertain, NEVER not_sent
// A terminal means the full protocol boundary was drained, not merely STOP sent.
enum class Observation { pending, succeeded, stopped, failed, broken };
struct Engine {
    virtual ~Engine() = default;
    // All callbacks are single-owner, bounded, noexcept, non-reentrant. They must
    // honor absolute deadlines and never retain borrowed references past return.
    // Optional resident startup hook. The supervisor must hold the same exclusive
    // engine turn (Authority::to_engine/to_reader) around this call as around GEN;
    // READY alone is not handback proof. Stateless test engines need no startup.
    virtual bool initialize(Clock::time_point, const std::atomic<bool> &) noexcept { return true; }
    virtual Start start(Identity, const Resources &, Clock::time_point,
                        const std::atomic<bool> &) noexcept = 0;
    virtual bool stop(Identity, Clock::time_point) noexcept = 0;
    // Nonblocking observation: deadline is a ceiling, NOT permission to wait
    // until total request expiry without observing cancellation.
    virtual Observation poll(Identity, Clock::time_point) noexcept = 0;
    // True requires confirmed termination/drain, not a kill request or pipe EOF.
    virtual bool contain(Identity, Clock::time_point) noexcept = 0;
    // Persistent owner calls this before its spawning thread exits, including if
    // no GEN was sent. True certifies owned process teardown/reaping. Stateless
    // test adapters may return true; native adapters must not leave a child alive.
    virtual bool shutdown(Clock::time_point) noexcept = 0;
    // Owner-only idle health observation; also called before/after bounded reader
    // transport polls. Stateless adapters have nothing to poll. No startup/retry,
    // blocking IO, owner reentry or process ownership transfer is permitted here.
    virtual bool healthy() noexcept { return true; }
};
struct Authority {
    virtual ~Authority() = default;
    // Atomic transfers of an ALREADY owned exclusive turn, never release+acquire.
    // First independently certifies reader quiescence and revokes reader work.
    // Second independently certifies engine safety (including global revocation).
    // False leaves the prior owner unchanged. No child boolean is a certificate.
    virtual bool to_engine(Identity) noexcept = 0;
    virtual bool to_reader(Identity) noexcept = 0;
};
enum class State { ready, active, draining, containing, proof_blocked, complete };
enum class Outcome { success, cancelled, failure };
class Handoff {
public:
    Handoff(Identity id, std::shared_ptr<const Resources> resources,
            std::shared_ptr<vision_control::Request> request, std::shared_ptr<Engine> engine,
            std::shared_ptr<Authority> authority, Clock::time_point start_deadline,
            Clock::duration cleanup)
        : id_(id), resources_(std::move(resources)), request_(std::move(request)),
          engine_(std::move(engine)), authority_(std::move(authority)),
          start_deadline_(start_deadline), cleanup_(cleanup) {
        if (!id.task || !id.instance || !id.job || !id.sequence || !resources_ || !request_ ||
            !engine_ || !authority_ || cleanup <= Clock::duration::zero())
            throw std::invalid_argument("invalid engine handoff");
    }
    Handoff(const Handoff &) = delete;
    Handoff & operator=(const Handoff &) = delete;
    // Owner must contain/certify before destruction. Never silently drop files
    // or the engine turn. Ready destruction is safe: no start was attempted.
    ~Handoff() { if (engine_turn_) std::terminate(); }
    State state() const noexcept { return state_; }
    bool possibly_started() const noexcept { return possibly_started_; }
    bool required_containment() const noexcept { return containment_started_ || proof_timed_out_; }
    // Exactly one start attempt, including denial, cancellation and timeout.
    bool begin(Identity id) noexcept {
        if (!(id == id_) || state_ != State::ready) return false;
        if (request_->observe()) { finish(Outcome::cancelled); return true; }
        if (Clock::now() >= start_deadline_) { finish(Outcome::failure); return true; }
        if (!authority_->to_engine(id_)) { finish(Outcome::failure); return true; }
        engine_turn_ = true;
        // Cancellation can race the authority callback. Do not send GEN in that case.
        if (request_->observe() || Clock::now() >= start_deadline_) {
            outcome_ = request_->observe() ? Outcome::cancelled : Outcome::failure;
            state_ = State::proof_blocked; certify(); return true;
        }
        state_ = State::active; // may-have-started boundary precedes native IO
        possibly_started_ = true;
        const auto result = engine_->start(id_, *resources_, std::min(start_deadline_, request_->deadline()), request_->flag());
        if (result == Start::not_sent) {
            possibly_started_ = false;
            outcome_ = request_->observe() ? Outcome::cancelled : Outcome::failure;
            state_ = State::proof_blocked; certify();
        } else if (result == Start::uncertain) {
            outcome_ = Outcome::failure; begin_cleanup(); state_ = State::containing;
        }
        return true;
    }
    // Parent stop string/output limit, distinct from sticky request cancellation.
    bool request_stop(Identity id) noexcept {
        if (!(id == id_) || (state_ != State::active && state_ != State::draining)) return false;
        normal_stop_ = true; return true;
    }
    // Owner polls even without queue traffic. Cleanup has an independent deadline;
    // sticky request cancellation is never passed to STOP/drain/contain callbacks.
    bool advance(Identity id) noexcept {
        if (!(id == id_) || state_ == State::ready || state_ == State::complete) return false;
        if (request_->observe()) outcome_ = Outcome::cancelled;
        if (state_ == State::proof_blocked) { certify(); return true; }
        if (state_ == State::active && (outcome_ == Outcome::cancelled || normal_stop_)) {
            begin_cleanup(); state_ = State::draining;
            if (!engine_->stop(id_, cleanup_deadline_)) state_ = State::containing;
        }
        if (state_ == State::active || state_ == State::draining) {
            if (state_ == State::draining && Clock::now() >= cleanup_deadline_) state_ = State::containing;
            else {
                const auto observed = engine_->poll(id_, state_ == State::active ? request_->deadline() : cleanup_deadline_);
                if (observed == Observation::succeeded || observed == Observation::stopped || observed == Observation::failed) {
                    if ((observed == Observation::failed || (observed == Observation::stopped && !normal_stop_)) &&
                        outcome_ != Outcome::cancelled) outcome_ = Outcome::failure;
                    state_ = State::proof_blocked;
                } else if (observed == Observation::broken) {
                    if (outcome_ != Outcome::cancelled) outcome_ = Outcome::failure;
                    begin_cleanup(); state_ = State::containing;
                }
            }
        }
        if (state_ == State::containing) {
            // Escalation gets its own once-only budget; an expired drain deadline
            // must not leave zero time to terminate/reap. Retries never renew it.
            if (!containment_started_) {
                containment_started_ = true;
                containment_deadline_ = deadline_after(cleanup_);
            }
            if (engine_->contain(id_, containment_deadline_)) state_ = State::proof_blocked;
        }
        if (state_ == State::proof_blocked) certify();
        return true;
    }
    std::optional<Outcome> take_result(Identity id) noexcept {
        if (!(id == id_) || state_ != State::complete || reported_) return {};
        reported_ = true; return outcome_;
    }
private:
    const Identity id_;
    std::shared_ptr<const Resources> resources_;
    std::shared_ptr<vision_control::Request> request_;
    std::shared_ptr<Engine> engine_;
    std::shared_ptr<Authority> authority_;
    const Clock::time_point start_deadline_;
    const Clock::duration cleanup_;
    Clock::time_point cleanup_deadline_{}, containment_deadline_{}, proof_deadline_{};
    State state_ = State::ready;
    Outcome outcome_ = Outcome::success;
    bool engine_turn_ = false, cleanup_started_ = false, containment_started_ = false, reported_ = false;
    bool possibly_started_ = false, proof_started_ = false, proof_timed_out_ = false, normal_stop_ = false;
    static Clock::time_point deadline_after(Clock::duration budget) noexcept {
        const auto now = Clock::now();
        return budget > Clock::time_point::max() - now ? Clock::time_point::max() : now + budget;
    }
    void begin_cleanup() noexcept {
        if (cleanup_started_) return;
        cleanup_started_ = true;
        cleanup_deadline_ = deadline_after(cleanup_);
    }
    void certify() noexcept {
        if (!proof_started_) { proof_started_ = true; proof_deadline_ = deadline_after(cleanup_); }
        if (!authority_->to_reader(id_)) {
            // A silent authority must not pin the serving turn indefinitely. One
            // failed proof budget escalates to containment, never releases files.
            // After containment the same proof is still mandatory; the supervisor
            // applies bounded whole-service shutdown/fail-stop if denial persists.
            if (!proof_timed_out_ && Clock::now() >= proof_deadline_) {
                proof_timed_out_ = true; state_ = State::containing;
                if (outcome_ != Outcome::cancelled) outcome_ = Outcome::failure;
            }
            return;
        }
        engine_turn_ = false;
        if (request_->observe()) outcome_ = Outcome::cancelled;
        finish(outcome_);
    }
    void finish(Outcome outcome) noexcept {
        outcome_ = outcome; state_ = State::complete; resources_.reset();
    }
};
} // namespace strata_frontend::vision_engine_handoff
