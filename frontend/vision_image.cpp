#include "vision_image.hpp"
#include "vendor/stb/stb_image.h"
#include <webp/decode.h>
#include <webp/demux.h>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <libyuv.h>
#include <dlfcn.h>
#include <climits>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>

namespace strata_frontend::vision_image {
namespace {
using Bytes = std::vector<unsigned char>;
unsigned orientation(const unsigned char * source, size_t size) {
    bool no_memory = false;
    std::unique_ptr<ExifData, decltype(&exif_data_unref)> data(exif_data_new(), exif_data_unref);
    std::unique_ptr<ExifLog, decltype(&exif_log_unref)> log(exif_log_new(), exif_log_unref);
    if (!data || !log) throw std::bad_alloc();
    exif_log_set_func(log.get(), [](ExifLog *, ExifLogCode code, const char *, const char *, va_list, void * state) {
        if (code == EXIF_LOG_CODE_NO_MEMORY) *static_cast<bool *>(state) = true;
    }, &no_memory); // Never forward metadata-derived diagnostic strings.
    exif_data_log(data.get(), log.get());
    exif_data_unset_option(data.get(), EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
    exif_data_load_data(data.get(), source, unsigned(size));
    if (no_memory) throw std::bad_alloc();
    const auto entry = exif_content_get_entry(data->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!entry || entry->format != EXIF_FORMAT_SHORT || entry->components != 1 || entry->size != 2 || !entry->data) return 1;
    const unsigned value = exif_get_short(entry->data, exif_data_get_byte_order(data.get()));
    return value >= 1 && value <= 8 ? value : 1;
}
void require(bool ok) { if (!ok) throw std::runtime_error("image library operation failed"); }
void discard(Bytes & bytes) { Bytes().swap(bytes); }
}
Image decode(const unsigned char * source, size_t size, Limits caps) {
    Image out;
    try {
        if (!source || !size) { out.status = Status::unreadable; return out; }
        if (size > 10 * 1024 * 1024 || size > INT_MAX) { out.status = Status::resource; return out; }
        if (!caps.dimension || caps.dimension > INT_MAX / 4 || !caps.pixels || !caps.rgb_bytes || !caps.workspace_bytes) return out;
        const bool webp = size >= 12 && !std::memcmp(source, "RIFF", 4) && !std::memcmp(source + 8, "WEBP", 4);
        int width = 0, height = 0, channels = 0;
        if (webp) {
            WebPBitstreamFeatures header{};
            const auto status = WebPGetFeatures(source, size, &header);
            if (status != VP8_STATUS_OK) {
                out.status = status == VP8_STATUS_OUT_OF_MEMORY ? Status::resource :
                    status == VP8_STATUS_BITSTREAM_ERROR || status == VP8_STATUS_NOT_ENOUGH_DATA ?
                    Status::invalid_webp_header : Status::decode_failed;
                return out;
            }
            width = header.width; height = header.height;
        } else if (!stbi_info_from_memory(source, int(size), &width, &height, &channels)) {
            out.status = Status::unreadable; return out;
        }
        if (width <= 0 || height <= 0) { out.status = Status::unreadable; return out; }
        const uint64_t pixels = uint64_t(width) * uint32_t(height);
        if (uint32_t(width) > caps.dimension || uint32_t(height) > caps.dimension || pixels > caps.pixels ||
            pixels > caps.rgb_bytes / 3 || pixels > caps.workspace_bytes / workspace_per_pixel || pixels > SIZE_MAX / workspace_per_pixel) {
            out.status = Status::resource; return out;
        }
        // All user-sized buffers and decoder canvas creation follow admission.
        out.width = uint32_t(width); out.height = uint32_t(height);
        if (webp) {
            WebPData data{source, size}; WebPAnimDecoderOptions options{};
            require(WebPAnimDecoderOptionsInit(&options)); options.color_mode = MODE_bgrA; options.use_threads = 0;
            std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
                WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete);
            // These APIs conflate malformed input and allocation failure. Do not
            // label either ambiguous failure as a safely omittable tool image.
            if (!decoder) { out.status = Status::decode_failed; return out; }
            WebPAnimInfo info{}; require(WebPAnimDecoderGetInfo(decoder.get(), &info));
            require(info.canvas_width == out.width && info.canvas_height == out.height && info.frame_count);
            uint8_t * frame = nullptr; int timestamp = 0;
            if (!WebPAnimDecoderGetNext(decoder.get(), &frame, &timestamp) || !frame) {
                out.status = Status::decode_failed; return out;
            }
            Bytes white(size_t(pixels * 4), 255), composite(size_t(pixels * 4));
            out.rgb.resize(size_t(pixels * 3));
            require(libyuv::ARGBBlend(frame, width * 4, white.data(), width * 4, composite.data(), width * 4, width, height) == 0);
            require(libyuv::ARGBToRAW(composite.data(), width * 4, out.rgb.data(), width * 3, width, height) == 0);
        } else {
            int x = 0, y = 0;
            std::unique_ptr<unsigned char, decltype(&stbi_image_free)> decoded(
                stbi_load_from_memory(source, int(size), &x, &y, &channels, 3), stbi_image_free);
            if (!decoded || x != width || y != height) { out.status = Status::decode_failed; return out; }
            const unsigned value = size >= 2 && source[0] == 0xff && source[1] == 0xd8 ? orientation(source, size) : 1;
            if (value == 1) out.rgb.assign(decoded.get(), decoded.get() + size_t(pixels * 3));
            else {
                const libyuv::RotationMode turns[] = {libyuv::kRotate0, libyuv::kRotate0, libyuv::kRotate180,
                    libyuv::kRotate180, libyuv::kRotate90, libyuv::kRotate90, libyuv::kRotate270, libyuv::kRotate270};
                Bytes argb(size_t(pixels * 4)), rotated(size_t(pixels * 4));
                require(libyuv::RAWToARGB(decoded.get(), width * 3, argb.data(), width * 4, width, height) == 0);
                decoded.reset();
                if (value >= 5) { out.width = uint32_t(height); out.height = uint32_t(width); }
                require(libyuv::ARGBRotate(argb.data(), width * 4, rotated.data(), int(out.width) * 4, width, height, turns[value - 1]) == 0);
                discard(argb);
                if (value == 2 || value == 4 || value == 5 || value == 7) {
                    Bytes mirrored(size_t(pixels * 4));
                    require(libyuv::ARGBMirror(rotated.data(), int(out.width) * 4, mirrored.data(), int(out.width) * 4, int(out.width), int(out.height)) == 0);
                    rotated.swap(mirrored);
                }
                out.rgb.resize(size_t(pixels * 3));
                require(libyuv::ARGBToRAW(rotated.data(), int(out.width) * 4, out.rgb.data(), int(out.width) * 3, int(out.width), int(out.height)) == 0);
            }
        }
        out.status = Status::ok; return out;
    } catch (const std::bad_alloc &) { out.status = Status::resource; }
    catch (...) { out.status = Status::internal; }
    out.rgb.clear(); out.width = out.height = 0; return out;
}

std::vector<Library> libraries() {
    std::vector<Library> result;
    auto add = [&](const char * role, const void * symbol) {
        Dl_info info{};
        require(symbol && ::dladdr(symbol, &info) && info.dli_fname);
        const auto path = std::filesystem::canonical(info.dli_fname);
        require(std::filesystem::is_regular_file(path) && !std::filesystem::equivalent(path, "/proc/self/exe"));
        result.push_back({role, path.string()});
    };
    add("webp", reinterpret_cast<const void *>(&WebPGetDecoderVersion));
    add("webp-demux", reinterpret_cast<const void *>(&WebPAnimDecoderGetInfo));
    add("exif", reinterpret_cast<const void *>(&exif_data_new));
    add("yuv", reinterpret_cast<const void *>(&libyuv::ARGBRotate));
    // Public symbols in the evaluated binaries' transitive dependencies. Builds
    // that embed these in a direct library are already covered by its hash.
    for (auto item : {std::pair{"sharpyuv", "SharpYuvGetVersion"}, std::pair{"jpeg", "jpeg_std_error"}})
        if (auto symbol = ::dlsym(RTLD_DEFAULT, item.second)) add(item.first, symbol);
    return result;
}
} // namespace strata_frontend::vision_image
