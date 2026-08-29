#pragma once

#include <cstdint>
#include <map>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kColorReconstructionOperationId = "ravo.color.colorreconstruct";
inline constexpr std::int64_t kColorReconstructionOperationSchemaVersion = 1;
inline constexpr std::string_view kColorReconstructionWorkingSpaceLabD50 = "lab_d50";
inline constexpr std::string_view kColorReconstructionAlgorithmBilateralGridV3 =
    "bilateral_grid_v3";

inline constexpr double kColorReconstructionThresholdMin = 50.0;
inline constexpr double kColorReconstructionThresholdMax = 150.0;
inline constexpr double kColorReconstructionSpatialMin = 0.0;
inline constexpr double kColorReconstructionSpatialMax = 1000.0;
inline constexpr double kColorReconstructionRangeMin = 0.0;
inline constexpr double kColorReconstructionRangeMax = 50.0;
inline constexpr double kColorReconstructionHueMin = 0.0;
inline constexpr double kColorReconstructionHueMax = 1.0;

enum class ColorReconstructionPrecedence : std::uint8_t
{
    kNone,
    kChroma,
    kHue,
};

struct ColorReconstructionParams
{
    double threshold = 100.0;
    double spatial = 400.0;
    double range = 10.0;
    double hue = 0.66;
    ColorReconstructionPrecedence precedence = ColorReconstructionPrecedence::kNone;

    [[nodiscard]] bool operator==(const ColorReconstructionParams &) const noexcept = default;
};

[[nodiscard]] std::string_view
color_reconstruction_precedence_name(ColorReconstructionPrecedence precedence) noexcept;
[[nodiscard]] Result<ColorReconstructionPrecedence>
parse_color_reconstruction_precedence(std::string_view name);
[[nodiscard]] Result<ColorReconstructionParams> color_reconstruction_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
color_reconstruction_to_parameters(const ColorReconstructionParams &params);
[[nodiscard]] Result<void> validate_color_reconstruction_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);

} // namespace ravo
