// Explicitly authorized PR6-only maintenance qualification. C++ fixtures/client.
// Never logs credentials, source bytes, prompts or assistant text. No downloads.
#include <nlohmann/json.hpp>
#include <cpp-httplib/httplib.h>
#include "codec-test-fixtures.hpp"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
#include "base64.hpp"
#include <openssl/evp.h>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sstream>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
extern char ** environ;
namespace fs = std::filesystem;
using J = nlohmann::json;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using namespace pr6_fixtures;
static const std::string GPU="GPU-d14daf49-45c8-58c5-cf61-5a40342672af", PROD="unbiased-strata-5070ti", NAME="ub-pr6-gpu-test";
static constexpr int PORT=18086;
struct Failure : std::runtime_error { explicit Failure(const char * fixed):std::runtime_error(fixed) {} };
static void need(bool ok,const char * fixed) { if(!ok) throw Failure(fixed); }
static std::string read_file(const fs::path & path) { std::ifstream f(path,std::ios::binary); need(bool(f),"required file unavailable"); return {std::istreambuf_iterator<char>(f),{}}; }
static std::string hash_bytes(const void * p,size_t n) {
    unsigned char out[32]; unsigned size=0; auto * c=EVP_MD_CTX_new(); need(c,"hash allocation failed");
    const bool ok=EVP_DigestInit_ex(c,EVP_sha256(),nullptr)==1 && EVP_DigestUpdate(c,p,n)==1 && EVP_DigestFinal_ex(c,out,&size)==1 && size==32;
    EVP_MD_CTX_free(c); need(ok,"hash failed"); static const char hex[]="0123456789abcdef"; std::string s;
    for(auto b:out) { s+=hex[b>>4]; s+=hex[b&15]; } return s;
}
static std::string hash_file(const fs::path & path) {
    std::ifstream f(path,std::ios::binary); need(bool(f),"hash input unavailable");
    auto * c=EVP_MD_CTX_new(); need(c && EVP_DigestInit_ex(c,EVP_sha256(),nullptr)==1,"hash init failed");
    char buffer[1<<20]; while(f.read(buffer,sizeof buffer) || f.gcount()) need(EVP_DigestUpdate(c,buffer,size_t(f.gcount()))==1,"hash read failed");
    unsigned char out[32]; unsigned n=0; need(f.eof() && EVP_DigestFinal_ex(c,out,&n)==1 && n==32,"hash finish failed"); EVP_MD_CTX_free(c);
    return [&] { static const char h[]="0123456789abcdef"; std::string s; for(auto b:out) { s+=h[b>>4];s+=h[b&15]; } return s; }();
}
static std::string command(const std::vector<std::string> & args,int seconds=120) {
    int pipes[2]; need(::pipe2(pipes,O_CLOEXEC)==0,"command pipe failed");
    need(::fcntl(pipes[0],F_SETFL,O_NONBLOCK)==0,"command pipe mode failed");
    posix_spawn_file_actions_t actions; need(posix_spawn_file_actions_init(&actions)==0,"spawn actions failed");
    posix_spawn_file_actions_adddup2(&actions,pipes[1],1); posix_spawn_file_actions_adddup2(&actions,pipes[1],2);
    std::vector<char *> argv; for(const auto & s:args) argv.push_back(const_cast<char *>(s.c_str())); argv.push_back(nullptr);
    pid_t pid=-1; const int rc=posix_spawnp(&pid,argv[0],&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions); ::close(pipes[1]); if(rc) { ::close(pipes[0]); throw std::runtime_error("command spawn failed"); }
    std::string out; const auto until=Clock::now()+std::chrono::seconds(seconds); int status=0; bool done=false,eof=false;
    while(!done || !eof) {
        char b[16384]; auto n=::read(pipes[0],b,sizeof b);
        if(n>0) out.append(b,size_t(n)); else if(n==0) eof=true;
        if(!done) done=::waitpid(pid,&status,WNOHANG)==pid;
        if(Clock::now()>=until || out.size()>16*1024*1024) { ::kill(pid,SIGKILL); ::waitpid(pid,&status,0); ::close(pipes[0]); throw std::runtime_error("command deadline or size limit"); }
        if(!done || !eof) { pollfd p{pipes[0],POLLIN,0}; ::poll(&p,1,10); }
    }
    ::close(pipes[0]); need(WIFEXITED(status) && WEXITSTATUS(status)==0,"Docker command failed"); return out;
}
static std::string docker(std::vector<std::string> args,int seconds=120) { args.insert(args.begin(),"docker"); return command(args,seconds); }
static J inspect(const std::string & name) { return J::parse(docker({"inspect",name})).at(0); }
static std::string settings_hash(const J & p) { const auto s=J::array({p.at("Config"),p.at("HostConfig"),p.at("Mounts")}).dump(); return hash_bytes(s.data(),s.size()); }
template<class F> static void await(F f,int seconds=180) { const auto end=Clock::now()+std::chrono::seconds(seconds); while(!f()) { need(Clock::now()<end,"bounded wait expired"); std::this_thread::sleep_for(100ms); } }
class Driver {
    struct Proc { unsigned pid; unsigned long long bytes; unsigned gi,ci; };
    struct Event { void * device; unsigned long long type,data; unsigned gi,ci; };
    struct Memory { unsigned long long total,free,used; };
    void * lib_=nullptr,*device_=nullptr,*events_=nullptr;
    int (*process_)(void *,unsigned *,Proc *)=nullptr; int (*wait_)(void *,Event *,unsigned)=nullptr;
    int (*memory_)(void *,Memory *)=nullptr; int (*free_)(void *)=nullptr; int (*shutdown_)()=nullptr;
    template<class T> T symbol(const char * s) { auto p=reinterpret_cast<T>(::dlsym(lib_,s)); need(p,"NVML API unavailable"); return p; }
public:
    std::mutex mutex; bool fault=false; std::atomic<uint64_t> minimum_free{UINT64_MAX};
    Driver() {
        lib_=::dlopen("libnvidia-ml.so.1",RTLD_NOW|RTLD_LOCAL); need(lib_,"NVML unavailable");
        need(symbol<int(*)()>("nvmlInit_v2")()==0,"NVML init denied"); shutdown_=symbol<int(*)()>("nvmlShutdown");
        need(symbol<int(*)(const char *,void **)>("nvmlDeviceGetHandleByUUID")(GPU.c_str(),&device_)==0,"approved GPU unavailable");
        process_=symbol<int(*)(void *,unsigned *,Proc *)>("nvmlDeviceGetComputeRunningProcesses_v2");
        wait_=symbol<int(*)(void *,Event *,unsigned)>("nvmlEventSetWait_v2"); memory_=symbol<int(*)(void *,Memory *)>("nvmlDeviceGetMemoryInfo");
        free_=symbol<int(*)(void *)>("nvmlEventSetFree");
        need(symbol<int(*)(void **)>("nvmlEventSetCreate")(&events_)==0 &&
            symbol<int(*)(void *,unsigned long long,void *)>("nvmlDeviceRegisterEvents")(device_,8,events_)==0,"XID observation denied");
    }
    ~Driver() { if(events_) free_(events_); if(shutdown_) shutdown_(); if(lib_) ::dlclose(lib_); }
    std::map<unsigned,uint64_t> registry() {
        std::lock_guard<std::mutex> lock(mutex); Event event{};
        if(wait_(events_,&event,0)!=10) fault=true;
        need(!fault,"driver fault or observation failure"); Proc p[16]{}; unsigned n=16;
        need(process_(device_,&n,p)==0 && n<=16,"GPU registry unavailable");
        Memory m{}; need(memory_(device_,&m)==0,"GPU memory unavailable"); minimum_free.store(std::min<uint64_t>(minimum_free.load(),m.free));
        std::map<unsigned,uint64_t> out; for(unsigned i=0;i<n;++i) out.emplace(p[i].pid,p[i].bytes); return out;
    }
    void empty() { await([&] { return registry().empty(); },30); }
};
static J text(const std::string & s) { return {{"type","text"},{"text",s}}; }
static J image(const Bytes & b) { std::string uri="data:image/octet-stream;base64,"; base64::encode(b.begin(),b.end(),std::back_inserter(uri)); return {{"type","image_url"},{"image_url",{{"url",uri}}}}; }
static J body(J content,bool stream=false,int tokens=48) { return {{"messages",J::array({{{"role","user"},{"content",content}}})},{"max_tokens",tokens},{"temperature",0},{"stream",stream},{"chat_template_kwargs",{{"enable_thinking",false}}}}; }
static Bytes rgb_webp(const Bytes & rgb,int w,int h) { Bytes argb(size_t(w)*h*4); check(libyuv::RAWToARGB(rgb.data(),w*3,argb.data(),w*4,w,h)==0); return webp(argb,w,h); }
static Bytes solid(unsigned r,unsigned g,unsigned b,int w=512,int h=256) { Bytes rgb(size_t(w)*h*3); for(size_t i=0;i<rgb.size();i+=3) {rgb[i]=r;rgb[i+1]=g;rgb[i+2]=b;} return rgb_webp(rgb,w,h); }
struct Case { std::string name,wanted; Bytes source,reference; bool semantic; };
static std::vector<Case> corpus() {
    std::vector<Case> out; const int w=512,h=256; Bytes rgb(w*h*3);
    const unsigned colors[4][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,0}};
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int c=0;c<3;++c) rgb[size_t(y*w+x)*3+c]=colors[(y>=h/2)*2+(x>=w/2)][c];
    const auto plain=jpeg({},w,h,rgb); int x=0,y=0,n=0;
    std::unique_ptr<unsigned char,decltype(&stbi_image_free)> decoded(stbi_load_from_memory(plain.data(),int(plain.size()),&x,&y,&n,3),stbi_image_free);
    check(decoded && x==w && y==h); const Bytes baseline(decoded.get(),decoded.get()+w*h*3);
    const char * names[]={"red","green","blue","yellow"};
    for(auto order:{EXIF_BYTE_ORDER_INTEL,EXIF_BYTE_ORDER_MOTOROLA}) for(unsigned v=1;v<=8;++v) {
        const auto expected=orient_rgb(baseline,w,h,v); unsigned closest=0; uint64_t distance=UINT64_MAX;
        for(unsigned k=0;k<4;++k) { uint64_t d=0;for(unsigned c=0;c<3;++c) { const int delta=int(expected[c])-int(colors[k][c]);d+=delta*delta;}if(d<distance) {distance=d;closest=k;} }
        out.push_back({"exif-"+std::to_string(order)+"-"+std::to_string(v),names[closest],jpeg(metadata(v,order),w,h,rgb),
            rgb_webp(expected,v>=5?h:w,v>=5?w:h),order==EXIF_BYTE_ORDER_INTEL});
    }
    const auto red=solid(255,0,0),blue=solid(0,0,255);
    out.push_back({"webp-still","red",red,red,true});
    out.push_back({"webp-first-animation","red",animation(red,blue,w,h),red,true});
    const auto alpha=webp(Bytes{0,0,255,0,0,0,255,128,255,0,0,255});
    out.push_back({"webp-alpha","",alpha,rgb_webp(Bytes{255,255,255,255,127,127,0,0,255},3,1),false});
    Bytes transparent(size_t(w)*h*4,0);
    out.push_back({"webp-transparent-white","white",webp(transparent,w,h),solid(255,255,255),true});
    Bytes canvas(7*5*3,255); canvas[(2*7+3)*3+1]=canvas[(2*7+3)*3+2]=127;canvas[(2*7+4)*3]=canvas[(2*7+4)*3+1]=0;
    out.push_back({"webp-offset-canvas","",animation(alpha,webp(Bytes{0,255,0,255,0,255,0,255,0,255,0,255}),7,5,2),rgb_webp(canvas,7,5),false});
    out.push_back({"png-transparent-black","black",transparent_png,solid(0,0,0,1,1),true}); return out;
}
class Trial {
    fs::path root_,build_,tools_,out_; J prod_; std::string key_,original_,stage_; Driver driver_;
    bool active_=false,paused_=false; std::atomic<bool> sampling_{false},sample_failed_{false}; std::thread sampler_;
    std::map<fs::path,std::string> protected_; std::mutex record_mutex_;
public:
    Trial(fs::path root,fs::path build,fs::path tools,fs::path out):root_(std::move(root)),build_(std::move(build)),tools_(std::move(tools)),out_(std::move(out)) {
        need(!fs::exists(out_),"evidence directory already exists"); fs::create_directories(out_); fs::permissions(out_,fs::perms::owner_all);
        key_=read_file(root_/"secrets/strata-api-key"); while(!key_.empty() && std::isspace(static_cast<unsigned char>(key_.back()))) key_.pop_back(); need(key_.size()>=16,"credential file invalid");
        prod_=inspect(PROD); need(prod_["State"]["Running"].get<bool>() && prod_["State"]["Health"]["Status"]=="healthy","production not healthy");
        need(prod_["HostConfig"]["DeviceRequests"][0]["DeviceIDs"]==J::array({GPU}),"production GPU mismatch");
        const auto cmd=prod_["Config"]["Cmd"].get<std::vector<std::string>>(); need(std::find(cmd.begin(),cmd.end(),"--api-key")==cmd.end() && prod_["Config"]["Cmd"].dump().find(key_)==std::string::npos,"literal credential argument refused");
        need(docker({"ps","-aq","--filter","name=^"+NAME+"$"}).empty(),"candidate name already owned"); original_=settings_hash(prod_);
        for(const auto * f:{"docker-compose.yaml","docker-compose.yaml.bak"}) protected_[root_/f]=hash_file(root_/f);
        auto profile=J::parse(read_file(root_/"frontend/tests/data/vision-runtime-gpu-5070ti.json")); need(profile["gpu_uuid"]==GPU,"profile GPU mismatch");
        profile["reader_environment"]["LD_LIBRARY_PATH"]="/codecs/usr/lib/x86_64-linux-gnu:/opt/unbiased-strata/bin";
        profile["engine_environment"]["LD_LIBRARY_PATH"]="/opt/unbiased-strata/bin:/codecs/usr/lib/x86_64-linux-gnu";
        std::ofstream(out_/"profile.json")<<profile.dump(2)<<'\n'; profile["engine_environment"]["STRATA_FREEZE_EXPERTS"]="1";
        std::ofstream(out_/"poly-profile.json")<<profile.dump(2)<<'\n';
        J manifest{{"production_id",prod_["Id"]},{"production_image",prod_["Image"]},{"production_settings_sha",original_},{"gpu",GPU}};
        for(auto & p:protected_) manifest["compose"][p.first.filename().string()]=p.second;
        for(const char * f:{"unbiased-strata","strata-vision-reader","strata-process-guard","strata-poly"}) manifest["binaries"][f]=hash_file(build_/"bin"/f);
        for(const char * f:{"unbiased-strata","strata-vision-reader","strata-process-guard"}) manifest["phase_binaries"][f]=hash_file(tools_/"phase-bin"/f);
        for(const char * f:{"probe","pr6-gpu-acceptance"}) manifest["tools"][f]=hash_file(tools_/f);
        for(const auto & p:fs::directory_iterator(root_/"frontend")) if(p.is_regular_file() && (p.path().extension()==".cpp" || p.path().extension()==".hpp")) manifest["sources"][p.path().filename().string()]=hash_file(p.path());
        for(const char * f:{"normalization-gpu-acceptance.cpp","normalization-gpu-phase.cpp","codec-test-fixtures.hpp","row-cache-gpu-phase.cpp","vision-gpu-phases.inc","vision-gpu-probe.cpp"}) manifest["test_sources"][f]=hash_file(root_/"frontend/tests"/f);
        manifest["profile_sha"]=hash_file(out_/"profile.json"); manifest["poly_profile_sha"]=hash_file(out_/"poly-profile.json");
        manifest["dependencies"]=J::array(); for(const auto & p:fs::directory_iterator("/tmp/ub-pr6-deps/root/usr/lib/x86_64-linux-gnu")) if(p.is_regular_file() && !p.is_symlink() && p.path().string().find(".so")!=std::string::npos) manifest["dependencies"].push_back({{"file",p.path().filename().string()},{"sha256",hash_file(p.path())}});
        std::ofstream(out_/"manifest.json")<<manifest.dump(2)<<'\n';
    }
    ~Trial() { stop_sampler(); }
    void record(const std::string & name,J data=J::object()) {
        data["case"]=name; const auto s=data.dump(); need(s.find(key_)==std::string::npos,"sensitive evidence refused");
        std::lock_guard<std::mutex> lock(record_mutex_); std::ofstream f(out_/"results.jsonl",std::ios::app); f<<s<<'\n'; need(bool(f),"evidence write failed"); std::cout<<s<<std::endl;
    }
    std::pair<int,std::string> request(const std::string & path,const J * data=nullptr,int port=PORT,bool auth=true) {
        httplib::Client client("127.0.0.1",port); client.set_connection_timeout(5); client.set_read_timeout(900); client.set_write_timeout(120);
        httplib::Headers headers; if(auth) headers.emplace("Authorization","Bearer "+key_);
        auto result=data?client.Post(path,headers,data->dump(),"application/json"):client.Get(path,headers);
        need(bool(result) && result->body.size()<8*1024*1024,"HTTP request failed or oversized"); return {result->status,result->body};
    }
    std::pair<std::string,J> chat(J data,int port=PORT) {
        auto result=request("/v1/chat/completions",&data,port); need(result.first==200,"chat refused"); std::string answer;J usage;
        if(data.value("stream",false)) {
            need(result.second.find("data: [DONE]")!=std::string::npos,"incomplete SSE"); std::istringstream lines(result.second);std::string line;
            while(std::getline(lines,line)) if(line.rfind("data: {",0)==0) {auto j=J::parse(line.substr(6));need(!j.contains("error"),"SSE error");if(j.contains("usage"))usage=j["usage"];if(j.contains("choices")&&!j["choices"].empty()) {const auto & d=j["choices"][0]["delta"];if(d.contains("content")&&d["content"].is_string())answer+=d["content"].get<std::string>();}}
        } else {auto j=J::parse(result.second);answer=j["choices"][0]["message"].value("content",std::string());usage=j.value("usage",J::object());}
        need(!answer.empty(),"empty assistant response"); return {answer,usage};
    }
    static void color(const std::string & answer,const std::string & wanted) {std::string s=answer;for(auto & c:s)c=char(std::tolower(static_cast<unsigned char>(c)));need(s.find(wanted)!=std::string::npos,"real-engine color mismatch");}
    std::map<std::string,unsigned> counts() const {
        std::ifstream f(out_/stage_/"audit/phases"); std::map<std::string,unsigned> c; std::string line;
        while(std::getline(f,line)) {std::istringstream in(line);std::string time,pid,p;in>>time>>pid>>p;++c[p];}return c;
    }
    static void zero_reader(std::map<std::string,unsigned> a,std::map<std::string,unsigned> b) {for(const char * p:{"reader_normalize_begin","reader_prepare_begin","reader_encode_begin"})need(a[p]==b[p],"unexpected warm reader work");}
    void stop_sampler() {sampling_=false;if(sampler_.joinable())sampler_.join();}
    void resources() {
        need(!sample_failed_,"physical GPU observation failed"); const auto p=inspect(NAME); const auto pid=p["State"]["Pid"].get<unsigned>(); need(pid,"candidate already stopped");
        const auto cgline=read_file("/proc/"+std::to_string(pid)+"/cgroup"); const auto at=cgline.find("0::");need(at!=std::string::npos,"cgroup path unavailable");
        auto rel=cgline.substr(at+3);rel=rel.substr(0,rel.find('\n')); const auto cg=fs::path("/sys/fs/cgroup")/rel.substr(1);
        std::istringstream events(read_file(cg/"memory.events"));std::string key;uint64_t n;J e;while(events>>key>>n)e[key]=n;
        need(e.value("max",0ULL)==0 && e.value("oom",0ULL)==0 && e.value("oom_kill",0ULL)==0,"candidate memory pressure/OOM");
        record(stage_+"-resources",{{"contexts",driver_.registry()},{"memory_peak",std::stoull(read_file(cg/"memory.peak"))},{"pids_peak",std::stoull(read_file(cg/"pids.peak"))},{"memory_events",e},{"minimum_free_gpu_bytes",driver_.minimum_free.load()}});
    }
    void exclusion() {
        std::ifstream f(out_/stage_/"audit/phases");std::string line;int reader=0,engine=0;unsigned operations=0;
        while(std::getline(f,line)) {std::istringstream in(line);std::string ns,pid,p;in>>ns>>pid>>p;
            if(p=="reader_load_begin"||p=="reader_prepare_begin"||p=="reader_encode_begin"||p=="reader_normalize_begin") {need(engine==0,"GPU phase overlap");++reader;++operations;}
            if(p=="reader_load_end"||p=="reader_prepare_end"||p=="reader_encode_end"||p=="reader_normalize_end") --reader;
            if(p=="engine_gen") {need(reader==0 && engine==0,"GPU generation overlapped reader");engine=1;}
            if(p=="engine_done") {need(engine==1,"unmatched engine terminal");engine=0;}
            need(reader>=0,"unmatched reader phase");
        }
        need(reader==0 && engine==0 && operations,"incomplete phase evidence");record(stage_+"-exclusion-pass",{{"reader_operations",operations}});
    }
    void stop(bool strict=true) {
        if(!active_) return;
        stop_sampler();
        const auto p=inspect(NAME);if(p["State"]["Running"].get<bool>()) {if(strict)resources();docker({"stop","-t","45",NAME},80);}
        const auto state=inspect(NAME)["State"];need(!state["Running"].get<bool>(),"candidate did not stop");driver_.empty();
        const auto logs=docker({"logs",NAME});need(logs.find(key_)==std::string::npos,"sensitive candidate logs refused");std::ofstream(out_/stage_/"container.log")<<logs;
        docker({"rm",NAME});active_=false;
        record(stage_+"-stopped",{{"exit",state["ExitCode"]},{"oom",state["OOMKilled"]},{"gpu_empty",true}});
        if(strict) {need(state["ExitCode"]==0 && !state["OOMKilled"].get<bool>(),"candidate exit failed");exclusion();}
        for(const auto & p:fs::directory_iterator(out_/stage_/"probe")) if(p.path().extension()==".rows")fs::remove(p.path());
    }
    void start(const std::string & family,bool probe) {
        stop();stage_=family+(probe?"-probe":"-native");fs::create_directories(out_/stage_/"audit");fs::create_directories(out_/stage_/"probe");
        auto original=prod_["Config"]["Cmd"].get<std::vector<std::string>>(); auto separator=std::find(original.begin(),original.end(),"--");need(separator!=original.end(),"production engine boundary missing");
        std::vector<std::string> front(original.begin(),separator),engine(separator+1,original.end());
        auto replace=[&](const std::string & flag,const std::string & value) {auto it=std::find(front.begin(),front.end(),flag);need(it!=front.end()&&it+1!=front.end(),"frontend argument absent");*(it+1)=value;};
        if(family=="poly") {replace("--engine","/engines/strata-poly");replace("-m","/poly/model.gguf");replace("--mmproj","/poly/projector.gguf");engine={"--model","/poly/model.gguf","--threads","16","--max-context","32768","--vram-reserve-mib","1200","--kv","q8_0","--no-draft","--prompt-cache","2","--kv-whole"};}
        if(probe) {replace("--engine","/qualification/probe");engine={"--fixture-dir","/probe"};}
        front.insert(front.end(),{"--threads","4","--threads-http","8","--picture-cache","2","--image-max-tokens","1024","--log-disable","--native-vision","/profile.json"});
        std::vector<std::string> cmd={"run","-d","--name",NAME,"--label","unbiasedstrata.test=pr6","--pull","never","--restart=no","--read-only","--user","1000:1000","--memory=96g","--memory-swap=96g","--pids-limit=512","--cpuset-cpus","0-47","--tmpfs","/tmp:rw,nosuid,nodev,size=64m","--cap-drop","ALL","--security-opt","no-new-privileges","--gpus","device="+GPU,"-e","NVIDIA_VISIBLE_DEVICES="+GPU,"-e","CUDA_VISIBLE_DEVICES="+GPU,"-e","CUDA_DEVICE_ORDER=PCI_BUS_ID","-e","NVIDIA_DRIVER_CAPABILITIES=compute,utility","-e","LD_LIBRARY_PATH=/codecs/usr/lib/x86_64-linux-gnu:/opt/unbiased-strata/bin","-p","127.0.0.1:"+std::to_string(PORT)+":8080"};
        auto mount=[&](fs::path src,std::string dst,bool ro=true) {cmd.insert(cmd.end(),{"-v",src.string()+":"+dst+(ro?":ro":":rw")});};
        mount(tools_,"/qualification");mount(tools_/"phase-bin","/candidate");mount(build_/"bin","/engines");mount("/tmp/ub-pr6-deps/root","/codecs");
        mount(out_/(family=="poly"&&!probe?"poly-profile.json":"profile.json"),"/profile.json");
        mount(out_/stage_/"audit","/audit",false);mount(out_/stage_/"probe","/probe",false);
        mount("/data/huggingface/hub/models--unsloth--Qwen3.6-35B-A3B-GGUF/snapshots/9280dd353ab587157920d5bd391ada414d84e552/Qwen3.6-35B-A3B-UD-Q4_K_XL.gguf","/poly/model.gguf");
        mount("/data/huggingface/hub/models--unsloth--Qwen3.6-35B-A3B-GGUF/snapshots/a483e9e6cbd595906af30beda3187c2663a1118c/mmproj-BF16.gguf","/poly/projector.gguf");
        for(const auto & m:prod_["Mounts"])mount(m["Source"].get<std::string>(),m["Destination"].get<std::string>());
        for(const auto & v:prod_["Config"]["Env"]) {const auto s=v.get<std::string>();if(s.rfind("STRATA_PF_FUSED=",0)==0||s.rfind("STRATA_PREFILL_CPU_SHARE=",0)==0||s.rfind("STRATA_PLE_PROBE=",0)==0)cmd.insert(cmd.end(),{"-e",s});}
        cmd.insert(cmd.end(),{"--entrypoint","/candidate/unbiased-strata",prod_["Image"].get<std::string>()});cmd.insert(cmd.end(),front.begin(),front.end());cmd.push_back("--");cmd.insert(cmd.end(),engine.begin(),engine.end());
        active_=true;docker(cmd);driver_.minimum_free=UINT64_MAX;sampling_=true;sample_failed_=false;
        sampler_=std::thread([&] {while(sampling_) {try {(void)driver_.registry();}catch(...) {sample_failed_=true;}std::this_thread::sleep_for(100ms);}});
        await([&] {need(inspect(NAME)["State"]["Running"].get<bool>(),"candidate exited before READY");try{return request("/health").first==200;}catch(...) {return false;}},600);
        auto contexts=driver_.registry();need(contexts.size()==(probe?1u:2u),"native GPU owner count mismatch");
        const auto top=docker({"top",NAME,"-eo","pid,comm"});std::istringstream lines(top);std::string line;while(std::getline(lines,line)) {std::istringstream in(line);unsigned pid;std::string name;if(in>>pid>>name && name=="unbiased-strata")need(!contexts.count(pid),"native parent owns GPU context");}
        record(stage_+"-ready",{{"contexts",contexts},{"parent_gpu_context",false},{"frozen_poly",family=="poly"&&!probe}});
    }
    J capture() {std::ifstream f(out_/stage_/"probe/results");J last;std::string line;while(std::getline(f,line))last=J::parse(line);need(!last.is_null(),"probe receipt missing");return last;}
    void parity(const std::vector<Case> & cases) {
        for(const auto & c:cases) {chat(body(J::array({image(c.source),text("Describe the image briefly.")})));const auto a=capture();chat(body(J::array({image(c.reference),text("Describe the image briefly.")})));const auto b=capture();for(const char * key:{"tokens","token_sha","row_sha","shapes","image_cells"})need(a[key]==b[key],"codec GPU rows/grid/token parity failed");record(stage_+"-codec-parity",{{"fixture",c.name},{"tokens",a["tokens"]},{"image_cells",a["image_cells"]},{"shapes",a["shapes"]},{"row_sha",a["row_sha"]}});}
    }
    void semantics(const std::vector<Case> & cases) {
        for(const auto & c:cases) if(c.semantic) {
            const bool oriented=c.name.rfind("exif-",0)==0;
            const std::string prompt=oriented?"What color fills the TOP LEFT quarter of this image? Answer with just the color name.":"Name the dominant color. Answer with one color word.";
            auto a=chat(body(J::array({image(c.source),text(prompt)})));auto b=chat(body(J::array({image(c.reference),text(prompt)})));
            color(a.first,c.wanted);color(b.first,c.wanted);need(a.second["prompt_tokens"]==b.second["prompt_tokens"],"semantic token budget mismatch");
            record(stage_+"-codec-semantics",{{"fixture",c.name},{"correct",true},{"prompt_tokens",a.second["prompt_tokens"]}});
        }
        const auto red=solid(255,0,0),blue=solid(0,0,255); auto broken=red;broken.resize(12);
        for(bool stream:{false,true}) {auto a=chat(body(J::array({image(blue),text("Name the dominant color. Answer with one color word.")}),stream));color(a.first,"blue");}
        auto before=counts();color(chat(body(J::array({image(blue),text("Name the dominant color. Answer with one color word.")}))).first,"blue");zero_reader(before,counts());
        J tools=body("unused");tools["messages"]=J::array({{{"role","user"},{"content",J::array({image(blue),text("Name the dominant color in my image. Answer with one color word.")})}},{{"role","assistant"},{"content","Checking the tool output."}},{{"role","tool"},{"tool_call_id","fixture"},{"content",J::array({image(broken),text("Use the user's image.")})}}});
        auto corrected=tools;corrected["messages"][2]["content"][0]=text("[Image omitted: unreadable tool-result image.]");
        for(bool stream:{false,true}) {tools["stream"]=stream;auto a=chat(tools);auto b=chat(corrected);color(a.first,"blue");color(b.first,"blue");need(a.second["prompt_tokens"]==b.second["prompt_tokens"],"tool omission changed matched prompt layout");}
        J history=body("unused");history["messages"]=J::array({{{"role","user"},{"content","Read the number in the tool text and reply with digits only."}},{{"role","assistant"},{"content","Reading the tool results."}},{{"role","tool"},{"tool_call_id","old"},{"content",J::array({image(broken),text("Number: 37.")})}},{{"role","tool"},{"tool_call_id","new"},{"content",J::array({image(broken),text("The same number is 37.")})}}});
        auto fixed=history;fixed["messages"][2]["content"][0]=text("[Image omitted: unreadable tool-result image.]");fixed["messages"][3]["content"][0]=text("[Image omitted: unreadable tool-result image.]");
        before=counts();for(bool stream:{false,true}) {history["stream"]=stream;auto a=chat(history);auto b=chat(fixed);need(a.first.find("37")!=std::string::npos && b.first.find("37")!=std::string::npos,"all-omitted history semantic failure");need(a.second["prompt_tokens"]==b.second["prompt_tokens"],"history prompt layout mismatch");}auto after=counts();need(after["reader_encode_begin"]==before["reader_encode_begin"],"all-omitted history encoded an image");
        auto bad=body(J::array({image(broken),text("Describe.")}),true);before=counts();need(request("/v1/chat/completions",&bad).first==400,"user image error was omitted");after=counts();need(after["engine_gen"]==before["engine_gen"],"invalid user image generated output");
        record(stage_+"-tool-omission-pass",{{"mixed",true},{"repeat_history",true},{"all_omitted_text_only",true},{"user_pre_sse_400",true},{"matched_prompt_tokens",true},{"warm_cache_zero_reader_work",true}});
        color(chat(body(J::array({image(red),image(blue),text("Name the dominant color in the FIRST image. Answer with one color word.")}))).first,"red");
        color(chat(body(J::array({image(blue),image(red),text("Name the dominant color in the FIRST image. Answer with one color word.")}))).first,"blue");
    }
    void headroom(const std::string & family) {
        const int nctx=family=="strata"?262144:32768;const auto warm=body(J::array({image(solid(255,0,0)),text("Name the dominant color. Answer with one color word.")}));
        color(chat(warm).first,"red");auto token_request=J{{"content"," hello"}};auto token=J::parse(request("/tokenize",&token_request).second)["tokens"][0];
        J high{{"prompt",J::array()},{"n_predict",1},{"temperature",0}};for(int i=0;i<nctx-9;++i)high["prompt"].push_back(token);
        auto full=[&] {auto r=request("/completion",&high);need(r.first==200 && J::parse(r.second)["tokens_evaluated"]==nctx-9,"full-context gate failed");};
        full();resources();auto resident=driver_.registry();unsigned reader=0;std::istringstream top(docker({"top",NAME,"-eo","pid,comm"}));std::string line;
        while(std::getline(top,line)) {std::istringstream in(line);unsigned pid;std::string comm;if(in>>pid>>comm && comm.rfind("strata-vision-r",0)==0) {need(!reader,"ambiguous reader process");reader=pid;}}
        need(reader && resident.count(reader) && resident.size()==2,"reader GPU ownership missing");resident.erase(reader);
        need(::kill(pid_t(reader),SIGKILL)==0,"owned reader kill failed");await([&] {return !driver_.registry().count(reader);});
        await([&] {auto r=request("/props");return r.first==200 && J::parse(r.second)["vision_status"]=="unavailable";});
        auto before=counts();color(chat(warm).first,"red");zero_reader(before,counts());
        auto miss=body(J::array({image(solid(1,2,3)),text("Describe.")}));need(request("/v1/chat/completions",&miss).first==503,"readerless miss was admitted");
        full();auto absent=driver_.registry();need(absent.size()==resident.size(),"readerless GPU owner changed");for(auto & p:resident)need(absent.at(p.first)<=p.second,"readerless engine stole restart headroom");
        resources();std::this_thread::sleep_for(1100ms);J empty=J::object();need(request("/vision/recover",&empty).first==200,"explicit recovery refused");
        await([&] {auto r=request("/props");return r.first==200 && J::parse(r.second)["vision_status"]=="available";});auto recovered=driver_.registry();for(auto & p:absent)need(recovered.at(p.first)==p.second,"engine allocation changed during recovery");
        before=counts();color(chat(body(J::array({image(solid(255,255,0,1024,1024)),text("Name the dominant color. Answer with one color word.")}))).first,"yellow");auto after=counts();for(const char * p:{"reader_normalize_begin","reader_prepare_begin","reader_encode_begin"})need(after[p]==before[p]+1,"fresh full-allocation encode not observed");
        before=counts();color(chat(warm).first,"red");zero_reader(before,counts());resources();
        record(stage_+"-headroom-restart-pass",{{"evaluated_tokens",nctx-9},{"resident_full_kv",resident},{"readerless_full_kv",absent},{"fresh_encode_before_cached_gen",true}});
    }
    void smoke() {
        need(request("/health",nullptr,8080,false).first==200 && request("/v1/models",nullptr,8080,false).first==401 && request("/v1/models",nullptr,8080).first==200,"production auth/health smoke failed");
        for(bool stream:{false,true})need(chat(body("What is six times seven? Reply with digits only.",stream,32),8080).first.find("42")!=std::string::npos,"production text/SSE smoke failed");
        Bytes rgb(224*224*3,0);for(size_t i=0;i<rgb.size();i+=3)rgb[i]=255;
        const auto red_jpeg=jpeg({},224,224,rgb);
        for(int i=0;i<2;++i)color(chat(body(J::array({image(red_jpeg),text("Name the dominant color. Answer with one color word.")})),8080).first,"red");
        record("production-smoke-pass",{{"auth",true},{"health_models",true},{"text_sse",true},{"vision",true}});
    }
    bool restore() noexcept {
        try {stop(false);driver_.empty();for(const auto & p:protected_)need(hash_file(p.first)==p.second,"protected Compose changed");auto now=inspect(PROD);need(now["Image"]==prod_["Image"] && settings_hash(now)==original_,"production settings changed");
            if(paused_) {docker({"start",prod_["Id"].get<std::string>()});await([&] {return inspect(PROD)["State"]["Health"]["Status"]=="healthy";},600);smoke();}
            record("production-restored",{{"original_image_settings",true},{"compose_backup_unchanged",true},{"candidate_removed",true}});return true;
        } catch(...) {try {record("restoration-requires-attention",{{"do_not_restart_gpu_blindly",true}});}catch(...) {}return false;}
    }
    int run() {
        bool passed=false;try {smoke();paused_=true;docker({"stop","-t","60",prod_["Id"].get<std::string>()},100);driver_.empty();record("maintenance-start",{{"gpu_empty",true}});
            const auto cases=corpus();for(const std::string family:{"strata","poly"}) {start(family,true);parity(cases);stop();start(family,false);semantics(cases);headroom(family);stop();}
            record("PR6_GPU_ACCEPTANCE_PASS",{{"projectors",2},{"codec_pairs_per_projector",cases.size()},{"engines",J::array({"strata","strata-poly"})},{"tool_omission",true},{"headroom_restart",true}});passed=true;
        }catch(const Failure & e) {record("qualification-failed",{{"stage",stage_},{"check",e.what()}});}
        catch(...) {record("qualification-failed",{{"stage",stage_},{"details","C++ exception; inspect private local evidence"}});}
        const bool restored=restore();return passed&&restored?0:1;
    }
};
int main(int argc,char ** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--fixtures-only") {auto cases=corpus();need(cases.size()==22,"fixture corpus incomplete");for(const auto & c:cases)need(!c.source.empty() && !c.reference.empty(),"empty fixture");std::cout<<"PR6_GPU_FIXTURES_PASS cases="<<cases.size()<<" gpu=none\n";return 0;}
        need(argc==6 && std::string(argv[1])=="--authorize-maintenance","explicit maintenance authorization required");
        int lock=::open("/tmp/unbiased-strata-vision-trial.lock",O_CREAT|O_RDWR|O_CLOEXEC,0600);need(lock>=0 && ::flock(lock,LOCK_EX|LOCK_NB)==0,"another trial owns maintenance");
        Trial trial(fs::canonical(argv[2]),fs::canonical(argv[3]),fs::canonical(argv[4]),fs::absolute(argv[5]));return trial.run();
    }catch(...) {std::cerr<<"PR6 qualification setup failed (no sensitive diagnostics)\n";return 1;}
}
