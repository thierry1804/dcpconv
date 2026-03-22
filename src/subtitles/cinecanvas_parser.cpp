/**
 * cinecanvas_parser.cpp - Interop DCP subtitle parser
 *
 * CineCanvas uses <DCSubtitle> root with <Subtitle> children.
 * Very similar structure to SMPTE-TT, different namespace.
 */

#include "cinecanvas_parser.h"

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>

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

std::string local_name_cc(DOMElement* elem) {
    std::string name = to_str(elem->getLocalName());
    if (name.empty()) name = to_str(elem->getNodeName());
    auto colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    return name;
}

void find_elements(DOMElement* parent, const std::string& tag,
                   std::vector<DOMElement*>& out) {
    DOMNodeList* children = parent->getChildNodes();
    for (XMLSize_t i = 0; i < children->getLength(); ++i) {
        DOMNode* node = children->item(i);
        if (node->getNodeType() != DOMNode::ELEMENT_NODE) continue;
        auto* elem = dynamic_cast<DOMElement*>(node);
        if (!elem) continue;
        if (local_name_cc(elem) == tag) out.push_back(elem);
        find_elements(elem, tag, out);
    }
}
} // anonymous

int64_t CineCanvasParser::parse_timecode(const std::string& tc, int edit_rate) {
    int h=0, m=0, s=0, f=0;
    if (sscanf(tc.c_str(), "%d:%d:%d:%d", &h, &m, &s, &f) == 4) {
        return (int64_t)h*3600000 + m*60000 + s*1000 + (int64_t)f*1000/edit_rate;
    }
    if (sscanf(tc.c_str(), "%d:%d:%d", &h, &m, &s) == 3) {
        return (int64_t)h*3600000 + m*60000 + s*1000;
    }
    return 0;
}

std::vector<SubtitleEvent>
CineCanvasParser::parse(const std::string& xml) {
    std::vector<SubtitleEvent> events;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);

    MemBufInputSource source(
        reinterpret_cast<const XMLByte*>(xml.c_str()), xml.size(), "cinecanvas");
    parser.parse(source);

    DOMDocument* doc = parser.getDocument();
    if (!doc) return events;

    int edit_rate = 24;
    DOMElement* root = doc->getDocumentElement();
    std::string rate = get_attr(root, "EditRate");
    if (!rate.empty()) edit_rate = std::stoi(rate);

    std::vector<DOMElement*> subs;
    find_elements(root, "Subtitle", subs);
    for (auto* sub : subs) {
        int64_t start = parse_timecode(get_attr(sub, "TimeIn"), edit_rate);
        int64_t end = parse_timecode(get_attr(sub, "TimeOut"), edit_rate);

        std::vector<DOMElement*> texts;
        find_elements(sub, "Text", texts);
        for (auto* te : texts) {

            SubtitleEvent evt;
            evt.start_ms = start;
            evt.end_ms = end;
            evt.text = to_str(te->getTextContent());

            std::string vpos = get_attr(te, "VPosition");
            std::string valign = get_attr(te, "VAlign");
            if (!vpos.empty()) {
                float v = std::stof(vpos) / 100.0f;
                evt.v_position = (valign == "top") ? v : 1.0f - v;
            } else {
                evt.v_position = 0.9f;
            }
            evt.h_position = 0.5f;

            evt.font_size = 42;
            evt.color = "FFFFFF";
            evt.italic = (get_attr(te, "Italic") == "yes");
            evt.bold = (get_attr(te, "Bold") == "yes");

            if (!evt.text.empty()) events.push_back(evt);
        }
    }

    return events;
}

} // namespace dcpconv
