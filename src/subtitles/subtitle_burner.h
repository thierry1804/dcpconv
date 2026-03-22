#pragma once
#include "../core/pipeline.h"
#include <string>
#include <vector>
#include <memory>

namespace dcpconv {

/**
 * SubtitleBurner - Renders subtitle text onto video frames
 *
 * Uses FreeType for text rendering with anti-aliasing.
 * Supports positioning, colors, italic/bold, and drop shadows.
 */
class SubtitleBurner {
public:
    struct Config {
        std::string font_path;   // TTF/OTF font file
        int font_size = 42;
        std::string font_color = "FFFFFF";
        int width = 0;
        int height = 0;
        int shadow_offset = 2;   // drop shadow pixels
        int outline_width = 1;   // text outline pixels
    };

    explicit SubtitleBurner(const Config& cfg);
    ~SubtitleBurner();

    /**
     * Render active subtitles onto an RGB frame buffer.
     * Modifies rgb_data in place.
     */
    void render(uint8_t* rgb_data, int width, int height,
                double time_ms,
                const std::vector<SubtitleEvent>& subs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dcpconv
