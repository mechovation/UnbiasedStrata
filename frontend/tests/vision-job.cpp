#include "../vision_job.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>

namespace vj = strata_frontend::vision_job;
namespace va = strata_frontend::vision_artifact;
namespace vp = strata_frontend::vision_payload;
namespace wire = strata_frontend::vision_protocol;
using namespace std::chrono_literals;
static std::atomic<bool> cancel{false};
static vp::Config config{};
static vp::Limits caps{16384, 4, 16, 1024, 65536, {4096, 64, 4096, 16384, 32, 16 * 1024 * 1024}};
static std::string metadata;
static auto deadline() { return wire::Clock::now() + 2s; }
static void u32(std::string & s, uint32_t n) { for (unsigned i = 0; i < 4; ++i) s += char(n >> (8 * i)); }
static void u64(std::string & s, uint64_t n) { for (unsigned i = 0; i < 8; ++i) s += char(n >> (8 * i)); }
static va::Tag tag(uint32_t occurrence) { return {7, 11, occurrence, {}}; }
static std::unique_ptr<vj::Job> job(va::Budget & budget, unsigned occurrences = 2, vj::Limits limits = {8192, 4096, 4096}, bool tool = false) {
    std::vector<vj::Input> inputs;
    for (unsigned i = 0; i < occurrences; ++i) {
        va::Artifact::Ptr source;
        auto source_tag = tag(i); source_tag.instance = 0;
        assert(va::Artifact::source(budget, source_tag, "source", 1024, deadline(), cancel, source) == va::Result::ok);
        inputs.push_back({std::move(source), "pr4-fixture", tool});
    }
    auto result = vj::Job::create(config, caps, limits, 7, 11, std::move(inputs)); assert(result);
    return result;
}
static wire::Frame reply(vj::Ticket t, wire::Kind kind, uint64_t handle = 0) {
    std::string p(32, '\0');
    if (kind == wire::Kind::error) u32(p, 3);
    else if (kind == wire::Kind::input_rejected) {
        u32(p,t.occurrence); p.append(32,'\0'); u64(p,6); u32(p,uint32_t(vp::InputError::webp_header));
    } else {
        u32(p, t.occurrence); p.append(32, '\0'); u64(p, handle ? handle : t.handle);
        if (kind == wire::Kind::prepared) {
            u32(p, 7); u32(p, 7); u32(p, config.width);
            u32(p, 1); u32(p, 0); u32(p, 1); u32(p, 42); u32(p, metadata.size()); p += metadata;
        } else if (kind == wire::Kind::encoded) {
            u32(p, 7); u32(p, 7); u32(p, config.width); u64(p, 7 * 7 * config.width * 4);
        }
    }
    wire::Frame f; f.header = {kind, uint32_t(p.size()), t.instance, t.job, t.sequence};
    f.payload.assign(p.begin(), p.end()); return f;
}
static void prepare_all(vj::Job & j) {
    while (j.phase() == vj::Phase::preparing) {
        auto ticket = j.prepare(); assert(ticket && !j.prepare());
        auto f = reply(*ticket, wire::Kind::prepared, 100 + ticket->occurrence);
        assert(j.accept(f, deadline(), cancel) == vj::Reply::accepted);
        f.payload.assign(f.payload.size(), 255); // accepted recipe must own its data, not frame views
        assert(j.recipe(ticket->occurrence)->metadata == metadata);
    }
}
static va::Artifact::Ptr output(va::Budget & budget, uint32_t occurrence, bool fill = true) {
    va::Artifact::Ptr out;
    assert(va::Artifact::rows(budget, tag(occurrence), 7, 7, config.width, caps.row_bytes, deadline(), cancel, out) == va::Result::ok);
    if (fill) {
        std::vector<float> data(7 * 7 * config.width, 1.0f);
        assert(::pwrite(out->fd(), data.data(), data.size() * 4, 0) == ssize_t(data.size() * 4));
    }
    return out;
}
static void encode_all(vj::Job & j, va::Budget & budget, unsigned count = 2) {
    for (unsigned i = 0; i < count; ++i) {
        auto ticket = j.encode(output(budget, i)); assert(ticket);
        assert(!j.rows(i)); // unvalidated artifacts never become consumable rows
        auto f = reply(*ticket, wire::Kind::encoded);
        assert(j.accept(f, deadline(), cancel) == vj::Reply::accepted && j.rows(i));
    }
}
static void release_all(vj::Job & j) {
    while (auto ticket = j.release()) {
        auto f = reply(*ticket, wire::Kind::released);
        assert(j.accept(f, deadline(), cancel) == vj::Reply::accepted);
        assert(j.accept(f, deadline(), cancel) == vj::Reply::stale);
    }
    assert(j.phase() == vj::Phase::done);
}
int main() {
    config.width = 4; config.pixel_stride = 32; config.vocabulary_size = 1024;
    std::ifstream file("frontend/tests/data/mtmd-metadata/qwen-still-0.bin", std::ios::binary); assert(file);
    metadata.assign(std::istreambuf_iterator<char>(file), {});
    va::Budget budget({65536, 16});
    // Explicit text jobs keep admission/start/drain/terminal ownership, but issue
    // no reader tickets. An accidentally empty vision submission stays invalid.
    assert(!vj::Job::create(config, caps, {8192,4096,4096}, 7, 11, {}));
    for (int result = 0; result < 4; ++result) {
        auto j = vj::Job::create(config, caps, {8192,4096,4096}, 7, 11, {}, true); assert(j);
        assert(j->phase() == vj::Phase::admitting && !j->prepare() && !j->engine_start_requested());
        assert(j->admit(result != 0));
        if (result != 0) {
            assert(j->phase() == vj::Phase::ready_for_engine && j->engine_start_requested());
            if (result == 1) assert(j->engine_not_started());
            else {
                assert(j->engine_started());
                if (result == 3) j->cancel();
                assert(!j->take_terminal() && !j->release());
                assert(j->engine_terminal(true));
            }
        }
        assert(!j->release() && j->phase() == vj::Phase::done);
        assert(j->take_terminal() == (result == 0 ? vj::Outcome::refused : result == 1 ? vj::Outcome::failure :
            result == 2 ? vj::Outcome::success : vj::Outcome::cancelled));
        assert(!j->take_terminal() && budget.usage().bytes == 0);
    }
    {
        auto j = job(budget);
        assert(!j->engine_started() && !j->admit(true) && !j->encode(output(budget, 0)));
        prepare_all(*j); assert(j->phase() == vj::Phase::admitting);
        assert(!j->encode(output(budget, 0)) && j->admit(true));
        encode_all(*j, budget); auto held = j->rows(0);
        assert(j->phase() == vj::Phase::ready_for_engine && j->engine_started());
        assert(!j->prepare() && !j->release() && !j->reader_failure() && !j->reader_stopped(true));
        assert(!j->take_terminal());
        assert(j->engine_terminal(true)); release_all(*j);
        assert(j->take_terminal() == vj::Outcome::success && !j->take_terminal());
        assert(budget.usage().artifacts == 1 && budget.usage().bytes == held->bytes());
        held.reset(); assert(budget.usage().bytes == 0);
    }
    // Pre-encode context refusal releases prepared handles without producing any ENCODE ticket.
    {
        auto j = job(budget); prepare_all(*j); assert(j->admit(false));
        assert(!j->encode(output(budget, 0))); release_all(*j);
        assert(j->take_terminal() == vj::Outcome::refused);
    }
    // Equal byte sizes are not enough: reject a differently shaped/cross-job output before ENCODE.
    {
        auto j = job(budget, 1); prepare_all(*j); assert(j->admit(true));
        va::Artifact::Ptr wrong;
        assert(va::Artifact::rows(budget, tag(0), 1, 49, config.width, caps.row_bytes, deadline(), cancel, wrong) == va::Result::ok);
        assert(!j->encode(std::move(wrong)));
        auto wrong_tag = tag(0); ++wrong_tag.job;
        assert(va::Artifact::rows(budget, wrong_tag, 7, 7, config.width, caps.row_bytes, deadline(), cancel, wrong) == va::Result::ok);
        assert(!j->encode(std::move(wrong)));
        auto t = j->encode(output(budget, 0)); assert(t);
        auto bad_handle = reply(*t, wire::Kind::encoded, t->handle + 1);
        assert(j->accept(bad_handle, deadline(), cancel) == vj::Reply::invalid);
        assert(!j->take_terminal() && j->reader_stopped(true));
    }
    // Cancellation in every native/engine phase; no terminal or accounting release before safe boundary.
    for (int phase = 0; phase < 7; ++phase) {
        auto j = job(budget);
        if (phase == 0) assert(j->prepare());
        if (phase >= 1) prepare_all(*j);
        if (phase >= 2) assert(j->admit(true));
        if (phase == 2) assert(j->encode(output(budget, 0)));
        if (phase >= 3) encode_all(*j, budget);
        if (phase >= 4) assert(j->engine_started());
        if (phase >= 5) assert(j->engine_terminal(true));
        std::optional<vj::Ticket> cleanup;
        if (phase == 6) { cleanup = j->release(); assert(cleanup); }
        auto before = budget.usage();
        cancel = true; // the real request cancellation flag stays sticky through RELEASE
        j->cancel(); j->cancel();
        assert(!j->prepare() && !j->take_terminal());
        assert(budget.usage().bytes == before.bytes);
        if (phase == 4) {
            assert(j->phase() == vj::Phase::draining && !j->reader_stopped(true));
            assert(!j->release() && !j->engine_started());
            assert(j->engine_terminal(true)); release_all(*j);
        } else if (phase >= 5) {
            assert(j->phase() == vj::Phase::finalizing);
            if (cleanup) {
                auto f = reply(*cleanup, wire::Kind::released);
                assert(j->accept(f, deadline(), cancel) == vj::Reply::accepted);
            }
            release_all(*j);
        } else {
            assert(j->phase() == vj::Phase::failed && !j->reader_stopped(false));
            assert(j->reader_stopped(true));
        }
        assert(j->take_terminal() == vj::Outcome::cancelled && !j->take_terminal());
        assert(budget.usage().bytes == 0);
        cancel = false;
    }
    // Engine terminal errors are failures, never successful completion after RELEASE.
    {
        auto j = job(budget, 1); prepare_all(*j); assert(j->admit(true)); encode_all(*j, budget, 1);
        assert(j->engine_started() && j->engine_terminal(false)); release_all(*j);
        assert(j->take_terminal() == vj::Outcome::failure);
    }
    // Stale event identities cannot consume the pending operation or its accounting.
    {
        auto j = job(budget); auto ticket = j->prepare(); assert(ticket);
        auto f = reply(*ticket, wire::Kind::prepared, 100);
        for (auto field : {&f.header.instance, &f.header.job, &f.header.sequence}) {
            ++*field; assert(j->accept(f, deadline(), cancel) == vj::Reply::stale && j->pending()); --*field;
        }
        assert(j->accept(f, deadline(), cancel) == vj::Reply::accepted);
        auto next = j->prepare(); assert(next);
        auto duplicate_handle = reply(*next, wire::Kind::prepared, 100);
        assert(j->accept(duplicate_handle, deadline(), cancel) == vj::Reply::invalid);
        assert(j->reader_stopped(true)); assert(j->take_terminal() == vj::Outcome::failure);
    }
    // Aggregate recipe/cell/position caps, not just per-frame checks.
    for (vj::Limits limits : {vj::Limits{100, 4096, 4096}, {8192, 49, 4096}, {8192, 4096, 9}}) {
        auto j = job(budget, 2, limits);
        auto first = j->prepare(); auto f = reply(*first, wire::Kind::prepared, 100);
        assert(j->accept(f, deadline(), cancel) == vj::Reply::accepted);
        auto second = j->prepare(); auto overflow = reply(*second, wire::Kind::prepared, 101);
        assert(j->accept(overflow, deadline(), cancel) == vj::Reply::resource && !j->take_terminal());
        assert(j->reader_stopped(true));
    }
    // Native ERROR, deadline, cancellation flag and unfinished rows all require safe teardown.
    for (int fault = 0; fault < 4; ++fault) {
        auto j = job(budget, 1);
        if (fault == 3) {
            prepare_all(*j); assert(j->admit(true));
            auto ticket = j->encode(output(budget, 0, false)); assert(ticket);
            auto f = reply(*ticket, wire::Kind::encoded);
            assert(j->accept(f, deadline(), cancel) == vj::Reply::artifact_error && !j->rows(0));
        } else {
            auto ticket = j->prepare(); assert(ticket);
            auto f = reply(*ticket, fault == 0 ? wire::Kind::error : wire::Kind::prepared, 100);
            if (fault == 2) cancel = true;
            auto result = j->accept(f, fault == 1 ? wire::Clock::now() : deadline(), cancel);
            assert(result == (fault == 0 ? vj::Reply::remote_error : fault == 1 ? vj::Reply::invalid : vj::Reply::cancelled));
            cancel = false; assert(!j->prepare());
        }
        assert(!j->take_terminal() && !j->reader_stopped(false) && j->reader_stopped(true));
    }
    // Typed rejection stops PREPARE without a handle for the bad occurrence.
    // Earlier handles must drain before a once-only typed terminal is available.
    for (unsigned earlier : {0u,1u}) {
        auto j = job(budget);
        if (earlier) {
            auto t = j->prepare(); auto f = reply(*t,wire::Kind::prepared,100);
            assert(j->accept(f,deadline(),cancel) == vj::Reply::accepted);
        }
        auto t = j->prepare(); auto f = reply(*t,wire::Kind::input_rejected);
        assert(j->accept(f,deadline(),cancel) == vj::Reply::input_error && !j->pending());
        assert(j->phase() == vj::Phase::finalizing && !j->prepare() && !j->admit(true) && !j->engine_start_requested());
        assert(j->input_failure()->occurrence == earlier && j->input_failure()->reason == vp::InputError::webp_header);
        assert(!j->recipe(earlier) && !j->take_terminal() && budget.usage().bytes);
        release_all(*j);
        assert(j->take_terminal() == vj::Outcome::input_error && !j->take_terminal());
        assert(budget.usage().bytes == 0);
    }
    // Generic errors, malformed typed replies, deadline/cancel and cleanup faults
    // never become input-error terminals, even after an earlier typed rejection.
    for (int fault = 0; fault < 7; ++fault) {
        auto j = job(budget);
        auto t = j->prepare(); auto good = reply(*t,wire::Kind::prepared,100);
        assert(j->accept(good,deadline(),cancel) == vj::Reply::accepted);
        t = j->prepare(); auto f = reply(*t,wire::Kind::input_rejected);
        if (fault == 0) f.payload.back() = 1; // unknown reason (high byte)
        if (fault == 1) f.payload[32] = 0; // wrong occurrence
        if (fault == 2) cancel = true;
        const auto result = j->accept(f,fault == 3 ? wire::Clock::now() : deadline(),cancel);
        cancel = false;
        if (fault < 4) assert(result == (fault == 2 ? vj::Reply::cancelled : vj::Reply::invalid));
        else {
            assert(result == vj::Reply::input_error);
            auto release = j->release(); assert(release && release->occurrence == 0);
            auto cleanup = reply(*release,fault == 4 ? wire::Kind::error : wire::Kind::released);
            if (fault == 6) cancel = true;
            const auto rc = j->accept(cleanup,fault == 5 ? wire::Clock::now() : deadline(),cancel);
            cancel = false;
            if (fault == 6) {
                assert(rc == vj::Reply::accepted); release_all(*j);
                assert(j->take_terminal() == vj::Outcome::cancelled); continue;
            }
            assert(rc == (fault == 4 ? vj::Reply::remote_error : vj::Reply::invalid));
        }
        assert(!j->take_terminal() && j->reader_stopped(true));
        assert(j->take_terminal() != vj::Outcome::input_error && budget.usage().bytes == 0);
    }
    // Role-authorized typed omissions keep the SAME job, skip only rejected
    // occurrences, and retain handles/rows for surviving inputs in original order.
    for (unsigned mask : {1u,2u,3u}) {
        auto j = job(budget,2,{8192,4096,4096},true);
        for (unsigned i = 0; i < 2; ++i) {
            auto t = j->prepare(); assert(t && t->occurrence == i);
            auto f = reply(*t,(mask & (1u<<i)) ? wire::Kind::input_rejected : wire::Kind::prepared,100+i);
            assert(j->accept(f,deadline(),cancel) == vj::Reply::accepted && !j->input_failure());
            assert(j->omitted(i) == bool(mask & (1u<<i)));
        }
        assert(j->admit(true));
        for (unsigned i = 0; i < 2; ++i) if (!(mask & (1u<<i))) {
            auto t = j->encode(output(budget,i)); assert(t && t->occurrence == i);
            auto f = reply(*t,wire::Kind::encoded); assert(j->accept(f,deadline(),cancel) == vj::Reply::accepted);
        }
        assert(j->phase() == vj::Phase::ready_for_engine && j->abandon_engine()); release_all(*j);
        assert(j->take_terminal() == vj::Outcome::cancelled && budget.usage().bytes == 0);
    }
    {
        auto j = job(budget,2,{8192,4096,4096},true);
        auto t = j->prepare(); auto f = reply(*t,wire::Kind::input_rejected);
        assert(j->accept(f,deadline(),cancel) == vj::Reply::accepted);
        t = j->prepare(); f = reply(*t,wire::Kind::error);
        assert(j->accept(f,deadline(),cancel) == vj::Reply::remote_error && !j->take_terminal());
        assert(j->reader_stopped(true) && j->take_terminal() == vj::Outcome::failure);
    }
    // A typed rejection is never an ENCODE reply.
    {
        auto j = job(budget,1); prepare_all(*j); assert(j->admit(true));
        auto t = j->encode(output(budget,0)); auto f = reply(*t,wire::Kind::input_rejected);
        assert(j->accept(f,deadline(),cancel) == vj::Reply::invalid && j->reader_stopped(true));
        assert(j->take_terminal() == vj::Outcome::failure);
    }
    assert(budget.usage().bytes == 0 && budget.usage().artifacts == 0);
    std::cout << "vision job ownership: PASS\n";
}
