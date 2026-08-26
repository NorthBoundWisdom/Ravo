#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kColorCorrectionOperationId = "ravo.color.colorcorrection";
inline constexpr std::int64_t kColorCorrectionOperationSchemaVersion = 1;
inline constexpr std::string_view kColorCorrectionWorkingSpaceLabD50 = "lab_d50";
inline constexpr std::string_view kColorCorrectionAlgorithmAffineLabV1 = "affine_lab_v1";

inline constexpr double kColorCorrectionEndpointMin = -40.0;
inline constexpr double kColorCorrectionEndpointMax = 40.0;
inline constexpr double kColorCorrectionSaturationMin = -3.0;
inline constexpr double kColorCorrectionSaturationMax = 3.0;

struct ColorCorrectionParams
{
    double highlight_a = 0.0;
    double highlight_b = 0.0;
    double shadow_a = 0.0;
    double shadow_b = 0.0;
    double saturation = 1.0;

    [[nodiscard]] bool operator==(const ColorCorrectionParams &) const noexcept = default;
};

[[nodiscard]] Result<ColorCorrectionParams> color_correction_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
color_correction_to_parameters(const ColorCorrectionParams &params);
[[nodiscard]] Result<void> validate_color_correction_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
