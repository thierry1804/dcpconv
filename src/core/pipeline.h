#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace dcpconv {

struct PipelineConfig {
    std::string input_path;
    std::string output_path;
    std::string codec = "h264";
    std::string prores_profile = "hq";
    int64_t bitrate_bps = 20000000;
    std::string subs_mode = "auto";
    std::string font_path;
    int font_size = 42;
    std::string font_color = "FFFFFF";
    std::string cpl_filter;
    bool info_only = false;
    bool verbose = false;
    int threads = 0;
};

// Represents a detected asset in a DCP/IMF package
struct Asset {
    enum class Type { VIDEO, AUDIO, SUBTITLE, UNKNOWN };
    Type type;
    std::string filepath;
    std::string uuid;
    std::string annotation;     // human-readable label
    int64_t duration_frames = 0;
    int edit_rate_num = 24;
    int edit_rate_den = 1;
    int width = 0;
    int height = 0;
    int channels = 0;
    int sample_rate = 0;
    int bits_per_sample = 0;
};

// Represents a parsed CPL (Composition Playlist)
struct Composition {
    std::string uuid;
    std::string title;
    std::string kind;           // "feature", "trailer", etc.
    std::vector<Asset> video_assets;
    std::vector<Asset> audio_assets;
    std::vector<Asset> subtitle_assets;
    int edit_rate_num = 24;
    int edit_rate_den = 1;
    int64_t total_frames = 0;
};

// Detected package format
enum class PackageType { DCP_INTEROP, DCP_SMPTE, IMF, UNKNOWN };

struct PackageInfo {
    PackageType type;
    std::string root_path;
    std::vector<Composition> compositions;
};

// Subtitle event for rendering/muxing
struct SubtitleEvent {
    int64_t start_ms;
    int64_t end_ms;
    std::string text;
    std::string font;
    int font_size;
    float v_position;       // 0.0 (top) to 1.0 (bottom)
    float h_position;       // 0.0 (left) to 1.0 (right)
    bool italic = false;
    bool bold = false;
    std::string color;      // hex RRGGBB
};

// Progress callback: (current_frame, total_frames)
using ProgressCallback = std::function<void(int64_t, int64_t)>;

class Pipeline {
public:
    explicit Pipeline(const PipelineConfig& config);
    ~Pipeline();

    // Analyze the package and print info
    void print_info();

    // Run the full conversion pipeline
    int run();

    // Set progress callback
    void set_progress_callback(ProgressCallback cb);

private:
    PipelineConfig config_;
    ProgressCallback progress_cb_;

    // Internal steps
    PackageInfo detect_package();
    Composition select_composition(const PackageInfo& pkg);
    std::vector<SubtitleEvent> parse_subtitles(const Composition& comp);

    void convert(const Composition& comp,
                 const std::vector<SubtitleEvent>& subs);

    // Progress display
    void show_progress(int64_t current, int64_t total);
};

} // namespace dcpconv
