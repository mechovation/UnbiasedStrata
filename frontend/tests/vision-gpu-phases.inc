// Qualification-only link wrappers; never linked into shipped executables.
// Fixed phase names/timestamps only: no prompts, paths, rows or credentials.
#include "mtmd.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static void phase(const char * name) {
    timespec t{}; if (clock_gettime(CLOCK_MONOTONIC, &t)) std::_Exit(90);
    char line[160]; int n=std::snprintf(line,sizeof line,"%lld %ld %s\n",
        static_cast<long long>(t.tv_sec)*1000000000LL+t.tv_nsec,static_cast<long>(getpid()),name);
    int fd=::open("/audit/phases",O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC,0600);
    if(fd<0 || n<=0 || n>=int(sizeof line) || ::syscall(SYS_write,fd,line,n)!=n) std::_Exit(90);
    ::close(fd);
}
extern "C" {
#ifdef PR4_READER_PHASE
mtmd_context * __real_mtmd_init_from_file(const char *,const llama_model *,mtmd_context_params);
mtmd_context * __wrap_mtmd_init_from_file(const char * file,const llama_model * model,mtmd_context_params params) {
    phase("reader_load_begin"); auto * ctx=__real_mtmd_init_from_file(file,model,params); phase("reader_load_end"); return ctx;
}
int32_t __real_mtmd_tokenize(const mtmd_context *,mtmd_input_chunks *,const mtmd_input_text *,const mtmd_bitmap * const *,size_t);
int32_t __wrap_mtmd_tokenize(const mtmd_context * ctx,mtmd_input_chunks * out,const mtmd_input_text * text,const mtmd_bitmap * const * images,size_t n) {
    phase("reader_prepare_begin"); auto r=__real_mtmd_tokenize(ctx,out,text,images,n); phase("reader_prepare_end"); return r;
}
int32_t __real_mtmd_encode_chunk(mtmd_context *,const mtmd_input_chunk *);
int32_t __wrap_mtmd_encode_chunk(mtmd_context * ctx,const mtmd_input_chunk * chunk) {
    phase("reader_encode_begin"); auto r=__real_mtmd_encode_chunk(ctx,chunk); phase("reader_encode_end"); return r;
}
#else
ssize_t __real_write(int,const void *,size_t);
ssize_t __wrap_write(int fd,const void * data,size_t bytes) {
    auto n=__real_write(fd,data,bytes);
    if(n>=4 && (!std::memcmp(data,"GEN ",4) || (n>=5 && !std::memcmp(data,"GENI ",5)))) phase("engine_gen");
    if(n==5 && !std::memcmp(data,"STOP\n",5)) phase("engine_stop");
    return n;
}
ssize_t __real_read(int,void *,size_t);
ssize_t __wrap_read(int fd,void * data,size_t bytes) {
    auto n=__real_read(fd,data,bytes);
    // Native adapter polls one byte at a time. Never retain child diagnostics
    // or credential-file buffers; only five engine-line prefix bytes are kept.
    if(n==1 && bytes==1 && fd>=0 && fd<256) {
        struct Prefix { char text[5]{}; unsigned size=0; };
        static thread_local Prefix lines[256]; auto & p=lines[fd]; char ch=*static_cast<char *>(data);
        if(ch=='\n') {
            if(p.size==5 && !std::memcmp(p.text,"DONE ",5)) phase("engine_done");
            if(p.size==5 && !std::memcmp(p.text,"READY",5)) phase("engine_ready");
            p.size=0;
        } else if(p.size<5) p.text[p.size++]=ch;
    }
    return n;
}
#endif
}
