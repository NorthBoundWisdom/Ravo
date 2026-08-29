#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kColorZonesOperationId = "ravo.color.colorzones";
inline constexpr std::int64_t kColorZonesOperationSchemaVersion = 1;
inline constexpr std::string_view kColorZonesWorkingSpace = "lab_d50";
inline constexpr std::string_view kColorZonesAlgorithm = "frozen_colorzones_v3";
inline constexpr std::size_t kColorZonesMaximumNodes = 20U;
inline constexpr double kColorZonesMinimumNodeDistance = 0.0025;

enum class ColorZonesChannel : std::uint8_t
{
    kLightness = 0,
    kChroma,
    kHue,
};

enum class ColorZonesInterpolation : std::uint8_t
{
    kCubicSpline = 0,
    kCatmullRom,
    kMonotoneHermite,
};

struct ColorZonesPoint
{
    double x = 0.0;
    double y = 0.5;

    [[nodiscard]] bool operator==(const ColorZonesPoint &) const noexcept = default;
};

struct ColorZonesCurve
{
    std::vector<ColorZonesPoint> points{{0.25, 0.5}, {0.75, 0.5}};
    ColorZonesInterpolation interpolation = ColorZonesInterpolation::kCatmullRom;

    [[nodiscard]] bool operator==(const ColorZonesCurve &) const noexcept = default;
};

struct ColorZonesParams
{
    ColorZonesChannel select_by = ColorZonesChannel::kHue;
    std::array<ColorZonesCurve, 3> curves;
    double strength = 0.0;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const ColorZonesParams &) const noexcept = default;
};

[[nodiscard]] std::string_view color_zones_channel_name(ColorZonesChannel channel) noexcept;
[[nodiscard]] Result<ColorZonesChannel> parse_color_zones_channel(std::string_view name);
[[nodiscard]] std::string_view
color_zones_interpolation_name(ColorZonesInterpolation interpolation) noexcept;
[[nodiscard]] Result<ColorZonesInterpolation>
parse_color_zones_interpolation(std::string_view name);
[[nodiscard]] Result<ColorZonesParams>
color_zones_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
color_zones_to_parameters(const ColorZonesParams &params);

} // namespace ravo
