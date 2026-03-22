#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace dcpconv {

/**
 * MP4Muxer - Writes MP4/MOV containers
 *
 * Uses minimp4 (header-only library) for lightweight MP4 muxing.
 * Supports H.264, H.265, ProRes video, AAC/PCM audio,
 * and tx3g subtitle tracks.
 */
class MP4Muxer {
public:
    struct Config {
        std::string output_path;
        int width = 0;
        int height = 0;
        int fps_num = 24;
        int fps_den = 1;
        bool has_audio = false;
        int audio_channels = 0;
        int audio_sample_rate = 0;
        bool is_prores = false;
        bool has_subtitles = false;
    };

    MP4Muxer();
    ~MP4Muxer();

    void init(const Config& cfg);

    // Write encoded video data (one or more NAL units)
    void write_video(const uint8_t* data, size_t size, int64_t frame);

    // Write encoded audio data
    void write_audio(const uint8_t* data, size_t size, int64_t frame);

    // Write subtitle sample
    void write_subtitle(const uint8_t* data, size_t size,
                        int64_t start_ms, int64_t end_ms);

    // Finalize the file (write moov atom, close)
    void finalize();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dcpconv
