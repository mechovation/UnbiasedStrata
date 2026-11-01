// PR6 public-API and actual normalizer tests. No Python, network, model or GPU.
#include "../vision_image.hpp"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "../../third_party/llama.cpp/vendor/stb/stb_image.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <jpeglib.h>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <libyuv.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include "pr6-codec-fixtures.hpp"
using namespace pr6_fixtures;

// Proposed metadata policy: missing, malformed or unsupported orientation is 1.
// Do not let libexif's automatic tag fixing silently change malformed values.
static unsigned orientation(const Bytes & source) {
    check(!source.empty() && source.size() <= 10 * 1024 * 1024);
    Exif data(exif_data_new(), exif_data_unref); check(bool(data));
    exif_data_unset_option(data.get(), EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
    exif_data_load_data(data.get(), source.data(), unsigned(source.size()));
    auto entry = exif_content_get_entry(data->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!entry || entry->format != EXIF_FORMAT_SHORT || entry->components != 1 || entry->size != 2 || !entry->data) return 1;
    const unsigned value = exif_get_short(entry->data, exif_data_get_byte_order(data.get()));
    return value >= 1 && value <= 8 ? value : 1;
}

static Bytes first_frame(const Bytes & source, int dimension, uint64_t pixel_cap, bool animated) {
    if (source.empty() || source.size() > 10 * 1024 * 1024 || dimension <= 0 || !pixel_cap) return {};
    WebPBitstreamFeatures header{};
    if (WebPGetFeatures(source.data(), source.size(), &header) != VP8_STATUS_OK ||
        header.width <= 0 || header.height <= 0 || header.width > dimension || header.height > dimension ||
        uint64_t(header.width) > pixel_cap / uint64_t(header.height)) return {};
    check(bool(header.has_animation) == animated);
    WebPData data{source.data(), source.size()}; WebPAnimDecoderOptions options{};
    check(WebPAnimDecoderOptionsInit(&options)); options.color_mode = MODE_bgrA; options.use_threads = 0;
    std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
        WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete);
    if (!decoder) return {};
    WebPAnimInfo info{};
    check(WebPAnimDecoderGetInfo(decoder.get(), &info));
    check(info.canvas_width == unsigned(header.width) && info.canvas_height == unsigned(header.height));
    check(info.frame_count == (animated ? 2u : 1u));
    uint8_t * frame = nullptr; int timestamp = 0;
    if (!WebPAnimDecoderGetNext(decoder.get(), &frame, &timestamp) || !frame) return {};
    const int width = header.width, height = header.height;
    Bytes white(size_t(width * height * 4), 255), composite(white.size()), rgb(size_t(width * height * 3));
    check(libyuv::ARGBBlend(frame, width * 4, white.data(), width * 4, composite.data(), width * 4, width, height) == 0);
    check(libyuv::ARGBToRAW(composite.data(), width * 4, rgb.data(), width * 3, width, height) == 0);
    return rgb; // Deliberately never decode the second frame.
}

