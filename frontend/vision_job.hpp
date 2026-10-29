// PR4 supervisor-owned per-job state/ownership core. Single-threaded mutations only.
// Not a process supervisor or GPU arbiter: caller must hold the exclusive serving turn.
// The owner MUST retain this Job through terminal drain or certified reader teardown;
// destroying an active Job is a caller lifetime error, not a cancellation mechanism.
#pragma once
#include "vision_artifact.hpp"
#include "vision_transport.hpp"
#include <optional>
#include <string>

namespace strata_frontend::vision_job {
using Kind = vision_protocol::Kind;
using Artifact = vision_artifact::Artifact;
using Clock = vision_protocol::Clock;
struct Input { Artifact::Ptr source; std::string metadata_id; };
struct Limits { uint64_t recipe_bytes, image_cells, positions; };
struct Recipe {
    std::string prefix_ids, suffix_ids, metadata; // owned, no pointers into a transport frame
    vision_metadata::Shape shape{};
    uint64_t row_bytes = 0;
};
struct Ticket { Kind command; uint64_t instance, job, sequence; uint32_t occurrence; uint64_t handle; };
enum class Phase { preparing, admitting, encoding, ready_for_engine, engine_active, draining, finalizing, failed, done };
enum class Outcome { success, refused, cancelled, failure };
enum class Reply { accepted, stale, invalid, remote_error, resource, artifact_error, cancelled };
class Job {
    struct Record {
        Input input;
        Recipe recipe;
        Artifact::Ptr rows;
        uint64_t handle = 0; // retained after RELEASE to reject handle reuse within a job
        bool released = false;
    };
public:
    static std::unique_ptr<Job> create(vision_payload::Config config, vision_payload::Limits wire_limits,
                                       Limits limits, uint64_t instance, uint64_t job, std::vector<Input> inputs) {
        if (!instance || !job || inputs.empty() || inputs.size() > wire_limits.occurrences ||
            !limits.recipe_bytes || !limits.image_cells || !limits.positions ||
            limits.image_cells > uint64_t(INT32_MAX) || limits.positions > uint64_t(INT32_MAX) ||
            !config.width || !config.pixel_stride || config.pixel_stride != wire_limits.metadata.pixel_stride ||
            !config.vocabulary_size || config.vocabulary_size > uint32_t(INT32_MAX) ||
            !wire_limits.payload_bytes || !wire_limits.row_bytes || !wire_limits.metadata.grid_cells ||
            !wire_limits.metadata.pixel_dimension || !wire_limits.metadata.pixels) return nullptr;
        for (size_t i = 0; i < inputs.size(); ++i) {
            const auto & in = inputs[i];
            if (!in.source || in.source->kind() != Artifact::Kind::source || in.source->tag().job != job ||
                in.source->tag().occurrence != i || (in.source->tag().instance && in.source->tag().instance != instance) ||
                in.metadata_id.empty() || in.metadata_id.size() > wire_limits.metadata.id_bytes ||
                in.metadata_id.find('\0') != std::string::npos ||
                vision_artifact::sealed_size(in.source->fd(), in.source->bytes(),
                    std::min<uint64_t>(wire_limits.source_bytes, 10 * 1024 * 1024)) != vision_artifact::Result::ok) return nullptr;
        }
        try { return std::unique_ptr<Job>(new Job(config, wire_limits, limits, instance, job, std::move(inputs))); }
        catch (const std::bad_alloc &) { return nullptr; }
    }
    Job(const Job &) = delete;
    Job & operator=(const Job &) = delete;
    Phase phase() const { return phase_; }
    bool pending() const { return pending_.has_value(); }
    // Recipe pointer borrows this Job. Queue integration must transfer/copy owned data, not this pointer.
    const Recipe * recipe(size_t i) const { return i < records_.size() && records_[i].handle ? &records_[i].recipe : nullptr; }
    Artifact::Ptr rows(size_t i) const {
        return i < records_.size() && records_[i].rows && records_[i].rows->validated_rows() ? records_[i].rows : nullptr;
    }

