#include "../vision_engine_handoff.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
using namespace strata_frontend;
namespace h = vision_engine_handoff;
using namespace std::chrono_literals;
const h::Identity id{1, 2, 3, 4};
struct Owned final : h::Resources {};
struct CpuOnlyAuthority final : h::Authority {
    bool allow_engine = true, allow_reader = true, engine_turn = false;
    int forward = 0, backward = 0;
    std::shared_ptr<vision_control::Request> cancel_on_transfer;
    bool to_engine(h::Identity actual) noexcept override {
        assert(actual == id); assert(!engine_turn); ++forward;
        if (cancel_on_transfer) cancel_on_transfer->cancel(vision_control::Reason::disconnect);
        if (!allow_engine) return false;
        engine_turn = true; return true;
    }
    bool to_reader(h::Identity actual) noexcept override {
        assert(actual == id); assert(engine_turn); ++backward;
        if (!allow_reader) return false;
        engine_turn = false; return true;
    }
};
struct FakeEngine final : h::Engine {
    std::shared_ptr<CpuOnlyAuthority> authority;
    h::Start start_result = h::Start::sent;
    h::Observation observation = h::Observation::pending;
    bool stop_ok = true, safe = true;
    int starts = 0, stops = 0, polls = 0, contains = 0;
    h::Clock::time_point first_cleanup{}, first_containment{};
    void check(h::Identity actual) { assert(actual == id); assert(authority->engine_turn); }
    void cleanup(h::Clock::time_point until) {
        if (first_cleanup == h::Clock::time_point{}) first_cleanup = until;
        else assert(first_cleanup == until); // progress/failed cleanup never renews deadline
    }
    h::Start start(h::Identity actual, const h::Resources &, h::Clock::time_point,
                   const std::atomic<bool> &) noexcept override { check(actual); ++starts; return start_result; }
    bool stop(h::Identity actual, h::Clock::time_point until) noexcept override {
        check(actual); cleanup(until); ++stops; return stop_ok;
    }
    h::Observation poll(h::Identity actual, h::Clock::time_point) noexcept override {
        check(actual); ++polls; return observation;
    }
    bool shutdown(h::Clock::time_point) noexcept override { return true; }
    bool contain(h::Identity actual, h::Clock::time_point until) noexcept override {
        check(actual);
        if (first_containment == h::Clock::time_point{}) first_containment = until;
        else assert(first_containment == until);
        if (first_cleanup != h::Clock::time_point{}) assert(until >= first_cleanup);
        ++contains; return safe;
    }
};
struct Fixture {
    std::shared_ptr<CpuOnlyAuthority> authority = std::make_shared<CpuOnlyAuthority>();
    std::shared_ptr<FakeEngine> engine = std::make_shared<FakeEngine>();
    std::shared_ptr<vision_control::Request> request = std::make_shared<vision_control::Request>(h::Clock::now() + 10s);
    std::shared_ptr<const Owned> resources = std::make_shared<Owned>();
    std::weak_ptr<const Owned> held = resources;
    std::unique_ptr<h::Handoff> handoff;
    Fixture() {
        engine->authority = authority;
        handoff = std::make_unique<h::Handoff>(id, resources, request, engine, authority, h::Clock::now() + 2s, 20ms);
        resources.reset();
    }
    void done(h::Outcome outcome) {
        assert(handoff->state() == h::State::complete); assert(held.expired());
        assert(!authority->engine_turn);
        assert(handoff->take_result(id) == outcome); assert(!handoff->take_result(id));
        assert(!handoff->begin(id)); assert(!handoff->advance(id));
    }
};
int main() {
    { Fixture f;
      for (auto stale : {h::Identity{9,2,3,4}, h::Identity{1,9,3,4}, h::Identity{1,2,9,4}, h::Identity{1,2,3,9}}) {
          assert(!f.handoff->begin(stale)); assert(!f.handoff->advance(stale)); assert(!f.handoff->take_result(stale));
      }
      assert(f.handoff->begin(id)); assert(!f.handoff->begin(id)); assert(!f.held.expired());
      f.engine->observation = h::Observation::succeeded;
      f.handoff->advance(id); f.done(h::Outcome::success); assert(f.engine->stops == 0); }
    { Fixture f; f.authority->allow_engine = false; f.handoff->begin(id);
      f.done(h::Outcome::failure); assert(f.engine->starts == 0); }
    { Fixture f; f.request->cancel(vision_control::Reason::disconnect); f.handoff->begin(id);
      f.done(h::Outcome::cancelled); assert(f.authority->forward == 0); assert(f.engine->stops == 0); }
    { Fixture f; f.authority->cancel_on_transfer = f.request; f.handoff->begin(id);
      f.done(h::Outcome::cancelled); assert(f.engine->starts == 0); assert(f.engine->stops == 0); }
    { Fixture f; f.engine->start_result = h::Start::not_sent; f.authority->allow_reader = false;
      f.handoff->begin(id); assert(f.handoff->state() == h::State::proof_blocked);
      assert(!f.held.expired()); assert(!f.handoff->take_result(id)); f.handoff->advance(id);
      f.authority->allow_reader = true; f.handoff->advance(id); f.done(h::Outcome::failure);
      assert(f.engine->stops == 0); assert(f.engine->starts == 1); }
    { Fixture f; f.engine->start_result = h::Start::uncertain; f.engine->safe = false;
      f.handoff->begin(id); f.handoff->advance(id); f.handoff->advance(id);
      assert(!f.held.expired()); assert(f.authority->backward == 0);
      f.engine->safe = true; f.handoff->advance(id); f.done(h::Outcome::failure);
      assert(f.engine->starts == 1); assert(f.engine->stops == 0); }
    { Fixture f; f.handoff->begin(id); f.request->cancel(vision_control::Reason::disconnect);
      f.handoff->advance(id); f.handoff->advance(id);
      assert(f.engine->stops == 1); assert(!f.held.expired()); assert(!f.handoff->take_result(id));
      f.engine->observation = h::Observation::succeeded; f.authority->allow_reader = false;
      f.handoff->advance(id); assert(!f.held.expired());
      f.authority->allow_reader = true; f.handoff->advance(id); f.done(h::Outcome::cancelled);
      assert(f.request->flag().load()); }
    { Fixture f; f.handoff->begin(id); f.request->cancel(vision_control::Reason::cancelled);
      f.engine->stop_ok = false; f.engine->safe = false; f.handoff->advance(id);
      assert(f.handoff->state() == h::State::containing); assert(!f.held.expired());
      f.engine->safe = true; f.handoff->advance(id); f.done(h::Outcome::cancelled); assert(f.engine->stops == 1); }
    { Fixture f; f.handoff->begin(id); f.request->cancel(vision_control::Reason::cancelled);
      f.handoff->advance(id); std::this_thread::sleep_for(25ms); f.handoff->advance(id);
      f.done(h::Outcome::cancelled); assert(f.engine->contains == 1); }
    for (auto observation : {h::Observation::failed, h::Observation::broken}) {
        Fixture f; f.handoff->begin(id); f.engine->observation = observation; f.handoff->advance(id);
        f.done(h::Outcome::failure);
    }
    { Fixture f;
      f.handoff = std::make_unique<h::Handoff>(id, std::make_shared<Owned>(), f.request, f.engine, f.authority,
                                              h::Clock::now() - 1s, 1s);
      f.handoff->begin(id); f.done(h::Outcome::failure); assert(f.engine->starts == 0); }
    { Fixture f;
      f.request = std::make_shared<vision_control::Request>(h::Clock::now() - 1s);
      f.handoff = std::make_unique<h::Handoff>(id, std::make_shared<Owned>(), f.request, f.engine, f.authority,
                                              h::Clock::now() + 1s, 1s);
      f.handoff->begin(id); f.done(h::Outcome::cancelled);
      assert(f.request->reason() == vision_control::Reason::deadline); }
    { Fixture f; f.handoff->begin(id);
      f.request->observe(f.request->deadline()); // deterministic total-deadline observation
      f.engine->observation = h::Observation::succeeded; f.handoff->advance(id);
      f.done(h::Outcome::cancelled); assert(f.engine->stops == 1);
      assert(f.request->reason() == vision_control::Reason::deadline); }
    // Isolated fail-stop check: an active owner cannot destruct and drop files.
    const pid_t child = fork(); assert(child >= 0);
    if (!child) {
        std::set_terminate([] { _exit(73); });
        Fixture f; f.handoff->begin(id); f.handoff.reset(); _exit(74);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 73);
    std::cout << "vision engine handoff: PASS (CPU-only isolated authority/adapter)\n";
}
