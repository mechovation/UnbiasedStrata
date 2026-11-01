// Qualification-only PR6 normalizer boundary, in addition to existing phases.
#include "row-cache-gpu-phase.cpp"
#ifdef PR4_READER_PHASE
#include "../vision_image.hpp"
using strata_frontend::vision_image::Image;
using strata_frontend::vision_image::Limits;
extern Image real_normalize(const unsigned char *,size_t,Limits)
    asm("__real__ZN15strata_frontend12vision_image6decodeEPKhmNS0_6LimitsE");
extern Image wrap_normalize(const unsigned char *,size_t,Limits)
    asm("__wrap__ZN15strata_frontend12vision_image6decodeEPKhmNS0_6LimitsE");
Image wrap_normalize(const unsigned char * data,size_t bytes,Limits caps) {
    phase("reader_normalize_begin");
    auto out = real_normalize(data,bytes,caps);
    phase("reader_normalize_end");
    return out;
}
#endif
