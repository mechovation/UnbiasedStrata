// Test-only in-memory fixtures through unmodified public library APIs.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>
#include <jpeglib.h>
#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>
#include <libyuv.h>
#include <webp/encode.h>
#include <webp/mux.h>

namespace pr6_fixtures {
using Bytes = std::vector<uint8_t>;
inline void check(bool ok) { if (!ok) throw std::runtime_error("PR6 library capability check failed"); }
using Exif = std::unique_ptr<ExifData, decltype(&exif_data_unref)>;

inline Bytes metadata(unsigned orientation, ExifByteOrder order) {
    Exif data(exif_data_new(), exif_data_unref); check(bool(data));
    exif_data_set_byte_order(data.get(), order);
    auto entry = exif_entry_new(); check(entry != nullptr);
    exif_content_add_entry(data->ifd[EXIF_IFD_0], entry);
    exif_entry_initialize(entry, EXIF_TAG_ORIENTATION);
    check(entry->format == EXIF_FORMAT_SHORT && entry->size == 2 && entry->data);
    exif_set_short(entry->data, order, ExifShort(orientation));
    exif_entry_unref(entry);
    unsigned char * raw = nullptr; unsigned size = 0;
    exif_data_save_data(data.get(), &raw, &size); check(raw && size);
    std::unique_ptr<unsigned char, decltype(&std::free)> owned(raw, std::free);
    return Bytes(raw, raw + size);
}

inline Bytes jpeg(const Bytes & exif, int width = 3, int height = 2, const Bytes & rgb = {}) {
    check(width > 0 && height > 0 && (rgb.empty() || rgb.size() == size_t(width) * height * 3));
    jpeg_compress_struct encoder{}; jpeg_error_mgr errors{};
    encoder.err = jpeg_std_error(&errors); jpeg_create_compress(&encoder);
    unsigned char * raw = nullptr; unsigned long size = 0;
    jpeg_mem_dest(&encoder, &raw, &size);
    encoder.image_width = width; encoder.image_height = height;
    encoder.input_components = 3; encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder); jpeg_start_compress(&encoder, TRUE);
    if (!exif.empty()) jpeg_write_marker(&encoder, JPEG_APP0 + 1, exif.data(), unsigned(exif.size()));
    std::array<uint8_t, 9> row{10,20,30,40,50,60,70,80,90};
    check(!rgb.empty() || width == 3);
    while (encoder.next_scanline < encoder.image_height) {
        JSAMPROW p = rgb.empty() ? row.data() : const_cast<uint8_t *>(rgb.data() + size_t(encoder.next_scanline) * width * 3);
        check(jpeg_write_scanlines(&encoder, &p, 1) == 1);
    }
    jpeg_finish_compress(&encoder); jpeg_destroy_compress(&encoder);
    std::unique_ptr<unsigned char, decltype(&std::free)> owned(raw, std::free);
    return Bytes(raw, raw + size);
}

// Reference transforms are library operations, never handwritten pixel movement.
inline Bytes orient_rgb(const Bytes & rgb, int width, int height, unsigned value) {
    check(width > 0 && height > 0 && value >= 1 && value <= 8 && rgb.size() == size_t(width * height * 3));
    const libyuv::RotationMode turns[] = {libyuv::kRotate0, libyuv::kRotate0, libyuv::kRotate180,
        libyuv::kRotate180, libyuv::kRotate90, libyuv::kRotate90, libyuv::kRotate270, libyuv::kRotate270};
    const bool mirror = value == 2 || value == 4 || value == 5 || value == 7;
    const int out_width = value >= 5 ? height : width, out_height = value >= 5 ? width : height;
    Bytes argb(size_t(width * height * 4)), rotated(argb.size()), mirrored(argb.size()), output(rgb.size());
    check(libyuv::RAWToARGB(rgb.data(), width * 3, argb.data(), width * 4, width, height) == 0);
    check(libyuv::ARGBRotate(argb.data(), width * 4, rotated.data(), out_width * 4, width, height, turns[value - 1]) == 0);
    const uint8_t * pixels = rotated.data();
    if (mirror) {
        check(libyuv::ARGBMirror(pixels, out_width * 4, mirrored.data(), out_width * 4, out_width, out_height) == 0);
        pixels = mirrored.data();
    }
    check(libyuv::ARGBToRAW(pixels, out_width * 4, output.data(), out_width * 3, out_width, out_height) == 0);
    return output;
}

inline Bytes webp(const Bytes & bgra, int width = 3, int height = 1) {
    check(width > 0 && height > 0 && bgra.size() == size_t(width) * height * 4);
    uint8_t * raw = nullptr;
    const size_t size = WebPEncodeLosslessBGRA(bgra.data(), width, height, width * 4, &raw); check(size && raw);
    std::unique_ptr<uint8_t, decltype(&WebPFree)> owned(raw, WebPFree);
    return Bytes(raw, raw + size);
}

inline Bytes animation(const Bytes & first, const Bytes & second, int width = 3, int height = 1, int offset = 0) {
    std::unique_ptr<WebPMux, decltype(&WebPMuxDelete)> mux(WebPMuxNew(), WebPMuxDelete); check(bool(mux));
    WebPMuxAnimParams params{};
    check(WebPMuxSetCanvasSize(mux.get(), width, height) == WEBP_MUX_OK);
    check(WebPMuxSetAnimationParams(mux.get(), &params) == WEBP_MUX_OK);
    for (const auto * bytes : {&first, &second}) {
        WebPMuxFrameInfo frame{}; frame.bitstream = {bytes->data(), bytes->size()};
        frame.x_offset = offset; frame.y_offset = offset;
        frame.duration = 20; frame.id = WEBP_CHUNK_ANMF; frame.blend_method = WEBP_MUX_NO_BLEND;
        check(WebPMuxPushFrame(mux.get(), &frame, 1) == WEBP_MUX_OK);
    }
    WebPData output{}; check(WebPMuxAssemble(mux.get(), &output) == WEBP_MUX_OK);
    Bytes result(output.bytes, output.bytes + output.size); WebPDataClear(&output); return result;
}

inline const Bytes transparent_png{0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,
    0x49,0x48,0x44,0x52,0,0,0,1,0,0,0,1,8,6,0,0,0,0x1f,0x15,0xc4,0x89,
    0,0,0,0x0a,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0,1,0,0,5,0,1,0x0d,0x0a,0x2d,0xb4,
    0,0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82};
} // namespace pr6_fixtures
