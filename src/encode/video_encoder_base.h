#pragma once
#include <vector>
#include <cstdint>
#include <string>

namespace dcpconv {

/**
 * Base class for video encoders
 */
class VideoEncoderBase {
public:
    virtual ~VideoEncoderBase() = default;
    virtual bool encode_frame(const uint8_t* rgb_data,
                              std::vector<uint8_t>& out) = 0;
    virtual bool flush(std::vector<uint8_t>& out) = 0;
};

} // namespace dcpconv
