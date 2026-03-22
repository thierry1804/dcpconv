/**
 * pipeline.cpp - Main conversion pipeline
 *
 * Orchestrates: package detection → CPL parsing → MXF demux →
 * J2K decode → color convert → subtitle overlay → encode → mux
 */

#include "pipeline.h"

#include "../input/dcp_reader.h"
#include "../input/imf_reader.h"
#include "../input/mxf_demuxer.h"
#include "../decode/j2k_decoder.h"
#include "../decode/pcm_decoder.h"
#include "../color/xyz_to_rgb.h"
#include "../subtitles/smpte_tt_parser.h"
#include "../subtitles/cinecanvas_parser.h"
#include "../subtitles/ttml_parser.h"
#include "../subtitles/subtitle_burner.h"
#include "../subtitles/subtitle_muxer.h"
#include "../encode/h264_encoder.h"
#include "../encode/h265_encoder.h"
#include "../encode/prores_encoder.h"
#include "../encode/aac_encoder.h"
#include "../output/mp4_muxer.h"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace dcpconv {

Pipeline::Pipeline(const PipelineConfig& config)
    : config_(config)
{
    // Default progress callback: terminal progress bar
    progress_cb_ = [this](int64_t current, int64_t total) {
        show_progress(current, total);
    };
}

Pipeline::~Pipeline() = default;

void Pipeline::set_progress_callback(ProgressCallback cb) {
    progress_cb_ = std::move(cb);
}

// ---------------------------------------------------------------------------
// Package detection
// ---------------------------------------------------------------------------

PackageInfo Pipeline::detect_package() {
    PackageInfo pkg;
    pkg.root_path = config_.input_path;

    // Look for ASSETMAP or ASSETMAP.xml
    bool has_assetmap = fs::exists(fs::path(config_.input_path) / "ASSETMAP") ||
                        fs::exists(fs::path(config_.input_path) / "ASSETMAP.xml");

    if (!has_assetmap) {
        throw std::runtime_error("No ASSETMAP found in " + config_.input_path +
                                 ". Is this a valid DCP/IMF package?");
    }

    // Detect format by looking for VOLINDEX vs VOLINDEX.xml and CPL namespace
    // DCP Interop: ASSETMAP (no .xml), VOLINDEX
    // DCP SMPTE:   ASSETMAP.xml, VOLINDEX.xml
    // IMF:         ASSETMAP.xml with IMF-specific CPL namespace

    bool has_volindex = fs::exists(fs::path(config_.input_path) / "VOLINDEX");
    bool has_volindex_xml = fs::exists(fs::path(config_.input_path) / "VOLINDEX.xml");

    if (has_volindex && !has_volindex_xml) {
        // Likely Interop DCP
        pkg.type = PackageType::DCP_INTEROP;
        DCPReader reader;
        pkg.compositions = reader.parse(config_.input_path, true /* interop */);
    } else {
        // Could be SMPTE DCP or IMF - need to check CPL namespace
        // Try IMF first (checks for OPL, output profile list)
        bool has_opl = false;
        for (auto& entry : fs::directory_iterator(config_.input_path)) {
            if (entry.path().extension() == ".xml") {
                // Quick check for IMF namespace in file
                // A proper implementation would parse the XML
                std::ifstream f(entry.path());
                std::string line;
                while (std::getline(f, line)) {
                    if (line.find("imf.com") != std::string::npos ||
                        line.find("www.smpte-ra.org/ns/2067") != std::string::npos) {
                        has_opl = true;
                        break;
                    }
                }
                if (has_opl) break;
            }
        }

        if (has_opl) {
            pkg.type = PackageType::IMF;
            IMFReader reader;
            pkg.compositions = reader.parse(config_.input_path);
        } else {
            pkg.type = PackageType::DCP_SMPTE;
            DCPReader reader;
            pkg.compositions = reader.parse(config_.input_path, false /* smpte */);
        }
    }

    if (pkg.compositions.empty()) {
        throw std::runtime_error("No compositions found in package.");
    }

    return pkg;
}

