// PR5 real reader + actual handler/queue early-cache, single-flight and recovery.
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
        {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter,"PR4_NATIVE_ENCODE_HOLD="+gate+"/hold","PR5_NATIVE_DECODE_HOLD="+gate+"/prepare-hold"},400,config,caps};
    ea::Launch engine{guard,executable,{executable,"engine",gate},
        {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter,"PR5_NATIVE_SIGNATURE="+gate+"/signatures"}};
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
    auto request = [&](const std::vector<raw_buffer> & images,bool streaming = false,
                       std::shared_ptr<std::atomic<bool>> disconnected = {}, std::string override_prompt = {}) {
        const auto text = images.empty() ? json::array({42,43,44}) : json(override_prompt.empty() ? prompt : override_prompt);
        std::function<bool()> stop = [disconnected] { return disconnected && disconnected->load(); };
        server_http_req req{{},{},"/v1/completions","","",{},stop};
        auto result = ctx.handle_completions(req,json{{"prompt",text},{"n_predict",1},{"temperature",0},{"stream",streaming}},images,
            TASK_RESPONSE_TYPE_OAI_CMPL,images.empty() ? std::vector<std::pair<size_t,size_t>>{} : plain);
        const int status = result->status;
        if (result->is_stream()) for (;;) { std::string chunk; if (!result->next(chunk)) break; }
        return status;
    };
    auto signatures = [&] {
        std::ifstream file(gate+"/signatures"); std::vector<std::string> result;
        for (std::string line; std::getline(file,line);) result.push_back(line);
        return result;
    };
    const auto before = encodes(), decoded = calls('D');
    assert(request(sources) == 200);
    assert(encodes() == before+2 && calls('D') == decoded+2); // A/B/A: reuse real recipe AND rows
    auto stats = ctx.native_vision->cache_stats(); assert(stats.misses == 3 && stats.hits == 0);
    const auto cold_signature = signatures(); assert(cold_signature.size() == 1);
    assert(request(sources,true) == 200);
    assert(signatures().size() == 2 && signatures().back() == cold_signature[0]); // exact literal IDs/keys/grid/rows
    assert(encodes() == before+2 && calls('D') == decoded+2); // warm: no PREPARE/decode/preprocess/ENCODE
    std::string too_long = prompt; for (unsigned i = 0; i < 4000; ++i) too_long += " xyz";
    const auto warm_files = calls('F');
    assert(request(sources,false,{},too_long) == 400 && encodes() == before+2 && calls('D') == decoded+2 && calls('F') == warm_files);
    const auto c = bmp(31), d = bmp(43);
    assert(request({sources[0],c,sources[0]}) == 200 && encodes() == before+3 && calls('D') == decoded+3); // sparse PREPARE occurrence 1
    assert(request({c,d,c}) == 200 && encodes() == before+4); // evict A/B, still ordered duplicates
    assert(request(sources) == 200 && encodes() == before+6);
    stats = ctx.native_vision->cache_stats(); assert(stats.evictions >= 4);
    auto metrics = [&] {
        const int id = ctx.queue_tasks.get_new_id(); ctx.queue_results.add_waiting_task_id(id);
        server_task task(SERVER_TASK_TYPE_METRICS); task.id = id; ctx.queue_tasks.post(std::move(task));
        server_task_result_ptr result;
        await([&] { result = ctx.queue_results.recv_with_timeout({id},1); return bool(result); });
        auto * value = dynamic_cast<server_task_result_metrics *>(result.get()); assert(value);
        auto deferred = value->n_tasks_deferred; ctx.queue_results.remove_waiting_task_id(id); return deferred;
    };
    auto live = [] { return std::make_shared<std::atomic<bool>>(false); };
    const std::vector<raw_buffer> shared{bmp(51),bmp(61),bmp(51)}, unrelated{bmp(71),bmp(81),bmp(71)};
    { std::ofstream hold(gate+"/hold"); hold << '1'; }
    const auto shared_encode = encodes(), shared_decode = calls('D'), shared_gen = calls('G');
    auto disconnect_producer = live(), disconnect_waiter = live();
    auto producer = std::async(std::launch::async,[&] { return request(shared,false,disconnect_producer); });
    await([&] { return encodes() > shared_encode; });
    auto evictor = std::async(std::launch::async,[&] { return request(unrelated); });
    await([&] { return metrics() == 1; });
    auto follower = std::async(std::launch::async,[&] { return request(shared,true); });
    auto cancelled_waiter = std::async(std::launch::async,[&] { return request(shared,false,disconnect_waiter); });
    await([&] { return metrics() == 3; });
    disconnect_waiter->store(true);
    assert(cancelled_waiter.wait_for(5s) == std::future_status::ready); (void)cancelled_waiter.get();
    await([&] { return metrics() == 2; });
    disconnect_producer->store(true);
    assert(producer.wait_for(5s) == std::future_status::ready); (void)producer.get();
    assert(follower.wait_for(50ms) == std::future_status::timeout && calls('G') == shared_gen);
    ::unlink((gate+"/hold").c_str());
    assert(evictor.wait_for(60s) == std::future_status::ready && evictor.get() == 200);
    assert(follower.wait_for(60s) == std::future_status::ready && follower.get() == 200);
    await([&] { return metrics() == 0; });
    assert(ctx.native_vision->available()); // no kill/restart for one cancelled waiter
    assert(encodes() == shared_encode+4 && calls('D') == shared_decode+4 && calls('G') == shared_gen+2);
    // All waiters gone: an outstanding real ENCODE is contained, incomplete
    // state is never published, and later work needs explicit reader recovery.
    const std::vector<raw_buffer> abandoned{bmp(91),bmp(101),bmp(91)};
    { std::ofstream hold(gate+"/hold"); hold << '1'; }
    const auto all_encode = encodes(), all_decode = calls('D'), all_gen = calls('G');
    auto disconnect_all_a = live(), disconnect_all_b = live();
    auto all_a = std::async(std::launch::async,[&] { return request(abandoned,false,disconnect_all_a); });
    await([&] { return encodes() > all_encode; });
    auto all_b = std::async(std::launch::async,[&] { return request(abandoned,false,disconnect_all_b); });
    await([&] { return metrics() == 1; });
    disconnect_all_b->store(true);
    assert(all_b.wait_for(5s) == std::future_status::ready); (void)all_b.get();
    await([&] { return metrics() == 0; });
    disconnect_all_a->store(true);
    assert(all_a.wait_for(5s) == std::future_status::ready); (void)all_a.get();
    await([&] { return !ctx.native_vision->available() && artifacts->usage().bytes < 1024*1024; });
    assert(encodes() == all_encode+1 && calls('D') == all_decode+2 && calls('G') == all_gen);
    ::unlink((gate+"/hold").c_str());
    std::this_thread::sleep_for(60ms); // explicitly injected recovery backoff
    await([&] { return ctx.native_vision->recover(); });
    await([&] { return ctx.native_vision->available(); });
    assert(request(abandoned) == 200 && encodes() == all_encode+3 && calls('D') == all_decode+4);
    // A parent composition failure is NOT a disconnect. It must abort its own
    // producer even with live followers, rather than strand the occupied turn.
    const std::vector<raw_buffer> malformed{bmp(111),bmp(121),bmp(111)};
    { std::ofstream hold(gate+"/prepare-hold"); hold << '1'; }
    const auto parent_decode = calls('D'), parent_encode = encodes();
    auto parent_failure = std::async(std::launch::async,[&] { return request(malformed,false,{},prompt+get_media_marker()); });
    await([&] { return calls('D') > parent_decode; });
    auto parent_follower = std::async(std::launch::async,[&] { return request(malformed); });
    await([&] { return metrics() == 1; });
    ::unlink((gate+"/prepare-hold").c_str());
    assert(parent_failure.wait_for(30s) == std::future_status::ready && parent_failure.get() == 500);
    assert(parent_follower.wait_for(30s) == std::future_status::ready && parent_follower.get() == 503);
    assert(encodes() == parent_encode);
    std::this_thread::sleep_for(60ms);
    await([&] { return ctx.native_vision->recover(); });
    await([&] { return ctx.native_vision->available(); });
    // Put A/B back so identical-restart coverage below still has complete hits.
    assert(request(sources) == 200);
    const auto recovery_encodes = encodes(), recovery_decodes = calls('D');
    // Classified native refusal tears down this CPU reader; retained cache rows
    // remain generation-bound, and text obtains a NEW exclusive no-reader turn.
    auto bad = sources; const uint32_t dimension = 5000;
    for (unsigned b = 0; b < 4; ++b) bad[0][18+b] = dimension >> (8*b);
    assert(request(bad) == 400);
    await([&] { return !ctx.native_vision->available(); });
    { std::ofstream expected(gate+"/expected-text"); expected << "42,43,44\n"; }
    assert(request({}) == 200 && encodes() == recovery_encodes);
    assert(request(sources) == 200 && encodes() == recovery_encodes && calls('D') == recovery_decodes); // complete hits need no reader
    assert(signatures().back() == cold_signature[0]);
    assert(!ctx.native_vision->available());
    assert(request({c,d,c}) == 503 && encodes() == recovery_encodes && calls('D') == recovery_decodes); // no implicit recovery/miss decode
    std::this_thread::sleep_for(60ms); // explicit injected backoff, not synchronization
    assert(ctx.native_vision->recover()); await([&] { return ctx.native_vision->available(); });
    assert(request(sources) == 200 && encodes() == recovery_encodes && calls('D') == recovery_decodes); // complete hits survive identical restart
    assert(signatures().back() == cold_signature[0]); // same identities through restart
    context.terminate(); owner.join();
    ctx.native_vision.reset();
    assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0 && recipes->usage().snapshots == 0);
    assert(safety->adapter->stopped() && !safety->leased && !safety->revoked);
    for (const auto * file : {"entered","release","expected-text","signatures"}) ::unlink((gate+"/"+file).c_str());
    assert(::rmdir(gate.c_str()) == 0);
    std::cout << "PR5_NATIVE_EARLY_CACHE_PASS zero_warm_prepare_encode=1 exact_warm_ids_keys_rows=1 cold_duplicates=1 producer_disconnect=1 waiter_disconnect=1 all_waiters_gone=1 parent_failure=1 eviction_pins=1 reader_restart=1 readerless_text_and_hits=1 readerless_miss_refusal=1 gpu=none\n";
}
