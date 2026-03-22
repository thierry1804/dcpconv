/**
 * aac_encoder.cpp - AAC encoding via FDK-AAC
 *
 * Converts 24-bit PCM (DCP/IMF standard) to AAC-LC.
 * DCP audio is typically 48kHz, 24-bit, 5.1 or 7.1 channels.
 */

#include "aac_encoder.h"
#include <fdk-aac/aacenc_lib.h>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace dcpconv {

struct AACEncoder::Impl {
    HANDLE_AACENCODER encoder = nullptr;
    int channels = 0;        // encoder channels (may be downmixed)
    int input_channels = 0;  // original input channels
    int sample_rate = 0;
    int frame_size = 0;  // samples per channel per frame

    // Intermediate buffer: 24-bit PCM → 16-bit PCM conversion
    std::vector<int16_t> pcm16_buffer;
    // Output buffer
    std::vector<uint8_t> out_buffer;
};

AACEncoder::AACEncoder() : impl_(std::make_unique<Impl>()) {}

AACEncoder::~AACEncoder() {
    if (impl_->encoder) {
        aacEncClose(&impl_->encoder);
    }
}

void AACEncoder::init(const Config& cfg) {
    impl_->input_channels = cfg.channels;
    impl_->channels = cfg.channels;
    impl_->sample_rate = cfg.sample_rate;

    // FDK-AAC may not support >2 channels in some builds.
    // Attempt with requested channels, fall back to stereo downmix.
    int channels_to_use = cfg.channels;
    CHANNEL_MODE mode;
    switch (channels_to_use) {
        case 1: mode = MODE_1; break;
        case 2: mode = MODE_2; break;
        case 6: mode = MODE_1_2_2_1; break;  // 5.1
        case 8: mode = MODE_7_1_REAR_SURROUND; break; // 7.1
        default: mode = MODE_2; channels_to_use = 2; break;
    }

    AACENC_ERROR err = aacEncOpen(&impl_->encoder, 0, channels_to_use);
    if (err != AACENC_OK && channels_to_use > 2) {
        // Fallback to stereo
        std::cerr << "FDK-AAC: " << channels_to_use << "ch not supported, "
                  << "falling back to stereo downmix\n";
        channels_to_use = 2;
        mode = MODE_2;
        err = aacEncOpen(&impl_->encoder, 0, channels_to_use);
    }
    if (err != AACENC_OK) {
        throw std::runtime_error("Failed to open FDK-AAC encoder (error "
                                 + std::to_string(err) + ", channels="
                                 + std::to_string(channels_to_use)
                                 + ", rate=" + std::to_string(cfg.sample_rate) + ")");
    }
    impl_->channels = channels_to_use;

    // AAC-LC
    aacEncoder_SetParam(impl_->encoder, AACENC_AOT, 2);
    aacEncoder_SetParam(impl_->encoder, AACENC_SAMPLERATE, cfg.sample_rate);
    aacEncoder_SetParam(impl_->encoder, AACENC_BITRATE,
                        channels_to_use > 2 ? cfg.bitrate : cfg.bitrate * channels_to_use / cfg.channels);
    aacEncoder_SetParam(impl_->encoder, AACENC_TRANSMUX, 0); // raw AAC
    aacEncoder_SetParam(impl_->encoder, AACENC_CHANNELMODE, mode);

    err = aacEncEncode(impl_->encoder, nullptr, nullptr, nullptr, nullptr);
    if (err != AACENC_OK) {
        throw std::runtime_error("Failed to initialize FDK-AAC encoder (error "
                                 + std::to_string(err) + ")");
    }

    // Get frame size
    AACENC_InfoStruct info;
    aacEncInfo(impl_->encoder, &info);
    impl_->frame_size = info.frameLength;

    impl_->out_buffer.resize(info.maxOutBufBytes);
}

