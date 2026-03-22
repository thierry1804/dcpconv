#pragma once
#include <vector>
#include <cstdint>
#include <memory>

namespace dcpconv {

class AACEncoder {
public:
    struct Config {
        int channels = 6;         // DCP typically 5.1
        int sample_rate = 48000;
        int bitrate = 256000;     // 256 kbps
    };

    AACEncoder();
    ~AACEncoder();

    void init(const Config& cfg);

    /**
     * Encode PCM samples to AAC.
     * Input: interleaved 24-bit PCM (DCP standard)
     * Output: raw AAC frames
     */
    bool encode(const uint8_t* pcm_data, size_t pcm_size,
                std::vector<uint8_t>& out);

    bool flush(std::vector<uint8_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dcpconv
