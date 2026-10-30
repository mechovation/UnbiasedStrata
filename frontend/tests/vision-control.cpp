#include "../vision_control.hpp"
#include <cassert>
#include <iostream>
#include <thread>

using namespace strata_frontend::vision_control;
struct Sink final : Wake {
    std::atomic<unsigned> pending{0}, total{0};
    void post() noexcept override { ++total; ++pending; }
};
int main() {
    const auto now = Clock::now();
    auto request = std::make_shared<Request>(now + std::chrono::seconds(1));
    assert(!request->observe(now));
    std::thread observer([request] { request->cancel(Reason::disconnect); });
    observer.join();
    assert(request->flag().load());
    assert(request->observe(now + std::chrono::seconds(2)));
    assert(request->reason() == Reason::disconnect);
    Request expired(now);
    assert(expired.observe(now));
    expired.cancel(Reason::cancelled);
    assert(expired.reason() == Reason::deadline);
    std::atomic<bool> shutdown{false};
    assert(!shutdown.load()); // request cancellation cannot abort cleanup

    auto sink = std::make_shared<Sink>();
    Inbox inbox(2, sink);
    auto a = inbox.reserve(), b = inbox.reserve();
    assert(a && b && !inbox.reserve());
    a.reset();
    a = inbox.reserve();
    assert(a.publish({1, 2, 3, 4, Event::Kind::completed}));
    assert(!a.publish({}));
    assert(b.publish({2, 2, 3, 5, Event::Kind::failed}));
    assert(sink->total == 1 && !inbox.reserve());
    auto e = inbox.pop();
    assert(e && e->task == 1 && e->instance == 2 && e->job == 3 && e->sequence == 4);
    a = inbox.reserve();
    assert(a);
    inbox.stop_accepting();
    assert(!inbox.reserve() && !inbox.settled());
    assert(a.publish({3, 2, 3, 6, Event::Kind::stopped}));
    assert(inbox.pop()->task == 2);
    assert(inbox.pop()->task == 3);
    assert(!inbox.pop() && inbox.settled());
    assert(sink->total == 1);

    // A wake after an empty drain is mandatory, even with no active slot.
    Inbox idle(1, sink);
    assert(idle.reserve().publish({}));
    assert(idle.pop() && !idle.pop());
    assert(idle.reserve().publish({}));
    assert(sink->total == 3);
    assert(idle.pop() && !idle.pop());

    // Reservation/sink ownership survives facade destruction and move assignment.
    Inbox::Reservation retained;
    std::weak_ptr<Sink> weak;
    {
        auto owned = std::make_shared<Sink>();
        weak = owned;
        Inbox temporary(2, owned);
        retained = temporary.reserve();
        auto second = temporary.reserve();
        retained = std::move(second); // returns the old capacity
        assert(temporary.reserve());
        temporary.stop_accepting();
    }
    assert(!weak.expired());
    retained.reset();
    assert(weak.expired());

    // Exercise producer/empty-drain races. Consumer only drains after a wake;
    // silently dropped wakes therefore hang and are caught by runner watchdog.
    auto concurrent_sink = std::make_shared<Sink>();
    Inbox concurrent(7, concurrent_sink); // synthetic test cap, not production
    constexpr unsigned producers = 4, per_producer = 2000;
    std::vector<std::thread> threads;
    for (unsigned p = 0; p < producers; ++p) threads.emplace_back([&, p] {
        for (unsigned i = 0; i < per_producer; ++i) {
            Inbox::Reservation reservation;
            while (!(reservation = concurrent.reserve())) std::this_thread::yield();
            assert(reservation.publish({1 + p * per_producer + i, 9, 8, i + 1, Event::Kind::completed}));
        }
    });
    std::vector<bool> seen(producers * per_producer, false);
    unsigned received = 0;
    while (received != seen.size()) {
        if (!concurrent_sink->pending.exchange(0)) { std::this_thread::yield(); continue; }
        while (auto event = concurrent.pop()) {
            assert(event->task && event->task <= seen.size());
            assert(!seen[event->task - 1]);
            seen[event->task - 1] = true;
            ++received;
        }
    }
    for (auto & thread : threads) thread.join();
    concurrent.stop_accepting();
    assert(concurrent.settled());

    // Cancellation works even when every event slot is reserved.
    auto full = idle.reserve();
    assert(full && !idle.reserve());
    Request cancelled(Clock::time_point::max());
    cancelled.cancel(Reason::cancelled);
    assert(cancelled.observe());
    idle.stop_accepting();
    assert(!idle.settled());
    full.reset();
    assert(idle.settled());
    std::cout << "vision request control/inbox: PASS (8000 raced completions)\n";
}
