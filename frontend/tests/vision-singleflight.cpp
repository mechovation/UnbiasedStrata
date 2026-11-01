// PR5 bounded waiter/flight ownership. No model, CUDA, network or service process.
#include "../vision_cache.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
using namespace strata_frontend;
namespace c = vision_cache;
namespace a = vision_artifact;
namespace v = vision_control;
using namespace std::chrono_literals;
int main() {
    const auto until = v::Clock::now()+10s;
    std::atomic<bool> never{false};
    vision_payload::Config config{{},{},4,32,1024};
    c::Key key{config,{}};
    const auto identity = c::identity_bytes(key);
    // Every output-affecting namespace component must change BOTH key and
    // engine identity material; worker instance/job IDs do not enter either.
    for (unsigned field = 0; field < 6; ++field) {
        auto changed = key;
        if (field == 0) ++changed.config.generation[0];
        if (field == 1) ++changed.config.device[0];
        if (field == 2) ++changed.config.width;
        if (field == 3) ++changed.config.pixel_stride;
        if (field == 4) ++changed.config.vocabulary_size;
        if (field == 5) ++changed.source[0];
        assert(!(key == changed) && c::identity_bytes(changed) != identity);
    }
    auto artifacts = std::make_shared<a::Budget>(a::Usage{2048,4});
    auto recipes = std::make_shared<vision_job_io::RecipeBudget>(vision_job_io::RecipeBudget::Usage{4096,4});
    auto cache = std::make_shared<c::Cache>(64,1,artifacts,recipes); // soft oversize permitted
    a::Artifact::Ptr row;
    assert(a::Artifact::rows(*artifacts,{1,1,0,{}},7,7,4,784,until,never,row) == a::Result::ok);
    std::array<float,196> finite{};
    assert(::pwrite(row->fd(),finite.data(),sizeof(finite),0) == ssize_t(sizeof(finite)));
    assert(row->finish_rows(row->tag(),7,7,4,until,never) == a::Result::ok);
    std::ifstream f("frontend/tests/data/mtmd-metadata/qwen-still-0.bin",std::ios::binary); assert(f);
    vision_job::Recipe recipe{std::string(4,'\0'),std::string(4,'\0'),{std::istreambuf_iterator<char>(f),{}},{7,7,49,7},784};
    auto complete = cache->put(key,recipe,row); assert(complete); row.reset();
    auto other_key = key; ++other_key.source[0];
    {
    c::Flights flights(2,3);
    auto producer = std::make_shared<v::Request>(until), waiter = std::make_shared<v::Request>(until);
    auto p = flights.join(key,producer), w = flights.join(key,waiter); assert(p && w && p->flight == w->flight);
    assert(!p->flight->result()); // PREPARE/in-progress data is not a complete hit
    auto unrelated = flights.join(other_key,producer); assert(unrelated);
    assert(!flights.join(key,producer)); // independent hard waiter bound
    c::Interest work({p,w});
    producer->cancel(v::Reason::disconnect);
    assert(!work.observe() && !work.flag()); // one disconnect is not producer cancellation
    unrelated.reset();
    auto third_key = key; ++third_key.config.generation[0];
    auto different = flights.join(third_key,waiter); assert(different && different->flight != p->flight);
    assert(!flights.join(other_key,waiter)); // hard flight bound, including live tickets
    different.reset();
    p->flight->publish(complete); assert(w->flight->result() == complete);
    assert(cache->evict_one() && cache->size() == 0);
    complete.reset();
    assert(artifacts->usage().bytes == 784 && recipes->usage().snapshots == 1); // evicted but flight-held
    waiter->cancel(v::Reason::disconnect);
    assert(work.observe() && work.flag()); // all-waiters-gone is sticky cancellation
    auto late = std::make_shared<v::Request>(until);
    auto l = flights.join(key,late); assert(l && l->flight == p->flight);
    c::Interest next_work({l}); assert(!next_work.observe() && l->flight->result());
    assert(work.observe()); // a cancelled producer is never revived by late registration
    auto expired = std::make_shared<v::Request>(v::Clock::now());
    auto e = flights.join(key,expired); assert(!e); // waiter capacity still pinned
    l.reset(); // next_work still owns its ticket: no premature registry refund
    assert(!flights.join(key,expired));
    // Concurrent registration/destruction is bounded even with expired controls.
    c::Flights raced(2,8);
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < 4; ++n) threads.emplace_back([&] {
        for (unsigned i = 0; i < 1000; ++i) {
            auto request = std::make_shared<v::Request>(until);
            auto ticket = raced.join(i % 2 ? key : other_key,request);
            if (ticket) { assert(ticket->flight->live()); request->cancel(v::Reason::disconnect); }
        }
    });
    for (auto & t : threads) t.join();
    assert(raced.join(third_key,late)); // dead weak flight slots are reclaimed
    }
    assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0 && recipes->usage().snapshots == 0);
    std::cout << "vision single-flight: PASS (bounded registries, cancelled waiters, held evictions, 4000 raced joins; no GPU)\n";
}
