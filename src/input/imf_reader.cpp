/**
 * imf_reader.cpp - IMF package parser
 *
 * Parses ASSETMAP.xml → CPL (ST 2067-3) → resolves MXF assets
 * IMF CPLs use segments and sequences rather than DCP-style reels.
 */

#include "imf_reader.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#include <AS_02.h>
#include <AS_DCP.h>
#include <KM_fileio.h>
#include <MXF.h>
#include <Metadata.h>

#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>

namespace fs = std::filesystem;
using namespace xercesc;

namespace dcpconv {

namespace {

std::string to_str(const XMLCh* xmlch) {
    if (!xmlch) return "";
    char* ch = XMLString::transcode(xmlch);
    std::string result(ch);
    XMLString::release(&ch);
    return result;
}

std::string get_text(DOMElement* parent, const char* tag) {
    auto* nodes = parent->getElementsByTagName(XMLString::transcode(tag));
    if (nodes->getLength() > 0) {
        auto* elem = dynamic_cast<DOMElement*>(nodes->item(0));
        if (elem && elem->getTextContent())
            return to_str(elem->getTextContent());
    }
    return "";
}

struct XercesInit {
    XercesInit()  { XMLPlatformUtils::Initialize(); }
    ~XercesInit() { XMLPlatformUtils::Terminate(); }
};

} // anonymous namespace

Asset IMFReader::read_mxf_info(const std::string& filepath, Asset::Type type) {
    Asset asset;
    asset.type = type;
    asset.filepath = filepath;

    // AS-02 MXFReader has no FillPictureDescriptor/FillAudioDescriptor.
    // Read metadata from the MXF header via OP1aHeader() descriptors.
    Kumu::FileReaderFactory defaultFactory;
    if (type == Asset::Type::VIDEO) {
        AS_02::JP2K::MXFReader reader(defaultFactory);
        if (ASDCP_SUCCESS(reader.OpenRead(filepath.c_str()))) {
            ASDCP::MXF::OP1aHeader& header = reader.OP1aHeader();
            std::list<ASDCP::MXF::InterchangeObject*> obj_list;
            header.GetMDObjectsByType(
                ASDCP::DefaultSMPTEDict().Type(ASDCP::MDD_JPEG2000PictureSubDescriptor).ul,
                obj_list);
            // Fallback: try RGBADescriptor for dimensions
            if (obj_list.empty()) {
                header.GetMDObjectsByType(
                    ASDCP::DefaultSMPTEDict().Type(ASDCP::MDD_RGBAEssenceDescriptor).ul,
                    obj_list);
            }
            // Extract from CDCI or RGBA descriptor
            std::list<ASDCP::MXF::InterchangeObject*> desc_list;
            header.GetMDObjectsByType(
                ASDCP::DefaultSMPTEDict().Type(ASDCP::MDD_CDCIEssenceDescriptor).ul,
                desc_list);
            if (desc_list.empty()) {
                header.GetMDObjectsByType(
                    ASDCP::DefaultSMPTEDict().Type(ASDCP::MDD_RGBAEssenceDescriptor).ul,
                    desc_list);
            }
            for (auto* obj : desc_list) {
                auto* gd = dynamic_cast<ASDCP::MXF::GenericPictureEssenceDescriptor*>(obj);
                if (gd) {
                    asset.width = gd->StoredWidth;
                    asset.height = gd->StoredHeight;
                    asset.edit_rate_num = gd->SampleRate.Numerator;
                    asset.edit_rate_den = gd->SampleRate.Denominator;
                    asset.duration_frames = gd->ContainerDuration;
                    break;
                }
            }
        }
    } else if (type == Asset::Type::AUDIO) {
        AS_02::PCM::MXFReader reader(defaultFactory);
        if (ASDCP_SUCCESS(reader.OpenRead(filepath.c_str(),
                          ASDCP::Rational(24, 1)))) {
            ASDCP::MXF::OP1aHeader& header = reader.OP1aHeader();
            std::list<ASDCP::MXF::InterchangeObject*> obj_list;
            header.GetMDObjectsByType(
                ASDCP::DefaultSMPTEDict().Type(ASDCP::MDD_WaveAudioDescriptor).ul,
                obj_list);
            for (auto* obj : obj_list) {
                auto* wd = dynamic_cast<ASDCP::MXF::WaveAudioDescriptor*>(obj);
                if (wd) {
                    asset.channels = wd->ChannelCount;
                    asset.sample_rate = wd->AudioSamplingRate.Numerator;
                    asset.bits_per_sample = wd->QuantizationBits;
                    asset.duration_frames = wd->ContainerDuration;
                    break;
                }
            }
        }
    }

    return asset;
}

std::vector<IMFReader::AssetMapEntry>
IMFReader::parse_assetmap(const std::string& path) {
    std::vector<AssetMapEntry> entries;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);
    parser.parse(path.c_str());

    DOMDocument* doc = parser.getDocument();
    if (!doc) throw std::runtime_error("Failed to parse ASSETMAP: " + path);

