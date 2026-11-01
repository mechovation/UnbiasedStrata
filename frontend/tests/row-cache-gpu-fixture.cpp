// Qualification-only actual handler/queue + physical GPU reader/safety fixture.
// Uses a protocol probe, NOT an LLM; real-engine checks are a separate matrix.
// Never compiled into production or exposed as an HTTP test/metrics endpoint.
#include "../strata_context.cpp"
#include <cassert>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
static size_t phases(const char * name) {
    std::ifstream f("/audit/phases"); size_t result = 0;
    for (std::string line; std::getline(f,line);) {
        std::istringstream in(line); std::string time,pid,phase; in >> time >> pid >> phase;
        result += phase == name;
    }
    return result;
}
template<class F> static void await(F f) {
    const auto until = Clock::now()+180s;
    while (!f()) { assert(Clock::now() < until); std::this_thread::sleep_for(2ms); }
}
static raw_buffer bmp(unsigned seed) {
    const unsigned w = 64, h = 64, stride = w*3;
    raw_buffer b(54+h*stride);
    auto number = [&](unsigned at,unsigned n,unsigned bytes) { for (unsigned i=0;i<bytes;++i) b[at+i] = n >> (8*i); };
    b[0]='B'; b[1]='M'; number(2,b.size(),4); number(10,54,4); number(14,40,4);
    number(18,w,4); number(22,h,4); number(26,1,2); number(28,24,2);
    for (size_t i=54;i<b.size();++i) b[i]=(i+seed)%251;
    return b;
}
static void hold(bool on) {
    if (on) { std::ofstream f("/audit/hold-encode"); f << '1'; assert(f); }
    else assert(::unlink("/audit/hold-encode") == 0);
}
int main(int argc,char ** argv) {
    assert(argc == 8); // profile model projector guard reader probe evidence-dir
    assert(::setenv("CUDA_VISIBLE_DEVICES","-1",1) == 0);
    std::set_terminate([] { std::_Exit(70); });
    common_init(); common_log_pause(common_log_main()); llama_backend_init();
    assert(!ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU)); // parent must stay CPU-only
    common_params p; p.model.path=argv[2]; p.mmproj.path=argv[3]; p.mmproj_use_gpu=true;
    p.image_max_tokens=64; p.cpuparams.n_threads=4; p.n_threads_http=8; p.n_ctx=4096;
    p.model_alias={"pr5-gpu-probe"};
    strata_context context; context.configure_native(argv[1],argv[4],argv[5]); context.set_picture_cache(2,1ULL<<30);
    std::string error;
    assert(context.start_engine(p,argv[6],{"--fixture-dir",argv[7]},error));
    assert(context.load(p,error));
    auto & ctx=*context.impl; ctx.n_ctx=4096;
    auto * bridge=ctx.native_vision.get(); assert(bridge && bridge->available());
    std::thread owner([&] { context.start_loop(); });
    const std::string literal="Literal <|image_pad|> <|vision_start|> ";
    const std::string marker=get_media_marker(), prompt=literal+marker+" second "+marker+" third "+marker+" end";
    const std::vector<std::pair<size_t,size_t>> plain{{0,literal.size()}};
    auto request = [&](const std::vector<raw_buffer> & sources,bool stream=false,
                       std::shared_ptr<std::atomic<bool>> disconnect={}) {
        std::function<bool()> stop=[disconnect] { return disconnect && disconnect->load(); };
        server_http_req req{{},{},"/v1/completions","","",{},stop};
        auto result=ctx.handle_completions(req,json{{"prompt",sources.empty()?json::array({42,43,44}):json(prompt)},
            {"n_predict",1},{"temperature",0},{"stream",stream}},sources,TASK_RESPONSE_TYPE_OAI_CMPL,
            sources.empty()?std::vector<std::pair<size_t,size_t>>{}:plain);
        const int status=result->status;
        if(result->is_stream()) for (;;) { std::string chunk; if(!result->next(chunk)) break; }
        return status;
    };
    auto metrics = [&] {
        const int id=ctx.queue_tasks.get_new_id(); ctx.queue_results.add_waiting_task_id(id);
        server_task task(SERVER_TASK_TYPE_METRICS); task.id=id; ctx.queue_tasks.post(std::move(task));
        server_task_result_ptr r; await([&] { r=ctx.queue_results.recv_with_timeout({id},1); return bool(r); });
        auto * m=dynamic_cast<server_task_result_metrics *>(r.get()); assert(m);
        const auto n=std::pair{m->n_processing_slots,m->n_tasks_deferred}; ctx.queue_results.remove_waiting_task_id(id); return n;
    };
    auto captures = [&] {
        std::ifstream f(std::string(argv[7])+"/results"); std::vector<json> out;
        for(std::string line;std::getline(f,line);) out.push_back(json::parse(line));
        return out;
    };
    auto same = [](const json & a,const json & b) {
        for(const auto * k:{"kind","tokens","token_sha","keys_sha","row_sha","shapes","image_cells"}) assert(a[k] == b[k]);
    };
    const std::vector<raw_buffer> a{bmp(1),bmp(2),bmp(1)};
    const auto d0=phases("reader_decode_begin"), p0=phases("reader_prepare_begin"), e0=phases("reader_encode_begin");
    assert(request(a)==200);
    assert(phases("reader_decode_begin")==d0+2 && phases("reader_prepare_begin")==p0+2 && phases("reader_encode_begin")==e0+2);
    assert(request(a,true)==200);
    assert(phases("reader_decode_begin")==d0+2 && phases("reader_prepare_begin")==p0+2 && phases("reader_encode_begin")==e0+2);
    auto cold=captures(); assert(cold.size()==2); same(cold[0],cold[1]);
    const std::vector<raw_buffer> mixed{a[0],bmp(3),a[0]};
    assert(request(mixed)==200 && phases("reader_encode_begin")==e0+3 && phases("reader_prepare_begin")==p0+3); // sparse PREPARE 1
    const std::vector<raw_buffer> shared{bmp(11),bmp(12),bmp(11)}, other{bmp(21),bmp(22),bmp(21)};
    auto live=[] { return std::make_shared<std::atomic<bool>>(false); };
    const auto e1=phases("reader_encode_begin"), d1=phases("reader_decode_begin"), g1=phases("engine_gen"), h1=phases("reader_encode_held");
    hold(true);
    auto dc=live(), dw=live();
    auto producer=std::async(std::launch::async,[&] { return request(shared,false,dc); });
    await([&] { return phases("reader_encode_held")>h1; }); // actual CUDA encode/readback completed; reply is held
    auto evictor=std::async(std::launch::async,[&] { return request(other); });
    await([&] { return metrics()==std::pair(1,1); });
    auto follower=std::async(std::launch::async,[&] { return request(shared,true); });
    auto cancelled=std::async(std::launch::async,[&] { return request(shared,false,dw); });
    await([&] { return metrics()==std::pair(1,3); }); // actual registered/deferred waiters, no sleep-only race test
    dw->store(true); assert(cancelled.wait_for(5s)==std::future_status::ready); (void)cancelled.get();
    await([&] { return metrics()==std::pair(1,2); });
    dc->store(true); assert(producer.wait_for(5s)==std::future_status::ready); (void)producer.get();
    assert(phases("engine_gen")==g1 && metrics()==std::pair(1,2));
    hold(false);
    assert(evictor.wait_for(60s)==std::future_status::ready && evictor.get()==200);
    assert(follower.wait_for(60s)==std::future_status::ready && follower.get()==200);
    await([&] { return metrics()==std::pair(0,0); });
    assert(bridge->available() && phases("reader_encode_begin")==e1+4 && phases("reader_decode_begin")==d1+4 && phases("engine_gen")==g1+2);
    assert(request(a)==200); // retain known complete hits for readerless/restart checks
    const auto stable=captures().back();
    const std::vector<raw_buffer> abandoned{bmp(31),bmp(32),bmp(31)};
    const auto e2=phases("reader_encode_begin"), h2=phases("reader_encode_held"), g2=phases("engine_gen");
    hold(true); auto da=live(), db=live();
    auto all_a=std::async(std::launch::async,[&] { return request(abandoned,false,da); });
    await([&] { return phases("reader_encode_held")>h2; });
    auto all_b=std::async(std::launch::async,[&] { return request(abandoned,false,db); });
    await([&] { return metrics()==std::pair(1,1); });
    db->store(true); assert(all_b.wait_for(5s)==std::future_status::ready); (void)all_b.get();
    await([&] { return metrics()==std::pair(1,0); });
    da->store(true); assert(all_a.wait_for(5s)==std::future_status::ready); (void)all_a.get();
    await([&] { return metrics()==std::pair(0,0) && !bridge->available(); });
    assert(phases("reader_encode_begin")==e2+1 && phases("engine_gen")==g2);
    hold(false);
    const auto e3=phases("reader_encode_begin"), p3=phases("reader_prepare_begin"), d3=phases("reader_decode_begin");
    assert(request(a)==200); same(stable,captures().back()); // actual physical safety authority permits certified cache-only work
    assert(request({})==200 && !bridge->available());
    assert(request(other)==503 && phases("reader_encode_begin")==e3 && phases("reader_prepare_begin")==p3 && phases("reader_decode_begin")==d3);
    await([&] { return bridge->recover(); }); await([&] { return bridge->available(); });
    assert(request(a,true)==200); same(stable,captures().back());
    assert(phases("reader_encode_begin")==e3 && phases("reader_prepare_begin")==p3 && phases("reader_decode_begin")==d3);
    assert(request(abandoned)==200 && phases("reader_encode_begin")==e3+2); // killed/incomplete encode was never published
    context.terminate(); owner.join();
    std::cout << "PR5_GPU_CACHE_QUEUE_PASS parent_gpu_context=none warm_prepare_decode_encode=zero exact_ids_keys_rows=1 sparse=1 producer_disconnect=1 queued_disconnect=1 eviction_pins=1 all_gone=1 readerless_hits=1 miss_refusal=1 restart=1 incomplete_not_published=1 physical_safety=1\n";
    return 0;
}
