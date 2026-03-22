#pragma once
#include "../core/pipeline.h"
#include "../output/mp4_muxer.h"
#include <vector>

namespace dcpconv {

/**
 * SubtitleMuxer - Converts subtitle events to MP4 text track format
 * (tx3g / mov_text for soft subtitles)
 */
class SubtitleMuxer {
public:
    /**
     * Write subtitle events as a text track to the MP4 muxer.
     * Uses tx3g (3GPP Timed Text) format compatible with most players.
     */
    void write_to_muxer(MP4Muxer& muxer,
                        const std::vector<SubtitleEvent>& subs);

    /**
     * Export subtitles as SRT file (fallback)
     */
    static void export_srt(const std::string& path,
                           const std::vector<SubtitleEvent>& subs);
};

} // namespace dcpconv