// ---------------------------------------------------------------------------
// Composition selection
// ---------------------------------------------------------------------------

Composition Pipeline::select_composition(const PackageInfo& pkg) {
    if (pkg.compositions.size() == 1) {
        return pkg.compositions[0];
    }

    // If CPL filter specified, find matching
    if (!config_.cpl_filter.empty()) {
        for (const auto& comp : pkg.compositions) {
            std::string title_lower = comp.title;
            std::transform(title_lower.begin(), title_lower.end(),
                           title_lower.begin(), ::tolower);
            std::string filter_lower = config_.cpl_filter;
            std::transform(filter_lower.begin(), filter_lower.end(),
                           filter_lower.begin(), ::tolower);

            if (title_lower.find(filter_lower) != std::string::npos ||
                comp.kind.find(filter_lower) != std::string::npos) {
                return comp;
            }
        }
        throw std::runtime_error("No CPL matching filter '" +
                                 config_.cpl_filter + "' found.");
    }

    // Default: pick the longest composition (usually the feature)
    auto it = std::max_element(pkg.compositions.begin(),
                               pkg.compositions.end(),
                               [](const Composition& a, const Composition& b) {
                                   return a.total_frames < b.total_frames;
                               });
    return *it;
}

// ---------------------------------------------------------------------------
// Subtitle parsing
// ---------------------------------------------------------------------------

std::vector<SubtitleEvent> Pipeline::parse_subtitles(const Composition& comp) {
    std::vector<SubtitleEvent> subs;

    if (config_.subs_mode == "none" || comp.subtitle_assets.empty()) {
        return subs;
    }

    for (const auto& asset : comp.subtitle_assets) {
        // Extract subtitle XML from MXF using asdcplib
        MXFDemuxer demuxer;
        std::string xml_content = demuxer.extract_subtitle_xml(asset.filepath);

        if (xml_content.empty()) {
            std::cerr << "Warning: Could not extract subtitles from "
                      << asset.filepath << "\n";
            continue;
        }

        // Parse based on format
        std::vector<SubtitleEvent> parsed;

        // Try SMPTE-TT first, then CineCanvas, then TTML
        if (xml_content.find("dcst:SubtitleReel") != std::string::npos ||
            xml_content.find("smpte-ra.org") != std::string::npos) {
            SMPTETTParser parser;
            parsed = parser.parse(xml_content);
        } else if (xml_content.find("DCSubtitle") != std::string::npos) {
            CineCanvasParser parser;
            parsed = parser.parse(xml_content);
        } else if (xml_content.find("tt:tt") != std::string::npos ||
                   xml_content.find("ttml") != std::string::npos) {
            TTMLParser parser;
            parsed = parser.parse(xml_content);
        } else {
            std::cerr << "Warning: Unknown subtitle format in "
                      << asset.filepath << "\n";
            continue;
        }

        subs.insert(subs.end(), parsed.begin(), parsed.end());
    }

    // Sort by start time
    std::sort(subs.begin(), subs.end(),
              [](const SubtitleEvent& a, const SubtitleEvent& b) {
                  return a.start_ms < b.start_ms;
              });

    if (config_.verbose) {
        std::cout << "Parsed " << subs.size() << " subtitle events.\n";
    }

    return subs;
}

// ---------------------------------------------------------------------------
// Main conversion
// ---------------------------------------------------------------------------

