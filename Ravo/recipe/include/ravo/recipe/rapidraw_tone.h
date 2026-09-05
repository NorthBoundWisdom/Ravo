#pragma once

#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/operation.h"

namespace ravo
{

inline constexpr std::string_view kRapidRawBasicToneOperationId =
    "ravo.display.rapidraw-basic";
inline constexpr int kRapidRawBasicToneSchemaVersion = 1;
inline constexpr std::string_view kRapidRawBasicToneWorkingSpace = "linear_srgb";

[[nodiscard]] Result<void> validate_rapidraw_basic_tone_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
