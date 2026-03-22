#pragma once
#include "../core/pipeline.h"
#include <string>
#include <vector>

namespace dcpconv {

/**
 * CineCanvas (Interop DCP) subtitle parser
 * Similar to SMPTE-TT but with different XML namespace and structure
 */
class CineCanvasParser {
public:
    std::vector<SubtitleEvent> parse(const std::string& xml);
private:
    int64_t parse_timecode(const std::string& tc, int edit_rate = 24);
};

} // namespace dcpconv
