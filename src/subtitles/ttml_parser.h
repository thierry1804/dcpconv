#pragma once
#include "../core/pipeline.h"
#include <string>
#include <vector>

namespace dcpconv {

/**
 * TTML / IMSC1 subtitle parser for IMF packages
 * Based on W3C Timed Text Markup Language
 */
class TTMLParser {
public:
    std::vector<SubtitleEvent> parse(const std::string& xml);
private:
    int64_t parse_time(const std::string& time_str);
};

} // namespace dcpconv
