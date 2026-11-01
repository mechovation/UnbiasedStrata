// No models/CUDA: exercises the actual native Session/Job wire path and cache.
#include "../vision_job_io.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <sys/socket.h>
using namespace strata_frontend;
namespace io = vision_job_io;
namespace art = vision_artifact;
namespace wire = vision_protocol;
namespace cache = vision_cache;
static auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536,64});
using namespace std::chrono_literals;
static std::atomic<bool> never{false};
static auto until() { return wire::Clock::now() + 5s; }
static vision_payload::Config config{{}, {}, 4, 32, 1024};
static vision_payload::Limits caps{16384, 4, 16, 1024, 65536, {4096, 64, 4096, 16384, 32, 16 * 1024 * 1024}};
static std::string metadata() {
    std::ifstream f("frontend/tests/data/pr4-metadata/qwen-still-0.bin", std::ios::binary); assert(f);
    return {std::istreambuf_iterator<char>(f), {}};
}
struct Counts { unsigned prepared = 0, encoded = 0, released = 0; };
static std::shared_ptr<const io::Encoded> run(uint64_t id, vision_payload::Config cfg,
    vision_payload::Digest digest, std::shared_ptr<art::Budget> budget, std::shared_ptr<cache::Cache> rows_cache,
    const std::vector<bool> & misses, bool accepted, Counts & counts, bool nonfinite = false, bool budget_failure = false) {
    auto s = std::make_unique<io::Submission>();
    s->job = id; s->control = std::make_shared<vision_control::Request>(until());
    s->artifacts = budget; s->recipes = recipes;
    s->cache = rows_cache; s->limits = {8192, 4096, 4096}; s->snapshot_bytes = 8192;
    s->prepare = s->encode = s->cleanup = 3s;
    for (size_t i = 0; i < misses.size(); ++i) {
        art::Artifact::Ptr source;
        assert(art::Artifact::source(*budget, {0,id,uint32_t(i),digest}, "source", caps.source_bytes, until(), never, source) == art::Result::ok);
        s->inputs.push_back({source,"pr4-fixture"});
    }
    auto session = io::Session::create({id,7,id}, cfg, caps, std::move(s)); assert(session);
    int fds[2]; assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    wire::Channel channel(fds[0]);
    std::thread reader([&, fd=fds[1]] {
        wire::Channel child(fd); uint64_t seq = 0;
        auto binding = [&](unsigned i) { return vision_payload::Binding{i,digest,"pr4-fixture",6,100+i,7,7}; };
        auto receive = [&](wire::Kind kind, unsigned i, size_t nfd) {
            wire::Frame f;
            assert(child.receive(f, {kind,7,id,++seq,nfd}, caps.payload_bytes, until(), never) == wire::IoResult::ok);
            auto b = binding(i); if (kind == wire::Kind::prepare) b.handle = 0;
            vision_payload::View view;
            assert(vision_payload::validate(kind, {reinterpret_cast<const char *>(f.payload.data()),f.payload.size()}, nfd,cfg,caps,b,view));
            return f;
        };
        auto send = [&](wire::Kind kind, unsigned i) {
            vision_commands::Payload p;
            const auto meta = metadata(); const std::string token(4,'\0');
            assert(vision_commands::build(kind,cfg,caps,binding(i),p,{100+i,7,7,token,token,meta}) == vision_commands::Result::ok);
            assert(child.send_borrowed({kind,uint32_t(p.bytes.size()),7,id,seq},p.bytes.data(),p.bytes.size(),nullptr,0,caps.payload_bytes,until(),never) == wire::IoResult::ok);
        };
        if (misses[0]) {
            auto f = receive(wire::Kind::prepare,0,1); ++counts.prepared;
            assert(art::sealed_size(f.fds[0],6,1024) == art::Result::ok); send(wire::Kind::prepared,0);
        }
        if (budget_failure) return; // no ENCODE/RELEASE possible; parent certifies teardown below
        if (accepted) for (unsigned i = 0; i < misses.size(); ++i) if (misses[i]) {
            auto f = receive(wire::Kind::encode,i,1); ++counts.encoded;
            std::array<float,196> values; values.fill(3.25f);
            if (nonfinite) { uint32_t nan=0x7fc00000; std::memcpy(values.data(),&nan,4); }
            assert(::pwrite(f.fds[0],values.data(),sizeof(values),0) == ssize_t(sizeof(values)));
            send(wire::Kind::encoded,i); if (nonfinite) return;
        }
        if (misses[0]) {
            auto f = receive(wire::Kind::release,0,0); ++counts.released; send(wire::Kind::released,0);
        }
    });
    assert(session->prepare(channel,never));
    auto success = session->admit(accepted,channel,never);
    std::shared_ptr<const io::Encoded> result;
    if (nonfinite || budget_failure) { assert(!success); reader.join(); session->reader_stopped(); }
    else {
        assert(success);
        if (accepted) {
            result = session->encoded(); assert(result && result->rows.size() == misses.size());
            for (size_t i = 0; i < result->rows.size(); ++i) {
                const auto & row = result->rows[i];
                assert(row->validated_rows() && row->tag().job == id && row->tag().instance == 7 && row->tag().occurrence == i && row->tag().source == digest);
                assert(art::sealed_size(row->fd(),784,784) == art::Result::ok);
                float value=0; assert(::pread(row->fd(),&value,4,0) == 4 && value == 3.25f);
            }
            assert(session->engine_begin()); session->engine_finish(true,true,false);
            // Sticky cancellation still permits RELEASE, including on cache hits.
            if (id == 2) session->control()->cancel(vision_control::Reason::cancelled);
            assert(session->release(channel,never));
        }
        reader.join();
    }
    assert(session->terminal()); assert(!session->terminal());
    return result;
}
int main() {
    config.generation.fill(0x31); config.device.fill(0x53);
    auto budget = std::make_shared<art::Budget>(art::Usage{100000,100});
    auto c = std::make_shared<cache::Cache>(1,784,budget,recipes);
    Counts n;
    // Cold duplicates reuse the real tokenizer recipe and completed rows.
    auto first = run(1,config,{},budget,c,{true,false},true,n);
    assert(n.prepared == 1 && n.encoded == 1 && n.released == 1);
    assert(first->rows[0] != first->rows[1]);
    auto second = run(2,config,{},budget,c,{false},true,n);
    assert(second->rows[0] != first->rows[0] && n.encoded == 1 && n.released == 1 && n.prepared == 1);
    run(3,config,{},budget,c,{false},false,n); // warm context refusal: no reader IPC
    assert(n.prepared == 1 && n.encoded == 1 && n.released == 1);
    auto changed = config; changed.generation[0]++;
    auto third = run(4,changed,{},budget,c,{true},true,n);
    vision_payload::Digest other{}; other[0]=1;
    auto fourth = run(5,changed,other,budget,c,{true},true,n);
    assert(n.encoded == 3 && c->size() == 1);
    cache::Key key{changed,other};
    const vision_row_cache::Cache::Key row_key{changed,other,{7,7,49,7},784};
    auto held = c->find(key); assert(held);
    art::Artifact::Ptr copy;
    assert(art::Artifact::rows(*budget,{7,99,0,other},7,7,4,784,until(),never,copy) == art::Result::ok);
    std::atomic<bool> stop{true};
    assert(vision_row_cache::copy(held->rows,row_key,*copy,until(),stop) == art::Result::cancelled);
    assert(vision_row_cache::copy(held->rows,row_key,*copy,wire::Clock::now(),never) == art::Result::deadline);
    auto mismatched=row_key; mismatched.source[0]++;
    assert(vision_row_cache::copy(held->rows,mismatched,*copy,until(),never) == art::Result::invalid);
    assert(vision_row_cache::copy(held->rows,row_key,*copy,until(),never) == art::Result::ok);
    assert(copy->finish_rows({7,100,0,other},7,7,4,until(),never) == art::Result::invalid); // cannot relabel a job
    assert(copy->finish_rows(copy->tag(),1,49,4,until(),never) == art::Result::invalid); // exact shape, not just bytes
    assert(copy->finish_rows(copy->tag(),7,7,4,until(),never) == art::Result::ok);
    copy.reset();
    auto wrong=key; wrong.config.width++; assert(!c->find(wrong));
    wrong=key; wrong.config.device[0]++; assert(!c->find(wrong));
    auto usage=budget->usage(); assert(c->evict_one()); assert(budget->usage().bytes == usage.bytes); // held after eviction
    first.reset(); second.reset(); third.reset(); fourth.reset();
    assert(budget->usage().bytes == 784); held.reset(); assert(budget->usage().bytes == 0);
    auto fresh = std::make_shared<cache::Cache>(1,784,budget,recipes);
    run(6,config,{},budget,fresh,{true},true,n,true); // finite validation before insertion
    assert(fresh->size() == 0 && budget->usage().bytes == 0);
    // Hard budget cannot refund an evicted but held row to admit a hit copy.
    auto tight = std::make_shared<art::Budget>(art::Usage{790,2});
    auto tc = std::make_shared<cache::Cache>(1,784,tight,recipes);
    auto retained = run(7,config,{},tight,tc,{true},true,n);
    run(8,config,{},tight,tc,{false},true,n,false,true);
    assert(tc->size() == 0 && tight->usage().bytes == 784);
    retained.reset(); assert(tight->usage().bytes == 0);
    // Cache disabled/oversize is best effort: real ENCODE succeeds without storage.
    auto disabled = std::make_shared<cache::Cache>(0,784,budget,recipes);
    run(9,config,{},budget,disabled,{true},true,n); assert(!disabled->size());
    auto small = std::make_shared<cache::Cache>(1,783,budget,recipes);
    run(10,config,{},budget,small,{true},true,n); assert(small->size() == 1); // soft cap, not the hard ledger
    small.reset();
    assert(budget->usage().bytes == 0);
    assert(recipes->usage().bytes == 0 && recipes->usage().snapshots == 0);
    std::cout << "vision native EARLY cache: PASS (zero warm PREPARE/ENCODE; exact admission; held budgets; no model/GPU)\n";
}
