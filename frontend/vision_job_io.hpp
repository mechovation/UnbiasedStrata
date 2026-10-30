// Single-owner Job/IPC adapter with PR5 complete recipe/row reuse.
// Engine handoff is driven by the supervisor.
#pragma once
#include "vision_commands.hpp"
#include "vision_control.hpp"
#include "vision_job.hpp"
#include "vision_cache.hpp"

namespace strata_frontend::vision_job_io {
using Clock = vision_protocol::Clock;
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
    bool complete_only = false; // certified readerless turn; cache misses MUST NOT issue IPC
    std::shared_ptr<vision_cache::Cache> cache;
    std::vector<vision_cache::Flights::Held> flights; // bounded, occurrence-aligned
    std::shared_ptr<vision_control::Request> work_control; // reader interests only; engine uses control
};
struct Identity { uint64_t task, instance, job; };
struct Prepared {
    Prepared() = default;
    Prepared(const Prepared &) = delete;
    Prepared & operator=(const Prepared &) = delete;
    Identity identity;
    vision_payload::Config config{}; // engine image-key namespace, independent of metadata IDs
    std::shared_ptr<const RecipeBudget::Charge> charge;
    std::vector<vision_job::Recipe> recipes; // owned copies, bounded separately from Job's recipe storage
    std::vector<uint32_t> source_occurrences, omitted; // count-bounded original occurrence map
};
struct Encoded {
    Encoded() = default;
    Encoded(const Encoded &) = delete;
    Encoded & operator=(const Encoded &) = delete;
    std::shared_ptr<const Prepared> prepared;
    std::vector<vision_artifact::Artifact::Ptr> rows; // sealed shared backing, charges follow final reference
};
struct Terminal {
    Identity identity;
    vision_job::Outcome outcome;
    uint32_t error_class = 0;
    bool timed_out = false;
    // Present ONLY after typed rejection + successful cleanup, never after faults.
    std::optional<vision_payload::InputFailure> input_failure = {};
};
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
        (!s.cache || s.cache->uses(s.artifacts,s.recipes)) && (!s.complete_only || (s.cache && !s.text_only)) &&
        (s.flights.empty() ? !s.work_control : (s.cache && s.flights.size() == s.inputs.size() && s.work_control));
}
class Session {
public:
    static std::unique_ptr<Session> create(Identity id, const vision_payload::Config & config,
                                         const vision_payload::Limits & caps, std::unique_ptr<Submission> submission) {
        if (!submission || !valid(*submission) || submission->job != id.job) return {};
        for (size_t i = 0; i < submission->flights.size(); ++i)
            if (!submission->inputs[i].source || !submission->flights[i] || !(submission->flights[i]->flight->key ==
                vision_cache::Key{config,submission->inputs[i].source->tag().source})) return {};
        auto job = vision_job::Job::create(config, caps, submission->limits, id.instance, id.job, std::move(submission->inputs), submission->text_only);
        if (!job) return {};
        return std::unique_ptr<Session>(new Session(id, config, caps, std::move(submission), std::move(job)));
    }
    std::shared_ptr<vision_control::Request> control() const { return submission_->control; }
    uint32_t error_class() const { return job_->error_class(); }
    bool timed_out() const { return timed_out_; }
    std::optional<vision_payload::InputFailure> input_failure() const { return job_->input_failure(); }
    bool cancelled() { return encoded_ ? submission_->control->observe() : reader_control()->observe(); }
    // Called by the supervisor's existing bounded transport health probe.
    void observe_reader() { (void) reader_control()->observe(); }
    bool abandon(vision_protocol::Channel & channel, const std::atomic<bool> & shutdown) {
        return encoded_ && job_->abandon_engine() && release(channel,shutdown);
    }
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
    bool release(vision_protocol::Channel & channel, const std::atomic<bool> & shutdown,
                 Clock::time_point bound = Clock::time_point::max()) {
        const auto until = std::min(after(submission_->cleanup),bound);
        if (shutdown.load() || Clock::now() >= until) return failure(until);
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
    std::optional<vision_job::Outcome> terminal() {
        auto result = job_->take_terminal();
        if (result == vision_job::Outcome::input_error) {
            if (timed_out_) return vision_job::Outcome::failure;
            if (submission_->control->observe() || cancelled()) return vision_job::Outcome::cancelled;
        }
        return result;
    }
    std::shared_ptr<const Prepared> prepared() const { return prepared_; }
    std::shared_ptr<const Encoded> encoded() const { return encoded_; }
    bool prepare(vision_protocol::Channel & channel, const std::atomic<bool> & shutdown) {
        const auto until = std::min(after(submission_->prepare), reader_control()->deadline());
        cached_.resize(job_->occurrences()); duplicates_.assign(job_->occurrences(),SIZE_MAX);
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            if (cancelled() || shutdown.load() || Clock::now() >= until) return failure(until);
            const vision_cache::Key key{config_,job_->input(i)->source->tag().source};
            cached_[i] = submission_->cache ? submission_->cache->find(key) : vision_cache::Cache::Held{};
            if (!cached_[i] && !submission_->flights.empty()) cached_[i] = submission_->flights[i]->flight->result();
            if (cached_[i]) {
                if (!(cached_[i]->key == key) || !job_->reuse_recipe(cached_[i]->recipe)) return failure(until);
                if (!submission_->flights.empty()) submission_->flights[i]->flight->publish(cached_[i]);
                continue;
            }
            if (submission_->complete_only) return failure(until); // no reader/handle fallback on a certified cache-only turn
            const vision_job::Recipe * duplicate = nullptr;
            if (submission_->cache) for (size_t j = 0; j < i; ++j) if (job_->recipe(j) && job_->input(j)->source->tag().source == key.source &&
                job_->input(j)->metadata_id == job_->input(i)->metadata_id) { duplicate = job_->recipe(j); duplicates_[i] = j; break; }
            if (duplicate) {
                if (!job_->reuse_recipe(*duplicate)) return failure(until);
            } else {
                auto ticket = job_->prepare();
                if (!ticket || !exchange(channel, *ticket, job_->input(i)->source->fd(), until, shutdown)) return failure(until);
                if (job_->input_failure()) {
                    // Stop the request, not the service. No encode, snapshot,
                    // retry or negative cache/flight publication on this path.
                    const bool cleaned = release(channel, shutdown, until);
                    timed_out_ = timed_out_ || Clock::now() >= until;
                    return cleaned;
                }
            }
        }
        uint64_t used = 0;
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            if (job_->omitted(i)) continue;
            const auto * recipe = job_->recipe(i);
            for (size_t bytes : {recipe->prefix_ids.size(), recipe->suffix_ids.size(), recipe->metadata.size()}) {
                if (bytes > submission_->snapshot_bytes - used) { job_->resource_failure(); return failure(until); }
                used += bytes;
            }
        }
        auto charge = submission_->recipes->reserve(used);
        while (!charge && submission_->cache && submission_->cache->evict_one()) charge = submission_->recipes->reserve(used);
        if (!charge) { job_->resource_failure(); return failure(until); }
        auto snapshot = std::make_shared<Prepared>(); snapshot->identity = id_; snapshot->config = config_;
        snapshot->charge = std::move(charge);
        snapshot->recipes.reserve(job_->occurrences());
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            if (job_->omitted(i)) snapshot->omitted.push_back(uint32_t(i));
            else { snapshot->source_occurrences.push_back(uint32_t(i)); snapshot->recipes.push_back(*job_->recipe(i)); }
            if (cancelled() || shutdown.load() || Clock::now() >= until) return failure(until);
        }
        prepared_ = std::move(snapshot); return true;
    }
    bool admit(bool accepted, vision_protocol::Channel & channel, const std::atomic<bool> & shutdown) {
        if (!job_->admit(accepted)) return failure();
        if (!accepted) return release(channel, shutdown);
        const auto until = std::min(after(submission_->encode), reader_control()->deadline());
        auto snapshot = std::make_shared<Encoded>(); snapshot->prepared = prepared_;
        snapshot->rows.reserve(job_->occurrences());
        for (size_t i = 0; i < job_->occurrences(); ++i) {
            if (cancelled() || shutdown.load() || Clock::now() >= until) return failure(until);
            if (job_->omitted(i)) continue;
            const auto * recipe = job_->recipe(i);
            auto tag = job_->input(i)->source->tag(); tag.instance = id_.instance;
            // Context admission is still mandatory, including on complete hits.
            const vision_cache::Key key{config_,tag.source};
            auto cached = cached_[i];
            if (!cached && !submission_->flights.empty()) cached = submission_->flights[i]->flight->result();
            vision_artifact::Artifact::Ptr rows;
            vision_artifact::Result allocated;
            do {
                allocated = vision_artifact::Artifact::rows(*submission_->artifacts, tag, recipe->shape.x, recipe->shape.y,
                    config_.width, caps_.row_bytes, until, reader_control()->flag(), rows);
            } while (allocated == vision_artifact::Result::budget && submission_->cache && submission_->cache->evict_one());
            if (allocated != vision_artifact::Result::ok) {
                if (allocated == vision_artifact::Result::budget || (allocated == vision_artifact::Result::system &&
                    (errno == ENOMEM || errno == EMFILE || errno == ENFILE || errno == ENOSPC))) job_->resource_failure();
                return failure(until);
            }
            auto held_rows = cached ? cached->rows : vision_row_cache::Cache::Held{};
            // Cold duplicate occurrences share completed earlier rows even
            // when optional cache retention fails under hard-ledger pressure.
            if (!held_rows && duplicates_[i] != SIZE_MAX) {
                held_rows = std::make_shared<const vision_row_cache::Cache::Entry>(vision_row_cache::Cache::Entry{
                    {config_,tag.source,recipe->shape,recipe->row_bytes},job_->rows(duplicates_[i])});
            }
            if (held_rows) {
                if ((cached && (!(cached->key == key) || cached->recipe.metadata != recipe->metadata)) ||
                    vision_row_cache::copy(held_rows, {config_,tag.source,recipe->shape,recipe->row_bytes}, *rows,
                        until, reader_control()->flag()) != vision_artifact::Result::ok ||
                    !job_->cached_rows(rows, until, reader_control()->flag())) return failure(until);
            } else {
                auto ticket = job_->encode(rows);
                if (!ticket || !exchange(channel, *ticket, rows->fd(), until, shutdown)) return failure(until);
                if (submission_->cache) {
                    auto complete = submission_->cache->put(key, *recipe, job_->rows(i));
                    if (!submission_->flights.empty()) submission_->flights[i]->flight->publish(std::move(complete));
                }
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
    std::vector<vision_cache::Cache::Held> cached_;
    std::vector<size_t> duplicates_;
    bool timed_out_ = false;
    std::shared_ptr<vision_control::Request> reader_control() const {
        return submission_->work_control ? submission_->work_control : submission_->control;
    }
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
        const auto & cancel = cleanup ? shutdown : reader_control()->flag();
        const vision_protocol::Header header{ticket.command, uint32_t(payload.bytes.size()), id_.instance, id_.job, ticket.sequence};
        auto rc = channel.send_borrowed(header, payload.bytes.data(), payload.bytes.size(), fd >= 0 ? &fd : nullptr,
                                       payload.fd_count, caps_.payload_bytes, until, cancel);
        vision_protocol::Frame reply;
        if (rc == vision_protocol::IoResult::ok) rc = channel.receive(reply,
            {ticket.command == Kind::prepare ? Kind::prepared : ticket.command == Kind::encode ? Kind::encoded : Kind::released,
             id_.instance, id_.job, ticket.sequence, 0}, caps_.payload_bytes, until, cancel);
        if (rc == vision_protocol::IoResult::deadline) timed_out_ = true;
        reader_control()->observe(); // latch all-waiters-gone, including while transport polls
        if (rc != vision_protocol::IoResult::ok || shutdown.load()) return false;
        const auto accepted = job_->accept(reply, until, cleanup ? submission_->control->flag() : reader_control()->flag());
        return accepted == vision_job::Reply::accepted ||
            (ticket.command == Kind::prepare && accepted == vision_job::Reply::input_error);
    }
};
} // namespace strata_frontend::vision_job_io
