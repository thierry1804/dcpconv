/**
 * h265_encoder.cpp - H.265/HEVC encoding via libx265
 */

#include "h265_encoder.h"
#include <iostream>
#include <cstring>
#include <algorithm>

extern "C" {
#include <x265.h>
}

namespace dcpconv {

struct H265Encoder::Impl {
    x265_encoder* encoder = nullptr;
    x265_param* param = nullptr;
    x265_picture* pic_in = nullptr;
    int64_t pts = 0;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> yuv_buffer;
};

H265Encoder::H265Encoder() : impl_(std::make_unique<Impl>()) {}

H265Encoder::~H265Encoder() {
    if (impl_->encoder) {
        x265_encoder_close(impl_->encoder);
    }
    if (impl_->param) {
        x265_param_free(impl_->param);
    }
    if (impl_->pic_in) {
        x265_picture_free(impl_->pic_in);
    }
}

void H265Encoder::init(const Config& cfg) {
    impl_->width = cfg.width;
    impl_->height = cfg.height;

    // Allocate and configure params
    impl_->param = x265_param_alloc();
    x265_param_default_preset(impl_->param,
                               cfg.preset.c_str(),
                               cfg.tune.empty() ? nullptr : cfg.tune.c_str());

    impl_->param->sourceWidth = cfg.width;
    impl_->param->sourceHeight = cfg.height;
    impl_->param->fpsNum = cfg.fps_num;
    impl_->param->fpsDenom = cfg.fps_den;
    impl_->param->internalCsp = X265_CSP_I420;
    impl_->param->internalBitDepth = 8;

    // Bitrate control
    impl_->param->rc.rateControlMode = X265_RC_ABR;
    impl_->param->rc.bitrate = (int)(cfg.bitrate / 1000); // kbps
    impl_->param->rc.vbvMaxBitrate = (int)(cfg.bitrate * 1.5 / 1000);
    impl_->param->rc.vbvBufferSize = (int)(cfg.bitrate * 2 / 1000);

    // Threads
    if (cfg.threads > 0) {
        impl_->param->frameNumThreads = cfg.threads;
    }

    // Repeat headers for muxing
    impl_->param->bRepeatHeaders = 1;
    impl_->param->bAnnexB = 1;

    // Apply main profile
    x265_param_apply_profile(impl_->param, "main");

    // Create encoder
    impl_->encoder = x265_encoder_open(impl_->param);
    if (!impl_->encoder) {
        throw std::runtime_error("Failed to open x265 encoder");
    }

    // Allocate picture
    impl_->pic_in = x265_picture_alloc();
    x265_picture_init(impl_->param, impl_->pic_in);

    // YUV420 buffer
    impl_->yuv_buffer.resize(cfg.width * cfg.height * 3 / 2);
    impl_->pic_in->planes[0] = impl_->yuv_buffer.data();
    impl_->pic_in->planes[1] = impl_->yuv_buffer.data() + cfg.width * cfg.height;
    impl_->pic_in->planes[2] = impl_->yuv_buffer.data() + cfg.width * cfg.height * 5 / 4;
    impl_->pic_in->stride[0] = cfg.width;
    impl_->pic_in->stride[1] = cfg.width / 2;
    impl_->pic_in->stride[2] = cfg.width / 2;
}

// RGB to YUV420 (BT.709) - shared logic with H264
static void rgb_to_yuv420_709(const uint8_t* rgb, uint8_t* y_plane,
                               uint8_t* u_plane, uint8_t* v_plane,
                               int width, int height) {
    for (int j = 0; j < height; ++j) {
        for (int i = 0; i < width; ++i) {
            int idx = (j * width + i) * 3;
            float r = rgb[idx + 0];
            float g = rgb[idx + 1];
            float b = rgb[idx + 2];
            int y = (int)(0.2126f * r + 0.7152f * g + 0.0722f * b);
            y_plane[j * width + i] = (uint8_t)std::max(0, std::min(255, y));
        }
    }

    int cw = width / 2;
    for (int j = 0; j < height / 2; ++j) {
        for (int i = 0; i < cw; ++i) {
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
            int u = (int)(-0.1146f * r - 0.3854f * g + 0.5f * b + 128);
            int v = (int)(0.5f * r - 0.4542f * g - 0.0458f * b + 128);
            u_plane[j * cw + i] = (uint8_t)std::max(0, std::min(255, u));
            v_plane[j * cw + i] = (uint8_t)std::max(0, std::min(255, v));
        }
    }
}

bool H265Encoder::encode_frame(const uint8_t* rgb_data,
                                std::vector<uint8_t>& out) {
    out.clear();

    rgb_to_yuv420_709(rgb_data,
                      (uint8_t*)impl_->pic_in->planes[0],
                      (uint8_t*)impl_->pic_in->planes[1],
                      (uint8_t*)impl_->pic_in->planes[2],
                      impl_->width, impl_->height);

    impl_->pic_in->pts = impl_->pts++;

    x265_nal* nals = nullptr;
    uint32_t num_nals = 0;

    int ret = x265_encoder_encode(impl_->encoder, &nals, &num_nals,
                                   impl_->pic_in, nullptr);

    if (ret < 0) {
        std::cerr << "x265 encode error\n";
        return false;
    }

    if (num_nals > 0) {
        // Calculate total size
        size_t total = 0;
        for (uint32_t i = 0; i < num_nals; ++i)
            total += nals[i].sizeBytes;

        out.resize(total);
        size_t offset = 0;
        for (uint32_t i = 0; i < num_nals; ++i) {
            memcpy(out.data() + offset, nals[i].payload, nals[i].sizeBytes);
            offset += nals[i].sizeBytes;
        }
    }

    return true;
}

bool H265Encoder::flush(std::vector<uint8_t>& out) {
    out.clear();

    x265_nal* nals = nullptr;
    uint32_t num_nals = 0;

    while (true) {
        int ret = x265_encoder_encode(impl_->encoder, &nals, &num_nals,
                                       nullptr, nullptr);
        if (ret <= 0) break;

        for (uint32_t i = 0; i < num_nals; ++i) {
            size_t prev = out.size();
            out.resize(prev + nals[i].sizeBytes);
            memcpy(out.data() + prev, nals[i].payload, nals[i].sizeBytes);
        }
    }

    return true;
}

} // namespace dcpconv
