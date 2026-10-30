// Real reader + actual handler/queue late-cache and contained-reader text recovery.
static void queue_cache_scenario(const char * executable, const char * guard, const char * model_path, const char * projector_path,
    const vision_payload::Config & config, const vision_payload::Limits & caps, const llama_vocab * vocab, int32_t pad,
    const std::string & prompt, const std::vector<std::pair<size_t,size_t>> & plain, const std::vector<raw_buffer> & sources) {
    namespace vq = vision_queue;
    char name[] = "/tmp/pr4-native-cache-XXXXXX"; assert(::mkdtemp(name)); const std::string gate = name;
    { std::ofstream release(gate+"/release"); release << '1'; }
    strata_context context; auto & ctx = *context.impl;
    ctx.vocab = vocab; ctx.n_ctx = 1024; ctx.model_name = "native-cpu-cache"; ctx.image_pad_id = pad; ctx.init_literals(); ctx.metrics.init();
    auto artifacts = std::make_shared<vision_artifact::Budget>(vision_artifact::Usage{64*1024*1024,32});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{128*1024,8});
    std::shared_ptr<CpuOnlySafety> safety;
    vs::Launch reader{guard,executable,{executable,"reader",model_path,projector_path,"cpu"},
        {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter},400,config,caps};
    ea::Launch engine{guard,executable,{executable,"engine",gate},
        {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter}};
    ea::Limits engine_limits{4*1024*1024,4096,16384,64,config.vocabulary_size,10s,1s,1s};
    ctx.native_vision = std::make_unique<vq::Bridge>(ctx.queue_owner,reader,vs::Limits{180s,1s,1s,5s,50ms,600s,4},engine,
        engine_limits,vq::Limits{8,1024*1024,128,16,32768,16*1024*1024,65536,{65536,16384,32768},180s,180s,180s,5s,10s},
        2,vocab,pad,get_media_marker(),[vocab](const std::string & text) {
            llama_tokens ids; strata_context_impl::tokenize_plain(vocab,text,ids); return ids;
        },artifacts,recipes,[&](std::shared_ptr<ea::Adapter> adapter) {
            safety = std::make_shared<CpuOnlySafety>(); safety->adapter = std::move(adapter); return safety;
        });
    assert(ctx.native_vision->set_cache(2,1024*1024));
    ctx.queue_tasks.on_new_task([&](server_task && task,bool yielding) { return ctx.process_new_task(std::move(task),yielding); });
    ctx.queue_tasks.on_update_slots([&] { ctx.update_slots(); });
    std::thread owner([&] { context.start_loop(); });
    assert(ctx.native_vision->recover()); await([&] { return ctx.native_vision->available(); });
    auto request = [&](const std::vector<raw_buffer> & images,bool streaming = false) {
        const auto text = images.empty() ? json::array({42,43,44}) : json(prompt);
        std::function<bool()> stop = [] { return false; };
        server_http_req req{{},{},"/v1/completions","","",{},stop};
        auto result = ctx.handle_completions(req,json{{"prompt",text},{"n_predict",1},{"temperature",0},{"stream",streaming}},images,
            TASK_RESPONSE_TYPE_OAI_CMPL,images.empty() ? std::vector<std::pair<size_t,size_t>>{} : plain);
        const int status = result->status;
        if (result->is_stream()) for (;;) { std::string chunk; if (!result->next(chunk)) break; }
        return status;
    };
    const auto before = encodes(), decoded = calls('D');
    assert(request(sources) == 200);
    assert(encodes() == before+2 && calls('D') == decoded+3); // A/B/A: last A late-hits
    auto stats = ctx.native_vision->cache_stats(); assert(stats.misses == 2 && stats.hits == 1);
    assert(request(sources,true) == 200);
    assert(encodes() == before+2 && calls('D') == decoded+6); // PREPARE still runs on warm hits
    const auto c = bmp(31), d = bmp(43);
    assert(request({c,d,c}) == 200 && encodes() == before+4); // evict A/B, still ordered duplicates
    assert(request(sources) == 200 && encodes() == before+6);
    stats = ctx.native_vision->cache_stats(); assert(stats.evictions >= 4);
    // Classified native refusal tears down this CPU reader; retained cache rows
    // remain generation-bound, and text obtains a NEW exclusive no-reader turn.
    auto bad = sources; const uint32_t dimension = 5000;
    for (unsigned b = 0; b < 4; ++b) bad[0][18+b] = dimension >> (8*b);
    assert(request(bad) == 400);
    await([&] { return !ctx.native_vision->available(); });
    { std::ofstream expected(gate+"/expected-text"); expected << "42,43,44\n"; }
    assert(request({}) == 200 && encodes() == before+6);
    std::this_thread::sleep_for(60ms); // explicit injected backoff, not synchronization
    assert(ctx.native_vision->recover()); await([&] { return ctx.native_vision->available(); });
    assert(request(sources) == 200 && encodes() == before+6); // rows survive identical reader restart
    context.terminate(); owner.join();
    ctx.native_vision.reset();
    assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0 && recipes->usage().snapshots == 0);
    assert(safety->adapter->stopped() && !safety->leased && !safety->revoked);
    for (const auto * file : {"entered","release","expected-text"}) ::unlink((gate+"/"+file).c_str());
    assert(::rmdir(gate.c_str()) == 0);
    std::cout << "PR4_NATIVE_LATE_CACHE_PASS real_prepare_on_hits=1 real_encode_misses=1 ordered_duplicates=1 eviction=1 held_budget=1 reader_restart=1 readerless_text=1 gpu=none\n";
}
