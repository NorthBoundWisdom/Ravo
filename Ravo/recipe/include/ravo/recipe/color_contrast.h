#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kColorContrastOperationId = "ravo.color.colorcontrast";
inline constexpr std::int64_t kColorContrastOperationSchemaVersion = 2;
inline constexpr std::string_view kColorContrastWorkingSpaceLabD50 = "lab_d50";
inline constexpr std::string_view kColorContrastAlgorithmAxisAffineV2 = "axis_affine_v2";

inline constexpr double kColorContrastSteepnessMin = 0.0;
inline constexpr double kColorContrastSteepnessMax = 5.0;

struct ColorContrastParams
{
    double a_steepness = 1.0;
    double a_offset = 0.0;
    double b_steepness = 1.0;
    double b_offset = 0.0;
    bool unbound = true;

    [[nodiscard]] bool operator==(const ColorContrastParams &) const noexcept = default;
};

[[nodiscard]] Result<ColorContrastParams> color_contrast_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
color_contrast_to_parameters(const ColorContrastParams &params);
[[nodiscard]] Result<void> validate_color_contrast_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<void> upgrade_color_contrast_operation(OperationInstance &operation);

} // namespace ravo
