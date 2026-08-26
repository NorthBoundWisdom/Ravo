#pragma once

#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/color_output.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

struct LegacyXmpImportRequest
{
    std::string_view xmp_utf8;
    AssetDescriptor asset;
};

// Parses a bounded legacy XMP document without loading the frozen module ABI.
[[nodiscard]] Result<Recipe> import_legacy_xmp(const LegacyXmpImportRequest &request);
[[nodiscard]] Result<InputColorParams>
decode_legacy_colorin_parameters(std::string_view encoded_parameters);
[[nodiscard]] Result<OutputColorParams>
decode_legacy_colorout_parameters(std::string_view encoded_parameters);

} // namespace ravo
