// PR4 single-owner lifecycle/Job/engine-handoff supervisor.
// One shared authority owns the lease across certified reader/engine transfers.
#pragma once
#include "clean_process.hpp"
#include "process_lifecycle.hpp"
#include "vision_commands.hpp"
#include "vision_control.hpp"
#include "vision_transport.hpp"
#include "vision_job_io.hpp"
#include "vision_engine_handoff.hpp"
#include <condition_variable>
#include <thread>

namespace strata_frontend::vision_supervisor {
using Clock = vision_protocol::Clock;
namespace handoff = vision_engine_handoff;
enum class State { stopped, starting, ready, preparing, encoding, engine_active, finalizing, stopping, safety_blocked, backoff, unavailable, exited };
// The supervisor binds immutable rows itself, never trusts a caller-supplied row
// snapshot. request owns composed prompt/options and engine-visible file storage.
struct EngineResources final : handoff::Resources {
    std::shared_ptr<const vision_job_io::Encoded> encoded;
    std::shared_ptr<const handoff::Resources> request;
};
struct Limits {
    Clock::duration ready, quit, term, kill, backoff, restart_window;
    uint32_t attempts; // lifetime attempt cap, including initial startup; no automatic reset
};
struct Launch {
    std::string guard, executable;
    std::vector<std::string> argv, environment; // explicit caller-reviewed allowlist, never environ
    uint64_t first_instance;
    vision_payload::Config config;
    vision_payload::Limits wire_limits;
    bool initialize_engine = false; // candidate startup: reader READY then engine READY before service readiness
};
// Parent-owned authority, never driven by a child-supplied boolean. All methods
// must be bounded, noexcept and non-reentrant. No default GPU certification exists.
struct Safety : handoff::Authority {
    virtual ~Safety() = default;
    virtual bool acquire(uint64_t instance) noexcept = 0;
    virtual void reader_spawned(pid_t) noexcept {}
    virtual bool healthy() noexcept { return true; }
    // Fail closed for older lifecycle-only authorities. One authority must bind
    // the job identity and atomically transfer this existing lease, never acquire
    // a second independent engine lease. to_reader is safety, not readmission:
    // whole-service revocation must remain sticky after a certified handback.
    bool to_engine(handoff::Identity) noexcept override { return false; }
    bool to_reader(handoff::Identity) noexcept override { return false; }
    // Called by engine_died() before waking the IO owner: revoke global admission
    // synchronously. release() must never undo this whole-service revocation.
    virtual void service_failed() noexcept = 0;
    virtual bool engine_stopped() noexcept = 0;
    // Invoked only after this owner reaped the child, or synchronous spawn failed.
    // service_failure additionally requires independent engine safety evidence.
    virtual bool certify(uint64_t instance, bool spawned, bool service_failure) noexcept = 0;
    virtual void release(uint64_t instance) noexcept = 0;
};
// Trusted platform seam. A successful implementation MUST actually reap this PID;
// timeout/error MUST retain ownership. Tests may inject failures, never fake success.
struct Reaper {
    virtual ~Reaper() = default;
    virtual ReapResult terminate(pid_t & pid, Clock::time_point term, Clock::time_point kill) noexcept {
        int status = 0;
        return terminate_until(pid, term, kill, status);
    }
};
class Supervisor {
public:
    // Inbox needs space for a service-lifetime emergency notification, each
    // command, and one idle-reader failure notification reserved before spawn. stderr is duplicated here; no borrowed launch FDs outlive construction.
    Supervisor(Launch launch, Limits limits, int stderr_fd, std::shared_ptr<Safety> safety,
               std::shared_ptr<vision_control::Inbox> inbox, std::shared_ptr<Reaper> reaper = std::make_shared<Reaper>(),
               std::shared_ptr<handoff::Engine> engine = {})
        : launch_(std::move(launch)), limits_(limits), safety_(std::move(safety)), inbox_(std::move(inbox)),
          reaper_(std::move(reaper)), engine_(std::move(engine)), next_instance_(launch_.first_instance) {
        if (!safety_ || !inbox_ || !reaper_ || !next_instance_ || launch_.guard.empty() ||
            launch_.executable.empty() || launch_.argv.empty() || !limits_.attempts ||
            !positive(limits_.ready) || !positive(limits_.quit) || !positive(limits_.term) ||
            !positive(limits_.kill) || !positive(limits_.backoff) || !positive(limits_.restart_window))
            throw std::invalid_argument("invalid supervisor configuration");
        vision_commands::Payload ready;
        if (vision_commands::build(vision_protocol::Kind::ready, launch_.config, launch_.wire_limits, {}, ready)
            != vision_commands::Result::ok) throw std::invalid_argument("invalid READY configuration");
        emergency_ = inbox_->reserve();
        if (!emergency_) throw std::invalid_argument("missing supervisor emergency capacity");
        diagnostic_.value = ::fcntl(stderr_fd, F_DUPFD_CLOEXEC, 3);
        if (diagnostic_.value < 0) throw std::runtime_error("supervisor diagnostic descriptor");
        owner_ = std::thread([this] { run(); });
    }
    Supervisor(const Supervisor &) = delete;
    Supervisor & operator=(const Supervisor &) = delete;
    ~Supervisor() {
        shutdown();
        if (owner_.joinable()) owner_.join();
        // Unresolved cleanup terminates the service in run(), never silently
        // destroys process ownership or returns a supposedly free GPU turn.
    }
    bool start(uint64_t task) { return submit(Command::start, task); }
    bool stop(uint64_t task) { return submit(Command::stop, task); }
    // Moves submission only on acceptance. Reserve command AND once-only terminal
    // capacity before any native handoff. Job IDs must increase across this owner's lifetime.
    bool prepare(uint64_t task, std::unique_ptr<vision_job_io::Submission> & submission) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!task || !submission || !vision_job_io::valid(*submission) || submission->job <= last_job_ ||
            busy_ || claimed_ || shutdown_ || (state_ != State::ready &&
                !((submission->text_only || submission->complete_only) && text_available()))) return false;
        auto event = inbox_->reserve(); auto terminal = inbox_->reserve();
        if (!event || !terminal) return false;
        Pending pending{Command::prepare, task, std::move(event)};
        pending.identity = {task, instance_, submission->job};
        pending.terminal = std::move(terminal);
        request_ = submission->control;
        reader_request_ = submission->work_control ? submission->work_control : submission->control;
        identity_ = pending.identity;
        last_job_ = submission->job; claimed_ = true; admission_claimed_ = false; engine_claimed_ = false;
        pending.submission = std::move(submission);
        pending_.emplace(std::move(pending)); busy_ = true; cv_.notify_one(); return true;
    }
    bool admit(vision_job_io::Identity id, bool accepted) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matches(id) || !claimed_ || !prepared_ || terminal_result_ || admission_claimed_ ||
            busy_ || shutdown_ || state_ != State::ready) return false;
        auto event = inbox_->reserve(); if (!event) return false;
        Pending pending{Command::admit, id.task, std::move(event)};
        pending.identity = id; pending.accepted = accepted;
        pending_.emplace(std::move(pending)); admission_claimed_ = true;
        busy_ = true; cv_.notify_one(); return true;
    }
    // Accepted at most once per Job, after validated rows. Deadline includes time
    // queued in the mailbox. Terminal capacity was reserved by prepare().
    bool generate(vision_job_io::Identity id, std::shared_ptr<const handoff::Resources> resources,
                  Clock::duration start_budget, Clock::duration cleanup_budget) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!engine_ || !resources || !positive(start_budget) || !positive(cleanup_budget) ||
            !matches(id) || !encoded_ || terminal_result_ || engine_claimed_ || busy_ || shutdown_ ||
            service_failure_ || state_ != State::ready) return false;
        auto event = inbox_->reserve(); if (!event) return false;
        Pending pending{Command::generate, id.task, std::move(event)};
        pending.identity = id; pending.resources = std::move(resources);
        pending.start_deadline = after(Clock::now(), start_budget); pending.cleanup = cleanup_budget;
        pending_.emplace(std::move(pending)); engine_claimed_ = true;
        busy_ = true; cv_.notify_one(); return true;
    }
    bool request_stop(vision_job_io::Identity id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matches(id) || !engine_claimed_ || terminal_result_) return false;
        normal_stop_ = true; cv_.notify_one(); return true;
    }
    std::shared_ptr<const vision_job_io::Prepared> prepared() const {
        std::lock_guard<std::mutex> lock(mutex_); return prepared_;
    }
    std::shared_ptr<const vision_job_io::Encoded> encoded() const {
        std::lock_guard<std::mutex> lock(mutex_); return encoded_;
    }
    std::optional<vision_job_io::Terminal> terminal() const {
        std::lock_guard<std::mutex> lock(mutex_); return terminal_result_;
    }
    // Only a safely terminal Job may be acknowledged. External snapshots/rows may
    // remain held; their separate recipe/backing charges do not disappear here.
    bool acknowledge(vision_job_io::Identity id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!matches(id) || !terminal_result_) return false;
        terminal_result_.reset(); claimed_ = false; normal_stop_ = false; identity_ = {}; return true;
    }
    // Independent of command/inbox capacity. Mark owned request control too, so
    // blocked row initialization/validation observes service cancellation.
    void shutdown() noexcept {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Publish cancellation before shutdown becomes visible to the IO
            // owner. It must not finalize/reset request_ in between these steps.
            if (request_) request_->cancel(vision_control::Reason::cancelled);
            if (reader_request_) reader_request_->cancel(vision_control::Reason::cancelled);
            shutdown_.store(true);
        }
        cv_.notify_one();
    }
    void engine_died() noexcept {
        safety_->service_failed(); service_failure_.store(true); shutdown();
    }
    bool text_available() const noexcept {
        const auto s = state_.load();
        return engine_ && instance_ && !shutdown_ && !service_failure_ &&
            (s == State::ready || s == State::backoff || s == State::unavailable || s == State::stopped);
    }
    bool restart_admissible() const noexcept {
        const auto s = state_.load();
        return !shutdown_ && (s == State::stopped || s == State::backoff) &&
            Clock::now().time_since_epoch().count() >= retry_tick_.load();
    }
    bool failed_service() const noexcept { return service_failure_.load(); }
    State state() const noexcept { return state_.load(); }
    uint32_t attempts() const noexcept { return attempts_.load(); }
    uint64_t instance() const noexcept { return instance_.load(); }
