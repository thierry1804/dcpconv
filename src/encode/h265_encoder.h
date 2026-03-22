#pragma once
#include "video_encoder_base.h"
#include <memory>
#include <string>

namespace dcpconv {

class H265Encoder : public VideoEncoderBase {
public:
    struct Config {
        int width = 0;
        int height = 0;
        int fps_num = 24;
        int fps_den = 1;
        int64_t bitrate = 20000000;
        int threads = 0;
        std::string preset = "slow";
        std::string tune = ""; // "grain" for film content
    };

    H265Encoder();
    ~H265Encoder() override;

    void init(const Config& cfg);
    bool encode_frame(const uint8_t* rgb_data,
                      std::vector<uint8_t>& out) override;
    bool flush(std::vector<uint8_t>& out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dcpconv
