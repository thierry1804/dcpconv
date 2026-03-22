/**
 * dcp_reader.cpp - DCP package parser
 *
 * Uses xerces-c for XML parsing and asdcplib for MXF header reading.
 */

#include "dcp_reader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>

// asdcplib headers
#include <AS_DCP.h>
#include <KM_fileio.h>

// xerces-c headers
#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/sax/HandlerBase.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>

namespace fs = std::filesystem;
using namespace xercesc;

namespace dcpconv {

namespace {

// Helper: XMLCh* to std::string
std::string to_string(const XMLCh* xmlch) {
    if (!xmlch) return "";
    char* ch = XMLString::transcode(xmlch);
    std::string result(ch);
    XMLString::release(&ch);
    return result;
}

// Extract local name from a possibly prefixed tag (e.g., "cpl:Segment" → "Segment")
std::string local_name(DOMElement* elem) {
    std::string name = to_string(elem->getLocalName());
    if (name.empty())
        name = to_string(elem->getNodeName());
    auto colon = name.find(':');
    if (colon != std::string::npos)
        name = name.substr(colon + 1);
    return name;
}

// Recursively find all descendant elements matching a local tag name
void find_elements(DOMElement* parent, const std::string& tag,
                   std::vector<DOMElement*>& out) {
    DOMNodeList* children = parent->getChildNodes();
    for (XMLSize_t i = 0; i < children->getLength(); ++i) {
        DOMNode* node = children->item(i);
        if (node->getNodeType() != DOMNode::ELEMENT_NODE) continue;
        auto* elem = dynamic_cast<DOMElement*>(node);
        if (!elem) continue;
        if (local_name(elem) == tag)
            out.push_back(elem);
        find_elements(elem, tag, out);
    }
}

// Helper: get text content of first child element with given tag
std::string get_element_text(DOMElement* parent, const char* tag) {
    std::vector<DOMElement*> elems;
    find_elements(parent, tag, elems);
    if (!elems.empty() && elems[0]->getTextContent())
        return to_string(elems[0]->getTextContent());
    return "";
}

// RAII wrapper for xerces initialization
struct XercesInit {
    XercesInit()  { XMLPlatformUtils::Initialize(); }
    ~XercesInit() { XMLPlatformUtils::Terminate(); }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// ASSETMAP parsing
// ---------------------------------------------------------------------------

std::vector<DCPReader::AssetMapEntry>
DCPReader::parse_assetmap(const std::string& path) {
    std::vector<AssetMapEntry> entries;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);
    parser.parse(path.c_str());

    DOMDocument* doc = parser.getDocument();
    if (!doc) {
        throw std::runtime_error("Failed to parse ASSETMAP: " + path);
    }

    // Find all <Asset> elements
    std::vector<DOMElement*> assets;
    find_elements(doc->getDocumentElement(), "Asset", assets);
    for (auto* asset : assets) {
        AssetMapEntry entry;
        entry.uuid = get_element_text(asset, "Id");

        // Remove urn:uuid: prefix if present
        if (entry.uuid.size() >= 9 && entry.uuid.substr(0, 9) == "urn:uuid:")
            entry.uuid = entry.uuid.substr(9);

        // Get file path from <ChunkList><Chunk><Path>
        std::vector<DOMElement*> path_elems;
        find_elements(asset, "Path", path_elems);
        if (!path_elems.empty()) {
            entry.filepath = to_string(path_elems[0]->getTextContent());
        }

        if (!entry.uuid.empty() && !entry.filepath.empty()) {
            entries.push_back(entry);
        }
    }

    return entries;
}

// ---------------------------------------------------------------------------
// CPL parsing
// ---------------------------------------------------------------------------

Composition DCPReader::parse_cpl(const std::string& path,
                                  const std::string& dcp_root,
                                  const std::vector<AssetMapEntry>& asset_map,
                                  bool interop) {
    Composition comp;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);
    parser.parse(path.c_str());

    DOMDocument* doc = parser.getDocument();
    if (!doc) {
        throw std::runtime_error("Failed to parse CPL: " + path);
    }

    DOMElement* root = doc->getDocumentElement();
    comp.uuid = get_element_text(root, "Id");
    if (comp.uuid.size() >= 9 && comp.uuid.substr(0, 9) == "urn:uuid:")
        comp.uuid = comp.uuid.substr(9);

    comp.title = get_element_text(root, "ContentTitleText");
    comp.kind = get_element_text(root, "ContentKind");

    // Parse EditRate
    std::string rate_str = get_element_text(root, "EditRate");
    if (!rate_str.empty()) {
        sscanf(rate_str.c_str(), "%d %d",
               &comp.edit_rate_num, &comp.edit_rate_den);
    }

