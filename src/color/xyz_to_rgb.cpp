/**
 * xyz_to_rgb.cpp - DCP color space conversion
 *
 * Converts CIE XYZ (D65, gamma 2.6) to Rec.709 sRGB.
 * Uses precomputed LUTs for performance.
 */

#include "xyz_to_rgb.h"
#include <cmath>
#include <algorithm>

namespace dcpconv {

constexpr float XYZtoRGB::xyz_to_709[3][3];

XYZtoRGB::XYZtoRGB(int width, int height)
    : width_(width), height_(height)
{
    build_luts();
}

XYZtoRGB::~XYZtoRGB() = default;

void XYZtoRGB::build_luts() {
    // De-gamma LUT: DCP gamma 2.6 → linear
    // DCP encodes as: encoded = linear^(1/2.6)
    // So: linear = encoded^2.6
    for (int i = 0; i < 65536; ++i) {
        float normalized = static_cast<float>(i) / 65535.0f;
        degamma_lut_[i] = std::pow(normalized, 2.6f);
    }

    // Re-gamma LUT: linear (float scaled to 0-65535) → sRGB 8-bit
    // sRGB transfer function
    for (int i = 0; i < 65536; ++i) {
        float linear = static_cast<float>(i) / 65535.0f;

        // Clamp
        linear = std::max(0.0f, std::min(1.0f, linear));

        // sRGB gamma
        float srgb;
        if (linear <= 0.0031308f) {
            srgb = linear * 12.92f;
        } else {
            srgb = 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
        }

        regamma_lut_[i] = static_cast<uint8_t>(
            std::max(0.0f, std::min(255.0f, srgb * 255.0f + 0.5f)));
    }
}

void XYZtoRGB::convert(const uint8_t* xyz_in, uint8_t* rgb_out) {
    const uint16_t* src = reinterpret_cast<const uint16_t*>(xyz_in);
    size_t pixel_count = width_ * height_;

    for (size_t i = 0; i < pixel_count; ++i) {
        // Read XYZ values and de-gamma
        float x = degamma_lut_[src[i * 3 + 0]];
        float y = degamma_lut_[src[i * 3 + 1]];
        float z = degamma_lut_[src[i * 3 + 2]];

        // XYZ → linear Rec.709 RGB
        float r_lin = xyz_to_709[0][0] * x + xyz_to_709[0][1] * y + xyz_to_709[0][2] * z;
        float g_lin = xyz_to_709[1][0] * x + xyz_to_709[1][1] * y + xyz_to_709[1][2] * z;
        float b_lin = xyz_to_709[2][0] * x + xyz_to_709[2][1] * y + xyz_to_709[2][2] * z;

        // Clamp to [0, 1] and scale to LUT range
        auto to_lut_index = [](float v) -> uint16_t {
            v = std::max(0.0f, std::min(1.0f, v));
            return static_cast<uint16_t>(v * 65535.0f + 0.5f);
        };

        // Apply sRGB gamma via LUT
        rgb_out[i * 3 + 0] = regamma_lut_[to_lut_index(r_lin)];
        rgb_out[i * 3 + 1] = regamma_lut_[to_lut_index(g_lin)];
        rgb_out[i * 3 + 2] = regamma_lut_[to_lut_index(b_lin)];
    }
}

} // namespace dcpconv
