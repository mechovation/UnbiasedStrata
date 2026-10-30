// PR4 native pipeline bound to the actual server_queue. All limits and the
// single-device authority come from explicit runtime configuration.
#pragma once
#include "vision_composition.hpp"
#include "vision_engine_adapter.hpp"
#include "server-queue.h"
#include "vendor/hash/hash.h"
#include <condition_variable>
#include <map>

namespace strata_frontend::vision_queue {
namespace io = vision_job_io;
namespace vc = vision_control;
namespace vs = vision_supervisor;
namespace ea = vision_engine_adapter;
using Clock = vc::Clock;
struct Limits {
    size_t pending_count, pending_bytes, output_records, inbox_events;
    uint32_t prompt_cells;
    uint64_t disk_bytes, snapshot_bytes;
    vision_job::Limits job;
    Clock::duration total, prepare, encode, cleanup, start;
};
// Queue callbacks run after server_queue unlocks mutex_tasks. No inverse
// queue->inbox lock edge is permitted (including sleeping/yield callbacks).
// post may allocate/briefly contend on the upstream mutex: failure is fail-stop,
// never a dropped notification. Production latency/queue-size qualification remains.
class Wake final : public vc::Wake {
    std::shared_ptr<server_queue> queue_;
    const int id_;
public:
    explicit Wake(std::shared_ptr<server_queue> queue) : queue_(std::move(queue)), id_(queue_->get_new_id()) {}
    void post() noexcept override {
        try { server_task task(SERVER_TASK_TYPE_NEXT_RESPONSE); task.id = id_; queue_->post(std::move(task)); }
        catch (...) { std::terminate(); }
    }
};
class Output final : public ea::Sink {
public:
    struct Record { vision_engine_handoff::Identity identity; EngineLine line; };
private:
    std::shared_ptr<Wake> wake_;
    std::mutex mutex_;
    std::vector<Record> ring_;
    size_t head_ = 0, count_ = 0;
    bool pending_ = false;
public:
    Output(std::shared_ptr<Wake> wake, size_t cap) : wake_(std::move(wake)), ring_(cap) {
        if (!cap) throw std::invalid_argument("empty native output budget");
    }
    bool info(std::string_view) noexcept override { return true; } // no retained startup diagnostics
    bool line(vision_engine_handoff::Identity id, const EngineLine & line) noexcept override {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (count_ == ring_.size()) return false; // bounded refusal, adapter contains
            ring_[(head_ + count_) % ring_.size()] = {id, line}; ++count_;
            if (!pending_) { pending_ = true; wake_->post(); }
            return true;
        } catch (...) { return false; }
    }
    std::optional<Record> pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!count_) { pending_ = false; return {}; }
        Record result = std::move(ring_[head_]); ring_[head_] = {};
        head_ = (head_ + 1) % ring_.size(); --count_; return result;
    }
};
enum class Error { none, unavailable, invalid, resource, context, deadline, internal };
class Bridge {
    struct Ledger {
        std::mutex mutex;
        size_t count = 0, bytes = 0;
        const size_t max_count, max_bytes;
        Ledger(size_t count, size_t bytes) : max_count(count), max_bytes(bytes) {}
    };
public:
    struct Envelope {
        int task;
        std::string prompt;
        std::vector<std::pair<size_t,size_t>> plain;
        std::vector<raw_buffer> sources;
        llama_tokens text; // already-tokenized parent text; no image placeholders
        std::shared_ptr<vc::Request> control;
        std::shared_ptr<Ledger> ledger;
        size_t bytes = 0;
        ~Envelope() {
            if (!ledger) return;
            std::lock_guard<std::mutex> lock(ledger->mutex); --ledger->count; ledger->bytes -= bytes;
        }
    };
    struct Prompt final : vision_engine_handoff::Resources { server_tokens tokens; std::shared_ptr<const Envelope> envelope; };
    struct Notice {
        enum Kind { prompt, line, terminal } kind;
        io::Identity identity;
        std::shared_ptr<const Prompt> composed;
        EngineLine output;
        Error error = Error::none;
        bool disconnected = false;
    };
private:
    struct Result {
        enum Kind { sources, composed, file } kind;
        io::Identity identity;
        Error error = Error::none;
        std::unique_ptr<io::Submission> submission;
        std::shared_ptr<const Prompt> prompt;
        std::shared_ptr<const ea::Request> request;
        Result(Kind k, io::Identity id) : kind(k), identity(id) {}
    };
    // Exactly one bounded parent IO/composition operation, plus one owned result.
    // No slot/task/HTTP pointers cross this boundary. Does not spawn children.
    class Worker {
        std::shared_ptr<Wake> wake_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::function<Result()> work_;
        std::optional<Result> done_;
        bool busy_ = false, stopping_ = false;
        std::thread owner_;
    public:
        explicit Worker(std::shared_ptr<Wake> wake) : wake_(std::move(wake)), owner_([this] {
            for (;;) {
                std::function<Result()> work;
                { std::unique_lock<std::mutex> lock(mutex_); cv_.wait(lock, [&] { return work_ || stopping_; });
                  if (!work_) return;
                  work = std::move(work_); }
                auto result = work(); work = {}; // release input captures before notifying
                { std::lock_guard<std::mutex> lock(mutex_); done_.emplace(std::move(result)); busy_ = false; wake_->post(); }
            }
        }) {}
        ~Worker() { join(); }
        bool submit(std::function<Result()> work) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (busy_ || done_ || stopping_) return false;
            work_ = std::move(work); busy_ = true; cv_.notify_one(); return true;
        }
        std::optional<Result> take() {
            std::lock_guard<std::mutex> lock(mutex_); auto result = std::move(done_); done_.reset(); return result;
        }
        bool idle() { std::lock_guard<std::mutex> lock(mutex_); return !busy_ && !done_; }
        void join() {
            { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; cv_.notify_one(); }
            if (owner_.joinable()) owner_.join();
        }
    };
    const Limits limits_;
    const vision_payload::Config config_;
    const vision_payload::Limits wire_;
    const ea::Limits engine_limits_;
    const llama_vocab * vocab_; // service lifetime, joined before vocabulary destruction
    const int32_t pad_;
    const std::string marker_;
    const std::function<llama_tokens(const std::string &)> literal_;
    std::shared_ptr<Ledger> ledger_;
    std::shared_ptr<Wake> wake_;
    std::shared_ptr<vc::Inbox> inbox_;
    std::shared_ptr<Output> output_;
    std::shared_ptr<ea::Adapter> engine_;
    std::unique_ptr<vs::Supervisor> supervisor_;
    mutable std::mutex lifecycle_mutex_; // recover/shutdown may race the final owner join
    Worker worker_;
    std::shared_ptr<vision_artifact::Budget> artifacts_;
    std::shared_ptr<io::RecipeBudget> recipes_;
    std::shared_ptr<vision_row_cache::Cache> cache_;
    bool cache_frozen_ = false; // lifecycle_mutex_; immutable after start/first enqueue
    std::mutex envelopes_mutex_;
    std::map<int,std::shared_ptr<Envelope>> envelopes_;
    std::shared_ptr<Envelope> active_; // main-queue only below here
    std::shared_ptr<const Prompt> prompt_;
    io::Identity identity_{};
    uint64_t next_job_ = 0;
    enum class Phase { sources, preparing, composing, admission, encoding, file, engine } phase_ = Phase::sources;
    bool handed_ = false, generated_ = false, terminal_pending_ = false, awaiting_command_ = false;
    Error error_ = Error::none;
    std::optional<EngineLine> terminal_line_;
    std::atomic<bool> available_{false}, stopping_{false}, exited_{false}, failure_latched_{false};
    static uint64_t task_id(int id) { return uint64_t(id) + 1; } // wire task zero is reserved
    bool same(io::Identity id) const { return active_ && id.task == identity_.task && id.instance == identity_.instance && id.job == identity_.job; }
    bool cancelled() { return active_->control->observe() || stopping_; }
    void fail(Error error) {
        error_ = stopping_ ? Error::unavailable : error;
        if (handed_) active_->control->cancel(vc::Reason::cancelled);
        else terminal_pending_ = true; // nothing could have reached a child
    }
    Notice finish() {
        auto control = active_->control;
        if (control->reason() == vc::Reason::deadline) error_ = Error::deadline;
        const bool disconnected = control->reason() == vc::Reason::disconnect;
        if (handed_ && !supervisor_->acknowledge(identity_)) std::terminate();
        Notice result{Notice::terminal, identity_, {}, terminal_line_.value_or(EngineLine{}), error_, disconnected};
        { std::lock_guard<std::mutex> lock(envelopes_mutex_); envelopes_.erase(active_->task); }
        active_.reset(); prompt_.reset(); terminal_line_.reset(); terminal_pending_ = false;
        handed_ = generated_ = false; error_ = Error::none; return result;
    }
