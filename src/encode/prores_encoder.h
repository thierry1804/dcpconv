#pragma once
#include "video_encoder_base.h"
#include <memory>
#include <string>

namespace dcpconv {

/**
 * ProRes encoder - software implementation
 *
 * Implements Apple ProRes encoding using the publicly documented
 * bitstream format. Supports profiles: Proxy, LT, Standard, HQ, 4444.
 *
 * ProRes is an intra-frame codec (each frame is independent),
 * using DCT-based compression with wavelet-like quality.
 */
class ProResEncoder : public VideoEncoderBase {
public:
    struct Config {
        int width = 0;
        int height = 0;
        int fps_num = 24;
        int fps_den = 1;
        std::string profile = "hq"; // proxy, lt, standard, hq, 4444
    };

    ProResEncoder();
    ~ProResEncoder() override;

    void init(const Config& cfg);
    bool encode_frame(const uint8_t* rgb_data,
                      std::vector<uint8_t>& out) override;
    bool flush(std::vector<uint8_t>& out) override;

public:
    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace dcpconv
