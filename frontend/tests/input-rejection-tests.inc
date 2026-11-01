// No-model PR6 typed PREPARE/cleanup tests, included by supervisor-job harness.
static vision_payload::Binding input_binding(unsigned occurrence, uint64_t handle = 0) {
    auto b = binding(occurrence,handle); b.source[0] = uint8_t(occurrence); return b;
}
static unsigned input_requests(const std::string & mode) {
    return mode == "repeat" ? 3 : mode == "first" || mode == "second" || mode == "live-waiter" ? 2 : 1;
}
static int input_fake(const std::string & mode, uint64_t instance) {
    wire::Channel channel(3);
    auto send = [&](wire::Kind kind, uint64_t id, uint64_t seq, unsigned occurrence = 0) {
        vision_commands::Payload p;
        const auto meta = metadata(); const std::string token(4,'\0');
        const uint64_t handle = 1000 + id*2 + occurrence;
        assert(vision_commands::build(kind,config,caps,input_binding(occurrence,handle),p,
            {handle,7,7,token,token,meta},kind == wire::Kind::error ? 1 : 0,
            vision_payload::InputError::webp_header) == vision_commands::Result::ok);
        if (kind == wire::Kind::input_rejected && mode == "unknown") p.bytes.back() = 1;
        if (kind == wire::Kind::input_rejected && mode == "binding") p.bytes[32] ^= 1;
        if (kind == wire::Kind::released && (mode == "release-stale" || mode == "proof-blocked")) ++seq;
        int extra = -1;
        if (kind == wire::Kind::input_rejected && mode == "fd") { extra = ::open("/dev/null",O_RDONLY|O_CLOEXEC); assert(extra >= 0); }
        const auto result = channel.send_borrowed({kind,uint32_t(p.bytes.size()),instance,id,seq},p.bytes.data(),p.bytes.size(),
            extra >= 0 ? &extra : nullptr,extra >= 0 ? 1 : 0,caps.payload_bytes,deadline(),never);
        if (extra >= 0) ::close(extra);
        assert(result == wire::IoResult::ok);
    };
    auto receive = [&](wire::Kind kind, uint64_t id, uint64_t seq, unsigned occurrence) {
        wire::Frame f;
        assert(channel.receive(f,{kind,instance,id,seq,kind == wire::Kind::prepare ? 1u : 0u},
            caps.payload_bytes,deadline(),never) == wire::IoResult::ok);
        vision_payload::View v;
        assert(vision_payload::validate(kind,{reinterpret_cast<const char *>(f.payload.data()),f.payload.size()},f.fd_count,
            config,caps,input_binding(occurrence,1000+id*2+occurrence),v));
    };
    send(wire::Kind::ready,0,0);
    for (unsigned request = 0; request < input_requests(mode); ++request) {
        const uint64_t id = 11+request; uint64_t seq = 0;
        const bool reject = mode == "repeat" ? request < 2 : request == 0;
        unsigned prepared = 0;
        for (unsigned i = 0; i < 2; ++i) {
            receive(wire::Kind::prepare,id,++seq,i);
            if (reject && i == (mode == "first" ? 0u : 1u)) {
                if (mode == "cancel-before") { signal_gate("prepare"); blocked(); }
                send(mode == "class-one" ? wire::Kind::error : wire::Kind::input_rejected,id,seq,i);
                if (mode == "class-one" || mode == "unknown" || mode == "binding" || mode == "fd") blocked();
                break;
            }
            send(wire::Kind::prepared,id,seq,i); ++prepared;
        }
        for (unsigned i = 0; i < prepared; ++i) {
            receive(wire::Kind::release,id,++seq,i);
            if (reject && (mode == "release-timeout" || mode == "prepare-budget" || mode == "shutdown" || mode == "live-waiter")) {
                signal_gate("release");
                if (mode == "release-timeout" || mode == "prepare-budget" || mode == "shutdown") blocked();
                const auto path = std::string(std::getenv("PR4_TEST_GATE"))+".resume";
                wait_for([&] { return ::access(path.c_str(),F_OK) == 0; });
            }
            send(mode == "release-error" ? wire::Kind::error : wire::Kind::released,id,seq,i);
            if (mode == "release-error" || mode == "release-stale" || mode == "proof-blocked") blocked();
        }
    }
    // Keep the reader alive. Supervisor owns teardown; no automatic request retry.
    blocked();
}
static void input_scenario(const char * executable, const char * guard, const std::string & mode) {
    const auto baseline = fd_count(); Gate gate; Events events;
    auto safety = std::make_shared<CpuOnlySafety>();
    auto artifacts = std::make_shared<art::Budget>(art::Usage{65536,16});
    auto recipes = std::make_shared<io::RecipeBudget>(io::RecipeBudget::Usage{65536,8});
    const bool healthy = mode == "first" || mode == "second" || mode == "repeat" || mode == "live-waiter";
    {
        vs::Launch launch{guard,executable,{executable,"input-fake",mode},{"PR4_TEST_GATE="+gate.file},100,config,caps};
        vs::Supervisor supervisor(launch,lifecycle,2,safety,events.inbox);
        assert(supervisor.start(1)); events.take(control::Event::Kind::ready,1);
        for (unsigned request = 0; request < input_requests(mode); ++request) {
            auto owner = std::make_shared<control::Request>(deadline());
            auto follower = std::make_shared<control::Request>(deadline());
            const io::Identity id{20+request,supervisor.instance(),11+request};
            auto s = submission(id.job,owner,artifacts,recipes);
            s->inputs.clear();
            vision_cache::Flights flights(2,4);
            std::vector<vision_cache::Flights::Held> follower_tickets;
            for (unsigned i = 0; i < 2; ++i) {
                auto b = input_binding(i); art::Artifact::Ptr source;
                assert(art::Artifact::source(*artifacts,{0,id.job,i,b.source},"source",caps.source_bytes,deadline(),never,source) == art::Result::ok);
                s->inputs.push_back({std::move(source),"pr4-fixture"});
                if (mode == "live-waiter" && request == 0) {
                    s->flights.push_back(flights.join({config,b.source},owner));
                    follower_tickets.push_back(flights.join({config,b.source},follower));
                    assert(s->flights.back() && follower_tickets.back());
                }
            }
            if (!s->flights.empty()) {
                s->cache = std::make_shared<vision_cache::Cache>(2,65536,artifacts,recipes);
                s->work_control = std::make_shared<vision_cache::Interest>(s->flights);
            }
            if (mode == "release-timeout") s->cleanup = 100ms;
            if (mode == "prepare-budget") s->prepare = 100ms; // cleanup must not renew PREPARE
            if (mode == "proof-blocked") safety->proof = false;
            assert(supervisor.prepare(id.task,s) && !s);
            if (mode == "cancel-before") { gate.await("prepare"); owner->cancel(control::Reason::disconnect); }
            if (mode == "shutdown") { gate.await("release"); supervisor.shutdown(); }
            if (mode == "live-waiter" && request == 0) {
                gate.await("release"); owner->cancel(control::Reason::disconnect); gate.resume();
            }
            if (mode == "proof-blocked") {
                events.take(control::Event::Kind::failed,id.task);
                assert(supervisor.state() == vs::State::safety_blocked && !supervisor.terminal() && safety->leased);
                assert(artifacts->usage().bytes == 12 && !safety->releases);
                safety->proof = true; assert(supervisor.stop(99)); events.take(control::Event::Kind::stopped,99);
            } else if (healthy) {
                const bool rejected = mode == "repeat" ? request < 2 : request == 0;
                if (rejected) events.take(control::Event::Kind::completed,id.task);
                else {
                    events.take(control::Event::Kind::prepared,id.task);
                    assert(supervisor.prepared() && supervisor.admit(id,false));
                    events.take(control::Event::Kind::completed,id.task);
                }
            } else events.take(control::Event::Kind::failed,id.task);
            events.take(control::Event::Kind::job_terminal,id.task);
            auto terminal = supervisor.terminal(); assert(terminal);
            const bool typed = healthy && (mode == "repeat" ? request < 2 : request == 0) && mode != "live-waiter";
            if (typed) {
                assert(terminal->outcome == job::Outcome::input_error && !terminal->error_class && !terminal->timed_out);
                assert(terminal->input_failure && terminal->input_failure->occurrence == (mode == "first" ? 0u : 1u));
                assert(terminal->input_failure->reason == vision_payload::InputError::webp_header);
            } else {
                assert(!terminal->input_failure && terminal->outcome != job::Outcome::input_error);
                if (mode == "live-waiter" && request == 0) assert(terminal->outcome == job::Outcome::cancelled);
                if (mode == "class-one" || mode == "release-error") assert(terminal->error_class == 1);
                if (mode == "release-timeout" || mode == "prepare-budget") assert(terminal->timed_out);
            }
            assert(!supervisor.prepared() && !supervisor.encoded() && !supervisor.admit(id,true));
            assert(supervisor.acknowledge(id) && !supervisor.acknowledge(id));
            assert(artifacts->usage().bytes == 0 && recipes->usage().bytes == 0 && !safety->revoked);
            if (healthy) assert(supervisor.instance() == 100 && supervisor.attempts() == 1 && safety->leased && !safety->releases);
            if (mode == "live-waiter") {
                assert(!follower->observe());
                for (const auto & ticket : follower_tickets) assert(!ticket->flight->result());
            }
        }
    }
    events.take(control::Event::Kind::stopped,0); events.empty();
    assert(!safety->leased && safety->releases == 1 && !safety->revoked);
    events.inbox->stop_accepting(); assert(events.inbox->settled());
    assert(artifacts->usage().bytes == 0 && recipes->usage().snapshots == 0 && fd_count() == baseline);
}
