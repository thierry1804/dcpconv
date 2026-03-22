#pragma once

#include "../core/pipeline.h"
#include <string>
#include <vector>

namespace dcpconv {

/**
 * DCPReader - Parses DCP packages (both Interop and SMPTE)
 *
 * Reads ASSETMAP → finds PKL → finds CPL → extracts asset references
 * and resolves them to actual file paths.
 */
class DCPReader {
public:
    std::vector<Composition> parse(const std::string& dcp_path, bool interop);

private:
    struct AssetMapEntry {
        std::string uuid;
        std::string filepath;
    };

    // Parse ASSETMAP or ASSETMAP.xml
    std::vector<AssetMapEntry> parse_assetmap(const std::string& path);

    // Parse PKL to identify asset types
    void parse_pkl(const std::string& path,
                   const std::vector<AssetMapEntry>& assets);

    // Parse CPL to build composition
    Composition parse_cpl(const std::string& path,
                          const std::string& dcp_root,
                          const std::vector<AssetMapEntry>& assets,
                          bool interop);

    // Read MXF header to get dimensions, channels, etc.
    Asset read_mxf_info(const std::string& filepath, Asset::Type type);
};

} // namespace dcpconv