void Pipeline::convert(const Composition& comp,
                       const std::vector<SubtitleEvent>& subs) {
    // 1. Initialize MXF demuxer for video
    MXFDemuxer video_demux;
    video_demux.open(comp.video_assets[0].filepath, Asset::Type::VIDEO);

    // 2. Initialize MXF demuxer for audio
    MXFDemuxer audio_demux;
    bool has_audio = !comp.audio_assets.empty();
    if (has_audio) {
        audio_demux.open(comp.audio_assets[0].filepath, Asset::Type::AUDIO);
    }

    // 3. Initialize JPEG 2000 decoder
    J2KDecoder j2k;

    // 4. Initialize color converter
    int width = comp.video_assets[0].width;
    int height = comp.video_assets[0].height;
    XYZtoRGB color_conv(width, height);

    // 5. Initialize subtitle burner (if needed)
    std::unique_ptr<SubtitleBurner> sub_burner;
    bool do_burn = (config_.subs_mode == "burn-in" || config_.subs_mode == "both");
    if (do_burn && !subs.empty()) {
        SubtitleBurner::Config burn_cfg;
        burn_cfg.font_path = config_.font_path;
        burn_cfg.font_size = config_.font_size;
        burn_cfg.font_color = config_.font_color;
        burn_cfg.width = width;
        burn_cfg.height = height;
        sub_burner = std::make_unique<SubtitleBurner>(burn_cfg);
    }

    // 6. Initialize video encoder
    std::unique_ptr<VideoEncoderBase> encoder;
    if (config_.codec == "h264") {
        H264Encoder::Config enc_cfg;
        enc_cfg.width = width;
        enc_cfg.height = height;
        enc_cfg.fps_num = comp.edit_rate_num;
        enc_cfg.fps_den = comp.edit_rate_den;
        enc_cfg.bitrate = config_.bitrate_bps;
        enc_cfg.threads = config_.threads;
        auto h264 = std::make_unique<H264Encoder>();
        h264->init(enc_cfg);
        encoder = std::move(h264);
    } else if (config_.codec == "h265") {
        H265Encoder::Config enc_cfg;
        enc_cfg.width = width;
        enc_cfg.height = height;
        enc_cfg.fps_num = comp.edit_rate_num;
        enc_cfg.fps_den = comp.edit_rate_den;
        enc_cfg.bitrate = config_.bitrate_bps;
        enc_cfg.threads = config_.threads;
        auto h265 = std::make_unique<H265Encoder>();
        h265->init(enc_cfg);
        encoder = std::move(h265);
    } else if (config_.codec == "prores") {
        ProResEncoder::Config enc_cfg;
        enc_cfg.width = width;
        enc_cfg.height = height;
        enc_cfg.fps_num = comp.edit_rate_num;
        enc_cfg.fps_den = comp.edit_rate_den;
        enc_cfg.profile = config_.prores_profile;
        auto prores = std::make_unique<ProResEncoder>();
        prores->init(enc_cfg);
        encoder = std::move(prores);
    }

    // 7. Initialize audio encoder
    std::unique_ptr<AACEncoder> aac;
    if (has_audio && config_.codec != "prores") {
        AACEncoder::Config aac_cfg;
        aac_cfg.channels = comp.audio_assets[0].channels;
        aac_cfg.sample_rate = comp.audio_assets[0].sample_rate;
        aac_cfg.bitrate = 256000; // 256 kbps
        aac = std::make_unique<AACEncoder>();
        aac->init(aac_cfg);
    }

    // 8. Initialize MP4/MOV muxer
    MP4Muxer muxer;
    MP4Muxer::Config mux_cfg;
    mux_cfg.output_path = config_.output_path;
    mux_cfg.width = width;
    mux_cfg.height = height;
    mux_cfg.fps_num = comp.edit_rate_num;
    mux_cfg.fps_den = comp.edit_rate_den;
    mux_cfg.has_audio = has_audio;
    mux_cfg.audio_channels = has_audio ? comp.audio_assets[0].channels : 0;
    mux_cfg.audio_sample_rate = has_audio ? comp.audio_assets[0].sample_rate : 0;
    mux_cfg.is_prores = (config_.codec == "prores");

    bool do_soft = (config_.subs_mode == "soft" || config_.subs_mode == "both");
    mux_cfg.has_subtitles = do_soft && !subs.empty();

    muxer.init(mux_cfg);

    // Write soft subs to muxer if needed
    if (do_soft && !subs.empty()) {
        SubtitleMuxer sub_mux;
        sub_mux.write_to_muxer(muxer, subs);
    }

    // 9. Frame-by-frame processing loop
    int64_t total_frames = comp.total_frames;
    auto start_time = std::chrono::steady_clock::now();

    // Frame buffer
    std::vector<uint8_t> j2k_frame;     // compressed J2K
    std::vector<uint8_t> xyz_buffer;     // decoded XYZ pixels
    std::vector<uint8_t> rgb_buffer;     // converted RGB pixels
    std::vector<uint8_t> encoded_video;  // encoded NAL units / ProRes
    std::vector<uint8_t> pcm_buffer;     // raw PCM audio
    std::vector<uint8_t> encoded_audio;  // encoded AAC

    rgb_buffer.resize(width * height * 3);

    for (int64_t frame = 0; frame < total_frames; ++frame) {
        // Read compressed J2K frame from MXF
        if (!video_demux.read_frame(j2k_frame)) {
            std::cerr << "\nWarning: Failed to read video frame " << frame << "\n";
            break;
        }

        // Decode JPEG 2000
        if (!j2k.decode(j2k_frame.data(), j2k_frame.size(),
                        xyz_buffer, width, height)) {
            std::cerr << "\nWarning: Failed to decode J2K frame " << frame << "\n";
            break;
        }

        // Color conversion: XYZ → Rec.709 RGB
        color_conv.convert(xyz_buffer.data(), rgb_buffer.data());

        // Burn-in subtitles if needed
        if (sub_burner) {
            double time_ms = (double)frame * 1000.0 *
                             comp.edit_rate_den / comp.edit_rate_num;
            sub_burner->render(rgb_buffer.data(), width, height,
                               time_ms, subs);
        }

        // Encode video frame
        encoder->encode_frame(rgb_buffer.data(), encoded_video);

        // Write video to muxer
        if (!encoded_video.empty()) {
            muxer.write_video(encoded_video.data(), encoded_video.size(), frame);
        }

        // Read and encode audio (if present)
        if (has_audio) {
            // Read enough PCM samples for one video frame
            int samples_per_frame = comp.audio_assets[0].sample_rate *
                                    comp.edit_rate_den / comp.edit_rate_num;
            if (audio_demux.read_audio(pcm_buffer, samples_per_frame)) {
                if (aac) {
                    aac->encode(pcm_buffer.data(), pcm_buffer.size(),
                                encoded_audio);
                    if (!encoded_audio.empty()) {
                        muxer.write_audio(encoded_audio.data(),
                                          encoded_audio.size(), frame);
                    }
                } else {
                    // ProRes: write PCM directly
                    muxer.write_audio(pcm_buffer.data(),
                                      pcm_buffer.size(), frame);
                }
            }
        }

        // Progress
        if (progress_cb_) {
            progress_cb_(frame + 1, total_frames);
        }
    }

    // Flush encoder
    encoder->flush(encoded_video);
    if (!encoded_video.empty()) {
        muxer.write_video(encoded_video.data(), encoded_video.size(), total_frames);
    }

    if (aac) {
        aac->flush(encoded_audio);
        if (!encoded_audio.empty()) {
            muxer.write_audio(encoded_audio.data(), encoded_audio.size(),
                              total_frames);
        }
    }

    // Finalize
    muxer.finalize();

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    double fps = total_frames / elapsed;

    std::cout << "\n\nEncoding complete: " << total_frames << " frames in "
              << std::fixed << std::setprecision(1) << elapsed << "s ("
              << std::setprecision(1) << fps << " fps)\n";
}

