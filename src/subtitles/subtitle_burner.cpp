/**
 * subtitle_burner.cpp - Text rendering onto video frames
 *
 * Uses FreeType for glyph rendering with anti-aliasing.
 * Renders text with drop shadow and optional outline.
 */

#include "subtitle_burner.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H

#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace dcpconv {

struct SubtitleBurner::Impl {
    FT_Library ft_library = nullptr;
    FT_Face ft_face = nullptr;
    Config config;

    // Parse hex color to RGB
    void parse_color(const std::string& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
        unsigned int val = 0;
        sscanf(hex.c_str(), "%x", &val);
        r = (val >> 16) & 0xFF;
        g = (val >> 8) & 0xFF;
        b = val & 0xFF;
    }

    // Render a single line of text at a given position
    void render_text_line(uint8_t* rgb, int img_w, int img_h,
                          const std::string& text,
                          int x, int y,
                          uint8_t cr, uint8_t cg, uint8_t cb,
                          bool shadow) {
        if (shadow) {
            // Draw shadow first (black, offset)
            draw_glyphs(rgb, img_w, img_h, text,
                        x + config.shadow_offset,
                        y + config.shadow_offset,
                        0, 0, 0);
        }
        // Draw main text
        draw_glyphs(rgb, img_w, img_h, text, x, y, cr, cg, cb);
    }

    void draw_glyphs(uint8_t* rgb, int img_w, int img_h,
                     const std::string& text,
                     int pen_x, int pen_y,
                     uint8_t cr, uint8_t cg, uint8_t cb) {
        for (char c : text) {
            FT_UInt glyph_index = FT_Get_Char_Index(ft_face, (FT_ULong)c);
            if (FT_Load_Glyph(ft_face, glyph_index, FT_LOAD_RENDER))
                continue;

            FT_GlyphSlot slot = ft_face->glyph;
            FT_Bitmap& bitmap = slot->bitmap;

            int bx = pen_x + slot->bitmap_left;
            int by = pen_y - slot->bitmap_top;

            for (unsigned int row = 0; row < bitmap.rows; ++row) {
                for (unsigned int col = 0; col < bitmap.width; ++col) {
                    int px = bx + col;
                    int py = by + row;

                    if (px < 0 || px >= img_w || py < 0 || py >= img_h)
                        continue;

                    uint8_t alpha = bitmap.buffer[row * bitmap.pitch + col];
                    if (alpha == 0) continue;

                    int idx = (py * img_w + px) * 3;
                    float a = alpha / 255.0f;

                    // Alpha blend
                    rgb[idx + 0] = (uint8_t)(cr * a + rgb[idx + 0] * (1 - a));
                    rgb[idx + 1] = (uint8_t)(cg * a + rgb[idx + 1] * (1 - a));
                    rgb[idx + 2] = (uint8_t)(cb * a + rgb[idx + 2] * (1 - a));
                }
            }

            pen_x += slot->advance.x >> 6;
        }
    }

    // Calculate text width in pixels
    int text_width(const std::string& text) {
        int width = 0;
        for (char c : text) {
            FT_UInt gi = FT_Get_Char_Index(ft_face, (FT_ULong)c);
            if (FT_Load_Glyph(ft_face, gi, FT_LOAD_DEFAULT)) continue;
            width += ft_face->glyph->advance.x >> 6;
        }
        return width;
    }
};

SubtitleBurner::SubtitleBurner(const Config& cfg)
    : impl_(std::make_unique<Impl>())
{
    impl_->config = cfg;

    if (FT_Init_FreeType(&impl_->ft_library)) {
        throw std::runtime_error("Failed to initialize FreeType");
    }

    // Try custom font path, fall back to system fonts
    std::string font_path = cfg.font_path;
    if (font_path.empty()) {
        // macOS system font paths
        const char* system_fonts[] = {
            "/System/Library/Fonts/Helvetica.ttc",
            "/System/Library/Fonts/HelveticaNeue.ttc",
            "/Library/Fonts/Arial.ttf",
            "/System/Library/Fonts/SFNSMono.ttf",
            "/System/Library/Fonts/Geneva.ttf",
            nullptr
        };
        for (int i = 0; system_fonts[i]; ++i) {
            if (FT_New_Face(impl_->ft_library, system_fonts[i], 0,
                            &impl_->ft_face) == 0) {
                font_path = system_fonts[i];
                break;
            }
        }
        if (!impl_->ft_face) {
            throw std::runtime_error("No system font found for subtitle rendering");
        }
    } else {
        if (FT_New_Face(impl_->ft_library, font_path.c_str(), 0,
                        &impl_->ft_face)) {
            throw std::runtime_error("Failed to load font: " + font_path);
        }
    }

    FT_Set_Pixel_Sizes(impl_->ft_face, 0, cfg.font_size);
}

SubtitleBurner::~SubtitleBurner() {
    if (impl_->ft_face) FT_Done_Face(impl_->ft_face);
    if (impl_->ft_library) FT_Done_FreeType(impl_->ft_library);
}

void SubtitleBurner::render(uint8_t* rgb_data, int width, int height,
                             double time_ms,
                             const std::vector<SubtitleEvent>& subs) {
    // Find active subtitles
    for (const auto& sub : subs) {
        if (time_ms < sub.start_ms || time_ms >= sub.end_ms)
            continue;

        // Parse color
        uint8_t cr, cg, cb;
        std::string color = sub.color.empty() ? impl_->config.font_color : sub.color;
        impl_->parse_color(color, cr, cg, cb);

        // Set font size (may differ per subtitle)
        int size = sub.font_size > 0 ? sub.font_size : impl_->config.font_size;
        FT_Set_Pixel_Sizes(impl_->ft_face, 0, size);

        // Split text by newlines
        std::vector<std::string> lines;
        std::string line;
        for (char c : sub.text) {
            if (c == '\n') {
                lines.push_back(line);
                line.clear();
            } else {
                line += c;
            }
        }
        if (!line.empty()) lines.push_back(line);

        // Calculate vertical position
        int line_height = size + 4;
        int total_height = lines.size() * line_height;
        int base_y = (int)(sub.v_position * height) - total_height / 2;

        for (size_t i = 0; i < lines.size(); ++i) {
            // Center horizontally
            int tw = impl_->text_width(lines[i]);
            int x = (int)(sub.h_position * width) - tw / 2;
            int y = base_y + (int)i * line_height + size;

            impl_->render_text_line(rgb_data, width, height,
                                    lines[i], x, y, cr, cg, cb, true);
        }
    }
}

} // namespace dcpconv
