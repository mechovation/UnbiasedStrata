// PR5 qualification-only wrappers. Never linked into shipped binaries.
#ifdef PR4_READER_PHASE
#define __wrap_mtmd_encode_chunk pr4_encode_without_gate
#endif
#include "vision-gpu-phases.inc"
#ifdef PR4_READER_PHASE
#undef __wrap_mtmd_encode_chunk
extern "C" {
int32_t __wrap_mtmd_encode_chunk(mtmd_context * ctx, const mtmd_input_chunk * chunk) {
    const auto rc = pr4_encode_without_gate(ctx,chunk);
    if (::access("/audit/hold-encode",F_OK) == 0) {
        phase("reader_encode_held");
        while (::access("/audit/hold-encode",F_OK) == 0) ::usleep(2000);
    }
    return rc;
}
unsigned char * __real_stbi_load_from_memory(const unsigned char *,int,int *,int *,int *,int);
unsigned char * __wrap_stbi_load_from_memory(const unsigned char * bytes,int n,int * x,int * y,int * channels,int wanted) {
    phase("reader_decode_begin");
    auto * out = __real_stbi_load_from_memory(bytes,n,x,y,channels,wanted);
    phase("reader_decode_end");
    return out;
}
}
#endif
