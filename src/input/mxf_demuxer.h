#pragma once

#include "../core/pipeline.h"
#include <string>
#include <vector>
#include <memory>

namespace dcpconv {

/**
 * MXFDemuxer - Extracts essence data from MXF containers
 *
 * Uses asdcplib for DCP (AS-DCP) and IMF (AS-02) MXF files.
 * Handles JPEG 2000 video frames, PCM audio, and subtitle XML.
 */
class MXFDemuxer {
public:
    MXFDemuxer();
    ~MXFDemuxer();

    // Open an MXF file for reading
    void open(const std::string& filepath, Asset::Type type);

    // Read next compressed J2K frame (video)
    bool read_frame(std::vector<uint8_t>& out);

    // Read PCM audio samples for N samples
    bool read_audio(std::vector<uint8_t>& out, int num_samples);

    // Extract subtitle XML from MXF
    std::string extract_subtitle_xml(const std::string& filepath);

    // Reset to beginning
    void seek(int64_t frame);

    int64_t current_frame() const { return current_frame_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int64_t current_frame_ = 0;
    Asset::Type type_;
};

} // namespace dcpconv
