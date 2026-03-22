/**
 * subtitle_muxer.cpp - Soft subtitle track creation
 */

#include "subtitle_muxer.h"
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dcpconv {

void SubtitleMuxer::write_to_muxer(MP4Muxer& muxer,
                                    const std::vector<SubtitleEvent>& subs) {
    // For MP4 soft subs, we write tx3g samples
    // Each sample contains: 2-byte length prefix + UTF-8 text
    for (const auto& sub : subs) {
        std::vector<uint8_t> sample;

        // tx3g format: uint16_t text_length (big endian) + UTF-8 text
        uint16_t text_len = (uint16_t)sub.text.size();
        sample.push_back((text_len >> 8) & 0xFF);
        sample.push_back(text_len & 0xFF);
        sample.insert(sample.end(), sub.text.begin(), sub.text.end());

        muxer.write_subtitle(sample.data(), sample.size(),
                             sub.start_ms, sub.end_ms);
    }
}

void SubtitleMuxer::export_srt(const std::string& path,
                                const std::vector<SubtitleEvent>& subs) {
    std::ofstream out(path);
    if (!out.is_open()) return;

    for (size_t i = 0; i < subs.size(); ++i) {
        const auto& sub = subs[i];

        // Index
        out << (i + 1) << "\n";

        // Timecodes: HH:MM:SS,mmm --> HH:MM:SS,mmm
        auto format_tc = [](int64_t ms) -> std::string {
            int h = (int)(ms / 3600000);
            int m = (int)((ms % 3600000) / 60000);
            int s = (int)((ms % 60000) / 1000);
            int f = (int)(ms % 1000);
            std::ostringstream oss;
            oss << std::setfill('0')
                << std::setw(2) << h << ":"
                << std::setw(2) << m << ":"
                << std::setw(2) << s << ","
                << std::setw(3) << f;
            return oss.str();
        };

        out << format_tc(sub.start_ms) << " --> "
            << format_tc(sub.end_ms) << "\n";

        // Text
        out << sub.text << "\n\n";
    }
}

} // namespace dcpconv
