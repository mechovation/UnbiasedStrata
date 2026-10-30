// PR4 single-owner Job/IPC adapter. Engine handoff is driven by the supervisor.
#pragma once
#include "vision_commands.hpp"
#include "vision_control.hpp"
#include "vision_job.hpp"
#include "vision_row_cache.hpp"

namespace strata_frontend::vision_job_io {
using Clock = vision_protocol::Clock;
// Aggregate copied-recipe accounting. Every held snapshot retains its charge;
// acknowledgement/eviction cannot free it early. Logical bytes exclude allocator
// capacity/overhead; a separate snapshot-count cap bounds the number of objects.
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
        // Text snapshots have zero recipe bytes, but still consume an object slot.
        if (bytes > state_->cap.bytes - state_->used.bytes || state_->used.snapshots == state_->cap.snapshots) return {};
        try {
            auto charge = std::shared_ptr<Charge>(new Charge(state_, bytes));
            state_->used.bytes += bytes; ++state_->used.snapshots; charge->charged_ = true;
            return charge;
        } catch (const std::bad_alloc &) { return {}; }
    }
};
struct Submission {
    uint64_t job;
    std::shared_ptr<vision_control::Request> control;
    std::shared_ptr<vision_artifact::Budget> artifacts;
    std::shared_ptr<RecipeBudget> recipes; // service-owned shared ledger, not a fresh per-request cap
    std::vector<vision_job::Input> inputs;
    vision_job::Limits limits;
    uint64_t snapshot_bytes; // additional copied recipe bytes; excludes vector/allocator overhead
    Clock::duration prepare, encode, cleanup; // whole-phase absolute budgets, not renewed per occurrence
    bool text_only = false; // explicit no-reader-IPC path under the SAME engine authority
    std::shared_ptr<vision_row_cache::Cache> cache; // optional service-owned LATE cache
};
struct Identity { uint64_t task, instance, job; };
struct Prepared {
    Prepared() = default;
    Prepared(const Prepared &) = delete;
    Prepared & operator=(const Prepared &) = delete;
    Identity identity;
    std::shared_ptr<const RecipeBudget::Charge> charge;
    std::vector<vision_job::Recipe> recipes; // owned copies, bounded separately from Job's recipe storage
};
struct Encoded {
    Encoded() = default;
    Encoded(const Encoded &) = delete;
    Encoded & operator=(const Encoded &) = delete;
    std::shared_ptr<const Prepared> prepared;
    std::vector<vision_artifact::Artifact::Ptr> rows; // sealed shared backing, charges follow final reference
};
struct Terminal { Identity identity; vision_job::Outcome outcome; uint32_t error_class = 0; bool timed_out = false; };
inline bool duration_valid(Clock::duration d) {
    return d > Clock::duration::zero() && d <= Clock::time_point::max() - Clock::now();
}
inline Clock::time_point after(Clock::duration d) {
    const auto now = Clock::now();
    return d > Clock::time_point::max() - now ? Clock::time_point::max() : now + d;
}
inline bool valid(const Submission & s) {
    return s.job && s.control && s.artifacts && s.recipes && (s.inputs.empty() == s.text_only) && s.snapshot_bytes &&
        duration_valid(s.prepare) && duration_valid(s.encode) && duration_valid(s.cleanup) &&
        (!s.cache || s.cache->uses(s.artifacts));
}
class Session {
public:
    static std::unique_ptr<Session> create(Identity id, const vision_payload::Config & config,
                                         const vision_payload::Limits & caps, std::unique_ptr<Submission> submission) {
        if (!submission || !valid(*submission) || submission->job != id.job) return {};
        auto job = vision_job::Job::create(config, caps, submission->limits, id.instance, id.job, std::move(submission->inputs), submission->text_only);
        if (!job) return {};
        return std::unique_ptr<Session>(new Session(id, config, caps, std::move(submission), std::move(job)));
    }
    std::shared_ptr<vision_control::Request> control() const { return submission_->control; }
    uint32_t error_class() const { return job_->error_class(); }
    bool timed_out() const { return timed_out_; }
    bool cancelled() { return submission_->control->observe(); }
    bool engine_begin() { return job_->engine_start_requested(); }
    // Invoked ONLY after the handoff returns a certified reader turn. A start
    // refusal is not a fabricated engine terminal; it has its own rollback.
    void engine_finish(bool possibly_started, bool succeeded, bool cancelled_outcome) {
        if (cancelled_outcome || cancelled()) job_->cancel();
        if (possibly_started) {
            if (!job_->engine_started()) std::terminate();
            if (cancelled_outcome || cancelled()) job_->cancel();
            if (!job_->engine_terminal(succeeded)) std::terminate();
        } else if (!job_->engine_not_started()) std::terminate();
    }
    bool release(vision_protocol::Channel & channel, const std::atomic<bool> & shutdown) {
        const auto until = after(submission_->cleanup);
        // Sticky request cancellation never aborts cooperative RELEASE.
        while (auto ticket = job_->release())
            if (!exchange(channel, *ticket, -1, until, shutdown)) return failure(until);
        return job_->phase() == vision_job::Phase::done;
    }
    void fail() {
        if (cancelled()) job_->cancel();
        if (job_->phase() != vision_job::Phase::done) job_->reader_failure();
    }
    // Trusted supervisor certification only, after reader reap AND applicable safety proof.
    void reader_stopped() { fail(); if (!job_->reader_stopped(true)) std::terminate(); }
    std::optional<vision_job::Outcome> terminal() { return job_->take_terminal(); }
    std::shared_ptr<const Prepared> prepared() const { return prepared_; }
    std::shared_ptr<const Encoded> encoded() const { return encoded_; }
    bool prepare(vision_protocol::Channel & channel, const std::atomic<bool> & shutdown) {
        const auto until = std::min(after(submission_->prepare), submission_->control->deadline());
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            if (cancelled() || shutdown.load()) return failure(until);
            auto ticket = job_->prepare();
            if (!ticket || !exchange(channel, *ticket, job_->input(i)->source->fd(), until, shutdown)) return failure(until);
        }
        uint64_t used = 0;
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            const auto * recipe = job_->recipe(i);
            for (size_t bytes : {recipe->prefix_ids.size(), recipe->suffix_ids.size(), recipe->metadata.size()}) {
                if (bytes > submission_->snapshot_bytes - used) { job_->resource_failure(); return failure(until); }
                used += bytes;
            }
        }
        auto charge = submission_->recipes->reserve(used);
        if (!charge) { job_->resource_failure(); return failure(until); }
        auto snapshot = std::make_shared<Prepared>(); snapshot->identity = id_;
        snapshot->charge = std::move(charge);
        snapshot->recipes.reserve(job_->occurrences());
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            snapshot->recipes.push_back(*job_->recipe(i));
            if (cancelled() || shutdown.load() || Clock::now() >= until) return failure(until);
        }
        prepared_ = std::move(snapshot); return true;
    }
    bool admit(bool accepted, vision_protocol::Channel & channel, const std::atomic<bool> & shutdown) {
        if (!job_->admit(accepted)) return failure();
        if (!accepted) return release(channel, shutdown);
        const auto until = std::min(after(submission_->encode), submission_->control->deadline());
        auto snapshot = std::make_shared<Encoded>(); snapshot->prepared = prepared_;
        snapshot->rows.reserve(job_->occurrences());
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            if (cancelled() || shutdown.load()) return failure(until);
            const auto * recipe = job_->recipe(i);
            auto tag = job_->input(i)->source->tag(); tag.instance = id_.instance;
            // This point is reached ONLY after real PREPARE of every occurrence
            // and the exact parent-composed prompt's successful context admission.
            const vision_row_cache::Cache::Key key{config_, tag.source, recipe->shape, recipe->row_bytes};
            auto cached = submission_->cache ? submission_->cache->find(key) : vision_row_cache::Cache::Held{};
            vision_artifact::Artifact::Ptr rows;
            vision_artifact::Result allocated;
            do {
                allocated = vision_artifact::Artifact::rows(*submission_->artifacts, tag, recipe->shape.x, recipe->shape.y,
                    config_.width, caps_.row_bytes, until, submission_->control->flag(), rows);
            } while (allocated == vision_artifact::Result::budget && submission_->cache && submission_->cache->evict_one());
            if (allocated != vision_artifact::Result::ok) {
                if (allocated == vision_artifact::Result::budget || (allocated == vision_artifact::Result::system &&
                    (errno == ENOMEM || errno == EMFILE || errno == ENFILE || errno == ENOSPC))) job_->resource_failure();
                return failure(until);
            }
            if (cached) {
                if (vision_row_cache::copy(cached, key, *rows, until, submission_->control->flag()) != vision_artifact::Result::ok ||
                    !job_->cached_rows(rows, until, submission_->control->flag())) return failure(until);
            } else {
                auto ticket = job_->encode(rows);
                if (!ticket || !exchange(channel, *ticket, rows->fd(), until, shutdown)) return failure(until);
                if (submission_->cache) submission_->cache->put(key, job_->rows(i));
            }
            snapshot->rows.push_back(job_->rows(i));
        }
        if (cancelled() || shutdown.load() || Clock::now() >= until) return failure(until);
        encoded_ = std::move(snapshot); return true;
    }
