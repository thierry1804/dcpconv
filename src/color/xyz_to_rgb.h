#pragma once
#include <cstdint>
#include <vector>

namespace dcpconv {

/**
 * XYZtoRGB - Color space conversion for DCP content
 *
 * DCP uses CIE XYZ color space with D65 white point and
 * 2.6 gamma. This converts to Rec.709 (sRGB) for display.
 *
 * Pipeline: XYZ (12-bit, gamma 2.6) → linear XYZ →
 *           linear Rec.709 RGB → sRGB gamma → 8-bit RGB
 */
class XYZtoRGB {
public:
    XYZtoRGB(int width, int height);
    ~XYZtoRGB();

    /**
     * Convert XYZ frame to RGB.
     * Input:  uint16_t[width*height*3] (XYZ, 16-bit)
     * Output: uint8_t[width*height*3]  (RGB, 8-bit)
     */
    void convert(const uint8_t* xyz_in, uint8_t* rgb_out);

private:
    int width_, height_;

    // Precomputed LUTs
    float degamma_lut_[65536];  // 2.6 gamma → linear
    uint8_t regamma_lut_[65536]; // linear (0-65535) → sRGB 8-bit

    // XYZ to Rec.709 3x3 matrix
    static constexpr float xyz_to_709[3][3] = {
        {  3.2406255f, -1.5372080f, -0.4986286f },
        { -0.9689307f,  1.8757561f,  0.0415175f },
        {  0.0557101f, -0.2040211f,  1.0569959f }
    };

    void build_luts();
};

} // namespace dcpconv
