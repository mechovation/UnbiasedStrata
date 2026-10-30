// NVML ABI fixture only. No NVIDIA library/device/API is used by this test.
#include <cassert>
#include <cstring>
#include <initializer_list>
struct Process { unsigned pid; unsigned long long bytes; unsigned gpu, compute; };
struct Event { void * device; unsigned long long type, data; unsigned gpu, compute; };
static unsigned pids[4]{}, count = 0; static bool xid = false;
static int wait_result = 10; // NVIDIA NVML_ERROR_TIMEOUT, not NOT_FOUND (6)
extern "C" {
void pr4_mock_contexts(unsigned a,unsigned b,unsigned c) { count=0; for (auto p : {a,b,c}) if (p) pids[count++]=p; }
void pr4_mock_xid() { xid=true; }
void pr4_mock_wait_result(int result) { wait_result=result; }
int nvmlInit_v2() { return 0; }
int nvmlShutdown() { return 0; }
int nvmlDeviceGetHandleByUUID(const char * uuid,void ** out) { assert(!std::strcmp(uuid,"GPU-d14daf49-45c8-58c5-cf61-5a40342672af")); *out=reinterpret_cast<void *>(1); return 0; }
int nvmlDeviceGetComputeRunningProcesses_v2(void * device,unsigned * n,Process * out) {
    assert(device==reinterpret_cast<void *>(1)); if (*n<count) { *n=count; return 7; }
    *n=count; for (unsigned i=0;i<count;++i) out[i]={pids[i],1,0,0}; return 0;
}
int nvmlEventSetCreate(void ** out) { *out=reinterpret_cast<void *>(2); return 0; }
int nvmlDeviceRegisterEvents(void *,unsigned long long type,void *) { assert(type==8); return 0; }
int nvmlEventSetWait_v2(void *,Event * event,unsigned timeout) { assert(timeout==0); if (!xid) return wait_result; xid=false; *event={reinterpret_cast<void *>(1),8,31,0,0}; return 0; }
int nvmlEventSetFree(void *) { return 0; }
}