private:
    Identity id_;
    vision_payload::Config config_;
    vision_payload::Limits caps_;
    std::unique_ptr<Submission> submission_;
    std::unique_ptr<vision_job::Job> job_;
    std::shared_ptr<const Prepared> prepared_;
    std::shared_ptr<const Encoded> encoded_;
    bool timed_out_ = false;
    Session(Identity id, vision_payload::Config config, vision_payload::Limits caps,
            std::unique_ptr<Submission> submission, std::unique_ptr<vision_job::Job> job)
        : id_(id), config_(config), caps_(caps), submission_(std::move(submission)), job_(std::move(job)) {}
    bool failure(Clock::time_point until = Clock::time_point::max()) {
        timed_out_ = timed_out_ || Clock::now() >= until; fail(); return false;
    }
    bool exchange(vision_protocol::Channel & channel, const vision_job::Ticket & ticket, int fd,
                  Clock::time_point until, const std::atomic<bool> & shutdown) {
        using Kind = vision_protocol::Kind;
        const auto * input = job_->input(ticket.occurrence);
        const auto * recipe = job_->recipe(ticket.occurrence);
        vision_payload::Binding binding{ticket.occurrence, input->source->tag().source, input->metadata_id,
            input->source->bytes(), ticket.handle, recipe ? recipe->shape.x : 0, recipe ? recipe->shape.y : 0};
        vision_commands::Payload payload;
        if (vision_commands::build(ticket.command, config_, caps_, binding, payload) != vision_commands::Result::ok) return false;
        const bool cleanup = ticket.command == Kind::release;
        const auto & cancel = cleanup ? shutdown : submission_->control->flag();
        const vision_protocol::Header header{ticket.command, uint32_t(payload.bytes.size()), id_.instance, id_.job, ticket.sequence};
        auto rc = channel.send_borrowed(header, payload.bytes.data(), payload.bytes.size(), fd >= 0 ? &fd : nullptr,
                                       payload.fd_count, caps_.payload_bytes, until, cancel);
        vision_protocol::Frame reply;
        if (rc == vision_protocol::IoResult::ok) rc = channel.receive(reply,
            {ticket.command == Kind::prepare ? Kind::prepared : ticket.command == Kind::encode ? Kind::encoded : Kind::released,
             id_.instance, id_.job, ticket.sequence, 0}, caps_.payload_bytes, until, cancel);
        if (rc == vision_protocol::IoResult::deadline) timed_out_ = true;
        submission_->control->observe(); // latch total expiry even when transport returned deadline
        if (rc != vision_protocol::IoResult::ok || shutdown.load()) return false;
        return job_->accept(reply, until, submission_->control->flag()) == vision_job::Reply::accepted;
    }
};
} // namespace strata_frontend::vision_job_io