bool AACEncoder::encode(const uint8_t* pcm_data, size_t pcm_size,
                         std::vector<uint8_t>& out) {
    out.clear();

    // Convert 24-bit PCM to 16-bit PCM
    // DCP PCM is typically 24-bit, big-endian, interleaved
    int in_ch = impl_->input_channels;
    int out_ch = impl_->channels;
    int total_samples = pcm_size / 3; // 3 bytes per 24-bit sample
    int num_frames = in_ch > 0 ? total_samples / in_ch : 0;

    if (in_ch == out_ch) {
        // No downmix needed
        impl_->pcm16_buffer.resize(total_samples);
        for (int i = 0; i < total_samples; ++i) {
            int32_t sample = (pcm_data[i * 3] << 24) |
                             (pcm_data[i * 3 + 1] << 16) |
                             (pcm_data[i * 3 + 2] << 8);
            sample >>= 16;
            impl_->pcm16_buffer[i] = (int16_t)sample;
        }
    } else {
        // Downmix to stereo: L = (L + 0.707*C + 0.707*Ls) / 2
        //                     R = (R + 0.707*C + 0.707*Rs) / 2
        // 5.1 layout: L R C LFE Ls Rs
        impl_->pcm16_buffer.resize(num_frames * out_ch);
        for (int f = 0; f < num_frames; ++f) {
            // Decode all input channels for this frame
            int32_t ch_samples[8] = {0};
            for (int c = 0; c < in_ch && c < 8; ++c) {
                int idx = (f * in_ch + c) * 3;
                int32_t s = (pcm_data[idx] << 24) |
                            (pcm_data[idx + 1] << 16) |
                            (pcm_data[idx + 2] << 8);
                ch_samples[c] = s >> 16;
            }
            // Stereo downmix
            int32_t L, R;
            if (in_ch >= 6) {
                // 5.1: L=0, R=1, C=2, LFE=3, Ls=4, Rs=5
                L = ch_samples[0] + (int32_t)(ch_samples[2] * 0.707)
                    + (int32_t)(ch_samples[4] * 0.707);
                R = ch_samples[1] + (int32_t)(ch_samples[2] * 0.707)
                    + (int32_t)(ch_samples[5] * 0.707);
            } else {
                L = ch_samples[0];
                R = in_ch > 1 ? ch_samples[1] : ch_samples[0];
            }
            impl_->pcm16_buffer[f * 2] = (int16_t)std::clamp(L, -32768, 32767);
            impl_->pcm16_buffer[f * 2 + 1] = (int16_t)std::clamp(R, -32768, 32767);
        }
    }

    int num_samples = (int)impl_->pcm16_buffer.size();

    // Set up input buffer
    AACENC_BufDesc in_buf = {0};
    int in_identifier = IN_AUDIO_DATA;
    int in_size = num_samples * sizeof(int16_t);
    int in_elem_size = sizeof(int16_t);
    void* in_ptr = impl_->pcm16_buffer.data();

    in_buf.numBufs = 1;
    in_buf.bufs = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes = &in_size;
    in_buf.bufElSizes = &in_elem_size;

    // Set up output buffer
    AACENC_BufDesc out_buf = {0};
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size = (int)impl_->out_buffer.size();
    int out_elem_size = 1;
    void* out_ptr = impl_->out_buffer.data();

    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    AACENC_InArgs in_args = {0};
    in_args.numInSamples = num_samples;

    AACENC_OutArgs out_args = {0};

    AACENC_ERROR err = aacEncEncode(impl_->encoder,
                                     &in_buf, &out_buf,
                                     &in_args, &out_args);

    if (err != AACENC_OK && err != AACENC_ENCODE_EOF) {
        std::cerr << "FDK-AAC encode error: " << err << "\n";
        return false;
    }

    if (out_args.numOutBytes > 0) {
        out.assign(impl_->out_buffer.data(),
                   impl_->out_buffer.data() + out_args.numOutBytes);
    }

    return true;
}

bool AACEncoder::flush(std::vector<uint8_t>& out) {
    out.clear();

    // Send EOF
    AACENC_BufDesc in_buf = {0};
    int in_identifier = IN_AUDIO_DATA;
    int in_size = 0;
    int in_elem_size = sizeof(int16_t);
    void* in_ptr = nullptr;

    in_buf.numBufs = 1;
    in_buf.bufs = &in_ptr;
    in_buf.bufferIdentifiers = &in_identifier;
    in_buf.bufSizes = &in_size;
    in_buf.bufElSizes = &in_elem_size;

    AACENC_BufDesc out_buf = {0};
    int out_identifier = OUT_BITSTREAM_DATA;
    int out_size = (int)impl_->out_buffer.size();
    int out_elem_size = 1;
    void* out_ptr = impl_->out_buffer.data();

    out_buf.numBufs = 1;
    out_buf.bufs = &out_ptr;
    out_buf.bufferIdentifiers = &out_identifier;
    out_buf.bufSizes = &out_size;
    out_buf.bufElSizes = &out_elem_size;

    AACENC_InArgs in_args = {0};
    in_args.numInSamples = -1; // EOF signal

    AACENC_OutArgs out_args = {0};

    aacEncEncode(impl_->encoder, &in_buf, &out_buf, &in_args, &out_args);

    if (out_args.numOutBytes > 0) {
        out.assign(impl_->out_buffer.data(),
                   impl_->out_buffer.data() + out_args.numOutBytes);
    }

    return true;
}

} // namespace dcpconv