private:
    enum class Command { start, stop, prepare, admit, generate };
    struct Pending {
        Command command; uint64_t task; vision_control::Inbox::Reservation event;
        vision_job_io::Identity identity{};
        bool accepted = false;
        std::unique_ptr<vision_job_io::Submission> submission{};
        vision_control::Inbox::Reservation terminal{};
        std::shared_ptr<const handoff::Resources> resources{};
        Clock::time_point start_deadline{};
        Clock::duration cleanup{};
    };
    Launch launch_;
    const Limits limits_;
    std::shared_ptr<Safety> safety_;
    std::shared_ptr<vision_control::Inbox> inbox_;
    std::shared_ptr<Reaper> reaper_;
    const std::shared_ptr<handoff::Engine> engine_;
    clean_process_detail::Fd diagnostic_;
    vision_control::Inbox::Reservation emergency_, fault_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Pending> pending_;
    bool busy_ = false, claimed_ = false, admission_claimed_ = false, engine_claimed_ = false;
    uint64_t last_job_ = 0;
    vision_job_io::Identity identity_{};
    std::shared_ptr<vision_control::Request> request_, reader_request_;
    std::shared_ptr<const vision_job_io::Prepared> prepared_;
    std::shared_ptr<const vision_job_io::Encoded> encoded_;
    std::optional<vision_job_io::Terminal> terminal_result_;
    bool matches(vision_job_io::Identity id) const {
        return id.task == identity_.task && id.instance == identity_.instance && id.job == identity_.job;
    }
    std::atomic<bool> shutdown_{false}, service_failure_{false}, normal_stop_{false};
    std::atomic<State> state_{State::stopped};
    std::atomic<uint32_t> attempts_{0};
    std::atomic<uint64_t> instance_{0};
    std::atomic<Clock::duration::rep> retry_tick_{0};
    std::thread owner_;
    // Everything below is accessed by the persistent owner only.
    std::unique_ptr<vision_job_io::Session> job_;
    std::unique_ptr<handoff::Handoff> engine_handoff_;
    handoff::Identity engine_identity_{};
    Clock::duration engine_cleanup_{};
    std::optional<Clock::time_point> engine_shutdown_deadline_;
    vision_job_io::Identity owner_identity_{};
    vision_control::Inbox::Reservation terminal_event_;
    SpawnedProcess child_;
    std::unique_ptr<vision_protocol::Channel> channel_;
    int control_fd_ = -1; // borrowed from channel_, accessed only by IO owner
    uint64_t next_instance_;
    bool leased_ = false, spawned_ = false, readerless_job_ = false;
    State readerless_return_ = State::backoff;
    vision_protocol::Channel no_reader_{-1};
    std::optional<Clock::time_point> episode_end_;
    Clock::time_point retry_after_{};
    std::optional<Clock::time_point> reader_proof_deadline_;
    static bool positive(Clock::duration d) {
        return d > Clock::duration::zero() && d <= Clock::time_point::max() - Clock::now();
    }
    static Clock::time_point after(Clock::time_point now, Clock::duration d) {
        return d > Clock::time_point::max() - now ? Clock::time_point::max() : now + d;
    }
    bool submit(Command command, uint64_t task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!task || busy_ || shutdown_.load()) return false;
        auto event = inbox_->reserve();
        if (!event) return false;
        pending_.emplace(Pending{command, task, std::move(event)});
        busy_ = true; cv_.notify_one(); return true;
    }
    // Runs only on the persistent IO owner, including while Session is blocked
    // in READY/PREPARE/ENCODE/RELEASE transport. No second engine poller/reaper or
    // thread is introduced. healthy() is idle-only and must never load/restart.
    bool reader_health() noexcept {
        if (job_) job_->observe_reader();
        if (shutdown_.load() || !engine_) return true;
        if (!safety_->healthy()) { engine_died(); return false; }
        if (engine_handoff_ || engine_->healthy()) return true;
        engine_died(); // revoke globally BEFORE publishing shutdown/request cancel
        return false;
    }
    static bool reader_health_probe(void * owner) noexcept {
        return static_cast<Supervisor *>(owner)->reader_health();
    }
    void pipes_close() noexcept {
        for (int * fd : {&child_.input, &child_.output}) {
            if (*fd >= 0) ::close(*fd);
            *fd = -1;
        }
    }
    bool contain(bool graceful) {
        // No reader QUIT/RELEASE/teardown or lease release while an engine can
        // consume files or own the turn. Cancellation is independent of inbox space.
        if (engine_handoff_) {
            job_->control()->cancel(vision_control::Reason::cancelled);
            // At most drain + proof + containment budgets; each Handoff phase
            // keeps its original deadline even when an explicit stop retries.
            const auto until = after(after(after(Clock::now(), engine_cleanup_), engine_cleanup_), engine_cleanup_);
            do {
                if (advance_engine()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (Clock::now() < until);
            if (engine_handoff_) { state_ = State::safety_blocked; return false; }
        }
        state_ = State::stopping;
        if (shutdown_ && engine_) {
            if (!engine_shutdown_deadline_)
                engine_shutdown_deadline_ = after(after(Clock::now(), limits_.term), limits_.kill);
            if (!engine_->shutdown(*engine_shutdown_deadline_) || !safety_->engine_stopped()) {
                state_ = State::safety_blocked; return false;
            }
        }
        if (job_) job_->fail(); // retain every artifact until certified containment below
        if (graceful && !job_ && channel_ && child_.pid > 0 && !shutdown_.load()) {
            vision_commands::Payload quit;
            std::atomic<bool> cleanup_abort{false};
            const auto until = after(Clock::now(), limits_.quit);
            if (vision_commands::build(vision_protocol::Kind::quit, launch_.config, launch_.wire_limits, {}, quit)
                == vision_commands::Result::ok) {
                const vision_protocol::Header header{vision_protocol::Kind::quit, uint32_t(quit.bytes.size()), instance_, 0, 0};
                if (channel_->send_borrowed(header, quit.bytes.data(), quit.bytes.size(), nullptr, 0,
                    launch_.wire_limits.payload_bytes, until, cleanup_abort) == vision_protocol::IoResult::ok) {
                    int status = 0;
                    (void) reap_until(child_.pid, until, status);
                }
            }
        }
        channel_.reset(); control_fd_ = -1; pipes_close();
        if (child_.pid > 0) {
            const auto term = after(Clock::now(), limits_.term);
            if (reaper_->terminate(child_.pid, term, after(term, limits_.kill)) != ReapResult::reaped || child_.pid > 0) {
                state_ = State::safety_blocked; return false;
            }
        }
        if (service_failure_.load() && !safety_->engine_stopped()) {
            state_ = State::safety_blocked; return false;
        }
        if (leased_) {
            if (!safety_->certify(instance_, spawned_, service_failure_.load())) {
                state_ = State::safety_blocked; return false;
            }
            safety_->release(instance_); leased_ = false;
        }
        fault_.reset();
        if (job_) {
            job_->reader_stopped();
            auto terminal = job_->terminal();
            if (!terminal) std::terminate();
            publish_terminal(*terminal);
        }
        readerless_job_ = false;
        return true;
    }
    void failed() {
        if (!contain(false)) {
            if (launch_.initialize_engine && !reader_proof_deadline_) reader_proof_deadline_ = after(Clock::now(),limits_.quit);
            return;
        }
        reader_proof_deadline_.reset();
        retry_after_ = after(Clock::now(), limits_.backoff);
        retry_tick_ = retry_after_.time_since_epoch().count();
        state_ = attempts_ >= limits_.attempts || !next_instance_ ||
            (episode_end_ && Clock::now() >= *episode_end_) ? State::unavailable : State::backoff;
    }
    vision_control::Event::Kind begin() {
        using E = vision_control::Event::Kind;
        const auto now = Clock::now();
        if ((state_ != State::stopped && state_ != State::backoff) || leased_ || child_.pid > 0)
            return E::unavailable;
        if (!next_instance_ || attempts_ >= limits_.attempts || (episode_end_ && now >= *episode_end_)) {
            state_ = State::unavailable; return E::unavailable;
        }
        if (now < retry_after_) return E::unavailable;
        auto fault = inbox_->reserve();
        if (!fault || !safety_->acquire(next_instance_)) return E::unavailable;
        fault_ = std::move(fault);
        leased_ = true; spawned_ = false;
        instance_ = next_instance_;
        next_instance_ = next_instance_ == UINT64_MAX ? 0 : next_instance_ + 1;
        if (!episode_end_) episode_end_ = after(now, limits_.restart_window);
        ++attempts_; state_ = State::starting;
        const auto until = after(now, limits_.ready);
        try {
            clean_process_detail::Fd parent, child;
            int sockets[2];
            if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) { failed(); return E::failed; }
            parent.value = sockets[0]; child.value = sockets[1];
            // Only a parent-generated numeric identity is appended, never request content.
            auto argv = launch_.argv; argv.push_back(std::to_string(instance_.load()));
            std::string error;
            if (!protected_spawn(launch_.guard, launch_.executable, argv, launch_.environment,
                                 diagnostic_.value, child_, error, child.value)) { failed(); return E::failed; }
            spawned_ = true; safety_->reader_spawned(child_.pid);
            pipes_close(); // production reader must use only FD 3, not stdin/stdout
            channel_ = std::make_unique<vision_protocol::Channel>(parent.value,
                vision_protocol::HealthProbe{&Supervisor::reader_health_probe, this});
            control_fd_ = parent.release();
            vision_protocol::Frame ready;
            auto rc = channel_->receive(ready, {vision_protocol::Kind::ready, instance_, 0, 0, 0},
                                       launch_.wire_limits.payload_bytes, until, shutdown_);
            vision_payload::View view;
            if (rc != vision_protocol::IoResult::ok || ready.header.kind != vision_protocol::Kind::ready ||
                !vision_payload::validate(vision_protocol::Kind::ready,
                    {reinterpret_cast<const char *>(ready.payload.data()), ready.payload.size()}, ready.fd_count,
                    launch_.config, launch_.wire_limits, {}, view) || shutdown_.load()) {
                failed(); return E::failed;
            }
            if (launch_.initialize_engine && engine_) {
                const handoff::Identity boot{UINT64_MAX, instance_, UINT64_MAX, 1};
                if (!safety_->to_engine(boot)) { shutdown(); failed(); return E::failed; }
                const bool loaded = engine_->initialize(until, shutdown_);
                if (!loaded) {
                    safety_->service_failed(); service_failure_ = true;
                    if (!engine_->shutdown(after(after(Clock::now(), limits_.term), limits_.kill))) std::terminate();
                }
                // Initialization is a may-have-allocated phase just like GEN.
                // Never release/reacquire the shared turn or publish READY early.
                const auto proof = after(Clock::now(), limits_.quit);
                while (!safety_->to_reader(boot)) {
                    if (Clock::now() >= proof) std::terminate();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (!loaded || shutdown_) { shutdown(); failed(); return E::failed; }
            }
            state_ = State::ready;
            return E::ready;
        } catch (...) {
            // Allocation/launch setup failure is not permission to abandon a live child.
            failed(); return E::failed;
        }
    }
    void publish_terminal(vision_job::Outcome outcome) {
        // Owner has certified cleanup, or this submission never reached the child.
        if (outcome == vision_job::Outcome::input_error && (shutdown_ || service_failure_))
            outcome = vision_job::Outcome::failure;
        const uint32_t error_class = job_ ? job_->error_class() : 0;
        const bool timed_out = job_ && job_->timed_out();
        const auto input_failure = job_ && outcome == vision_job::Outcome::input_error && !error_class && !timed_out ?
            job_->input_failure() : std::optional<vision_payload::InputFailure>{};
        if (outcome == vision_job::Outcome::input_error && !input_failure) std::terminate();
        job_.reset();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            prepared_.reset(); encoded_.reset(); request_.reset(); reader_request_.reset();
            terminal_result_ = vision_job_io::Terminal{owner_identity_, outcome, error_class, timed_out, input_failure};
        }
        terminal_event_.publish({owner_identity_.task, owner_identity_.instance, owner_identity_.job, 0,
                                 vision_control::Event::Kind::job_terminal});
    }
    void reject_job(Pending & work) {
        if (work.command != Command::prepare) return;
        owner_identity_ = work.identity; terminal_event_ = std::move(work.terminal);
        const bool cancelled = work.submission->control->observe();
        work.submission.reset();
        publish_terminal(cancelled ? vision_job::Outcome::cancelled : vision_job::Outcome::failure);
    }
    vision_control::Event::Kind job_command(Pending & work) {
        using E = vision_control::Event::Kind;
        const bool text_without_reader = work.command == Command::prepare && work.submission &&
            (work.submission->text_only || work.submission->complete_only) &&
            !channel_ && text_available();
        if ((state_ != State::ready || !channel_) && !text_without_reader && !readerless_job_) { reject_job(work); return E::unavailable; }
        auto & channel = channel_ ? *channel_ : no_reader_;
        try {
            if (work.command == Command::prepare) {
                if (text_without_reader) {
                    if (leased_ || !safety_->acquire(instance_)) { reject_job(work); return E::unavailable; }
                    leased_ = true; spawned_ = false; readerless_job_ = true; readerless_return_ = state_;
                }
                owner_identity_ = work.identity; terminal_event_ = std::move(work.terminal);
                job_ = vision_job_io::Session::create(work.identity, launch_.config, launch_.wire_limits, std::move(work.submission));
                if (!job_) {
                    finish_text_lease(); publish_terminal(vision_job::Outcome::refused);
                    if (readerless_job_) state_ = readerless_return_;
                    readerless_job_ = false;
                    return E::failed;
                }
                state_ = State::preparing;
                if (!job_->prepare(channel, shutdown_)) { failed(); return E::failed; }
                if (auto terminal = job_->terminal()) {
                    // Typed PREPARE rejection, after cooperative release of every
                    // earlier handle. The reader/lease remain alive and reusable.
                    publish_terminal(*terminal); state_ = State::ready; return E::completed;
                }
                { std::lock_guard<std::mutex> lock(mutex_); prepared_ = job_->prepared(); }
                state_ = State::ready; return E::prepared;
            }
            if (!job_) return E::unavailable;
            if (work.command == Command::generate) {
                auto resources = std::make_shared<EngineResources>();
                resources->encoded = job_->encoded(); resources->request = std::move(work.resources);
                // Job IDs increase for this owner; only one handoff per Job, so
                // sequence 1 is unique within the full task/instance/job identity.
                engine_identity_ = {work.identity.task, work.identity.instance, work.identity.job, 1};
                auto operation = std::make_unique<handoff::Handoff>(engine_identity_, resources, job_->control(),
                    engine_, safety_, work.start_deadline, work.cleanup);
                if (!job_->engine_begin()) std::terminate();
                engine_cleanup_ = work.cleanup; engine_handoff_ = std::move(operation);
                state_ = State::engine_active;
                if (!engine_handoff_->begin(engine_identity_)) std::terminate();
                return E::engine_submitted;
            }
            state_ = work.accepted ? State::encoding : State::finalizing;
            if (!job_->admit(work.accepted, channel, shutdown_)) { failed(); return E::failed; }
            if (auto terminal = job_->terminal()) {
                finish_text_lease(); publish_terminal(*terminal);
                state_ = readerless_job_ ? readerless_return_ : State::ready; readerless_job_ = false;
            } else { std::lock_guard<std::mutex> lock(mutex_); encoded_ = job_->encoded(); state_ = State::ready; }
            return work.accepted ? E::encoded : E::completed;
        } catch (...) {
            if (job_) failed(); // retain active native handles through certified cleanup
            else { finish_text_lease(); publish_terminal(vision_job::Outcome::failure); readerless_job_ = false; } // no handoff occurred
            return E::failed;
        }
    }
    void finish_text_lease() {
        if (!readerless_job_ || !leased_) return;
        if (!safety_->certify(instance_, false, service_failure_)) std::terminate();
        safety_->release(instance_); leased_ = false;
    }
    // Returns true only after certified handback, never on STOP/EOF alone.
    bool advance_engine() {
        if (!engine_handoff_) return true;
        if (normal_stop_) (void) engine_handoff_->request_stop(engine_identity_);
        engine_handoff_->advance(engine_identity_);
        if (engine_handoff_->required_containment() && !service_failure_.exchange(true)) {
            safety_->service_failed(); // engine containment forbids recovery/readmission
            shutdown(); // failed containment must not turn into an unbounded idle retry loop
        }
        auto result = engine_handoff_->take_result(engine_identity_);
        if (!result) {
            if (engine_handoff_->state() == handoff::State::proof_blocked ||
                engine_handoff_->state() == handoff::State::containing) state_ = State::safety_blocked;
            return false;
        }
        job_->engine_finish(engine_handoff_->possibly_started(), *result == handoff::Outcome::success,
                            *result == handoff::Outcome::cancelled);
        engine_handoff_.reset(); state_ = State::finalizing;
        if (service_failure_) shutdown();
        return true;
    }
    void run() {
        using E = vision_control::Event::Kind;
        for (;;) {
            std::optional<Pending> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(10), [this] { return pending_.has_value() || shutdown_.load(); });
                work = std::move(pending_); pending_.reset();
            }
            (void) reader_health();
            if (!shutdown_ && state_ == State::safety_blocked && !engine_handoff_ && reader_proof_deadline_) {
                if (Clock::now() >= *reader_proof_deadline_) engine_died();
                else failed(); // same absolute proof budget; never advertise or admit while uncertain
            }
            if (!shutdown_.load() && engine_handoff_ && advance_engine()) {
                if (!shutdown_.load()) {
                    if (!job_->release(channel_ ? *channel_ : no_reader_, shutdown_)) failed();
                    else {
                        auto terminal = job_->terminal(); if (!terminal) std::terminate();
                        finish_text_lease(); publish_terminal(*terminal);
                        state_ = readerless_job_ ? readerless_return_ : State::ready; readerless_job_ = false;
                    }
                }
            }
            if (!shutdown_.load() && state_ == State::ready && job_ && job_->cancelled()) {
                if (job_->encoded() && job_->abandon(channel_ ? *channel_ : no_reader_,shutdown_)) {
                    auto terminal = job_->terminal(); if (!terminal) std::terminate();
                    finish_text_lease(); publish_terminal(*terminal);
                    state_ = readerless_job_ ? readerless_return_ : State::ready; readerless_job_ = false;
                } else failed();
            }
            if (!shutdown_.load() && child_.pid > 0 && (state_ == State::ready || engine_handoff_)) {
                int status = 0;
                const auto rc = reap_until(child_.pid, Clock::now(), status);
                pollfd socket{control_fd_, POLLIN, 0};
                const int readable = ::poll(&socket, 1, 0);
                // There are no outstanding operations while lifecycle-ready:
                // unsolicited bytes, EOF or control failure all require teardown.
                if (rc != ReapResult::timeout || readable > 0 || (readable < 0 && errno != EINTR)) {
                    // Save the reservation before containment returns other capacity.
                    auto fault = std::move(fault_);
                    failed();
                    fault.publish({0, instance_, 0, 0, E::failed});
                }
            }
            if (work) {
                E kind = E::failed;
                if (!shutdown_.load()) {
                    if (work->command == Command::start) kind = begin();
                    else if (work->command == Command::prepare || work->command == Command::admit ||
                             work->command == Command::generate) kind = job_command(*work);
                    else if (contain(true)) {
                        // An explicit stop/late certification must not bypass
                        // failure backoff. Pace every completed reader teardown.
                        if (attempts_) { retry_after_ = after(Clock::now(), limits_.backoff); retry_tick_ = retry_after_.time_since_epoch().count(); }
                        state_ = State::stopped; kind = E::stopped;
                    }
                } else reject_job(*work);
                {
                    std::lock_guard<std::mutex> lock(mutex_); busy_ = false;
                }
                work->event.publish({work->task, work->identity.job ? work->identity.instance : instance_.load(),
                                      work->identity.job, 0, kind});
            }
            if (shutdown_.load()) {
                // A submitter may have passed the admission check before shutdown
                // and installed work after this iteration extracted its command.
                // Synchronize with it, then reject every accepted pending command.
                std::optional<Pending> abandoned;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    abandoned = std::move(pending_); pending_.reset(); busy_ = false;
                }
                if (abandoned) {
                    reject_job(*abandoned);
                    abandoned->event.publish({abandoned->task, abandoned->identity.job ? abandoned->identity.instance : instance_.load(),
                                              abandoned->identity.job, 0, E::failed});
                }
                // Fail the whole service closed if bounded teardown/certification
                // fails. Never detach a thread, abandon a PID, or silently free a turn.
                if (!contain(false)) std::terminate();
                state_ = State::exited;
                emergency_.publish({0, instance_, 0, 0, E::stopped});
                return;
            }
        }
    }
};
} // namespace strata_frontend::vision_supervisor
