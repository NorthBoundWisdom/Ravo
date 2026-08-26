#pragma once

#include <map>
#include <numbers>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kPrimariesOperationId = "ravo.color.primaries";
inline constexpr double kPrimariesHueMin = -std::numbers::pi;
inline constexpr double kPrimariesHueMax = std::numbers::pi;
inline constexpr double kPrimariesAchromaticTintPurityMin = 0.0;
inline constexpr double kPrimariesAchromaticTintPurityMax = 0.99;
inline constexpr double kPrimariesPrimaryPurityMin = 0.01;
inline constexpr double kPrimariesPrimaryPurityMax = 5.0;

// Canonical scene-linear working-RGB primary adjustments. Hue is radians.
struct PrimariesParams
{
    double achromatic_tint_hue = 0.0;
    double achromatic_tint_purity = 0.0;
    double red_hue = 0.0;
    double red_purity = 1.0;
    double green_hue = 0.0;
    double green_purity = 1.0;
    double blue_hue = 0.0;
    double blue_purity = 1.0;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const PrimariesParams &) const noexcept = default;
};

[[nodiscard]] Result<void>
validate_primaries_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<PrimariesParams>
primaries_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
primaries_to_parameters(const PrimariesParams &params);

} // namespace ravo
