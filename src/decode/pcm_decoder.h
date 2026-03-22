#pragma once
#include <vector>
#include <cstdint>

namespace dcpconv {

/**
 * PCM audio handling for DCP/IMF
 * DCP audio: 48kHz or 96kHz, 24-bit, big-endian, interleaved
 */
class PCMDecoder {
public:
    // Convert 24-bit big-endian PCM to 16-bit little-endian PCM
    static void convert_24to16(const uint8_t* in, size_t in_size,
                               std::vector<int16_t>& out);

    // Convert 24-bit big-endian PCM to 32-bit float
    static void convert_24to_float(const uint8_t* in, size_t in_size,
                                    std::vector<float>& out);
};

} // namespace dcpconv
