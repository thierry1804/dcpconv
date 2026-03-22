#pragma once
#include "../core/pipeline.h"
#include <string>
#include <vector>

namespace dcpconv {

/**
 * SMPTE-TT (ST 428-7) subtitle parser
 * Used in SMPTE DCP packages
 */
class SMPTETTParser {
public:
    std::vector<SubtitleEvent> parse(const std::string& xml);

private:
    // Parse time code string "HH:MM:SS:FF" or "HH:MM:SS.mmm"
    int64_t parse_timecode(const std::string& tc, int edit_rate = 24);
};

} // namespace dcpconv