    std::optional<Ticket> prepare() {
        if (phase_ != Phase::preparing || pending_) return {};
        return issue(Kind::prepare, next_prepare_);
    }
    bool admit(bool accepted) {
        if (phase_ != Phase::admitting || pending_) return false;
        if (accepted) phase_ = Phase::encoding;
        else { outcome_ = Outcome::refused; phase_ = Phase::finalizing; }
        return true;
    }
    std::optional<Ticket> encode(Artifact::Ptr output) {
        if (phase_ != Phase::encoding || pending_ || !output) return {};
        const auto & record = records_[next_encode_];
        const auto expected = tag(next_encode_);
        if (output->kind() != Artifact::Kind::rows || !(output->tag() == expected) ||
            output->bytes() != record.recipe.row_bytes ||
            !output->writable_shape(record.recipe.shape.x, record.recipe.shape.y, config_.width)) return {};
        auto ticket = issue(Kind::encode, next_encode_);
        if (ticket) records_[next_encode_].rows = std::move(output);
        return ticket;
    }
    bool engine_started() {
        if (phase_ != Phase::ready_for_engine || pending_) return false;
        phase_ = Phase::engine_active; return true;
    }
    // Caller has observed terminal drain or confirmed engine termination, not just sent STOP.
    bool engine_terminal(bool succeeded) {
        if (phase_ != Phase::engine_active && phase_ != Phase::draining) return false;
        if (!succeeded && outcome_ != Outcome::cancelled) outcome_ = Outcome::failure;
        phase_ = Phase::finalizing; return true;
    }
    std::optional<Ticket> release() {
        if (phase_ != Phase::finalizing || pending_) return {};
        for (size_t i = 0; i < records_.size(); ++i)
            if (records_[i].handle && !records_[i].released) return issue(Kind::release, uint32_t(i));
        complete(); return {};
    }
    void cancel() {
        if (phase_ == Phase::done) return;
        outcome_ = Outcome::cancelled;
        if (phase_ == Phase::engine_active || phase_ == Phase::draining) phase_ = Phase::draining;
        else if (phase_ != Phase::finalizing) fail(); // unknown reader work/handles require teardown
        // After engine drain/context refusal, RELEASE remains bounded cooperative cleanup.
        // A sticky request-cancel flag must not itself abort that cleanup.
    }
    // Invoke for IO/protocol/native faults, not as a shortcut around engine STOP/drain.
    bool reader_failure() {
        if (phase_ == Phase::engine_active || phase_ == Phase::draining || phase_ == Phase::done) return false;
        fail(); return true;
    }
    // Caller certifies process reaping AND applicable GPU safety checks; this class cannot prove them.
    // False never releases backing storage or permits a successor. Whole-service death needs both children safe.
    bool reader_stopped(bool quiescent) {
        if (phase_ != Phase::failed || !quiescent) return false;
        pending_.reset(); complete(); return true;
    }
    std::optional<Outcome> take_terminal() {
        if (phase_ != Phase::done || terminal_sent_) return {};
        terminal_sent_ = true; return outcome_;
    }