    auto* assets = doc->getElementsByTagName(XMLString::transcode("Asset"));
    for (XMLSize_t i = 0; i < assets->getLength(); ++i) {
        auto* asset = dynamic_cast<DOMElement*>(assets->item(i));
        if (!asset) continue;

        AssetMapEntry entry;
        entry.uuid = get_text(asset, "Id");
        if (entry.uuid.substr(0, 9) == "urn:uuid:")
            entry.uuid = entry.uuid.substr(9);

        auto* paths = asset->getElementsByTagName(XMLString::transcode("Path"));
        if (paths->getLength() > 0) {
            entry.filepath = to_str(
                dynamic_cast<DOMElement*>(paths->item(0))->getTextContent());
        }

        if (!entry.uuid.empty() && !entry.filepath.empty())
            entries.push_back(entry);
    }

    return entries;
}

Composition IMFReader::parse_cpl(const std::string& path,
                                  const std::string& imf_root,
                                  const std::vector<AssetMapEntry>& asset_map) {
    Composition comp;

    XercesInit xerces;
    XercesDOMParser parser;
    parser.setValidationScheme(XercesDOMParser::Val_Never);
    parser.parse(path.c_str());

    DOMDocument* doc = parser.getDocument();
    if (!doc) throw std::runtime_error("Failed to parse CPL: " + path);

    DOMElement* root = doc->getDocumentElement();
    comp.uuid = get_text(root, "Id");
    if (comp.uuid.substr(0, 9) == "urn:uuid:")
        comp.uuid = comp.uuid.substr(9);

    comp.title = get_text(root, "ContentTitle");
    if (comp.title.empty())
        comp.title = get_text(root, "ContentTitleText");

    // IMF CPL uses Segments → Sequences → Resources
    auto* segments = doc->getElementsByTagName(XMLString::transcode("Segment"));
    for (XMLSize_t s = 0; s < segments->getLength(); ++s) {
        auto* segment = dynamic_cast<DOMElement*>(segments->item(s));
        if (!segment) continue;

        // Each sequence type: MainImageSequence, MainAudioSequence, SubtitlesSequence
        auto process_sequence = [&](const char* seq_tag, Asset::Type type,
                                     std::vector<Asset>& target) {
            auto* sequences = segment->getElementsByTagName(
                XMLString::transcode(seq_tag));
            for (XMLSize_t q = 0; q < sequences->getLength(); ++q) {
                auto* seq = dynamic_cast<DOMElement*>(sequences->item(q));
                if (!seq) continue;

                // Resources within the sequence
                auto* resources = seq->getElementsByTagName(
                    XMLString::transcode("Resource"));
                for (XMLSize_t r = 0; r < resources->getLength(); ++r) {
                    auto* res = dynamic_cast<DOMElement*>(resources->item(r));
                    if (!res) continue;

                    std::string track_id = get_text(res, "TrackFileId");
                    if (track_id.substr(0, 9) == "urn:uuid:")
                        track_id = track_id.substr(9);

                    // Resolve to file
                    std::string filepath;
                    for (const auto& entry : asset_map) {
                        if (entry.uuid == track_id) {
                            filepath = (fs::path(imf_root) / entry.filepath).string();
                            break;
                        }
                    }

                    if (filepath.empty() || !fs::exists(filepath)) continue;

                    Asset asset = read_mxf_info(filepath, type);
                    asset.uuid = track_id;

                    std::string dur = get_text(res, "SourceDuration");
                    if (!dur.empty())
                        asset.duration_frames = std::stoll(dur);

                    std::string rate = get_text(res, "EditRate");
                    if (!rate.empty()) {
                        sscanf(rate.c_str(), "%d %d",
                               &asset.edit_rate_num, &asset.edit_rate_den);
                        if (comp.edit_rate_num == 0) {
                            comp.edit_rate_num = asset.edit_rate_num;
                            comp.edit_rate_den = asset.edit_rate_den;
                        }
                    }

                    target.push_back(asset);
                }
            }
        };

        process_sequence("MainImageSequence", Asset::Type::VIDEO,
                         comp.video_assets);
        process_sequence("MainAudioSequence", Asset::Type::AUDIO,
                         comp.audio_assets);
        process_sequence("SubtitlesSequence", Asset::Type::SUBTITLE,
                         comp.subtitle_assets);
    }

    // Total frames
    for (const auto& v : comp.video_assets)
        comp.total_frames += v.duration_frames;

    return comp;
}

std::vector<Composition>
IMFReader::parse(const std::string& imf_path) {
    std::string assetmap_path;
    if (fs::exists(fs::path(imf_path) / "ASSETMAP.xml"))
        assetmap_path = (fs::path(imf_path) / "ASSETMAP.xml").string();
    else if (fs::exists(fs::path(imf_path) / "ASSETMAP"))
        assetmap_path = (fs::path(imf_path) / "ASSETMAP").string();
    else
        throw std::runtime_error("No ASSETMAP found in " + imf_path);

    auto asset_map = parse_assetmap(assetmap_path);

    std::vector<Composition> compositions;
    for (const auto& entry : asset_map) {
        std::string full_path = (fs::path(imf_path) / entry.filepath).string();
        if (!fs::exists(full_path)) continue;

        std::ifstream f(full_path);
        std::string content;
        content.resize(2048);
        f.read(&content[0], 2048);

        if (content.find("CompositionPlaylist") != std::string::npos) {
            auto comp = parse_cpl(full_path, imf_path, asset_map);
            compositions.push_back(comp);
        }
    }

    return compositions;
}

} // namespace dcpconv
