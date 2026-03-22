#pragma once
#include <vector>
#include <cstdint>

namespace dcpconv {

class J2KDecoder {
public:
    J2KDecoder();
    ~J2KDecoder();

    /**
     * Decode a JPEG 2000 codestream to raw pixel data.
     * Output is 12-bit XYZ in 16-bit containers (3 components).
     */
    bool decode(const uint8_t* data, size_t size,
                std::vector<uint8_t>& out,
                int& width, int& height);

private:
    // OpenJPEG codec state is created per-frame for thread safety
};

} // namespace dcpconv