    Reply accept(const vision_protocol::Frame & frame, Clock::time_point deadline, const std::atomic<bool> & cancel_flag) {
        // Late inbox events do not mutate a successor/current operation. Transport itself rejects
        // unexpected frames on a live channel; here stale *delivered events* are harmless.
        if (!pending_ || frame.header.instance != instance_ || frame.header.job != job_ ||
            frame.header.sequence != pending_->sequence) return Reply::stale;
        if (phase_ == Phase::failed || phase_ == Phase::done) return Reply::stale;
        const auto ticket = *pending_;
        const bool cleanup = ticket.command == Kind::release && phase_ == Phase::finalizing;
        if (cancel_flag.load()) {
            cancel();
            if (!cleanup) return Reply::cancelled;
        }
        if (Clock::now() >= deadline) { fail(); return Reply::invalid; }
        Kind expected = ticket.command == Kind::prepare ? Kind::prepared : ticket.command == Kind::encode ? Kind::encoded : Kind::released;
        if ((frame.header.kind != expected && frame.header.kind != Kind::error) ||
            frame.header.payload_size != frame.payload.size() || frame.fd_count) { fail(); return Reply::invalid; }
        auto & record = records_[ticket.occurrence];
        vision_payload::Binding binding{ticket.occurrence, record.input.source->tag().source,
            record.input.metadata_id, record.input.source->bytes(), record.handle, record.recipe.shape.x, record.recipe.shape.y};
        vision_payload::View view;
        std::string_view payload(reinterpret_cast<const char *>(frame.payload.data()), frame.payload.size());
        if (!vision_payload::validate(frame.header.kind, payload, frame.fd_count, config_, wire_limits_, binding, view)) {
            fail(); return Reply::invalid;
        }
        if (frame.header.kind == Kind::error) { fail(); return Reply::remote_error; }
        if (ticket.command == Kind::prepare) {
            for (const auto & r : records_) if (r.handle == view.handle) { fail(); return Reply::invalid; }
            const uint64_t text_bytes = uint64_t(view.prefix_ids.size()) + view.suffix_ids.size();
            const uint64_t bytes = uint64_t(view.metadata.size()) + text_bytes;
            const uint64_t positions = view.shape.positions + text_bytes / 4;
            const uint64_t cells = view.shape.cells + text_bytes / 4;
            if (bytes > limits_.recipe_bytes - recipe_bytes_ || view.shape.cells > limits_.image_cells - image_cells_ ||
                positions > limits_.positions - positions_ || cells > uint64_t(INT32_MAX) - cells_) { fail(); return Reply::resource; }
            Recipe owned;
            try { owned = {std::string(view.prefix_ids), std::string(view.suffix_ids), std::string(view.metadata), view.shape, view.row_bytes}; }
            catch (const std::bad_alloc &) { fail(); return Reply::resource; }
            record.recipe = std::move(owned); record.handle = view.handle;
            recipe_bytes_ += bytes; image_cells_ += view.shape.cells; positions_ += positions; cells_ += cells;
            if (++next_prepare_ == records_.size()) phase_ = Phase::admitting;
        } else if (ticket.command == Kind::encode) {
            auto result = record.rows->finish_rows(tag(ticket.occurrence), view.shape.x, view.shape.y, config_.width, deadline, cancel_flag);
            if (result != vision_artifact::Result::ok) {
                if (result == vision_artifact::Result::cancelled) { cancel(); return Reply::cancelled; }
                fail(); return Reply::artifact_error;
            }
            if (++next_encode_ == records_.size()) phase_ = Phase::ready_for_engine;
        } else {
            record.released = true;
        }
        if (cancel_flag.load()) {
            cancel();
            if (!cleanup) return Reply::cancelled;
        }
        if (Clock::now() >= deadline) { fail(); return Reply::invalid; }
        pending_.reset();
        if (phase_ == Phase::finalizing) {
            bool remaining = false;
            for (const auto & r : records_) remaining |= r.handle && !r.released;
            if (!remaining) complete();
        }
        return Reply::accepted;
    }
private:
    const vision_payload::Config config_;
    const vision_payload::Limits wire_limits_;
    const Limits limits_;
    const uint64_t instance_, job_;
    std::vector<Record> records_;
    Phase phase_ = Phase::preparing;
    Outcome outcome_ = Outcome::success;
    std::optional<Ticket> pending_;
    uint64_t sequence_ = 0, recipe_bytes_ = 0, image_cells_ = 0, positions_ = 0, cells_ = 0;
    uint32_t next_prepare_ = 0, next_encode_ = 0;
    bool terminal_sent_ = false;
    Job(vision_payload::Config config, vision_payload::Limits wire_limits, Limits limits,
        uint64_t instance, uint64_t job, std::vector<Input> inputs)
        : config_(config), wire_limits_(wire_limits), limits_(limits), instance_(instance), job_(job) {
        records_.reserve(inputs.size());
        for (auto & in : inputs) records_.push_back({std::move(in), {}, {}, 0, false});
    }
    vision_artifact::Tag tag(uint32_t i) const { return {instance_, job_, i, records_[i].input.source->tag().source}; }
    std::optional<Ticket> issue(Kind command, uint32_t occurrence) {
        if (sequence_ == UINT64_MAX) { fail(); return {}; }
        Ticket t{command, instance_, job_, ++sequence_, occurrence, records_[occurrence].handle};
        pending_ = t; return t;
    }
    void fail() {
        if (outcome_ == Outcome::success) outcome_ = Outcome::failure;
        phase_ = Phase::failed; // keep pending identity, handles and every artifact until safe teardown
    }
    void complete() {
        phase_ = Phase::done; pending_.reset();
        for (auto & r : records_) { r.input.source.reset(); r.rows.reset(); r.recipe = {}; }
    }
};
} // namespace strata_frontend::vision_job