    // Parse reels
    std::vector<DOMElement*> reels;
    find_elements(root, "Reel", reels);
    for (auto* reel : reels) {
        // Look for MainPicture, MainSound, MainSubtitle
        auto process_asset = [&](const char* tag, Asset::Type type,
                                  std::vector<Asset>& target) {
            std::vector<DOMElement*> found;
            find_elements(reel, tag, found);
            if (found.empty()) return;

            auto* elem = found[0];
            if (!elem) return;

            std::string uuid = get_element_text(elem, "Id");
            if (uuid.size() >= 9 && uuid.substr(0, 9) == "urn:uuid:")
                uuid = uuid.substr(9);

            // Resolve UUID to file path via asset map
            std::string filepath;
            for (const auto& entry : asset_map) {
                if (entry.uuid == uuid) {
                    filepath = (fs::path(dcp_root) / entry.filepath).string();
                    break;
                }
            }

            if (filepath.empty() || !fs::exists(filepath)) {
                std::cerr << "Warning: Could not resolve asset " << uuid << "\n";
                return;
            }

            // Read MXF header for detailed info
            Asset asset = read_mxf_info(filepath, type);
            asset.uuid = uuid;
            asset.annotation = get_element_text(elem, "AnnotationText");

            // Duration
            std::string dur_str = get_element_text(elem, "Duration");
            if (!dur_str.empty()) {
                asset.duration_frames = std::stoll(dur_str);
            }

            // Edit rate (per-asset override)
            std::string asset_rate = get_element_text(elem, "EditRate");
            if (!asset_rate.empty()) {
                sscanf(asset_rate.c_str(), "%d %d",
                       &asset.edit_rate_num, &asset.edit_rate_den);
            } else {
                asset.edit_rate_num = comp.edit_rate_num;
                asset.edit_rate_den = comp.edit_rate_den;
            }

            target.push_back(asset);
        };

        process_asset("MainPicture", Asset::Type::VIDEO, comp.video_assets);
        process_asset("MainSound", Asset::Type::AUDIO, comp.audio_assets);
        process_asset("MainSubtitle", Asset::Type::SUBTITLE, comp.subtitle_assets);
    }

    // Calculate total frames
    for (const auto& v : comp.video_assets) {
        comp.total_frames += v.duration_frames;
    }

    return comp;
}

// ---------------------------------------------------------------------------
// MXF header reading via asdcplib
// ---------------------------------------------------------------------------

Asset DCPReader::read_mxf_info(const std::string& filepath, Asset::Type type) {
    Asset asset;
    asset.type = type;
    asset.filepath = filepath;

    Kumu::FileReaderFactory defaultFactory;
    if (type == Asset::Type::VIDEO) {
        ASDCP::JP2K::MXFReader reader(defaultFactory);
        if (ASDCP_SUCCESS(reader.OpenRead(filepath.c_str()))) {
            ASDCP::JP2K::PictureDescriptor desc;
            if (ASDCP_SUCCESS(reader.FillPictureDescriptor(desc))) {
                asset.width = desc.StoredWidth;
                asset.height = desc.StoredHeight;
                asset.edit_rate_num = desc.EditRate.Numerator;
                asset.edit_rate_den = desc.EditRate.Denominator;
                asset.duration_frames = desc.ContainerDuration;
            }
        }
    } else if (type == Asset::Type::AUDIO) {
        ASDCP::PCM::MXFReader reader(defaultFactory);
        if (ASDCP_SUCCESS(reader.OpenRead(filepath.c_str()))) {
            ASDCP::PCM::AudioDescriptor desc;
            if (ASDCP_SUCCESS(reader.FillAudioDescriptor(desc))) {
                asset.channels = desc.ChannelCount;
                asset.sample_rate = desc.AudioSamplingRate.Numerator;
                asset.bits_per_sample = desc.QuantizationBits;
                asset.duration_frames = desc.ContainerDuration;
            }
        }
    }

    return asset;
}

// ---------------------------------------------------------------------------
// Main parse entry point
// ---------------------------------------------------------------------------

std::vector<Composition>
DCPReader::parse(const std::string& dcp_path, bool interop) {
    // Find ASSETMAP
    std::string assetmap_path;
    if (fs::exists(fs::path(dcp_path) / "ASSETMAP"))
        assetmap_path = (fs::path(dcp_path) / "ASSETMAP").string();
    else if (fs::exists(fs::path(dcp_path) / "ASSETMAP.xml"))
        assetmap_path = (fs::path(dcp_path) / "ASSETMAP.xml").string();
    else
        throw std::runtime_error("No ASSETMAP found in " + dcp_path);

    auto asset_map = parse_assetmap(assetmap_path);

    // Find CPL files (they have "CompositionPlaylist" in them)
    std::vector<Composition> compositions;
    for (const auto& entry : asset_map) {
        std::string full_path = (fs::path(dcp_path) / entry.filepath).string();
        if (!fs::exists(full_path)) continue;

        // Check if this is a CPL by reading first few lines
        std::ifstream f(full_path);
        std::string content;
        content.resize(2048);
        f.read(&content[0], 2048);

        if (content.find("CompositionPlaylist") != std::string::npos) {
            auto comp = parse_cpl(full_path, dcp_path, asset_map, interop);
            compositions.push_back(comp);
        }
    }

    return compositions;
}

} // namespace dcpconv