// ---------------------------------------------------------------------------
// Info display
// ---------------------------------------------------------------------------

void Pipeline::print_info() {
    PackageInfo pkg = detect_package();

    std::string type_str;
    switch (pkg.type) {
        case PackageType::DCP_INTEROP: type_str = "DCP (Interop)"; break;
        case PackageType::DCP_SMPTE:   type_str = "DCP (SMPTE)"; break;
        case PackageType::IMF:         type_str = "IMF"; break;
        default:                       type_str = "Unknown"; break;
    }

    std::cout << "Package: " << pkg.root_path << "\n";
    std::cout << "Type:    " << type_str << "\n";
    std::cout << "CPLs:    " << pkg.compositions.size() << "\n\n";

    for (size_t i = 0; i < pkg.compositions.size(); ++i) {
        const auto& comp = pkg.compositions[i];
        double duration_sec = (double)comp.total_frames *
                              comp.edit_rate_den / comp.edit_rate_num;
        int mins = (int)duration_sec / 60;
        int secs = (int)duration_sec % 60;

        std::cout << "CPL #" << (i + 1) << ": " << comp.title << "\n";
        std::cout << "  UUID:     " << comp.uuid << "\n";
        std::cout << "  Kind:     " << comp.kind << "\n";
        std::cout << "  Duration: " << mins << "m " << secs << "s ("
                  << comp.total_frames << " frames)\n";
        std::cout << "  Rate:     " << comp.edit_rate_num << "/"
                  << comp.edit_rate_den << " fps\n";

        std::cout << "  Video:    " << comp.video_assets.size() << " reel(s)";
        if (!comp.video_assets.empty()) {
            std::cout << " — " << comp.video_assets[0].width << "x"
                      << comp.video_assets[0].height;
        }
        std::cout << "\n";

        std::cout << "  Audio:    " << comp.audio_assets.size() << " reel(s)";
        if (!comp.audio_assets.empty()) {
            std::cout << " — " << comp.audio_assets[0].channels << "ch, "
                      << comp.audio_assets[0].sample_rate << " Hz";
        }
        std::cout << "\n";

        std::cout << "  Subs:     " << comp.subtitle_assets.size() << " reel(s)\n";
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// Main run
// ---------------------------------------------------------------------------

int Pipeline::run() {
    // 1. Detect package type and parse structure
    std::cout << "Analyzing package...\n";
    PackageInfo pkg = detect_package();

    std::string type_str;
    switch (pkg.type) {
        case PackageType::DCP_INTEROP: type_str = "DCP (Interop)"; break;
        case PackageType::DCP_SMPTE:   type_str = "DCP (SMPTE)"; break;
        case PackageType::IMF:         type_str = "IMF"; break;
        default:                       type_str = "Unknown"; break;
    }
    std::cout << "Detected: " << type_str << "\n";

    // 2. Select composition
    Composition comp = select_composition(pkg);
    std::cout << "CPL: " << comp.title << "\n";

    double duration_sec = (double)comp.total_frames *
                          comp.edit_rate_den / comp.edit_rate_num;
    int mins = (int)duration_sec / 60;
    int secs = (int)duration_sec % 60;
    std::cout << "Duration: " << mins << "m " << secs << "s\n";

    if (comp.video_assets.empty()) {
        throw std::runtime_error("No video assets in selected composition.");
    }

    std::cout << "Video: " << comp.video_assets[0].width << "x"
              << comp.video_assets[0].height << " @ "
              << comp.edit_rate_num << " fps\n";

    // 3. Parse subtitles
    auto subs = parse_subtitles(comp);
    if (!subs.empty()) {
        std::cout << "Subtitles: " << subs.size() << " events\n";
    }

    // 4. Convert
    std::cout << "\nEncoding...\n";
    convert(comp, subs);

    return 0;
}

// ---------------------------------------------------------------------------
// Progress bar
// ---------------------------------------------------------------------------

void Pipeline::show_progress(int64_t current, int64_t total) {
    if (total == 0) return;

    int percent = (int)(current * 100 / total);
    int bar_width = 40;
    int filled = bar_width * current / total;

    std::cout << "\r[";
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) std::cout << "█";
        else std::cout << "░";
    }
    std::cout << "] " << std::setw(3) << percent << "% "
              << current << "/" << total << " frames" << std::flush;
}

} // namespace dcpconv
