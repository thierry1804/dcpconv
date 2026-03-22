/**
 * ttml_parser.cpp - TTML/IMSC1 subtitle parser for IMF
 */

#include "ttml_parser.h"

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>

#include <regex>
#include <iostream>

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
    std::string r(ch); XMLString::release(&ch); return r;
}

std::string get_attr(DOMElement* e, const char* a) {
    XMLCh* n = XMLString::transcode(a);
    const XMLCh* v = e->getAttribute(n);
    XMLString::release(&n);
    return to_str(v);
}

// Recursively collect all <p> elements
void collect_p_elements(DOMElement* elem, std::vector<DOMElement*>& result) {
    std::string tag = to_str(elem->getLocalName());
    if (tag.empty()) tag = to_str(elem->getNodeName());

    if (tag == "p") {
        result.push_back(elem);
    }

    DOMNodeList* children = elem->getChildNodes();
    for (XMLSize_t i = 0; i < children->getLength(); ++i) {
        DOMNode* child = children->item(i);
        if (child->getNodeType() == DOMNode::ELEMENT_NODE) {
            collect_p_elements(dynamic_cast<DOMElement*>(child), result);
        }
    }
}

// Recursively extract text content, handling <br/> as newlines
std::string extract_text(DOMNode* node) {
    std::string result;
    DOMNodeList* children = node->getChildNodes();
    for (XMLSize_t i = 0; i < children->getLength(); ++i) {
        DOMNode* child = children->item(i);
        if (child->getNodeType() == DOMNode::TEXT_NODE) {
            result += to_str(child->getTextContent());
        } else if (child->getNodeType() == DOMNode::ELEMENT_NODE) {
            std::string tag = to_str(child->getLocalName());
            if (tag == "br") {
                result += "\n";
            } else {
                result += extract_text(child);
            }
        }
    }
    return result;
}

} // anonymous

int64_t TTMLParser::parse_time(const std::string& ts) {
    // Formats:
    // HH:MM:SS.mmm
    // HH:MM:SS:FF (with frame rate)
    // HH:MM:SS.FFF (fractional seconds)
    // NNt (ticks)
    // NN.NNs (seconds)
    // NN.NNms (milliseconds)

    // Try HH:MM:SS.mmm
    int h, m, s, ms;
    if (sscanf(ts.c_str(), "%d:%d:%d.%d", &h, &m, &s, &ms) == 4) {
        // Handle varying decimal places
        std::string frac = ts.substr(ts.find('.') + 1);
        while (frac.size() < 3) frac += "0";
        if (frac.size() > 3) frac = frac.substr(0, 3);
        ms = std::stoi(frac);
        return (int64_t)h * 3600000 + m * 60000 + s * 1000 + ms;
    }

    // Try HH:MM:SS:FF (assume 24fps)
    int f;
    if (sscanf(ts.c_str(), "%d:%d:%d:%d", &h, &m, &s, &f) == 4) {
        return (int64_t)h * 3600000 + m * 60000 + s * 1000 + f * 1000 / 24;
    }

    // Try seconds
    if (ts.back() == 's' && ts[ts.size()-2] != 'm') {
        double sec = std::stod(ts.substr(0, ts.size() - 1));
        return (int64_t)(sec * 1000);
    }

    // Try milliseconds
    if (ts.size() > 2 && ts.substr(ts.size()-2) == "ms") {
        return (int64_t)std::stod(ts.substr(0, ts.size() - 2));
    }

    return 0;
}

std::vector<SubtitleEvent>
TTMLParser::parse(const std::string& xml) {
    std::vector<SubtitleEvent> events;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);
    parser.setDoNamespaces(true);

    MemBufInputSource source(
        reinterpret_cast<const XMLByte*>(xml.c_str()), xml.size(), "ttml");
    parser.parse(source);

    DOMDocument* doc = parser.getDocument();
    if (!doc) return events;

    // Find all <p> elements (subtitle cues) in the <body>
    DOMElement* root = doc->getDocumentElement();
    std::vector<DOMElement*> p_elements;
    collect_p_elements(root, p_elements);

    for (auto* p : p_elements) {
        // TTML uses begin/end or begin/dur attributes
        std::string begin_str = get_attr(p, "begin");
        std::string end_str = get_attr(p, "end");
        std::string dur_str = get_attr(p, "dur");

        if (begin_str.empty()) continue;

        int64_t start = parse_time(begin_str);
        int64_t end = 0;

        if (!end_str.empty()) {
            end = parse_time(end_str);
        } else if (!dur_str.empty()) {
            end = start + parse_time(dur_str);
        } else {
            continue; // No duration info
        }

        SubtitleEvent evt;
        evt.start_ms = start;
        evt.end_ms = end;
        evt.text = extract_text(p);

        // Default positioning
        evt.v_position = 0.9f;
        evt.h_position = 0.5f;
        evt.font_size = 42;
        evt.color = "FFFFFF";

        // Check region/style for positioning
        // (simplified - a full implementation would resolve style refs)
        std::string style = get_attr(p, "style");
        std::string region = get_attr(p, "region");

        if (!evt.text.empty()) {
            events.push_back(evt);
        }
    }

    return events;
}

} // namespace dcpconv