int main(int argc, char ** argv) {
    try {
        check(argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--scalar"));
        if (argc == 2) libyuv::MaskCpuFlags(1); // Documented public API: C paths only.
        const Bytes rgb{1,11,21,2,12,22,3,13,23,4,14,24,5,15,25,6,16,26};
        const unsigned expected[8][6] = {{1,2,3,4,5,6},{3,2,1,6,5,4},{6,5,4,3,2,1},{4,5,6,1,2,3},
            {1,4,2,5,3,6},{4,1,5,2,6,3},{6,3,5,2,4,1},{3,6,2,5,1,4}};
        for (auto order : {EXIF_BYTE_ORDER_INTEL, EXIF_BYTE_ORDER_MOTOROLA}) {
            for (unsigned value = 1; value <= 8; ++value) {
                const auto source = jpeg(metadata(value, order)); check(orientation(source) == value);
                const auto actual = orient_rgb(rgb, 3, 2, orientation(source));
                for (unsigned i = 0; i < 6; ++i)
                    for (unsigned channel = 0; channel < 3; ++channel)
                        check(actual[3*i+channel] == rgb[3*(expected[value-1][i]-1)+channel]);
            }
        }
        check(orientation(jpeg({})) == 1);
        check(orientation(jpeg(Bytes{'E','x','i','f',0,0,0xff})) == 1);
        for (unsigned bad : {0u, 9u, 65535u}) check(orientation(jpeg(metadata(bad, EXIF_BYTE_ORDER_INTEL))) == 1);
        const auto exif = metadata(6, EXIF_BYTE_ORDER_INTEL);
        for (size_t size = 1; size < exif.size(); ++size) {
            const auto value = orientation(Bytes(exif.begin(), exif.begin() + size));
            check(value == 1 || value == 6); // Some truncated tails do not affect the complete orientation entry.
        }
        const Bytes bgra{0,0,255,0, 0,0,255,128, 255,0,0,255};
        const auto still = webp(bgra);
        const auto moving = animation(still, webp(Bytes{0,255,0,255, 0,255,0,255, 0,255,0,255}));
        const Bytes expected_rgb{255,255,255, 255,127,127, 0,0,255};
        check(first_frame(still, 64, 4096, false) == expected_rgb);
        check(first_frame(moving, 64, 4096, true) == expected_rgb);
        const auto green = webp(Bytes{0,255,0,255, 0,255,0,255, 0,255,0,255});
        check(first_frame(green, 64, 4096, false) == Bytes({0,255,0,0,255,0,0,255,0}));
        const auto offset = animation(still, green, 7, 5, 2);
        Bytes offset_expected(7 * 5 * 3, 255);
        offset_expected[(2*7+3)*3+1] = offset_expected[(2*7+3)*3+2] = 127;
        offset_expected[(2*7+4)*3] = offset_expected[(2*7+4)*3+1] = 0;
        check(first_frame(offset, 64, 4096, true) == offset_expected);
        check(first_frame(offset, 6, 4096, true).empty());
        check(first_frame(offset, 64, 34, true).empty());
        check(first_frame(animation(still, green, 4097, 1), 4096, 16777216, true).empty());
        for (const auto * source : {&still, &moving}) {
            check(first_frame(*source, 2, 4096, source == &moving).empty());
            check(first_frame(*source, 64, 2, source == &moving).empty());
            check(first_frame(*source, 0, 4096, source == &moving).empty());
        }
        check(first_frame(Bytes{1,2,3}, 64, 4096, false).empty());
        check(first_frame(Bytes(still.begin(), still.begin() + still.size()/2), 64, 4096, false).empty());
        check(first_frame(Bytes(moving.begin(), moving.begin() + moving.size()/2), 64, 4096, true).empty());
        check(first_frame(Bytes(10*1024*1024+1), 64, 4096, false).empty());
        namespace vi = strata_frontend::vision_image;
        const vi::Limits limits{4096, 16777216, 67108864, 536870912};
        auto normalized = [&](const Bytes & bytes) {
            auto result = vi::decode(bytes.data(), bytes.size(), limits);
            check(result.status == vi::Status::ok && result.rgb.size() == size_t(result.width) * result.height * 3);
            return result;
        };
        const auto baseline = normalized(jpeg({}));
        check(baseline.width == 3 && baseline.height == 2);
        for (auto order : {EXIF_BYTE_ORDER_INTEL, EXIF_BYTE_ORDER_MOTOROLA}) {
            for (unsigned value = 1; value <= 8; ++value) {
                const auto actual = normalized(jpeg(metadata(value, order)));
                check(actual.width == (value >= 5 ? 2u : 3u) && actual.height == (value >= 5 ? 3u : 2u));
                check(actual.rgb == orient_rgb(baseline.rgb, 3, 2, value));
            }
        }
        check(normalized(jpeg(Bytes{'E','x','i','f',0,0,0xff})).rgb == baseline.rgb);
        for (unsigned bad : {0u, 9u, 65535u}) check(normalized(jpeg(metadata(bad, EXIF_BYTE_ORDER_INTEL))).rgb == baseline.rgb);
        check(normalized(still).rgb == expected_rgb && normalized(moving).rgb == expected_rgb);
        auto offset_image = normalized(offset);
        check(offset_image.width == 7 && offset_image.height == 5 && offset_image.rgb == offset_expected);
        const auto & png = transparent_png;
        int px = 0, py = 0, channels = 0;
        std::unique_ptr<unsigned char, decltype(&stbi_image_free)> rgba(
            stbi_load_from_memory(png.data(), int(png.size()), &px, &py, &channels, 4), stbi_image_free);
        check(rgba && px == 1 && py == 1 && channels == 4 && rgba.get()[3] == 0);
        check(normalized(png).rgb == Bytes({0,0,0})); // Pass-through alpha is not white-composited.
        for (const auto * source : {&still, &moving, &offset, &png}) {
            auto caps = limits; caps.workspace_bytes = 1;
            check(vi::decode(source->data(), source->size(), caps).status == vi::Status::resource);
            caps = limits; caps.rgb_bytes = 1;
            check(vi::decode(source->data(), source->size(), caps).status == vi::Status::resource);
        }
        auto caps = limits; caps.dimension = 2;
        check(vi::decode(still.data(), still.size(), caps).status == vi::Status::resource);
        caps = limits; caps.pixels = 34;
        check(vi::decode(offset.data(), offset.size(), caps).status == vi::Status::resource);
        check(vi::decode(nullptr, 0, limits).status == vi::Status::unreadable);
        const Bytes invalid{1,2,3};
        check(vi::decode(invalid.data(), invalid.size(), limits).status == vi::Status::unreadable);
        for (size_t size = 1; size < still.size(); ++size) check(vi::decode(still.data(), size, limits).status != vi::Status::ok);
        // Only public typed header statuses authorize the new rejection path.
        Bytes damaged_header = still; damaged_header[12] ^= 0x80;
        WebPBitstreamFeatures damaged_features{};
        check(WebPGetFeatures(damaged_header.data(),damaged_header.size(),&damaged_features) == VP8_STATUS_BITSTREAM_ERROR);
        check(vi::decode(damaged_header.data(),damaged_header.size(),limits).status == vi::Status::invalid_webp_header);
        check(vi::decode(still.data(),12,limits).status == vi::Status::invalid_webp_header);
        check(vi::decode(still.data(),11,limits).status == vi::Status::unreadable); // No recognized WebP header yet.
        auto libraries = vi::libraries(); check(libraries.size() >= 4);
        for (const auto & library : libraries) check(!library.role.empty() && !library.path.empty() && library.path[0] == '/');
        std::cout << "PR6_NORMALIZER_PASS jpeg=16 png_alpha=unchanged webp_canvas=pass limits=pass loaded_library_identity=pass\n";
        std::cout << "PR6_LIBRARY_CAPABILITY_PASS exif_orientations=16 metadata_controls=pass webp=still,first-animation-frame alpha=white bounds=pass public_api_only=true\n";
        return 0;
    } catch (const std::exception &) {
        std::cerr << "PR6 library capability proof failed\n"; return 1;
    }
}
