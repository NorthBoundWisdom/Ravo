#pragma once

#include <map>
#include <string>
#include <string_view>

#include "ravo/adapters/crs_xmp.h"

namespace ravo
{
// Parses serialized Lightroom tables as bounded data; never executes Lua.
[[nodiscard]] Result<std::map<std::string, std::string, std::less<>>>
parse_lightroom_develop_fields(std::string_view text);

// Converts independent supported groups with the existing CRS owner. Rejected
// groups are explicit omissions; no Adobe renderer or profile is substituted.
struct LightroomDevelopResult
{
    DevelopParams look;
    CrsLookMask mask;
    bool geometry = false;
    bool texture = false;
    std::size_t compatible_groups = 0;
    std::vector<CrsOmission> omitted;
};

[[nodiscard]] Result<LightroomDevelopResult> import_lightroom_develop(std::string_view text,
                                                                      const AssetDescriptor &asset);

void apply_lightroom_develop(DevelopParams &destination, const LightroomDevelopResult &converted);
} // namespace ravo
