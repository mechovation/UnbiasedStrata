// Included by vision-native.cpp: actual request handler, server_queue, slot and
// response reader. All decoder/encoder work remains in the real reader child.
#include "codec-test-fixtures.hpp"
static void queue_scenario(const char * executable, const char * guard, const char * model_path, const char * projector_path,
    const vision_payload::Config & config, const vision_payload::Limits & caps, const llama_vocab * vocab, int32_t pad,
    const std::string & prompt, const std::vector<std::pair<size_t,size_t>> & plain, const std::vector<raw_buffer> & sources,
    const server_chat_params & chat_params) {
    namespace vq = vision_queue;
    char name[] = "/tmp/pr4-native-queue-XXXXXX"; assert(::mkdtemp(name)); const std::string gate = name;
    const auto before_encode = encodes(), before_commands = calls('G');
    strata_context context; auto & ctx = *context.impl;
    ctx.vocab = vocab; ctx.n_ctx = 1024; ctx.model_name = "native-cpu-fixture"; ctx.image_pad_id = pad; ctx.init_literals(); ctx.metrics.init();
    auto artifacts = std::make_shared<vision_artifact::Budget>(vision_artifact::Usage{64*1024*1024, 32});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{128*1024, 8});
    std::shared_ptr<CpuOnlySafety> safety;
    vs::Launch reader{guard, executable, {executable,"reader",model_path,projector_path,"cpu"},
        {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter,"PR4_NATIVE_ENCODE_HOLD="+gate+"/hold"}, 200, config, caps};
    ea::Launch engine{guard, executable, {executable,"engine",gate},
        {"CUDA_VISIBLE_DEVICES=-1","NVIDIA_VISIBLE_DEVICES=void","PR4_NATIVE_COUNTER="+counter,"PR5_NATIVE_SIGNATURE="+gate+"/omission-signatures"}};
    ea::Limits engine_limits{4*1024*1024,4096,16384,64,config.vocabulary_size,10s,1s,1s};
    ctx.native_vision = std::make_unique<vq::Bridge>(ctx.queue_owner, reader, vs::Limits{180s,1s,1s,5s,50ms,600s,3}, engine,
        engine_limits, vq::Limits{8,1024*1024,128,16,32768,16*1024*1024,65536,{65536,16384,32768},180s,180s,180s,5s,10s},
        2, vocab, pad, get_media_marker(), [vocab](const std::string & text) {
            llama_tokens ids; strata_context_impl::tokenize_plain(vocab,text,ids); return ids;
        }, artifacts, recipes, [&](std::shared_ptr<ea::Adapter> adapter) {
            safety = std::make_shared<CpuOnlySafety>(); safety->adapter = std::move(adapter); return safety;
        });
    common_params blocked; std::string startup_error;
    assert(!context.start_engine(blocked,"/not-opened",{},startup_error) && startup_error.find("native pipeline") != std::string::npos);
    assert(!context.load(blocked,startup_error) && startup_error.find("native startup") != std::string::npos);
    ctx.queue_tasks.on_new_task([&](server_task && task, bool yielding) { return ctx.process_new_task(std::move(task),yielding); });
    ctx.queue_tasks.on_update_slots([&] { ctx.update_slots(); });
    std::thread main_queue([&] { context.start_loop(); });
    assert(ctx.native_vision->recover());
    await([&] { return ctx.native_vision->available(); }); // READY drained with NO active slot
    auto metrics = [&] {
        const int id = ctx.queue_tasks.get_new_id(); ctx.queue_results.add_waiting_task_id(id);
        server_task task(SERVER_TASK_TYPE_METRICS); task.id = id; ctx.queue_tasks.post(std::move(task));
        server_task_result_ptr result;
        await([&] { result = ctx.queue_results.recv_with_timeout({id},1); return bool(result); });
        auto * value = dynamic_cast<server_task_result_metrics *>(result.get()); assert(value);
        auto counts = std::pair{value->n_processing_slots,value->n_tasks_deferred};
        ctx.queue_results.remove_waiting_task_id(id); return counts;
    };
    struct Response { int status; bool streaming; std::string data; };
    auto request = [&](std::string text, bool streaming, std::shared_ptr<std::atomic<bool>> disconnected) {
        return std::async(std::launch::async,[&,text=std::move(text),streaming,disconnected] {
            std::function<bool()> stop = [disconnected] { return disconnected->load(); };
            server_http_req req{{},{},"/v1/chat/completions","","",{},stop};
            json data{{"prompt",text},{"n_predict",1},{"temperature",0},{"stream",streaming}};
            auto result = ctx.handle_completions(req,data,sources,TASK_RESPONSE_TYPE_OAI_CMPL,plain);
            Response response{result->status,result->is_stream(),{}};
            if (!result->is_stream()) response.data = result->data;
            else {
                for (;;) {
                    std::string chunk; bool more = result->next(chunk); response.data += chunk;
                    if (!more) break;
                }
            }
            return response; // req/stop remain alive until result destruction
        });
    };
    auto live = [] { return std::make_shared<std::atomic<bool>>(false); };
    // Typed header rejection after a good PREPARE is a pre-SSE input error,
    // not an empty GEN, a retry, or a service outage. Repeat on the same bridge.
    auto bad_sources = sources;
    const auto short_webp = pr6_fixtures::webp(pr6_fixtures::Bytes{0,0,255,255,0,255,0,255,255,0,0,255});
    bad_sources[1].assign(short_webp.begin(),short_webp.begin()+12);
    for (bool streaming : {false,true}) {
        const auto decoded = calls('D'), files = calls('F');
        auto rejected = std::async(std::launch::async,[&,streaming] {
            std::function<bool()> stop = [] { return false; };
            server_http_req req{{},{},"/v1/chat/completions","","",{},stop};
            auto result = ctx.handle_completions(req,json{{"prompt",prompt},{"n_predict",1},{"stream",streaming}},
                bad_sources,TASK_RESPONSE_TYPE_OAI_CMPL,plain);
            return Response{result->status,result->is_stream(),result->data};
        });
        assert(rejected.wait_for(30s) == std::future_status::ready);
        const auto value = rejected.get(); assert(value.status == 400 && !value.streaming);
        assert(calls('D') == decoded+1 && calls('F') == files && encodes() == before_encode && calls('G') == before_commands);
        assert(ctx.native_vision->available() && !ctx.native_vision->recover() && safety->leased && !safety->revoked);
        await([&] { return metrics() == std::pair{0,0}; });
    }
    std::cout << "PR6_NATIVE_TYPED_QUEUE_PASS repeated=2 pre_sse_400=1 encode=0 engine=0 recovery=none\n";
    auto a = request(prompt,false,live());
    await([&] { return ::access((gate+"/entered").c_str(),F_OK) == 0; });
    std::string oversized = prompt; for (unsigned i = 0; i < 4000; ++i) oversized += " xyz";
    auto b = request(oversized,true,live()); // context refusal must remain pre-SSE 400
    auto c = request(prompt,true,live());
    auto disconnect_queued = live(); auto cancelled_queued = request(prompt,false,disconnect_queued);
    await([&] { return metrics() == std::pair{1,3}; }); // actual deferral, no sleeps
    disconnect_queued->store(true);
    assert(cancelled_queued.wait_for(5s) == std::future_status::ready); (void)cancelled_queued.get();
    await([&] { return metrics() == std::pair{1,2}; });
    assert(calls('G') == before_commands+1 && encodes() == before_encode+3);
    { std::ofstream release(gate+"/release"); release << '1'; }
    assert(a.wait_for(30s) == std::future_status::ready && b.wait_for(30s) == std::future_status::ready && c.wait_for(30s) == std::future_status::ready);
    auto ra = a.get(), rb = b.get(), rc = c.get();
    assert(ra.status == 200 && rb.status == 400 && !rb.streaming && rc.status == 200 && rc.streaming);
    assert(rc.data.find("[DONE]") != std::string::npos);
    assert(encodes() == before_encode+6 && calls('G') == before_commands+2); // no B/queued-cancel encode or GEN

    // Client observer cancels an outstanding real ENCODE, independently of the
    // queue. The test holds only after real encoding; parent must kill/reap it.
    { std::ofstream hold(gate+"/hold"); hold << '1'; }
    auto disconnect_active = live(); const auto encoded_before_cancel = encodes();
    auto cancelled_active = request(prompt,false,disconnect_active);
    await([&] { return encodes() > encoded_before_cancel; });
    auto follower = request(prompt,false,live());
    await([&] { return metrics() == std::pair{1,1}; });
    disconnect_active->store(true);
    assert(cancelled_active.wait_for(5s) == std::future_status::ready); (void)cancelled_active.get();
    assert(follower.wait_for(15s) == std::future_status::ready && follower.get().status == 503);
    await([&] { return metrics() == std::pair{0,0}; });
    assert(calls('G') == before_commands+2 && !ctx.native_vision->available());
    ::unlink((gate+"/hold").c_str());
    std::this_thread::sleep_for(60ms); // injected explicit recovery cooldown, not a race assertion
    assert(ctx.native_vision->recover()); await([&] { return ctx.native_vision->available(); });
    auto recovered = request(prompt,false,live());
    assert(recovered.wait_for(30s) == std::future_status::ready && recovered.get().status == 200);
    assert(calls('G') == before_commands+3);

    // After GENI: STOP once, drain residual output, retain the old slot/files,
    // and do not prepare or generate the deferred successor before DONE.
    const auto before_engine_cancel = encodes(), before_stop = calls('S');
    { std::ofstream cancel(gate+"/cancel"); cancel << '1'; }
    auto disconnect_engine = live(); auto active_engine = request(prompt,false,disconnect_engine);
    await([&] { return ::access((gate+"/active").c_str(),F_OK) == 0; });
    auto after_engine = request(prompt,false,live());
    await([&] { return metrics() == std::pair{1,1}; });
    disconnect_engine->store(true);
    assert(active_engine.wait_for(5s) == std::future_status::ready); (void)active_engine.get();
    await([&] { return ::access((gate+"/draining").c_str(),F_OK) == 0; });
    assert(calls('S') == before_stop+1 && encodes() == before_engine_cancel+3);
    assert(metrics() == std::pair(1,1) && after_engine.wait_for(100ms) == std::future_status::timeout);
    ::unlink((gate+"/cancel").c_str());
    { std::ofstream release(gate+"/drain-release"); release << '1'; }
    assert(after_engine.wait_for(30s) == std::future_status::ready && after_engine.get().status == 200);
    assert(calls('G') == before_commands+5 && encodes() == before_engine_cancel+6);

    // A parent output-limit STOP is not request cancellation: a native cancel
    // terminal still yields one successful final result after certified drain.
    { std::ofstream soft(gate+"/soft"); soft << '1'; }
    auto normal_stop = request(prompt,false,live());
    assert(normal_stop.wait_for(30s) == std::future_status::ready && normal_stop.get().status == 200);
    assert(calls('S') == before_stop+2 && calls('G') == before_commands+6);
    // Same resident supervisor/engine: plain/literal text keeps exact legacy
    // IDs and GEN, with no source decode, projector encode or SVE1 attempt.
    ::unlink((gate+"/soft").c_str());
    auto text_request = [&](json text, std::vector<std::pair<size_t,size_t>> spans, bool streaming,
                            std::shared_ptr<std::atomic<bool>> disconnected) {
        return std::async(std::launch::async,[&,text=std::move(text),spans=std::move(spans),streaming,disconnected] {
            std::function<bool()> stop = [disconnected] { return disconnected->load(); };
            server_http_req req{{},{},"/v1/completions","","",{},stop};
            auto result = ctx.handle_completions(req,json{{"prompt",text},{"n_predict",1},{"temperature",0},{"stream",streaming}},
                {},TASK_RESPONSE_TYPE_OAI_CMPL,spans);
            Response response{result->status,result->is_stream(),{}};
            if (!result->is_stream()) response.data = result->data;
            else for (;;) { std::string chunk; bool more = result->next(chunk); response.data += chunk; if (!more) break; }
            return response;
        });
    };
    auto expect_text = [&](const llama_tokens & ids) {
        std::ofstream expected(gate+"/expected-text");
        for (size_t i = 0; i < ids.size(); ++i) { if (i) expected << ','; expected << ids[i]; }
        expected << '\n'; assert(expected.good());
    };
    const llama_tokens raw_ids{42,43,44}; expect_text(raw_ids);
    // Also defer text behind a real GENI cancellation, not just after an idle
    // image completion. No text GEN or extra file may overtake its native DONE.
    ::unlink((gate+"/active").c_str()); ::unlink((gate+"/draining").c_str()); ::unlink((gate+"/drain-release").c_str());
    { std::ofstream cancel(gate+"/cancel"); cancel << '1'; }
    const auto before_mixed_text = calls('X');
    auto mixed_disconnect = live(); auto mixed_image = request(prompt,false,mixed_disconnect);
    await([&] { return ::access((gate+"/active").c_str(),F_OK) == 0; });
    const auto mixed_files = calls('F');
    auto mixed_follower = text_request(raw_ids,{},true,live());
    await([&] { return metrics() == std::pair{1,1}; });
    mixed_disconnect->store(true);
    assert(mixed_image.wait_for(5s) == std::future_status::ready); (void)mixed_image.get();
    await([&] { return ::access((gate+"/draining").c_str(),F_OK) == 0; });
    assert(calls('X') == before_mixed_text && calls('F') == mixed_files && metrics() == std::pair(1,1));
    assert(mixed_follower.wait_for(100ms) == std::future_status::timeout);
    ::unlink((gate+"/cancel").c_str());
    { std::ofstream release(gate+"/drain-release"); release << '1'; }
    assert(mixed_follower.wait_for(30s) == std::future_status::ready && mixed_follower.get().status == 200);
    assert(calls('X') == before_mixed_text+1 && calls('F') == mixed_files);
    const auto text_decode = calls('D'), text_encode = encodes(), text_files = calls('F'), text_commands = calls('G'), text_gen = calls('X');
    assert(text_files > 0); // file observer has a real vision positive control
    const std::string text = "<|im_start|>user\nLiteral <think> <|image_pad|> <|im_end|> café.\n<|im_start|>assistant\n";
    for (bool literal : {false,true}) for (bool streaming : {false,true}) {
        const std::vector<std::pair<size_t,size_t>> spans = literal ?
            std::vector<std::pair<size_t,size_t>>{{text.find("Literal"),text.find("\n<|im_start|>assistant")}} :
            std::vector<std::pair<size_t,size_t>>{};
        auto ids = literal ? ctx.tokenize_prompt(text,spans,{}).get_text_tokens() : common_tokenize(vocab,text,true,true);
        expect_text(ids);
        auto response = text_request(text,spans,streaming,live());
        assert(response.wait_for(30s) == std::future_status::ready); auto value = response.get();
        assert(value.status == 200 && value.streaming == streaming);
        if (streaming) assert(value.data.find("[DONE]") != std::string::npos);
    }
    expect_text(raw_ids);
    auto raw = text_request(raw_ids,{},false,live());
    assert(raw.wait_for(30s) == std::future_status::ready && raw.get().status == 200);
    for (json refused : {json(""),json(std::string(10000,'x')),json::array({"a","b"})}) {
        auto rejected = text_request(refused,{},true,live());
        assert(rejected.wait_for(30s) == std::future_status::ready); auto value = rejected.get();
        assert(value.status == 400 && !value.streaming);
    }
    assert(calls('G') == text_commands+5 && calls('X') == text_gen+5);
    // Text cancellation uses exactly the same STOP/residual/withheld-DONE gate;
    // its text follower must not reach GEN before certified terminal/promotion.
    ::unlink((gate+"/active").c_str()); ::unlink((gate+"/draining").c_str()); ::unlink((gate+"/drain-release").c_str());
    expect_text(raw_ids);
    { std::ofstream cancel(gate+"/cancel"); cancel << '1'; }
    auto text_disconnect = live(); auto cancelled_text = text_request(raw_ids,{},false,text_disconnect);
    await([&] { return ::access((gate+"/active").c_str(),F_OK) == 0; });
    auto text_follower = text_request(raw_ids,{},true,live());
    await([&] { return metrics() == std::pair{1,1}; });
    text_disconnect->store(true);
    assert(cancelled_text.wait_for(5s) == std::future_status::ready); (void)cancelled_text.get();
    await([&] { return ::access((gate+"/draining").c_str(),F_OK) == 0; });
    assert(calls('G') == text_commands+6 && calls('S') == before_stop+4);
    assert(metrics() == std::pair(1,1) && text_follower.wait_for(100ms) == std::future_status::timeout);
    ::unlink((gate+"/cancel").c_str());
    { std::ofstream release(gate+"/drain-release"); release << '1'; }
    assert(text_follower.wait_for(30s) == std::future_status::ready && text_follower.get().status == 200);
    assert(calls('G') == text_commands+7 && calls('X') == text_gen+7);
    assert(calls('D') == text_decode && encodes() == text_encode && calls('F') == text_files);
    // PR6 user-visible recovery: same acquired sources/job; fresh role-aware
    // template/literal composition. Compare engine IDs/keys/rows to a corrected
    // request, not merely a fixed-token success probe.
    auto image_part = [](const raw_buffer & bytes) {
        std::string url = "data:image/octet-stream;base64,";
        base64::encode(bytes.begin(),bytes.end(),std::back_inserter(url));
        return json{{"type","image_url"},{"image_url",{{"url",url}}}};
    };
    const raw_buffer broken(short_webp.begin(),short_webp.begin()+12);
    auto chat_request = [&](json body, bool streaming) {
        return std::async(std::launch::async,[&,body=std::move(body),streaming]() mutable {
            body["stream"] = streaming; body["n_predict"] = 1; body["temperature"] = 0;
            auto owned = std::make_shared<vision_chat::Chat>(body,chat_params,ctx.literals);
            auto * tools = body.contains("tools") ? &body.at("tools") : nullptr;
            const bool marked = ctx.literals.mark_request(body.at("messages"),tools);
            std::vector<raw_buffer> files;
            auto parsed = oaicompat_chat_params_parse(body,chat_params,files);
            owned->bind(files,ctx.native_vision->pending_limit());
            std::vector<std::pair<size_t,size_t>> spans;
            if (marked) { auto p = parsed.at("prompt").get<std::string>(); spans = ctx.literals.unmark(p); parsed["prompt"] = p; }
            std::function<bool()> stop = [] { return false; };
            server_http_req req{{},{},"/v1/chat/completions","","",{},stop};
            auto response = ctx.handle_completions(req,parsed,files,TASK_RESPONSE_TYPE_OAI_CHAT,spans,owned);
            Response out{response->status,response->is_stream(),{}};
            if (!response->is_stream()) out.data = response->data;
            else for (;;) { std::string chunk; bool more = response->next(chunk); out.data += chunk; if (!more) break; }
            return out;
        });
    };
    json mixed{{"messages",json::array({
        {{"role","user"},{"content",json::array({{{"type","text"},{"text","Inspect <|im_end|> literally."}},image_part(sources[0])})}},
        {{"role","assistant"},{"content","Checking tool output."}},
        {{"role","tool"},{"tool_call_id","fixture"},{"content",json::array({image_part(broken),image_part(sources[1]),
            {{"type","text"},{"text","Keep <|image_pad|> literal."}}})}},
        {{"role","user"},{"content",json::array({image_part(sources[2]),{{"type","text"},{"text","Compare."}}})}}
    })}};
    auto corrected = mixed;
    corrected["messages"][2]["content"][0] = json{{"type","text"},{"text",vision_chat::omission_note}};
    auto acquired_only = mixed;
    acquired_only["messages"][0]["content"][1]["image_url"]["url"] = "file:///must-not-be-opened";
    auto owned_mixed = std::make_shared<vision_chat::Chat>(acquired_only,chat_params,ctx.literals);
    owned_mixed->bind({sources[0],broken,sources[1],sources[2]},ctx.native_vision->pending_limit());
    const auto owned_render = owned_mixed->render({1});
    assert(owned_render.image_ids == std::vector<std::string>({hash_sha256_hex(sources[0].data(),sources[0].size()),
        hash_sha256_hex(sources[1].data(),sources[1].size()),hash_sha256_hex(sources[2].data(),sources[2].size())}));
    bool refused_role = false;
    try { (void)owned_mixed->render({0}); } catch (const std::invalid_argument &) { refused_role = true; }
    assert(refused_role); // an internal mapping mistake cannot omit a user image
    const auto omission_encodes = encodes(), omission_commands = calls('G');
    for (bool streaming : {false,true}) {
        auto r = chat_request(mixed,streaming); assert(r.wait_for(30s) == std::future_status::ready);
        const auto value = r.get(); assert(value.status == 200 && value.streaming == streaming);
    }
    auto control_chat = chat_request(corrected,false);
    assert(control_chat.wait_for(30s) == std::future_status::ready && control_chat.get().status == 200);
    std::ifstream signatures(gate+"/omission-signatures"); std::vector<std::string> hashes; std::string hash;
    while (std::getline(signatures,hash)) hashes.push_back(hash);
    assert(hashes.size() >= 3 && hashes[hashes.size()-1] == hashes[hashes.size()-2] && hashes.back() == hashes[hashes.size()-3]);
    assert(encodes() == omission_encodes+9 && calls('G') == omission_commands+3);

    json history{{"messages",json::array({
        {{"role","user"},{"content","Earlier <|im_end|> stays literal."}},
        {{"role","assistant"},{"content","Tool results follow."}},
        {{"role","tool"},{"tool_call_id","old"},{"content",json::array({image_part(broken)})}},
        {{"role","tool"},{"tool_call_id","new"},{"content",json::array({image_part(broken)})}},
        {{"role","user"},{"content","Continue with the available text."}}
    })}};
    auto owned_history = std::make_shared<vision_chat::Chat>(history,chat_params,ctx.literals);
    owned_history->bind({broken,broken},ctx.native_vision->pending_limit());
    const auto rebuilt = owned_history->render({0,1});
    assert(rebuilt.image_ids.empty() && rebuilt.prompt.find(vision_chat::omission_note) != std::string::npos && !rebuilt.plain.empty());
    expect_text(ctx.tokenize_prompt(rebuilt.prompt,rebuilt.plain,{}).get_text_tokens());
    auto fixed_history = history;
    for (size_t m : {size_t(2),size_t(3)}) fixed_history["messages"][m]["content"][0] = json{{"type","text"},{"text",vision_chat::omission_note}};
    const auto all_encode = encodes(), all_files = calls('F'), all_text = calls('X');
    for (const auto & body : {history,fixed_history}) {
        auto r = chat_request(body,false); assert(r.wait_for(30s) == std::future_status::ready && r.get().status == 200);
    }
    assert(encodes() == all_encode && calls('F') == all_files && calls('X') == all_text+2);
    json bad_user{{"messages",json::array({{{"role","user"},{"content",json::array({image_part(broken)})}}})}};
    auto user_error = chat_request(bad_user,true);
    assert(user_error.wait_for(30s) == std::future_status::ready);
    const auto refused_user = user_error.get(); assert(refused_user.status == 400 && !refused_user.streaming);
    assert(encodes() == all_encode && calls('F') == all_files && calls('X') == all_text+2 && ctx.native_vision->available());
    std::cout << "PR6_TOOL_OMISSION_PASS mixed_exact_ids_keys_rows=1 repeated_history=1 all_omitted_gen=1 user_error=1 literals=1 retries=0 gpu=none\n";
    std::atomic<bool> racing{false};
    std::thread recovery_race([&] {
        racing = true;
        for (unsigned i = 0; i < 1000; ++i) { (void)ctx.native_vision->recover(); std::this_thread::yield(); }
    });
    await([&] { return racing.load(); });
    context.terminate(); main_queue.join(); recovery_race.join();
    assert(!ctx.slot.active() && ctx.metrics.predict.count == 17 && ctx.queue_tasks.queue_tasks_deferred_size() == 0);
    ctx.native_vision.reset();
    assert(safety->adapter->stopped() && !safety->leased && !safety->revoked);
    assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0);
    for (const auto * file : {"entered","release","active","draining","drain-release","soft","expected-text","omission-signatures"}) ::unlink((gate+"/"+file).c_str());
    assert(::rmdir(gate.c_str()) == 0);
    std::cout << "PR4_NATIVE_QUEUE_PASS handler=real queue=real slot_results=real pre_sse_context=1 queued_cancel=1 encode_disconnect=1 recovery_success=1 engine_disconnect_drain=1 normal_stop=1 text_gen_parity=1 text_cancel_drain=1 gpu=none\n";
}