public:
    // Every cap/authority is explicit. The factory binds the SAME authority to
    // this engine adapter; no default CPU-reap-as-GPU-safety implementation.
    Bridge(std::shared_ptr<server_queue> queue, vs::Launch reader, vs::Limits lifecycle, ea::Launch engine,
           ea::Limits engine_limits, Limits limits, int diagnostic, const llama_vocab * vocab, int32_t pad,
           std::string marker, std::function<llama_tokens(const std::string &)> literal,
           std::shared_ptr<vision_artifact::Budget> artifacts, std::shared_ptr<io::RecipeBudget> recipes,
           std::function<std::shared_ptr<vs::Safety>(std::shared_ptr<ea::Adapter>)> authority)
        : limits_(limits), config_(reader.config), wire_(reader.wire_limits), engine_limits_(engine_limits), vocab_(vocab), pad_(pad),
          marker_(std::move(marker)), literal_(std::move(literal)), ledger_(std::make_shared<Ledger>(limits.pending_count, limits.pending_bytes)),
          wake_(std::make_shared<Wake>(std::move(queue))), inbox_(std::make_shared<vc::Inbox>(limits.inbox_events, wake_)),
          output_(std::make_shared<Output>(wake_, limits.output_records)),
          engine_(std::make_shared<ea::Adapter>(std::move(engine), engine_limits, diagnostic, output_)), worker_(wake_),
          artifacts_(std::move(artifacts)), recipes_(std::move(recipes)) {
        if (!vocab || !artifacts_ || !recipes_ || !authority || !literal_ || !limits.pending_count || !limits.pending_bytes ||
            !limits.prompt_cells || !limits.disk_bytes || !limits.snapshot_bytes || marker_.empty() ||
            !io::duration_valid(limits.total) || !io::duration_valid(limits.prepare) || !io::duration_valid(limits.encode) ||
            !io::duration_valid(limits.cleanup) || !io::duration_valid(limits.start)) throw std::invalid_argument("invalid native queue limits");
        supervisor_ = std::make_unique<vs::Supervisor>(std::move(reader), lifecycle, diagnostic, authority(engine_), inbox_, std::make_shared<vs::Reaper>(), engine_);
    }
    ~Bridge() { request_shutdown(); join(); }
    // Optional service configuration, BEFORE recover()/the first enqueue.
    // Zero entries disables caching. Both cache ownership and held entries retain
    // the existing service artifact ledger, never a fresh per-request budget.
    bool set_cache(size_t entries, uint64_t bytes) {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (cache_frozen_ || stopping_ || (entries && !bytes)) return false;
        cache_ = entries ? std::make_shared<vision_row_cache::Cache>(entries, bytes, artifacts_) : nullptr;
        return true;
    }
    vision_row_cache::Cache::Stats cache_stats() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        return cache_ ? cache_->stats() : vision_row_cache::Cache::Stats{};
    }
    bool available() const { return available_; }
    bool service_alive() const { std::lock_guard<std::mutex> lock(lifecycle_mutex_); return supervisor_ && !stopping_ && !supervisor_->failed_service() && supervisor_->state() != vs::State::safety_blocked && !engine_->stopped(); }
    bool service_failed() const { std::lock_guard<std::mutex> lock(lifecycle_mutex_); return failure_latched_ || (supervisor_ && supervisor_->failed_service()); }
    uint64_t context() const { return engine_->context(); }
    bool wait_ready(Clock::time_point until) {
        if (!recover()) return false;
        while (Clock::now() < until && !stopping_) {
            while (poll()) {}
            if (available_) return true;
            const auto state = supervisor_->state();
            if (state == vs::State::backoff || state == vs::State::unavailable || state == vs::State::safety_blocked || state == vs::State::exited) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }
    void stop_generation() { if (active_ && generated_) (void) supervisor_->request_stop(identity_); }
    bool recover() {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (stopping_ || available_ || !supervisor_ || !supervisor_->restart_admissible()) return false;
        { std::lock_guard<std::mutex> pending(envelopes_mutex_); if (!envelopes_.empty()) return false; }
        cache_frozen_ = true;
        available_ = false; return supervisor_->start(UINT64_MAX);
    }
    // HTTP owner only. Admission charges BEFORE copying the envelope. Logical
    // bytes plus count bounds; task options/tokenizer/RSS have separate gates.
    std::shared_ptr<vc::Request> enqueue(int task, const std::string & prompt,
        const std::vector<std::pair<size_t,size_t>> & plain, const std::vector<raw_buffer> & sources,
        const llama_tokens & text = {}, Error * rejection = nullptr) {
        auto reject = [&](Error error) { if (rejection) *rejection = error; return std::shared_ptr<vc::Request>{}; };
        { std::lock_guard<std::mutex> lock(lifecycle_mutex_); cache_frozen_ = true; }
        if (stopping_) return reject(Error::unavailable);
        if (task < 0 || (!sources.empty() && !text.empty())) return reject(Error::invalid);
        if (sources.size() > wire_.occurrences || text.size() > limits_.prompt_cells) return reject(Error::resource);
        for (auto id : text) if (id < 0 || uint32_t(id) >= config_.vocabulary_size) return reject(Error::invalid);
        size_t bytes = prompt.size();
        if (bytes > limits_.pending_bytes || plain.size() > (limits_.pending_bytes - bytes) / sizeof(plain[0])) return reject(Error::resource);
        bytes += plain.size() * sizeof(plain[0]);
        if (text.size() > (limits_.pending_bytes - bytes) / sizeof(text[0])) return reject(Error::resource);
        bytes += text.size() * sizeof(text[0]);
        for (const auto & source : sources) {
            if (source.empty()) return reject(Error::invalid);
            if (source.size() > wire_.source_bytes || source.size() > 10*1024*1024 || source.size() > limits_.pending_bytes - bytes) return reject(Error::resource);
            bytes += source.size();
        }
        auto envelope = std::make_shared<Envelope>();
        { std::lock_guard<std::mutex> lock(ledger_->mutex);
          if (ledger_->count == ledger_->max_count || bytes > ledger_->max_bytes - ledger_->bytes) return reject(Error::unavailable);
          ++ledger_->count; ledger_->bytes += bytes; envelope->bytes = bytes; envelope->ledger = ledger_; }
        envelope->task = task; envelope->control = std::make_shared<vc::Request>(io::after(limits_.total));
        envelope->prompt = prompt; envelope->plain = plain; envelope->sources = sources; envelope->text = text;
        std::lock_guard<std::mutex> lock(envelopes_mutex_);
        if (stopping_ || !envelopes_.emplace(task, envelope).second) return reject(Error::unavailable);
        return envelope->control;
    }
    // HTTP posting rollback only, before the task could enter the queue.
    void withdraw(int task) {
        { std::lock_guard<std::mutex> lock(envelopes_mutex_); envelopes_.erase(task); }
        wake_->post(); // a posting rollback may remove shutdown's last pending envelope
    }
    // Main queue only. Upstream CANCEL removes queued/deferred tasks first.
    void cancel(int task) {
        std::lock_guard<std::mutex> lock(envelopes_mutex_);
        auto found = envelopes_.find(task); if (found == envelopes_.end()) return;
        found->second->control->cancel(vc::Reason::disconnect);
        if (!active_ || active_->task != task) envelopes_.erase(found);
    }
    bool begin(int task) {
        if (active_) std::terminate();
        { std::lock_guard<std::mutex> lock(envelopes_mutex_);
          auto found = envelopes_.find(task); if (found == envelopes_.end()) return false;
          active_ = found->second; }
        identity_ = {task_id(task), supervisor_->instance(), next_job_ == UINT64_MAX ? 0 : ++next_job_}; phase_ = Phase::sources;
        if (!identity_.job || (!available_ && !(active_->sources.empty() && supervisor_->text_available())) || stopping_) { fail(Error::unavailable); return true; }
        if (cancelled()) { fail(Error::internal); return true; }
        auto envelope = active_; auto id = identity_;
        if (!worker_.submit([this, envelope, id] {
            Result result{Result::sources, id};
            try {
                auto s = std::make_unique<io::Submission>(); s->job = id.job; s->control = envelope->control;
                s->artifacts = artifacts_; s->recipes = recipes_; s->cache = cache_;
                s->limits = limits_.job; s->snapshot_bytes = limits_.snapshot_bytes;
                s->prepare = limits_.prepare; s->encode = limits_.encode; s->cleanup = limits_.cleanup;
                s->text_only = envelope->sources.empty();
                for (size_t i = 0; i < envelope->sources.size(); ++i) {
                    const auto & source = envelope->sources[i];
                    const auto hex = hash_sha256_hex(source.data(), source.size()); vision_payload::Digest digest{};
                    auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
                    for (size_t j = 0; j < digest.size(); ++j) digest[j] = uint8_t(16*digit(hex[2*j]) + digit(hex[2*j+1]));
                    vision_artifact::Artifact::Ptr artifact;
                    vision_artifact::Result allocated;
                    do {
                        allocated = vision_artifact::Artifact::source(*artifacts_, {0,id.job,uint32_t(i),digest},
                            {reinterpret_cast<const char *>(source.data()), source.size()}, wire_.source_bytes,
                            envelope->control->deadline(), envelope->control->flag(), artifact);
                    } while (allocated == vision_artifact::Result::budget && cache_ && cache_->evict_one());
                    if (allocated != vision_artifact::Result::ok) { result.error = Error::resource; return result; }
                    s->inputs.push_back({artifact, hex});
                }
                envelope->sources.clear(); result.submission = std::move(s);
            } catch (...) { result.error = Error::resource; }
            return result;
        })) fail(Error::internal);
        return true;
    }
    void admit(int32_t maximum, const std::string & header, bool accepted) {
        if (!active_ || !handed_ || !prompt_ || generated_ || phase_ != Phase::admission) std::terminate();
        if (!accepted) error_ = Error::context;
        // Store only a bounded owned header, never task/slot pointers.
        if (header.size() > engine_limits_.command_bytes || maximum <= 0) {
            if (accepted) { fail(Error::resource); return; }
        }
        header_ = header; maximum_ = maximum;
        if (!supervisor_->admit(identity_, accepted)) fail(Error::internal);
        else { phase_ = Phase::encoding; awaiting_command_ = true; }
    }
private:
    std::string header_;
    int32_t maximum_ = 0;
public:
    // Main queue: call until empty BEFORE any idle-slot return. All publications
    // are identity/stage checked. A DONE/ERR is retained until certified terminal.
    std::optional<Notice> poll() {
        const auto state = supervisor_->state();
        if (state == vs::State::backoff || state == vs::State::unavailable || state == vs::State::safety_blocked || state == vs::State::exited) available_ = false;
        if (terminal_pending_) return finish();
        while (auto record = output_->pop()) {
            if (!same({record->identity.task,record->identity.instance,record->identity.job}) || record->identity.sequence != 1 || !generated_) continue;
            if (record->line.kind == EngineLine::DONE || record->line.kind == EngineLine::ERR) terminal_line_ = std::move(record->line);
            else if (!active_->control->observe()) return Notice{Notice::line, identity_, {}, std::move(record->line)};
        }
        if (auto result = worker_.take()) {
            if (!same(result->identity)) std::terminate();
            if (result->error != Error::none || cancelled()) fail(result->error == Error::none ? Error::internal : result->error);
            else if (draining_terminal_) { /* certified terminal overtook parent work: discard its owned result */ }
            else if (result->kind == Result::sources && phase_ == Phase::sources) {
                image_ids_.clear(); for (const auto & input : result->submission->inputs) image_ids_.push_back(input.metadata_id);
                if (supervisor_->prepare(identity_.task, result->submission)) { handed_ = true; phase_ = Phase::preparing; awaiting_command_ = true; }
                else fail(Error::unavailable);
            } else if (result->kind == Result::composed && phase_ == Phase::composing) {
                phase_ = Phase::admission; prompt_ = std::move(result->prompt); return Notice{Notice::prompt, identity_, prompt_, {}};
            } else if (result->kind == Result::file && phase_ == Phase::file) {
                if (!supervisor_->generate(identity_, result->request, limits_.start, limits_.cleanup)) fail(Error::internal);
                else { generated_ = true; phase_ = Phase::engine; awaiting_command_ = true; }
            } else std::terminate();
        }
        while (auto event = inbox_->pop()) {
            using K = vc::Event::Kind;
            if (!event->job) {
                if (event->kind == K::ready && !stopping_) available_ = true;
                if (event->kind == K::failed || event->kind == K::unavailable || event->kind == K::stopped) available_ = false;
                if (event->kind == K::stopped && !event->task) exited_ = true;
                continue;
            }
            if (!same({event->task,event->instance,event->job}) || event->sequence) continue;
            if (event->kind == K::prepared || event->kind == K::encoded || event->kind == K::completed ||
                event->kind == K::engine_submitted || event->kind == K::failed || event->kind == K::unavailable)
                awaiting_command_ = false; // stage event is published AFTER the supervisor frees its mailbox
            if (event->kind == K::failed || event->kind == K::unavailable) {
                if (supervisor_->state() == vs::State::backoff || supervisor_->state() == vs::State::unavailable ||
                    supervisor_->state() == vs::State::safety_blocked || supervisor_->state() == vs::State::exited) available_ = false;
            }
            if (event->kind == K::prepared && phase_ == Phase::preparing) {
                phase_ = Phase::composing;
                auto prepared = supervisor_->prepared(); auto envelope = active_; auto id = identity_; auto ids = image_ids_;
                if (!prepared || !worker_.submit([this, prepared, envelope, id, ids] {
                    Result result{Result::composed, id};
                    try {
                        auto composed = std::make_shared<Prompt>(); composed->envelope = envelope;
                        if (prepared->recipes.empty()) composed->tokens = server_tokens(envelope->text, false);
                        else composed->tokens = vision_composition::compose(vocab_, envelope->prompt, envelope->plain, marker_,
                            *prepared, ids, config_, wire_, limits_.prompt_cells, literal_);
                        result.prompt = std::move(composed);
                    } catch (...) { result.error = Error::internal; }
                    return result;
                })) fail(Error::internal);
            } else if (event->kind == K::encoded && phase_ == Phase::encoding) {
                phase_ = Phase::file;
                auto encoded = supervisor_->encoded(); auto prompt = prompt_; auto id = identity_; auto envelope = active_;
                const auto header = header_; const auto maximum = maximum_;
                if (!encoded || !worker_.submit([this, encoded, prompt, id, envelope, header, maximum] {
                    Result result{Result::file, id};
                    try {
                        if (encoded->rows.empty()) {
                            // Plain GEN: no temporary directory/file and no reader command.
                            result.request = ea::Request::create({id.task,id.instance,id.job,1}, header,
                                prompt->tokens.get_text_tokens(), engine_limits_, prompt);
                            if (!result.request || maximum <= 0) result.error = Error::resource;
                            return result;
                        }
                        auto file = vision_composition::File::create(prompt->tokens, *encoded, pad_, config_.vocabulary_size,
                            limits_.disk_bytes, envelope->control->deadline(), envelope->control->flag());
                        if (!file) { result.error = Error::resource; return result; }
                        std::string command = header;
                        command.replace(0, 3, "GENI");
                        if (!file->keys.empty()) command += " img_keys=" + file->keys;
                        command += " image_pad=" + std::to_string(pad_) + " " + file->path();
                        result.request = ea::Request::create({id.task,id.instance,id.job,1}, command, file->ids, engine_limits_, file);
                        if (!result.request || maximum <= 0) result.error = Error::resource;
                    } catch (...) { result.error = Error::internal; }
                    return result;
                })) fail(Error::internal);
            } else if (event->kind == K::job_terminal) {
                // Sink publications precede this event. Drain their coalesced ring
                // first on the next poll, even if its wake arrived in another order.
                auto terminal = supervisor_->terminal();
                if (!terminal || !same(terminal->identity)) std::terminate();
                if (terminal->timed_out) error_ = Error::deadline;
                else if (terminal->error_class == 1) error_ = Error::invalid;
                else if (terminal->error_class == 2) error_ = Error::resource;
                else if (terminal->outcome != vision_job::Outcome::success && error_ == Error::none) error_ = Error::internal;
                terminal_pending_ = true;
            }
        }
        if (terminal_pending_) {
            // Re-enter output draining before publishing terminal. Outputs cannot
            // be appended after Supervisor's certified terminal publication.
            terminal_pending_ = false; draining_terminal_ = true;
        }
        if (draining_terminal_) {
            if (!worker_.idle() || awaiting_command_) return {}; // retain parent work and wait for mailbox settlement
            while (auto record = output_->pop()) {
                if (!same({record->identity.task,record->identity.instance,record->identity.job}) || record->identity.sequence != 1) continue;
                if (record->line.kind == EngineLine::DONE || record->line.kind == EngineLine::ERR) terminal_line_ = std::move(record->line);
                else if (!active_->control->observe()) return Notice{Notice::line, identity_, {}, std::move(record->line)};
            }
            draining_terminal_ = false; return finish();
        }
        if (terminal_pending_) return finish();
        return {};
    }
private:
    bool draining_terminal_ = false;
    std::vector<std::string> image_ids_; // main-owned, copied to worker before use
public:
    bool settled() { std::lock_guard<std::mutex> lock(envelopes_mutex_); return exited_ && !active_ && envelopes_.empty() && worker_.idle(); }
    void request_shutdown() noexcept {
        stopping_ = true; available_ = false;
        { std::lock_guard<std::mutex> lock(envelopes_mutex_); for (auto & item : envelopes_) item.second->control->cancel(vc::Reason::cancelled); }
        { std::lock_guard<std::mutex> lock(lifecycle_mutex_); if (supervisor_) supervisor_->shutdown(); }
        wake_->post();
    }
    // Only after queue callbacks have stopped; producers are joined while the
    // shared queue/wake remain alive. Caller keeps vocabulary alive through here.
    void join() {
        stopping_ = true; available_ = false;
        worker_.join();
        { std::lock_guard<std::mutex> lock(lifecycle_mutex_);
          if (supervisor_) failure_latched_ = failure_latched_ || supervisor_->failed_service();
          supervisor_.reset(); }
        inbox_->stop_accepting(); while (inbox_->pop()) {}
        while (output_->pop()) {}
    }
};
} // namespace strata_frontend::vision_queue
