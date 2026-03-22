/**
 * pcm_decoder.cpp - PCM audio format conversion
 */

#include "pcm_decoder.h"
#include <algorithm>

namespace dcpconv {

void PCMDecoder::convert_24to16(const uint8_t* in, size_t in_size,
                                 std::vector<int16_t>& out) {
    int num_samples = in_size / 3;
    out.resize(num_samples);

    for (int i = 0; i < num_samples; ++i) {
        // 24-bit big-endian → take top 16 bits
        int32_t sample = (in[i * 3] << 24) |
                         (in[i * 3 + 1] << 16) |
                         (in[i * 3 + 2] << 8);
        out[i] = (int16_t)(sample >> 16);
    }
}

void PCMDecoder::convert_24to_float(const uint8_t* in, size_t in_size,
                                     std::vector<float>& out) {
    int num_samples = in_size / 3;
    out.resize(num_samples);

    for (int i = 0; i < num_samples; ++i) {
        int32_t sample = (in[i * 3] << 24) |
                         (in[i * 3 + 1] << 16) |
                         (in[i * 3 + 2] << 8);
        // Normalize to [-1.0, 1.0]
        out[i] = (float)sample / 2147483648.0f;
    }
}

} // namespace dcpconv
