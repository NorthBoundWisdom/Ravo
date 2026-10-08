#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ravo/domain/types.h"

namespace ravo
{
// Owned values from a closed foreign catalog snapshot; no database handles escape.
struct ForeignCatalogPhoto
{
    std::string foreign_id;
    std::string original_path;
    std::optional<int> rating;
    std::optional<ColorLabel> color_label;
    std::optional<bool> rejected;
    WritableMetadata metadata;
    std::vector<std::string> keywords;
    std::optional<std::string> crs_xmp_path;
    std::vector<std::string> unsupported_adjusts;
    std::optional<std::string> skip_reason;
};
} // namespace ravo
