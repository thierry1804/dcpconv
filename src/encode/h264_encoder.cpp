/**
 * h264_encoder.cpp - H.264 encoding via libx264
 */

#include "h264_encoder.h"
#include <stdint.h>
#include <string.h>
#include <iostream>

extern "C" {
#include <x264.h>
}

namespace dcpconv {

struct H264Encoder::Impl {
    x264_t* encoder = nullptr;
    x264_param_t param;
    x264_picture_t pic_in;
    x264_picture_t pic_out;
    int64_t pts = 0;
    int width = 0;
    int height = 0;

    // RGB → YUV conversion buffer
    std::vector<uint8_t> yuv_buffer;
};

H264Encoder::H264Encoder() : impl_(std::make_unique<Impl>()) {}

H264Encoder::~H264Encoder() {
    if (impl_->encoder) {
        x264_encoder_close(impl_->encoder);
    }
}

void H264Encoder::init(const Config& cfg) {
    impl_->width = cfg.width;
    impl_->height = cfg.height;

    // Configure x264
    x264_param_default_preset(&impl_->param,
                               cfg.preset.c_str(),
                               cfg.tune.c_str());

    impl_->param.i_csp = X264_CSP_I420;
    impl_->param.i_width = cfg.width;
    impl_->param.i_height = cfg.height;
    impl_->param.i_fps_num = cfg.fps_num;
    impl_->param.i_fps_den = cfg.fps_den;
    impl_->param.i_timebase_num = cfg.fps_den;
    impl_->param.i_timebase_den = cfg.fps_num;

    // Bitrate
    impl_->param.rc.i_rc_method = X264_RC_ABR;
    impl_->param.rc.i_bitrate = (int)(cfg.bitrate / 1000); // kbps
    impl_->param.rc.i_vbv_max_bitrate = (int)(cfg.bitrate * 1.5 / 1000);
    impl_->param.rc.i_vbv_buffer_size = (int)(cfg.bitrate * 2 / 1000);

    // Threads
    if (cfg.threads > 0)
        impl_->param.i_threads = cfg.threads;
    else
        impl_->param.i_threads = X264_THREADS_AUTO;

    // High profile, level 4.1 (supports 4K)
    x264_param_apply_profile(&impl_->param, "high");

    // Annexb for muxing
    impl_->param.b_annexb = 1;
    impl_->param.b_repeat_headers = 1;

    // Create encoder
    impl_->encoder = x264_encoder_open(&impl_->param);
    if (!impl_->encoder) {
        throw std::runtime_error("Failed to open x264 encoder");
    }

    // Allocate picture
    x264_picture_init(&impl_->pic_in);
    impl_->pic_in.i_type = X264_TYPE_AUTO;
    impl_->pic_in.img.i_csp = X264_CSP_I420;
    impl_->pic_in.img.i_plane = 3;

    // YUV420 buffer
    impl_->yuv_buffer.resize(cfg.width * cfg.height * 3 / 2);
    impl_->pic_in.img.plane[0] = impl_->yuv_buffer.data();
    impl_->pic_in.img.plane[1] = impl_->yuv_buffer.data() + cfg.width * cfg.height;
    impl_->pic_in.img.plane[2] = impl_->yuv_buffer.data() + cfg.width * cfg.height * 5 / 4;
    impl_->pic_in.img.i_stride[0] = cfg.width;
    impl_->pic_in.img.i_stride[1] = cfg.width / 2;
    impl_->pic_in.img.i_stride[2] = cfg.width / 2;
}

// Simple RGB to YUV420 conversion (BT.709)
static void rgb_to_yuv420(const uint8_t* rgb, uint8_t* y_plane,
                           uint8_t* u_plane, uint8_t* v_plane,
                           int width, int height) {
    // Y plane
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            int idx = (j * width + i) * 3;
            float r = rgb[idx + 0];
            float g = rgb[idx + 1];
            float b = rgb[idx + 2];

            // BT.709
            int y = (int)(0.2126f * r + 0.7152f * g + 0.0722f * b);
            y_plane[j * width + i] = (uint8_t)std::max(0, std::min(255, y));
        }
    }

    // U and V planes (subsampled 2x2)
    int cw = width / 2;
    for (int j = 0; j < height / 2; ++j) {
        for (int i = 0; i < cw; ++i) {
            // Average 2x2 block
            float r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    int idx = ((j * 2 + dy) * width + (i * 2 + dx)) * 3;
                    r += rgb[idx + 0];
                    g += rgb[idx + 1];
                    b += rgb[idx + 2];
                }
            }
            r /= 4; g /= 4; b /= 4;

            // BT.709 chroma
            int u = (int)(-0.1146f * r - 0.3854f * g + 0.5f * b + 128);
            int v = (int)(0.5f * r - 0.4542f * g - 0.0458f * b + 128);

            u_plane[j * cw + i] = (uint8_t)std::max(0, std::min(255, u));
            v_plane[j * cw + i] = (uint8_t)std::max(0, std::min(255, v));
        }
    }
}

bool H264Encoder::encode_frame(const uint8_t* rgb_data,
                                std::vector<uint8_t>& out) {
    out.clear();

    // Convert RGB → YUV420
    rgb_to_yuv420(rgb_data,
                  impl_->pic_in.img.plane[0],
                  impl_->pic_in.img.plane[1],
                  impl_->pic_in.img.plane[2],
                  impl_->width, impl_->height);

    impl_->pic_in.i_pts = impl_->pts++;

    x264_nal_t* nals;
    int num_nals;
    int frame_size = x264_encoder_encode(impl_->encoder, &nals, &num_nals,
                                          &impl_->pic_in, &impl_->pic_out);

    if (frame_size < 0) {
        std::cerr << "x264 encode error\n";
        return false;
    }

    if (frame_size > 0) {
        out.resize(frame_size);
        size_t offset = 0;
        for (int i = 0; i < num_nals; ++i) {
            memcpy(out.data() + offset, nals[i].p_payload, nals[i].i_payload);
            offset += nals[i].i_payload;
        }
    }

    return true;
}

bool H264Encoder::flush(std::vector<uint8_t>& out) {
    out.clear();

    while (x264_encoder_delayed_frames(impl_->encoder) > 0) {
        x264_nal_t* nals;
        int num_nals;
        int frame_size = x264_encoder_encode(impl_->encoder, &nals, &num_nals,
                                              nullptr, &impl_->pic_out);
        if (frame_size > 0) {
            size_t prev = out.size();
            out.resize(prev + frame_size);
            size_t offset = prev;
            for (int i = 0; i < num_nals; ++i) {
                memcpy(out.data() + offset, nals[i].p_payload, nals[i].i_payload);
                offset += nals[i].i_payload;
            }
        }
    }

    return true;
}

} // namespace dcpconv
