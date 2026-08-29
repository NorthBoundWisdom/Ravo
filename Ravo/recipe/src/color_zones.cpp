#include "ravo/recipe/color_zones.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace ravo
{
namespace
{

[[nodiscard]] TaskError invalid(const std::string_view message,
                                const std::string_view parameter = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"reason", "invalid_color_zones_parameters"}};
    if (!parameter.empty())
        context.emplace("parameter", parameter);
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    return found == parameters.end() ? Result<const ParameterValue *>{invalid(
                                           "Required Color Zones field is missing", name)} :
                                       Result<const ParameterValue *>{&found->second};
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    return parsed == nullptr ?
               Result<std::string>{invalid("Color Zones field must be text", name)} :
               Result<std::string>{*parsed};
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name, const double minimum, const double maximum)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    double parsed = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&value.value()->value); floating != nullptr)
        parsed = *floating;
    else if (const auto *integer = std::get_if<std::int64_t>(&value.value()->value);
             integer != nullptr)
        parsed = static_cast<double>(*integer);
    if (!std::isfinite(parsed) || !std::isfinite(static_cast<float>(parsed)) || parsed < minimum ||
        parsed > maximum)
        return invalid("Color Zones numeric field is outside its supported range", name);
    return parsed;
}

[[nodiscard]] Result<ColorZonesCurve>
curve(const std::map<std::string, ParameterValue, std::less<>> &parameters,
      const std::string_view points_name, const std::string_view interpolation_name)
{
    auto value = required(parameters, points_name);
    auto interpolation_text = text(parameters, interpolation_name);
    if (!value || !interpolation_text)
        return !value ? value.error() : interpolation_text.error();
    const auto *points = std::get_if<ParameterValue::Array>(&value.value()->value);
    if (points == nullptr || points->size() < 2U || points->size() > kColorZonesMaximumNodes)
        return invalid("Color Zones curve must contain 2 through 20 points", points_name);
    ColorZonesCurve result;
    result.points.clear();
    result.points.reserve(points->size());
    for (std::size_t index = 0U; index < points->size(); ++index)
    {
        const auto *pair = std::get_if<ParameterValue::Array>(&(*points)[index].value);
        if (pair == nullptr || pair->size() != 2U)
            return invalid("Color Zones point must contain x and y", points_name);
        std::array<double, 2> values{};
        for (std::size_t component = 0U; component < values.size(); ++component)
        {
            const auto *floating = std::get_if<double>(&(*pair)[component].value);
            const auto *integer = std::get_if<std::int64_t>(&(*pair)[component].value);
            if (floating == nullptr && integer == nullptr)
                return invalid("Color Zones point component must be numeric", points_name);
            values[component] = floating != nullptr ? *floating : static_cast<double>(*integer);
            if (!std::isfinite(values[component]) ||
                !std::isfinite(static_cast<float>(values[component])) || values[component] < 0.0 ||
                values[component] > 1.0)
                return invalid("Color Zones point is outside [0,1]", points_name);
        }
        if (!result.points.empty() &&
            values[0] - result.points.back().x <= kColorZonesMinimumNodeDistance)
            return invalid("Color Zones point x values must be strictly separated", points_name);
        result.points.push_back({values[0], values[1]});
    }
    auto parsed = parse_color_zones_interpolation(interpolation_text.value());
    if (!parsed)
        return parsed.error();
    result.interpolation = parsed.value();
    return result;
}

[[nodiscard]] ParameterValue points_value(const std::vector<ColorZonesPoint> &points)
{
    ParameterValue::Array result;
    result.reserve(points.size());
    for (const auto &point : points)
        result.emplace_back(
            ParameterValue::Array{ParameterValue{point.x}, ParameterValue{point.y}});
    return ParameterValue{std::move(result)};
}

} // namespace

bool ColorZonesParams::is_identity() const noexcept
{
    return std::all_of(curves.begin(), curves.end(),
                       [](const ColorZonesCurve &curve)
                       {
                           return std::all_of(curve.points.begin(), curve.points.end(),
                                              [](const ColorZonesPoint &point)
                                              { return point.y == 0.5; });
                       });
}

std::string_view color_zones_channel_name(const ColorZonesChannel channel) noexcept
{
    switch (channel)
    {
    case ColorZonesChannel::kLightness:
        return "lightness";
    case ColorZonesChannel::kChroma:
        return "chroma";
    case ColorZonesChannel::kHue:
        return "hue";
    }
    return {};
}

