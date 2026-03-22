/**
 * dcpconv - DCP/IMF to MP4/ProRes converter
 *
 * Standalone command-line tool for macOS.
 * Converts Digital Cinema Packages and Interoperable Master Format
 * packages to MP4 (H.264/H.265) or MOV (ProRes).
 */

#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <getopt.h>

#include "core/pipeline.h"

namespace fs = std::filesystem;

struct Config {
    std::string input_path;
    std::string output_path;
    std::string codec = "h264";       // h264, h265, prores
    std::string prores_profile = "hq"; // proxy, lt, standard, hq, 4444
    int bitrate_mbps = 20;
    std::string subs_mode = "none";   // none, soft, burn-in, both
    std::string font_path;
    int font_size = 42;
    std::string font_color = "FFFFFF";
    std::string cpl_filter;           // select CPL by keyword
    bool info_only = false;
    bool verbose = false;
    int threads = 0;                  // 0 = auto
};

void print_usage(const char* progname) {
    std::cout << R"(
dcpconv - DCP/IMF to MP4/ProRes Converter v0.1.0

USAGE:
    )" << progname << R"( <input_dir> -o <output_file> [options]

INPUT:
    <input_dir>              Path to DCP or IMF package directory

OUTPUT:
    -o, --output <file>      Output file path (.mp4 or .mov)

VIDEO ENCODING:
    -c, --codec <name>       Video codec: h264 (default), h265, prores
    -b, --bitrate <N>M       Video bitrate in Mbps (default: 20)
    --prores-profile <name>  ProRes profile: proxy, lt, standard, hq (default), 4444

SUBTITLES:
    -s, --subs <mode>        Subtitle mode: none (default), soft, burn-in, both
    --font <path>            Font file for burn-in (default: system Helvetica)
    --font-size <N>          Font size in pixels (default: 42)
    --font-color <hex>       Font color as hex RGB (default: FFFFFF)

SELECTION:
    --cpl <keyword>          Select CPL containing keyword (if multiple CPLs)
    --info                   Show package contents without converting

GENERAL:
    -t, --threads <N>        Number of encoding threads (default: auto)
    -v, --verbose            Verbose output
    -h, --help               Show this help

EXAMPLES:
    # DCP to H.264 MP4
    )" << progname << R"( /path/to/DCP/ -o output.mp4

    # IMF to H.265 with burned-in subtitles
    )" << progname << R"( /path/to/IMF/ -o output.mp4 -c h265 -s burn-in

    # DCP to ProRes HQ
    )" << progname << R"( /path/to/DCP/ -o output.mov -c prores --prores-profile hq

    # Show package info
    )" << progname << R"( /path/to/DCP/ --info
)";
}

Config parse_args(int argc, char* argv[]) {
    Config cfg;

    static struct option long_options[] = {
        {"output",         required_argument, nullptr, 'o'},
        {"codec",          required_argument, nullptr, 'c'},
        {"bitrate",        required_argument, nullptr, 'b'},
        {"prores-profile", required_argument, nullptr, 'P'},
        {"subs",           required_argument, nullptr, 's'},
        {"font",           required_argument, nullptr, 'F'},
        {"font-size",      required_argument, nullptr, 'S'},
        {"font-color",     required_argument, nullptr, 'C'},
        {"cpl",            required_argument, nullptr, 'L'},
        {"info",           no_argument,       nullptr, 'I'},
        {"threads",        required_argument, nullptr, 't'},
        {"verbose",        no_argument,       nullptr, 'v'},
        {"help",           no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "o:c:b:s:t:vh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'o': cfg.output_path = optarg; break;
            case 'c': cfg.codec = optarg; break;
            case 'b': {
                std::string val(optarg);
                // Remove trailing 'M' or 'm'
                if (!val.empty() && (val.back() == 'M' || val.back() == 'm'))
                    val.pop_back();
                cfg.bitrate_mbps = std::stoi(val);
                break;
            }
            case 'P': cfg.prores_profile = optarg; break;
            case 's': cfg.subs_mode = optarg; break;
            case 'F': cfg.font_path = optarg; break;
            case 'S': cfg.font_size = std::stoi(optarg); break;
            case 'C': cfg.font_color = optarg; break;
            case 'L': cfg.cpl_filter = optarg; break;
            case 'I': cfg.info_only = true; break;
            case 't': cfg.threads = std::stoi(optarg); break;
            case 'v': cfg.verbose = true; break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  print_usage(argv[0]); exit(1);
        }
    }

    // Remaining argument is input path
    if (optind < argc) {
        cfg.input_path = argv[optind];
    }

    return cfg;
}

