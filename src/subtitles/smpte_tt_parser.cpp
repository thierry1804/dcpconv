/**
 * smpte_tt_parser.cpp - SMPTE Timed Text subtitle parser
 */

#include "smpte_tt_parser.h"

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>

#include <iostream>
#include <sstream>

using namespace xercesc;

namespace dcpconv {

namespace {

struct XercesInit {
    XercesInit()  { XMLPlatformUtils::Initialize(); }
    ~XercesInit() { XMLPlatformUtils::Terminate(); }
};

std::string to_str(const XMLCh* xmlch) {
    if (!xmlch) return "";
    char* ch = XMLString::transcode(xmlch);
    std::string result(ch);
    XMLString::release(&ch);
    return result;
}

std::string get_attr(DOMElement* elem, const char* attr) {
    XMLCh* name = XMLString::transcode(attr);
    const XMLCh* val = elem->getAttribute(name);
    XMLString::release(&name);
    return to_str(val);
}

} // anonymous namespace

int64_t SMPTETTParser::parse_timecode(const std::string& tc, int edit_rate) {
    int h = 0, m = 0, s = 0, f = 0;

    if (tc.find(':') != std::string::npos) {
        // Try HH:MM:SS:FF
        if (sscanf(tc.c_str(), "%d:%d:%d:%d", &h, &m, &s, &f) == 4) {
            int64_t ms = (int64_t)h * 3600000 + m * 60000 + s * 1000;
            ms += (int64_t)f * 1000 / edit_rate;
            return ms;
        }
        // Try HH:MM:SS.mmm
        int ms_part = 0;
        if (sscanf(tc.c_str(), "%d:%d:%d.%d", &h, &m, &s, &ms_part) == 4) {
            return (int64_t)h * 3600000 + m * 60000 + s * 1000 + ms_part;
        }
        // HH:MM:SS
        if (sscanf(tc.c_str(), "%d:%d:%d", &h, &m, &s) == 3) {
            return (int64_t)h * 3600000 + m * 60000 + s * 1000;
        }
    }

    return 0;
}

std::vector<SubtitleEvent>
SMPTETTParser::parse(const std::string& xml) {
    std::vector<SubtitleEvent> events;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);

    MemBufInputSource source(
        reinterpret_cast<const XMLByte*>(xml.c_str()),
        xml.size(), "smpte_tt");
    parser.parse(source);

    DOMDocument* doc = parser.getDocument();
    if (!doc) return events;

    // Get edit rate from SubtitleReel or root
    int edit_rate = 24;
    DOMElement* root = doc->getDocumentElement();
    std::string rate_str = get_attr(root, "EditRate");
    if (!rate_str.empty()) {
        int num, den;
        if (sscanf(rate_str.c_str(), "%d %d", &num, &den) == 2 && den > 0) {
            edit_rate = num / den;
        }
    }

    // Default font properties
    std::string default_font;
    int default_size = 42;
    std::string default_color = "FFFFFF";

    // Parse <Font> defaults at SubtitleList level
    auto* fonts = doc->getElementsByTagName(XMLString::transcode("Font"));
    if (fonts->getLength() > 0) {
        auto* font_elem = dynamic_cast<DOMElement*>(fonts->item(0));
        std::string f = get_attr(font_elem, "ID");
        if (!f.empty()) default_font = f;
        std::string sz = get_attr(font_elem, "Size");
        if (!sz.empty()) default_size = std::stoi(sz);
        std::string col = get_attr(font_elem, "Color");
        if (!col.empty()) default_color = col;
    }

    // Parse <Subtitle> elements
    auto* subs = doc->getElementsByTagName(XMLString::transcode("Subtitle"));
    for (XMLSize_t i = 0; i < subs->getLength(); ++i) {
        auto* sub = dynamic_cast<DOMElement*>(subs->item(i));
        if (!sub) continue;

        std::string time_in = get_attr(sub, "TimeIn");
        std::string time_out = get_attr(sub, "TimeOut");

        if (time_in.empty() || time_out.empty()) continue;

        int64_t start = parse_timecode(time_in, edit_rate);
        int64_t end = parse_timecode(time_out, edit_rate);

        // Collect text from <Text> children
        auto* texts = sub->getElementsByTagName(XMLString::transcode("Text"));
        for (XMLSize_t t = 0; t < texts->getLength(); ++t) {
            auto* text_elem = dynamic_cast<DOMElement*>(texts->item(t));
            if (!text_elem) continue;

            SubtitleEvent evt;
            evt.start_ms = start;
            evt.end_ms = end;
            evt.text = to_str(text_elem->getTextContent());

            // Position
            std::string vpos = get_attr(text_elem, "VPosition");
            std::string valign = get_attr(text_elem, "VAlign");
            if (!vpos.empty()) {
                float v = std::stof(vpos) / 100.0f;
                if (valign == "top") evt.v_position = v;
                else evt.v_position = 1.0f - v; // bottom-relative
            } else {
                evt.v_position = 0.9f; // default near bottom
            }

            std::string hpos = get_attr(text_elem, "HPosition");
            evt.h_position = hpos.empty() ? 0.5f : std::stof(hpos) / 100.0f;

            // Font overrides
            evt.font = default_font;
            evt.font_size = default_size;
            evt.color = default_color;

            std::string font_id = get_attr(text_elem, "Font");
            if (!font_id.empty()) evt.font = font_id;
            std::string sz = get_attr(text_elem, "Size");
            if (!sz.empty()) evt.font_size = std::stoi(sz);
            std::string col = get_attr(text_elem, "Color");
            if (!col.empty()) evt.color = col;

            // Style
            std::string italic = get_attr(text_elem, "Italic");
            evt.italic = (italic == "yes");
            std::string bold = get_attr(text_elem, "Bold");
            evt.bold = (bold == "yes");

            if (!evt.text.empty()) {
                events.push_back(evt);
            }
        }
    }

    return events;
}

} // namespace dcpconv
