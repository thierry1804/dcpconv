#pragma once

#include "../core/pipeline.h"
#include <string>
#include <vector>

namespace dcpconv {

/**
 * IMFReader - Parses IMF packages
 *
 * IMF is similar to DCP but uses SMPTE ST 2067 standards.
 * Key differences: OPL (Output Profile List), segment-based CPL,
 * and potentially different essence wrapping.
 */
class IMFReader {
public:
    std::vector<Composition> parse(const std::string& imf_path);

private:
    struct AssetMapEntry {
        std::string uuid;
        std::string filepath;
    };

    std::vector<AssetMapEntry> parse_assetmap(const std::string& path);
    Composition parse_cpl(const std::string& path,
                          const std::string& imf_root,
                          const std::vector<AssetMapEntry>& assets);
    Asset read_mxf_info(const std::string& filepath, Asset::Type type);
};

} // namespace dcpconv