Result<ColorZonesChannel> parse_color_zones_channel(const std::string_view name)
{
    for (std::uint8_t index = 0U; index < 3U; ++index)
    {
        const auto value = static_cast<ColorZonesChannel>(index);
        if (color_zones_channel_name(value) == name)
            return value;
    }
    return make_error(
        ErrorCode::kValidation, "Color Zones selection channel is unsupported",
        {{"select_by", std::string(name)}, {"reason", "invalid_color_zones_channel"}});
}

std::string_view
color_zones_interpolation_name(const ColorZonesInterpolation interpolation) noexcept
{
    switch (interpolation)
    {
    case ColorZonesInterpolation::kCubicSpline:
        return "cubic_spline";
    case ColorZonesInterpolation::kCatmullRom:
        return "catmull_rom";
    case ColorZonesInterpolation::kMonotoneHermite:
        return "monotone_hermite";
    }
    return {};
}

Result<ColorZonesInterpolation> parse_color_zones_interpolation(const std::string_view name)
{
    for (std::uint8_t index = 0U; index < 3U; ++index)
    {
        const auto value = static_cast<ColorZonesInterpolation>(index);
        if (color_zones_interpolation_name(value) == name)
            return value;
    }
    return make_error(
        ErrorCode::kValidation, "Color Zones interpolation is unsupported",
        {{"interpolation", std::string(name)}, {"reason", "invalid_color_zones_interpolation"}});
}

Result<ColorZonesParams>
color_zones_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 10> names{"working_space",
                                                     "algorithm",
                                                     "select_by",
                                                     "lightness_curve",
                                                     "lightness_interpolation",
                                                     "chroma_curve",
                                                     "chroma_interpolation",
                                                     "hue_curve",
                                                     "hue_interpolation",
                                                     "strength"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("Color Zones parameters must contain exactly ten known fields");
    auto working = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto select = text(parameters, "select_by");
    auto lightness = curve(parameters, "lightness_curve", "lightness_interpolation");
    auto chroma = curve(parameters, "chroma_curve", "chroma_interpolation");
    auto hue = curve(parameters, "hue_curve", "hue_interpolation");
    auto strength = number(parameters, "strength", -200.0, 200.0);
    if (!working || !algorithm || !select || !lightness || !chroma || !hue || !strength)
        return !working   ? working.error() :
               !algorithm ? algorithm.error() :
               !select    ? select.error() :
               !lightness ? lightness.error() :
               !chroma    ? chroma.error() :
               !hue       ? hue.error() :
                            strength.error();
    if (working.value() != kColorZonesWorkingSpace)
        return invalid("Color Zones working space is unsupported", "working_space");
    if (algorithm.value() != kColorZonesAlgorithm)
        return invalid("Color Zones algorithm is unsupported", "algorithm");
    auto parsed_select = parse_color_zones_channel(select.value());
    if (!parsed_select)
        return parsed_select.error();
    ColorZonesParams result;
    result.select_by = parsed_select.value();
    result.curves = {lightness.value(), chroma.value(), hue.value()};
    result.strength = strength.value();
    if (result.select_by == ColorZonesChannel::kHue)
    {
        for (std::size_t channel = 0U; channel < result.curves.size(); ++channel)
        {
            const auto &points = result.curves[channel].points;
            if (points.front().x + 1.0 - points.back().x <= kColorZonesMinimumNodeDistance)
                return invalid("Periodic Color Zones endpoints are too close",
                               channel == 0U ? "lightness_curve" :
                               channel == 1U ? "chroma_curve" :
                                               "hue_curve");
        }
    }
    return result;
}

Result<std::map<std::string, ParameterValue, std::less<>>>
color_zones_to_parameters(const ColorZonesParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kColorZonesWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kColorZonesAlgorithm)}},
        {"select_by", ParameterValue{std::string(color_zones_channel_name(params.select_by))}},
        {"lightness_curve", points_value(params.curves[0].points)},
        {"lightness_interpolation", ParameterValue{std::string(color_zones_interpolation_name(
                                        params.curves[0].interpolation))}},
        {"chroma_curve", points_value(params.curves[1].points)},
        {"chroma_interpolation", ParameterValue{std::string(color_zones_interpolation_name(
                                     params.curves[1].interpolation))}},
        {"hue_curve", points_value(params.curves[2].points)},
        {"hue_interpolation", ParameterValue{std::string(
                                  color_zones_interpolation_name(params.curves[2].interpolation))}},
        {"strength", ParameterValue{params.strength}},
    };
    auto valid = color_zones_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

} // namespace ravo
