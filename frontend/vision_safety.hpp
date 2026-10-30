// Single-device candidate authority. CPU process proof is never GPU proof.
// NVML queries address ONLY the approved UUID; no clock/reset/allocation changes.
// Requires exclusive operational ownership of that device (checked empty before
// initial load). NVML reports HOST PIDs, so never compare them with container PIDs:
// bind the device context set at the serialized reader/engine allocation boundaries.
#pragma once
#include "vision_engine_adapter.hpp"
#include <dlfcn.h>

namespace strata_frontend::vision_safety {
class Exclusive final : public vision_supervisor::Safety {
    using Device = void *;
    using EventSet = void *;
    struct Process { unsigned pid; unsigned long long bytes; unsigned gpu_instance, compute_instance; };
    struct Event { Device device; unsigned long long type, data; unsigned gpu_instance, compute_instance; };
    struct Registry { Process list[4]{}; unsigned count = 4; };
    std::shared_ptr<vision_engine_adapter::Adapter> engine_;
    const bool gpu_;
    std::atomic<bool> revoked_{false};
    bool leased_ = false, engine_turn_ = false, reader_live_ = false, driver_fault_ = false;
    unsigned reader_host_ = 0, engine_host_ = 0;
    void * library_ = nullptr;
    Device device_ = nullptr;
    EventSet events_ = nullptr;
    int (*shutdown_)() = nullptr;
    int (*processes_)(Device,unsigned *,Process *) = nullptr;
    int (*event_wait_)(EventSet,Event *,unsigned) = nullptr;
    int (*event_free_)(EventSet) = nullptr;
    template<class T> T symbol(const char * name) {
        auto p = reinterpret_cast<T>(::dlsym(library_, name));
        if (!p) throw std::runtime_error("native GPU safety interface unavailable");
        return p;
    }
    bool driver_ok() noexcept {
        if (!gpu_) return true;
        if (driver_fault_) return false;
        Event event{};
        if (event_wait_(events_, &event, 0) == 10) return true; // NVML_ERROR_TIMEOUT (6 is NOT_FOUND): no XID
        driver_fault_ = true; revoked_ = true; return false; // sticky XID/lost-driver/observation failure
    }
    bool registry(Registry & r) noexcept {
        if (!driver_ok() || processes_(device_, &r.count, r.list) != 0 || r.count > 2) return false;
        for (unsigned i = 0; i < r.count; ++i) {
            if (!r.list[i].pid) return false;
            for (unsigned j = 0; j < i; ++j) if (r.list[i].pid == r.list[j].pid) return false;
        }
        return true;
    }
    static bool only(const Registry & r, unsigned a, unsigned b = 0) noexcept {
        for (unsigned i = 0; i < r.count; ++i) if (r.list[i].pid != a && r.list[i].pid != b) return false;
        return true;
    }
    static bool contains(const Registry & r, unsigned pid) noexcept {
        for (unsigned i = 0; i < r.count; ++i) if (r.list[i].pid == pid) return true;
        return false;
    }
    void close() noexcept {
        if (events_ && event_free_) event_free_(events_);
        if (shutdown_) shutdown_();
        if (library_) ::dlclose(library_);
        events_ = nullptr; library_ = nullptr; shutdown_ = nullptr;
    }
public:
    Exclusive(std::shared_ptr<vision_engine_adapter::Adapter> engine, bool gpu) : engine_(std::move(engine)), gpu_(gpu) {
        if (!engine_) throw std::invalid_argument("missing native engine authority");
        if (!gpu_) return;
        try {
            library_ = ::dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!library_) throw std::runtime_error("native GPU safety interface unavailable");
            auto init = symbol<int(*)()>("nvmlInit_v2");
            if (init() != 0) throw std::runtime_error("native GPU safety initialization failed");
            shutdown_ = symbol<int(*)()>("nvmlShutdown");
            auto handle = symbol<int(*)(const char *,Device *)>("nvmlDeviceGetHandleByUUID");
            if (handle("GPU-d14daf49-45c8-58c5-cf61-5a40342672af", &device_) != 0)
                throw std::runtime_error("approved native GPU unavailable");
            processes_ = symbol<int(*)(Device,unsigned *,Process *)>("nvmlDeviceGetComputeRunningProcesses_v2");
            auto create = symbol<int(*)(EventSet *)>("nvmlEventSetCreate");
            auto subscribe = symbol<int(*)(Device,unsigned long long,EventSet)>("nvmlDeviceRegisterEvents");
            event_wait_ = symbol<int(*)(EventSet,Event *,unsigned)>("nvmlEventSetWait_v2");
            event_free_ = symbol<int(*)(EventSet)>("nvmlEventSetFree");
            if (create(&events_) != 0 || subscribe(device_, 8, events_) != 0)
                throw std::runtime_error("native GPU fault observation unavailable");
        } catch (...) { close(); throw; }
    }
    ~Exclusive() { close(); }
    bool acquire(uint64_t) noexcept override {
        if (leased_ || revoked_ || (!engine_->idle() && !engine_->stopped())) return false;
        if (gpu_) {
            Registry r;
            // Initially both IDs are zero: an occupied device is refused, never
            // borrowed or disrupted. After safe reader loss only the engine remains.
            if (!registry(r) || !only(r,engine_host_)) return false;
        }
        leased_ = true; return true;
    }
    void reader_spawned(pid_t) noexcept override { reader_live_ = true; reader_host_ = 0; }
    bool healthy() noexcept override { return !revoked_ && driver_ok(); }
    void service_failed() noexcept override { revoked_ = true; }
    bool engine_stopped() noexcept override {
        if (!engine_->stopped()) return false;
        if (!gpu_) return true;
        Registry r;
        if (!registry(r) || !only(r,reader_live_ ? reader_host_ : 0) || (engine_host_ && contains(r,engine_host_))) return false;
        engine_host_ = 0; return true;
    }
    bool certify(uint64_t, bool spawned, bool service) noexcept override {
        if (engine_turn_ || (service && !engine_stopped())) return false;
        if (gpu_ && spawned) {
            Registry r;
            // Called ONLY after this owner's waitpid. Any leftover reader or
            // unknown context (including a pre-READY failure) denies certification.
            if (!registry(r) || !only(r,engine_host_) || (reader_host_ && contains(r,reader_host_))) return false;
        }
        if (spawned) { reader_live_ = false; reader_host_ = 0; }
        return true;
    }
    void release(uint64_t) noexcept override { if (!leased_ || engine_turn_) std::terminate(); leased_ = false; }
    bool to_engine(vision_engine_handoff::Identity) noexcept override {
        if (!leased_ || engine_turn_ || revoked_ || !driver_ok()) return false;
        if (gpu_) {
            Registry r; if (!registry(r)) return false;
            if (reader_live_ && !reader_host_) {
                for (unsigned i = 0; i < r.count; ++i) if (r.list[i].pid != engine_host_) {
                    if (reader_host_) return false;
                    reader_host_ = r.list[i].pid;
                }
                if (!reader_host_) return false; // GPU reader may not silently fall back to CPU
            }
            if (!only(r,reader_host_,engine_host_) || (reader_live_ && !contains(r,reader_host_))) return false;
        }
        engine_turn_ = true; return true;
    }
    bool to_reader(vision_engine_handoff::Identity) noexcept override {
        if (!engine_turn_ || (!engine_->idle() && !engine_->stopped())) return false;
        if (gpu_) {
            Registry r; if (!registry(r)) return false;
            if (engine_->stopped()) {
                if (!only(r,reader_live_ ? reader_host_ : 0) || (engine_host_ && contains(r,engine_host_))) return false;
                engine_host_ = 0;
            } else {
                unsigned observed = 0;
                for (unsigned i = 0; i < r.count; ++i) if (r.list[i].pid != reader_host_) {
                    if (observed) return false;
                    observed = r.list[i].pid;
                }
                if (engine_host_ && observed && engine_host_ != observed) return false;
                engine_host_ = observed; // zero is legitimate for the CPU protocol probe
                if (!only(r,reader_live_ ? reader_host_ : 0,engine_host_)) return false;
            }
        }
        // Revocation stays sticky: safe handback permits teardown, not readmission.
        engine_turn_ = false; return true;
    }
};
} // namespace strata_frontend::vision_safety
