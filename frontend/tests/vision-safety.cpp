// Pure CPU binding test: LD_LIBRARY_PATH MUST name the local fake NVML fixture.
#include "../vision_safety.hpp"
#include <cassert>
#include <iostream>
using namespace strata_frontend;
using namespace std::chrono_literals;
struct Sink final : vision_engine_adapter::Sink {
    bool line(vision_engine_handoff::Identity,const EngineLine &) noexcept override { return true; }
    bool info(std::string_view) noexcept override { return true; }
};
int main(int argc,char ** argv) {
    if (argc==2 && std::string(argv[1])=="engine") {
        std::cout << "READY 4096 stop\n" << std::flush;
        std::string cmd; while (std::getline(std::cin,cmd)) if (cmd=="QUIT") return 0;
        return 1;
    }
    assert(argc==2);
    void * mock=::dlopen("libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL); assert(mock);
    auto set=reinterpret_cast<void(*)(unsigned,unsigned,unsigned)>(::dlsym(mock,"pr4_mock_contexts"));
    auto xid=reinterpret_cast<void(*)()>(::dlsym(mock,"pr4_mock_xid")); assert(set && xid);
    namespace ea=vision_engine_adapter;
    auto engine=std::make_shared<ea::Adapter>(ea::Launch{argv[1],argv[0],{argv[0],"engine"},{}},
        ea::Limits{65536,1024,4096,16,1024,2s,1s,1s},2,std::make_shared<Sink>());
    vision_safety::Exclusive safety(engine,true);
    const vision_engine_handoff::Identity id{1,1,1,1}; const std::atomic<bool> never{false};
    set(900001,0,0); assert(!safety.acquire(1)); // never borrow/disrupt an occupied device
    set(0,0,0); assert(safety.acquire(1));
    safety.reader_spawned(17); // namespace PID deliberately NOT the host PID below
    set(100001,0,0); assert(safety.to_engine(id));
    assert(engine->initialize(vision_job_io::after(2s),never));
    set(100001,200002,0); assert(safety.to_reader(id));
    assert(!safety.certify(1,true,false)); // reaping alone cannot refund a remaining GPU context
    set(200002,0,0); assert(safety.certify(1,true,false)); safety.release(1);
    assert(safety.acquire(1) && safety.to_engine(id) && safety.to_reader(id)); // readerless text, same engine context
    assert(safety.certify(1,false,false)); safety.release(1);
    assert(safety.acquire(2)); safety.reader_spawned(23);
    set(200002,300003,0); assert(safety.to_engine(id) && safety.to_reader(id));
    set(200002,300003,400004); assert(!safety.certify(2,true,false)); // unknown/foreign context fails closed
    set(200002,0,0); assert(safety.certify(2,true,false)); safety.release(2);
    assert(engine->shutdown(vision_job_io::after(2s)));
    assert(!safety.engine_stopped()); // old engine context still registered after waitpid
    set(0,0,0); assert(safety.engine_stopped());
    xid(); assert(!safety.healthy() && !safety.healthy() && !safety.acquire(3)); // sticky XID even after event consumed
    auto wait_result=reinterpret_cast<void(*)(int)>(::dlsym(mock,"pr4_mock_wait_result")); assert(wait_result);
    vision_safety::Exclusive observation(engine,true);
    assert(observation.healthy()); // real ABI timeout (10) is normal
    wait_result(6); assert(!observation.healthy()); // NOT_FOUND is observation failure
    wait_result(10); assert(!observation.healthy()); // denial remains sticky
    ::dlclose(mock);
    std::cout << "vision safety binding: PASS (CPU fake NVML; namespace IDs, context retention, XID; NOT GPU proof)\n";
}