bool validate_config(const Config& cfg) {
    if (cfg.input_path.empty()) {
        std::cerr << "Error: No input path specified.\n";
        return false;
    }

    if (!fs::exists(cfg.input_path)) {
        std::cerr << "Error: Input path does not exist: " << cfg.input_path << "\n";
        return false;
    }

    if (!fs::is_directory(cfg.input_path)) {
        std::cerr << "Error: Input must be a directory (DCP or IMF package).\n";
        return false;
    }

    if (!cfg.info_only && cfg.output_path.empty()) {
        std::cerr << "Error: No output file specified. Use -o <file>.\n";
        return false;
    }

    // Validate codec
    if (cfg.codec != "h264" && cfg.codec != "h265" && cfg.codec != "prores") {
        std::cerr << "Error: Unknown codec '" << cfg.codec << "'. Use h264, h265, or prores.\n";
        return false;
    }

    // Validate subtitle mode
    if (cfg.subs_mode != "none" && cfg.subs_mode != "soft" &&
        cfg.subs_mode != "burn-in" && cfg.subs_mode != "both") {
        std::cerr << "Error: Unknown subtitle mode '" << cfg.subs_mode
                  << "'. Use none, soft, burn-in, or both.\n";
        return false;
    }

    // ProRes → MOV
    if (cfg.codec == "prores") {
        std::string ext = fs::path(cfg.output_path).extension().string();
        if (ext != ".mov" && ext != ".MOV") {
            std::cerr << "Warning: ProRes is typically in .mov container. "
                      << "Consider using .mov extension.\n";
        }
    }

    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    Config cfg = parse_args(argc, argv);

    if (!validate_config(cfg)) {
        return 1;
    }

    // Build pipeline configuration
    dcpconv::PipelineConfig pipeline_cfg;
    pipeline_cfg.input_path = cfg.input_path;
    pipeline_cfg.output_path = cfg.output_path;
    pipeline_cfg.codec = cfg.codec;
    pipeline_cfg.prores_profile = cfg.prores_profile;
    pipeline_cfg.bitrate_bps = cfg.bitrate_mbps * 1000000;
    pipeline_cfg.subs_mode = cfg.subs_mode;
    pipeline_cfg.font_path = cfg.font_path;
    pipeline_cfg.font_size = cfg.font_size;
    pipeline_cfg.font_color = cfg.font_color;
    pipeline_cfg.cpl_filter = cfg.cpl_filter;
    pipeline_cfg.info_only = cfg.info_only;
    pipeline_cfg.verbose = cfg.verbose;
    pipeline_cfg.threads = cfg.threads;

    // Run
    dcpconv::Pipeline pipeline(pipeline_cfg);

    try {
        if (cfg.info_only) {
            pipeline.print_info();
        } else {
            std::cout << "dcpconv v0.1.0\n";
            std::cout << "Input:  " << cfg.input_path << "\n";
            std::cout << "Output: " << cfg.output_path << "\n";
            std::cout << "Codec:  " << cfg.codec;
            if (cfg.codec == "prores")
                std::cout << " (" << cfg.prores_profile << ")";
            std::cout << "\n";
            if (cfg.subs_mode != "none")
                std::cout << "Subs:   " << cfg.subs_mode << "\n";
            std::cout << "\n";

            int result = pipeline.run();
            if (result == 0) {
                std::cout << "Done!\n";
            }
            return result;
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
