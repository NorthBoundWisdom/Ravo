#include "develop_internal.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <variant>

namespace ravo::develop_internal
{

const double kEpsilon = 1e-6;

bool near(const double left, const double right) noexcept
{
    return std::abs(left - right) <= kEpsilon;
}

double clamp_value(const double value, const double minimum, const double maximum) noexcept
{
    return std::clamp(value, minimum, maximum);
}

double as_number(const ParameterValue &value, const double fallback)
{
    if (std::holds_alternative<double>(value.value))
    {
        return std::get<double>(value.value);
    }
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return static_cast<double>(std::get<std::int64_t>(value.value));
    }
    if (std::holds_alternative<bool>(value.value))
    {
        return std::get<bool>(value.value) ? 1.0 : 0.0;
    }
    return fallback;
}

std::int64_t as_integer(const ParameterValue &value, const std::int64_t fallback)
{
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return std::get<std::int64_t>(value.value);
    }
    if (std::holds_alternative<double>(value.value))
    {
        return static_cast<std::int64_t>(std::llround(std::get<double>(value.value)));
    }
    if (std::holds_alternative<bool>(value.value))
    {
        return std::get<bool>(value.value) ? 1 : 0;
    }
    return fallback;
}

const std::string *as_string_if(const ParameterValue &value)
{
    return std::get_if<std::string>(&value.value);
}

std::int64_t flag01(const std::int64_t value) noexcept
{
    return value != 0 ? 1 : 0;
}

void add_operation(Recipe &recipe, std::string id, std::string instance_id,
                   std::map<std::string, ParameterValue, std::less<>> parameters,
                   const std::int64_t schema_version, std::optional<std::string> mask_id,
                   const bool enabled)
{
    recipe.operations.push_back({std::move(id), schema_version, std::move(instance_id), enabled,
                                 std::move(parameters), std::move(mask_id)});
}

bool bands_near_zero(const std::array<double, kColorEqualizerBandCount> &values) noexcept
{
    for (const double value : values)
    {
        if (!near(value, 0.0))
        {
            return false;
        }
    }
    return true;
}

void make_studio_color_zones_curves(ColorZonesParams &params)
{
    for (auto &curve : params.curves)
    {
        curve.points.clear();
        curve.points.reserve(kColorEqualizerBandCount);
        for (std::size_t index = 0U; index < kColorEqualizerBandCount; ++index)
        {
            curve.points.push_back(
                {static_cast<double>(index) / static_cast<double>(kColorEqualizerBandCount), 0.5});
        }
        curve.interpolation = ColorZonesInterpolation::kMonotoneHermite;
    }
}

bool studio_color_zones_curves(const ColorZonesParams &params) noexcept
{
    return std::all_of(params.curves.begin(), params.curves.end(), [](const ColorZonesCurve &curve)
                       { return curve.points.size() == kColorEqualizerBandCount; });
}

ParameterValue band_array_parameter(
    const std::array<double, kColorEqualizerBandCount> &values)
{
    ParameterValue::Array array;
    array.reserve(values.size());
    for (const double value : values)
    {
        array.push_back(ParameterValue{value});
    }
    return ParameterValue{std::move(array)};
}

std::array<std::string_view, 7> rgb_levels_preserve_names() noexcept
{
    return {kToneCurvePreserveColorsNone, kToneCurvePreserveColorsLuminance,
            kToneCurvePreserveColorsMax,  kToneCurvePreserveColorsAverage,
            kToneCurvePreserveColorsSum,  kToneCurvePreserveColorsNorm,
            kToneCurvePreserveColorsPower};
}

} // namespace ravo::develop_internal
