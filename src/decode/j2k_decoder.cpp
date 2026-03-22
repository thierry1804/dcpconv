/**
 * j2k_decoder.cpp - JPEG 2000 decoding via OpenJPEG
 */

#include "j2k_decoder.h"
#include <openjpeg.h>
#include <iostream>
#include <cstring>

namespace dcpconv {

namespace {

void opj_error_callback(const char* msg, void*) {
    std::cerr << "[OpenJPEG Error] " << msg;
}

void opj_warning_callback(const char* msg, void*) {
    // Suppress warnings in normal mode
}

void opj_info_callback(const char* msg, void*) {
    // Suppress info
}

// Memory stream for OpenJPEG
struct MemStream {
    const uint8_t* data;
    size_t size;
    size_t offset;
};

OPJ_SIZE_T mem_read(void* buffer, OPJ_SIZE_T bytes, void* user) {
    auto* s = static_cast<MemStream*>(user);
    OPJ_SIZE_T remaining = s->size - s->offset;
    if (remaining == 0) return (OPJ_SIZE_T)-1;
    OPJ_SIZE_T to_read = (bytes < remaining) ? bytes : remaining;
    memcpy(buffer, s->data + s->offset, to_read);
    s->offset += to_read;
    return to_read;
}

OPJ_OFF_T mem_skip(OPJ_OFF_T bytes, void* user) {
    auto* s = static_cast<MemStream*>(user);
    if (bytes < 0) {
        if (s->offset < (size_t)(-bytes))
            s->offset = 0;
        else
            s->offset += bytes;
    } else {
        s->offset += bytes;
        if (s->offset > s->size) s->offset = s->size;
    }
    return bytes;
}

OPJ_BOOL mem_seek(OPJ_OFF_T bytes, void* user) {
    auto* s = static_cast<MemStream*>(user);
    s->offset = (size_t)bytes;
    if (s->offset > s->size) s->offset = s->size;
    return OPJ_TRUE;
}

} // anonymous namespace

J2KDecoder::J2KDecoder() {}
J2KDecoder::~J2KDecoder() {}

bool J2KDecoder::decode(const uint8_t* data, size_t size,
                        std::vector<uint8_t>& out,
                        int& width, int& height) {
    // Create decoder
    opj_codec_t* codec = opj_create_decompress(OPJ_CODEC_J2K);
    if (!codec) {
        std::cerr << "Failed to create OpenJPEG decoder\n";
        return false;
    }

    opj_set_error_handler(codec, opj_error_callback, nullptr);
    opj_set_warning_handler(codec, opj_warning_callback, nullptr);
    opj_set_info_handler(codec, opj_info_callback, nullptr);

    opj_dparameters_t params;
    opj_set_default_decoder_parameters(&params);

    if (!opj_setup_decoder(codec, &params)) {
        opj_destroy_codec(codec);
        return false;
    }

    // Create memory stream
    MemStream mem{data, size, 0};
    opj_stream_t* stream = opj_stream_create(size, OPJ_TRUE);
    opj_stream_set_read_function(stream, mem_read);
    opj_stream_set_skip_function(stream, mem_skip);
    opj_stream_set_seek_function(stream, mem_seek);
    opj_stream_set_user_data(stream, &mem, nullptr);
    opj_stream_set_user_data_length(stream, size);

    // Read header
    opj_image_t* image = nullptr;
    if (!opj_read_header(stream, codec, &image)) {
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return false;
    }

    // Decode
    if (!opj_decode(codec, stream, image)) {
        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return false;
    }

    opj_end_decompress(codec, stream);

    // Extract pixel data
    // DCP J2K is typically 3 components (XYZ), 12-bit
    if (image->numcomps < 3) {
        std::cerr << "Expected 3 components, got " << image->numcomps << "\n";
        opj_image_destroy(image);
        opj_stream_destroy(stream);
        opj_destroy_codec(codec);
        return false;
    }

    width = image->comps[0].w;
    height = image->comps[0].h;

    // Output as 16-bit per component (XYZ)
    // Layout: [X0, Y0, Z0, X1, Y1, Z1, ...] as uint16_t
    size_t pixel_count = width * height;
    out.resize(pixel_count * 3 * sizeof(uint16_t));
    uint16_t* out_ptr = reinterpret_cast<uint16_t*>(out.data());

    int prec = image->comps[0].prec; // typically 12

    for (size_t i = 0; i < pixel_count; ++i) {
        // Normalize to 16-bit range
        int shift = 16 - prec;
        out_ptr[i * 3 + 0] = (uint16_t)(image->comps[0].data[i] << shift);
        out_ptr[i * 3 + 1] = (uint16_t)(image->comps[1].data[i] << shift);
        out_ptr[i * 3 + 2] = (uint16_t)(image->comps[2].data[i] << shift);
    }

    opj_image_destroy(image);
    opj_stream_destroy(stream);
    opj_destroy_codec(codec);

    return true;
}

} // namespace dcpconv
