#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <iomanip>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ravo
{
namespace
{

constexpr double kEpsilon = 1e-6;

[[nodiscard]] bool near(const double left, const double right) noexcept
{
    return std::abs(left - right) <= kEpsilon;
}

[[nodiscard]] double clamp_value(const double value, const double lo, const double hi) noexcept
{
    return std::clamp(value, lo, hi);
}

[[nodiscard]] double as_number(const ParameterValue &value, const double fallback)
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

[[nodiscard]] std::int64_t as_integer(const ParameterValue &value, const std::int64_t fallback)
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

void add_operation(Recipe &recipe, std::string id, std::string instance_id,
                   std::map<std::string, ParameterValue, std::less<>> parameters,
                   const std::int64_t schema_version = 1,
                   std::optional<std::string> mask_id = std::nullopt, const bool enabled = true)
{
    recipe.operations.push_back({std::move(id), schema_version, std::move(instance_id), enabled,
                                 std::move(parameters), std::move(mask_id)});
}

[[nodiscard]] std::int64_t flag01(const std::int64_t value) noexcept
{
    return value != 0 ? 1 : 0;
}

struct ToneCurveSpline
{
    std::vector<ToneCurvePoint> points;
    std::vector<double> tangents;
};

[[nodiscard]] ToneCurveSpline make_tone_curve_spline(const std::vector<ToneCurvePoint> &points)
{
    ToneCurveSpline spline;
    spline.points = points;
    if (points.size() < 2)
    {
        return spline;
    }
    const auto count = points.size();
    std::vector<double> delta(count - 1U);
    for (std::size_t index = 0; index + 1 < count; ++index)
    {
        const double dx = points[index + 1U].x - points[index].x;
        delta[index] = dx > 1e-12 ? (points[index + 1U].y - points[index].y) / dx : 0.0;
    }
    spline.tangents.assign(count, 0.0);
    spline.tangents.front() = delta.front();
    spline.tangents.back() = delta.back();
    for (std::size_t index = 1; index + 1 < count; ++index)
    {
        if (delta[index - 1U] * delta[index] <= 0.0)
        {
            spline.tangents[index] = 0.0;
        }
        else
        {
            spline.tangents[index] = 0.5 * (delta[index - 1U] + delta[index]);
        }
    }
    for (std::size_t index = 0; index + 1 < count; ++index)
    {
        if (std::abs(delta[index]) <= 1e-12)
        {
            spline.tangents[index] = 0.0;
            spline.tangents[index + 1U] = 0.0;
            continue;
        }
        const double alpha = spline.tangents[index] / delta[index];
        const double beta = spline.tangents[index + 1U] / delta[index];
        const double sumsq = alpha * alpha + beta * beta;
        if (sumsq > 9.0)
        {
            const double tau = 3.0 / std::sqrt(sumsq);
            spline.tangents[index] = tau * alpha * delta[index];
            spline.tangents[index + 1U] = tau * beta * delta[index];
        }
    }
    return spline;
}

[[nodiscard]] double evaluate_tone_curve_spline(const ToneCurveSpline &spline, const double x)
{
    if (spline.points.size() < 2)
    {
        return std::clamp(x, 0.0, 1.0);
    }
    if (x <= spline.points.front().x)
    {
        return spline.points.front().y;
    }
    if (x >= spline.points.back().x)
    {
        return spline.points.back().y;
    }
    std::size_t index = 0;
    while (index + 2 < spline.points.size() && x > spline.points[index + 1U].x)
    {
        ++index;
    }
    const auto &left = spline.points[index];
    const auto &right = spline.points[index + 1U];
    const double dx = right.x - left.x;
    if (dx <= 1e-12)
    {
        return left.y;
    }
    const double t = (x - left.x) / dx;
    const double t2 = t * t;
    const double t3 = t2 * t;
    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;
    return h00 * left.y + h10 * dx * spline.tangents[index] + h01 * right.y +
           h11 * dx * spline.tangents[index + 1U];
}

[[nodiscard]] const std::string *as_string_if(const ParameterValue &value)
{
    return std::get_if<std::string>(&value.value);
}

[[nodiscard]] bool
bands_near_zero(const std::array<double, kColorEqualizerBandCount> &values) noexcept
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
            curve.points.push_back(
                {static_cast<double>(index) / static_cast<double>(kColorEqualizerBandCount), 0.5});
        curve.interpolation = ColorZonesInterpolation::kMonotoneHermite;
    }
}

[[nodiscard]] bool studio_color_zones_curves(const ColorZonesParams &params) noexcept
{
    return std::all_of(params.curves.begin(), params.curves.end(), [](const ColorZonesCurve &curve)
                       { return curve.points.size() == kColorEqualizerBandCount; });
}

[[nodiscard]] ParameterValue
band_array_parameter(const std::array<double, kColorEqualizerBandCount> &values)
{
    ParameterValue::Array array;
    array.reserve(values.size());
    for (const double value : values)
    {
        array.push_back(ParameterValue{value});
    }
    return ParameterValue{std::move(array)};
}

[[nodiscard]] bool
channel_triplet_near(const std::array<double, kChannelMixerChannelCount> &left,
                     const std::array<double, kChannelMixerChannelCount> &right) noexcept
{
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        if (!near(left[index], right[index]))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] ParameterValue
channel_triplet_parameter(const std::array<double, kChannelMixerChannelCount> &values)
{
    ParameterValue::Array array;
    array.reserve(values.size());
    for (const double value : values)
    {
        array.push_back(ParameterValue{value});
    }
    return ParameterValue{std::move(array)};
}

[[nodiscard]] bool temperature_mode_supported(const std::string_view mode) noexcept
{
    return mode == kTemperatureModeAsShot || mode == kTemperatureModeCameraReference ||
           mode == kTemperatureModeAsShotToReference || mode == kTemperatureModeManual;
}

[[nodiscard]] ParameterValue
temperature_coefficients_parameter(const std::array<double, kTemperatureChannelCount> &coefficients)
{
    ParameterValue::Array array;
    array.reserve(coefficients.size());
    for (const double coefficient : coefficients)
    {
        array.emplace_back(coefficient);
    }
    return ParameterValue{std::move(array)};
}

[[nodiscard]] Result<std::array<double, kTemperatureChannelCount>>
parse_temperature_coefficients(const ParameterValue &value)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Temperature coefficients must be an array");
    }
    if (array->size() != kTemperatureChannelCount)
    {
        return make_error(ErrorCode::kValidation,
                          "Temperature coefficients must contain exactly 4 values",
                          {{"count", std::to_string(array->size())}});
    }
    std::array<double, kTemperatureChannelCount> coefficients{};
    for (std::size_t index = 0; index < coefficients.size(); ++index)
    {
        double coefficient = std::numeric_limits<double>::quiet_NaN();
        if (const auto *floating = std::get_if<double>(&(*array)[index].value); floating != nullptr)
        {
            coefficient = *floating;
        }
        else if (const auto *integer = std::get_if<std::int64_t>(&(*array)[index].value);
                 integer != nullptr)
        {
            coefficient = static_cast<double>(*integer);
        }
        if (!std::isfinite(coefficient) || coefficient <= 0.0 || coefficient > 8.0)
        {
            return make_error(ErrorCode::kValidation,
                              "Temperature coefficient must be finite and within (0, 8]",
                              {{"index", std::to_string(index)}});
        }
        coefficients[index] = coefficient;
    }
    return coefficients;
}

[[nodiscard]] bool apply_temperature_field(TemperatureParams &params, const std::string_view name,
                                           const double value) noexcept
{
    if (name == "whiteBalanceMode")
    {
        const int mode = std::clamp(static_cast<int>(std::llround(value)), 0, 3);
        params.mode = mode == 0 ? std::string(kTemperatureModeAsShot) :
                      mode == 1 ? std::string(kTemperatureModeCameraReference) :
                      mode == 2 ? std::string(kTemperatureModeAsShotToReference) :
                                  std::string(kTemperatureModeManual);
        if (params.mode == kTemperatureModeManual)
        {
            if (!params.coefficients)
            {
                params.coefficients =
                    std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
            }
        }
        else
        {
            params.coefficients.reset();
        }
        return true;
    }

    std::size_t index = 0;
    if (name == "whiteBalanceRed")
    {
        index = 0;
    }
    else if (name == "whiteBalanceGreen")
    {
        index = 1;
    }
    else if (name == "whiteBalanceBlue")
    {
        index = 2;
    }
    else if (name == "whiteBalanceFourth")
    {
        index = 3;
    }
    else
    {
        return false;
    }
    if (!params.coefficients)
    {
        params.coefficients = std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    }
    params.mode = std::string(kTemperatureModeManual);
    (*params.coefficients)[index] = value;
    return true;
}

[[nodiscard]] bool reset_temperature_field(TemperatureParams &params,
                                           const std::string_view name) noexcept
{
    if (name == "whiteBalance" || name == "whiteBalanceMode")
    {
        params = TemperatureParams{};
        return true;
    }
    std::size_t index = 0;
    if (name == "whiteBalanceRed")
    {
        index = 0;
    }
    else if (name == "whiteBalanceGreen")
    {
        index = 1;
    }
    else if (name == "whiteBalanceBlue")
    {
        index = 2;
    }
    else if (name == "whiteBalanceFourth")
    {
        index = 3;
    }
    else
    {
        return false;
    }
    if (!params.coefficients)
    {
        params.coefficients = std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    }
    params.mode = std::string(kTemperatureModeManual);
    (*params.coefficients)[index] = 1.0;
    return true;
}

void clamp_temperature(TemperatureParams &params) noexcept
{
    if (!temperature_mode_supported(params.mode))
    {
        params = TemperatureParams{};
        return;
    }
    if (params.coefficients)
    {
        for (double &coefficient : *params.coefficients)
        {
            coefficient = clamp_value(coefficient, 0.000001, 8.0);
        }
    }
    if (params.mode == kTemperatureModeManual && !params.coefficients)
    {
        params.coefficients = std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    }
}

enum class LegacyColorBalanceFieldGroup
{
    kLift,
    kGamma,
    kGain,
    kInputSaturation,
    kContrast,
    kGreyFulcrumPercent,
    kOutputSaturation,
};

struct LegacyColorBalanceNumericField
{
    std::string_view parameter_name;
    std::string_view develop_name;
    LegacyColorBalanceFieldGroup group;
    std::size_t channel;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<LegacyColorBalanceNumericField, 16> &
legacy_color_balance_numeric_fields() noexcept
{
    using Group = LegacyColorBalanceFieldGroup;
    static const std::array<LegacyColorBalanceNumericField, 16> fields{{
        {"lift_factor", "legacyColorBalanceLiftFactor", Group::kLift, 0U, 0.0, 2.0},
        {"lift_red", "legacyColorBalanceLiftRed", Group::kLift, 1U, 0.0, 2.0},
        {"lift_green", "legacyColorBalanceLiftGreen", Group::kLift, 2U, 0.0, 2.0},
        {"lift_blue", "legacyColorBalanceLiftBlue", Group::kLift, 3U, 0.0, 2.0},
        {"gamma_factor", "legacyColorBalanceGammaFactor", Group::kGamma, 0U, 0.0, 2.0},
        {"gamma_red", "legacyColorBalanceGammaRed", Group::kGamma, 1U, 0.0, 2.0},
        {"gamma_green", "legacyColorBalanceGammaGreen", Group::kGamma, 2U, 0.0, 2.0},
        {"gamma_blue", "legacyColorBalanceGammaBlue", Group::kGamma, 3U, 0.0, 2.0},
        {"gain_factor", "legacyColorBalanceGainFactor", Group::kGain, 0U, 0.0, 2.0},
        {"gain_red", "legacyColorBalanceGainRed", Group::kGain, 1U, 0.0, 2.0},
        {"gain_green", "legacyColorBalanceGainGreen", Group::kGain, 2U, 0.0, 2.0},
        {"gain_blue", "legacyColorBalanceGainBlue", Group::kGain, 3U, 0.0, 2.0},
        {"input_saturation", "legacyColorBalanceInputSaturation", Group::kInputSaturation, 0U, 0.0,
         2.0},
        {"contrast", "legacyColorBalanceContrast", Group::kContrast, 0U, 0.01, 1.99},
        {"grey_fulcrum_percent", "legacyColorBalanceGreyFulcrum", Group::kGreyFulcrumPercent, 0U,
         0.1, 100.0},
        {"output_saturation", "legacyColorBalanceOutputSaturation", Group::kOutputSaturation, 0U,
         0.0, 2.0},
    }};
    return fields;
}

[[nodiscard]] double &
legacy_color_balance_value(ColorBalanceParams &params,
                           const LegacyColorBalanceNumericField &field) noexcept
{
    switch (field.group)
    {
    case LegacyColorBalanceFieldGroup::kLift:
        return params.lift[field.channel];
    case LegacyColorBalanceFieldGroup::kGamma:
        return params.gamma[field.channel];
    case LegacyColorBalanceFieldGroup::kGain:
        return params.gain[field.channel];
    case LegacyColorBalanceFieldGroup::kInputSaturation:
        return params.input_saturation;
    case LegacyColorBalanceFieldGroup::kContrast:
        return params.contrast;
    case LegacyColorBalanceFieldGroup::kGreyFulcrumPercent:
        return params.grey_fulcrum_percent;
    case LegacyColorBalanceFieldGroup::kOutputSaturation:
        return params.output_saturation;
    }
    return params.input_saturation;
}

[[nodiscard]] const double &
legacy_color_balance_value(const ColorBalanceParams &params,
                           const LegacyColorBalanceNumericField &field) noexcept
{
    switch (field.group)
    {
    case LegacyColorBalanceFieldGroup::kLift:
        return params.lift[field.channel];
    case LegacyColorBalanceFieldGroup::kGamma:
        return params.gamma[field.channel];
    case LegacyColorBalanceFieldGroup::kGain:
        return params.gain[field.channel];
    case LegacyColorBalanceFieldGroup::kInputSaturation:
        return params.input_saturation;
    case LegacyColorBalanceFieldGroup::kContrast:
        return params.contrast;
    case LegacyColorBalanceFieldGroup::kGreyFulcrumPercent:
        return params.grey_fulcrum_percent;
    case LegacyColorBalanceFieldGroup::kOutputSaturation:
        return params.output_saturation;
    }
    return params.input_saturation;
}

[[nodiscard]] bool apply_legacy_color_balance_field(ColorBalanceParams &params,
                                                    const std::string_view name,
                                                    const double value) noexcept
{
    if (name == "legacyColorBalanceMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.mode = value == 0.0 ? std::string(kColorBalanceModeLiftGammaGain) :
                                     std::string(kColorBalanceModeSlopeOffsetPower);
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        if (name == field.develop_name)
        {
            legacy_color_balance_value(params, field) = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset_legacy_color_balance_field(ColorBalanceParams &params,
                                                    const std::string_view name) noexcept
{
    const ColorBalanceParams defaults;
    if (name == "legacyColorBalance")
    {
        params = defaults;
        return true;
    }
    if (name == "legacyColorBalanceMode")
    {
        params.mode = defaults.mode;
        return true;
    }
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        if (name == field.develop_name)
        {
            legacy_color_balance_value(params, field) = legacy_color_balance_value(defaults, field);
            return true;
        }
    }
    return false;
}

void clamp_legacy_color_balance(ColorBalanceParams &params) noexcept
{
    const ColorBalanceParams defaults;
    if (params.mode != kColorBalanceModeLiftGammaGain &&
        params.mode != kColorBalanceModeSlopeOffsetPower)
    {
        params.mode = defaults.mode;
    }
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        double &value = legacy_color_balance_value(params, field);
        value = std::isfinite(value) ? clamp_value(value, field.minimum, field.maximum) :
                                       legacy_color_balance_value(defaults, field);
    }
}

[[nodiscard]] std::optional<std::size_t>
selected_color_checker_patch(const DevelopParams &params) noexcept
{
    if (params.color_checker_patch < 0 ||
        params.color_checker_patch >=
            static_cast<std::int64_t>(params.color_checker.patches.size()))
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(params.color_checker_patch);
}

[[nodiscard]] bool apply_color_checker_field(DevelopParams &params, const std::string_view name,
                                             const double value)
{
    if (!std::isfinite(value))
    {
        return false;
    }
    if (name == "colorCheckerEnabled")
    {
        params.color_checker_enabled = value >= 0.5;
        return true;
    }
    if (name == "colorCheckerPreset")
    {
        const auto index = static_cast<std::int64_t>(std::llround(value));
        const auto presets = color_checker_presets();
        if (value != static_cast<double>(index) || index < 0 ||
            index >= static_cast<std::int64_t>(presets.size()))
        {
            return false;
        }
        auto preset = color_checker_params_for_preset(presets[static_cast<std::size_t>(index)].id);
        if (!preset)
        {
            return false;
        }
        params.color_checker = std::move(preset).value();
        params.color_checker_enabled = true;
        params.color_checker_patch = 0;
        return true;
    }
    if (name == "colorCheckerPatch")
    {
        const auto index = static_cast<std::int64_t>(std::llround(value));
        if (value != static_cast<double>(index) || index < 0 ||
            index >= static_cast<std::int64_t>(params.color_checker.patches.size()))
        {
            return false;
        }
        params.color_checker_patch = index;
        return true;
    }
    const auto patch = selected_color_checker_patch(params);
    if (!patch || !std::isfinite(static_cast<float>(value)))
    {
        return false;
    }
    auto &selected = params.color_checker.patches[*patch];
    double *component = nullptr;
    if (name == "colorCheckerSourceL")
    {
        component = &selected.source_lab[0];
    }
    else if (name == "colorCheckerSourceA")
    {
        component = &selected.source_lab[1];
    }
    else if (name == "colorCheckerSourceB")
    {
        component = &selected.source_lab[2];
    }
    else if (name == "colorCheckerTargetL")
    {
        component = &selected.target_lab[0];
    }
    else if (name == "colorCheckerTargetA")
    {
        component = &selected.target_lab[1];
    }
    else if (name == "colorCheckerTargetB")
    {
        component = &selected.target_lab[2];
    }
    else
    {
        return false;
    }
    *component = value;
    params.color_checker_enabled = true;
    return true;
}

[[nodiscard]] bool reset_color_checker_field(DevelopParams &params, const std::string_view name)
{
    if (name == "colorChecker")
    {
        params.color_checker_enabled = false;
        params.color_checker = ColorCheckerParams{};
        params.color_checker_patch = 0;
        return true;
    }
    if (name == "colorCheckerEnabled")
    {
        params.color_checker_enabled = false;
        return true;
    }
    if (name == "colorCheckerPatch")
    {
        params.color_checker_patch = 0;
        return true;
    }
    const auto patch = selected_color_checker_patch(params);
    if (!patch)
    {
        return false;
    }
    auto &selected = params.color_checker.patches[*patch];
    if (name == "colorCheckerSourceL")
    {
        selected.source_lab[0] = selected.target_lab[0];
    }
    else if (name == "colorCheckerSourceA")
    {
        selected.source_lab[1] = selected.target_lab[1];
    }
    else if (name == "colorCheckerSourceB")
    {
        selected.source_lab[2] = selected.target_lab[2];
    }
    else if (name == "colorCheckerTargetL")
    {
        selected.target_lab[0] = selected.source_lab[0];
    }
    else if (name == "colorCheckerTargetA")
    {
        selected.target_lab[1] = selected.source_lab[1];
    }
    else if (name == "colorCheckerTargetB")
    {
        selected.target_lab[2] = selected.source_lab[2];
    }
    else
    {
        return false;
    }
    return true;
}

struct ColorCorrectionNumericField
{
    std::string_view develop_name;
    double ColorCorrectionParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorCorrectionNumericField, 5> &
color_correction_numeric_fields() noexcept
{
    static const std::array<ColorCorrectionNumericField, 5> fields{{
        {"colorCorrectionHighlightA", &ColorCorrectionParams::highlight_a,
         kColorCorrectionEndpointMin, kColorCorrectionEndpointMax},
        {"colorCorrectionHighlightB", &ColorCorrectionParams::highlight_b,
         kColorCorrectionEndpointMin, kColorCorrectionEndpointMax},
        {"colorCorrectionShadowA", &ColorCorrectionParams::shadow_a, kColorCorrectionEndpointMin,
         kColorCorrectionEndpointMax},
        {"colorCorrectionShadowB", &ColorCorrectionParams::shadow_b, kColorCorrectionEndpointMin,
         kColorCorrectionEndpointMax},
        {"colorCorrectionSaturation", &ColorCorrectionParams::saturation,
         kColorCorrectionSaturationMin, kColorCorrectionSaturationMax},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_correction_field(DevelopParams &params, const std::string_view name,
                                                const double value) noexcept
{
    if (name == "colorCorrectionEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_correction_enabled = value == 1.0;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : color_correction_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_correction.*(field.member) = value;
            params.color_correction_enabled = true;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset_color_correction_field(DevelopParams &params,
                                                const std::string_view name) noexcept
{
    const ColorCorrectionParams defaults;
    if (name == "colorCorrection")
    {
        params.color_correction_enabled = false;
        params.color_correction = defaults;
        return true;
    }
    if (name == "colorCorrectionEnabled")
    {
        params.color_correction_enabled = false;
        return true;
    }
    for (const auto &field : color_correction_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_correction.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_correction(ColorCorrectionParams &params) noexcept
{
    const ColorCorrectionParams defaults;
    for (const auto &field : color_correction_numeric_fields())
    {
        double &value = params.*(field.member);
        value = std::isfinite(value) ? clamp_value(value, field.minimum, field.maximum) :
                                       defaults.*(field.member);
    }
}

struct ColorContrastNumericField
{
    std::string_view develop_name;
    double ColorContrastParams::*member;
    bool is_steepness;
};

[[nodiscard]] const std::array<ColorContrastNumericField, 4> &
color_contrast_numeric_fields() noexcept
{
    static const std::array<ColorContrastNumericField, 4> fields{{
        {"colorContrastASteepness", &ColorContrastParams::a_steepness, true},
        {"colorContrastAOffset", &ColorContrastParams::a_offset, false},
        {"colorContrastBSteepness", &ColorContrastParams::b_steepness, true},
        {"colorContrastBOffset", &ColorContrastParams::b_offset, false},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_contrast_field(DevelopParams &params, const std::string_view name,
                                              const double value) noexcept
{
    if (name == "colorContrastEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_contrast_enabled = value == 1.0;
        return true;
    }
    if (name == "colorContrastUnbound")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_contrast.unbound = value == 1.0;
        params.color_contrast_enabled = true;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : color_contrast_numeric_fields())
    {
        if (name != field.develop_name)
        {
            continue;
        }
        if (!field.is_steepness && !std::isfinite(static_cast<float>(value)))
        {
            return false;
        }
        params.color_contrast.*(field.member) = value;
        params.color_contrast_enabled = true;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_color_contrast_field(DevelopParams &params,
                                              const std::string_view name) noexcept
{
    const ColorContrastParams defaults;
    if (name == "colorContrast")
    {
        params.color_contrast_enabled = false;
        params.color_contrast = defaults;
        return true;
    }
    if (name == "colorContrastEnabled")
    {
        params.color_contrast_enabled = false;
        return true;
    }
    if (name == "colorContrastUnbound")
    {
        params.color_contrast.unbound = defaults.unbound;
        return true;
    }
    for (const auto &field : color_contrast_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_contrast.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_contrast(ColorContrastParams &params) noexcept
{
    const ColorContrastParams defaults;
    for (const auto &field : color_contrast_numeric_fields())
    {
        double &value = params.*(field.member);
        if (!std::isfinite(value))
        {
            value = defaults.*(field.member);
        }
        else if (field.is_steepness)
        {
            value = clamp_value(value, kColorContrastSteepnessMin, kColorContrastSteepnessMax);
        }
        else if (!std::isfinite(static_cast<float>(value)))
        {
            value = defaults.*(field.member);
        }
    }
}

struct ColorReconstructionNumericField
{
    std::string_view develop_name;
    double ColorReconstructionParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorReconstructionNumericField, 3> &
color_reconstruction_numeric_fields() noexcept
{
    static const std::array<ColorReconstructionNumericField, 3> fields{{
        {"colorReconstructionThreshold", &ColorReconstructionParams::threshold,
         kColorReconstructionThresholdMin, kColorReconstructionThresholdMax},
        {"colorReconstructionSpatial", &ColorReconstructionParams::spatial,
         kColorReconstructionSpatialMin, kColorReconstructionSpatialMax},
        {"colorReconstructionRange", &ColorReconstructionParams::range,
         kColorReconstructionRangeMin, kColorReconstructionRangeMax},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_reconstruction_field(DevelopParams &params,
                                                    const std::string_view name,
                                                    const double value) noexcept
{
    if (name == "colorReconstructionEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_reconstruction_enabled = value == 1.0;
        return true;
    }
    if (name == "colorReconstructionPrecedenceIndex")
    {
        if (!std::isfinite(value) || std::trunc(value) != value || value < 0.0 || value > 2.0)
        {
            return false;
        }
        params.color_reconstruction.precedence =
            static_cast<ColorReconstructionPrecedence>(static_cast<std::uint8_t>(value));
        params.color_reconstruction_enabled = true;
        return true;
    }
    if (name == "colorReconstructionHueDegrees")
    {
        if (!std::isfinite(value) || value < 0.0 || value > 360.0)
        {
            return false;
        }
        params.color_reconstruction.hue = value / 360.0;
        params.color_reconstruction_enabled = true;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        if (name != field.develop_name)
        {
            continue;
        }
        params.color_reconstruction.*(field.member) = value;
        params.color_reconstruction_enabled = true;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_color_reconstruction_field(DevelopParams &params,
                                                    const std::string_view name) noexcept
{
    const ColorReconstructionParams defaults;
    if (name == "colorReconstruction")
    {
        params.color_reconstruction_enabled = false;
        params.color_reconstruction = defaults;
        return true;
    }
    if (name == "colorReconstructionEnabled")
    {
        params.color_reconstruction_enabled = false;
        return true;
    }
    if (name == "colorReconstructionPrecedenceIndex")
    {
        params.color_reconstruction.precedence = defaults.precedence;
        return true;
    }
    if (name == "colorReconstructionHueDegrees")
    {
        params.color_reconstruction.hue = defaults.hue;
        return true;
    }
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_reconstruction.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_reconstruction(ColorReconstructionParams &params) noexcept
{
    const ColorReconstructionParams defaults;
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        double &value = params.*(field.member);
        value = std::isfinite(value) ? clamp_value(value, field.minimum, field.maximum) :
                                       defaults.*(field.member);
    }
    params.hue = std::isfinite(params.hue) ? clamp_value(params.hue, kColorReconstructionHueMin,
                                                         kColorReconstructionHueMax) :
                                             defaults.hue;
    switch (params.precedence)
    {
    case ColorReconstructionPrecedence::kNone:
    case ColorReconstructionPrecedence::kChroma:
    case ColorReconstructionPrecedence::kHue:
        break;
    default:
        params.precedence = defaults.precedence;
        break;
    }
}

[[nodiscard]] constexpr std::array<std::string_view, 7> rgb_levels_preserve_names() noexcept
{
    return {kToneCurvePreserveColorsNone, kToneCurvePreserveColorsLuminance,
            kToneCurvePreserveColorsMax,  kToneCurvePreserveColorsAverage,
            kToneCurvePreserveColorsSum,  kToneCurvePreserveColorsNorm,
            kToneCurvePreserveColorsPower};
}

struct RgbLevelsStopField
{
    std::string_view develop_name;
    std::size_t channel;
    std::size_t stop;
};

[[nodiscard]] const std::array<RgbLevelsStopField, 9> &rgb_levels_stop_fields() noexcept
{
    static const std::array<RgbLevelsStopField, 9> fields{{
        {"rgbLevelsBlack", 0, 0},
        {"rgbLevelsGrey", 0, 1},
        {"rgbLevelsWhite", 0, 2},
        {"rgbLevelsBlackG", 1, 0},
        {"rgbLevelsGreyG", 1, 1},
        {"rgbLevelsWhiteG", 1, 2},
        {"rgbLevelsBlackB", 2, 0},
        {"rgbLevelsGreyB", 2, 1},
        {"rgbLevelsWhiteB", 2, 2},
    }};
    return fields;
}

[[nodiscard]] bool exact_develop_integer(const double value, const std::int64_t minimum,
                                         const std::int64_t maximum, std::int64_t &out) noexcept;

void clamp_rgb_levels(RgbLevelsParams &params) noexcept
{
    if (params.mode != kRgbLevelsModeIndependent)
    {
        params.mode = std::string(kRgbLevelsModeLinked);
    }
    bool preserve_known = false;
    for (const auto name : rgb_levels_preserve_names())
    {
        if (params.preserve_colors == name)
        {
            preserve_known = true;
            break;
        }
    }
    if (!preserve_known)
    {
        params.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
    }
    for (auto &channel : params.levels)
    {
        for (auto &stop : channel)
        {
            if (!std::isfinite(stop))
            {
                stop = 0.0;
            }
            stop = clamp_value(stop, 0.0, 1.0);
        }
        if (!(channel[2] > channel[0]))
        {
            channel[2] = std::min(1.0, channel[0] + 1.0e-3);
        }
    }
}

[[nodiscard]] bool apply_rgb_levels_field(DevelopParams &params, const std::string_view name,
                                          const double value) noexcept
{
    if (name == "rgbLevelsMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.rgb_levels.mode = value == 1.0 ? std::string(kRgbLevelsModeIndependent) :
                                                std::string(kRgbLevelsModeLinked);
        return true;
    }
    if (name == "rgbLevelsPreserve")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 6, index))
        {
            return false;
        }
        params.rgb_levels.preserve_colors =
            std::string(rgb_levels_preserve_names()[static_cast<std::size_t>(index)]);
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : rgb_levels_stop_fields())
    {
        if (name != field.develop_name)
        {
            continue;
        }
        params.rgb_levels.levels[field.channel][field.stop] = value;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_rgb_levels_field(DevelopParams &params,
                                          const std::string_view name) noexcept
{
    const RgbLevelsParams defaults;
    if (name == "rgbLevels")
    {
        params.rgb_levels = defaults;
        return true;
    }
    if (name == "rgbLevelsMode")
    {
        params.rgb_levels.mode = defaults.mode;
        return true;
    }
    if (name == "rgbLevelsPreserve")
    {
        params.rgb_levels.preserve_colors = defaults.preserve_colors;
        return true;
    }
    for (const auto &field : rgb_levels_stop_fields())
    {
        if (name == field.develop_name)
        {
            params.rgb_levels.levels[field.channel][field.stop] =
                defaults.levels[field.channel][field.stop];
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool exact_develop_integer(const double value, const std::int64_t minimum,
                                         const std::int64_t maximum, std::int64_t &out) noexcept
{
    if (!std::isfinite(value) || value < static_cast<double>(minimum) ||
        value > static_cast<double>(maximum) || value != std::trunc(value))
    {
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool assign_color_harmonizer_hue_turns(double &target, const double degrees) noexcept
{
    const auto turns = color_harmonizer_hue_degrees_to_turns(degrees);
    if (!turns)
    {
        return false;
    }
    target = turns.value();
    return true;
}

struct ColorHarmonizerNumericField
{
    std::string_view develop_name;
    double ColorHarmonizerParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorHarmonizerNumericField, 4> &
color_harmonizer_linear_fields() noexcept
{
    static const std::array<ColorHarmonizerNumericField, 4> fields{{
        {"colorHarmonizerPullStrength", &ColorHarmonizerParams::pull_strength,
         kColorHarmonizerPullStrengthMin, kColorHarmonizerPullStrengthMax},
        {"colorHarmonizerNeutralProtection", &ColorHarmonizerParams::neutral_protection,
         kColorHarmonizerNeutralProtectionMin, kColorHarmonizerNeutralProtectionMax},
        {"colorHarmonizerPullWidth", &ColorHarmonizerParams::pull_width,
         kColorHarmonizerPullWidthMin, kColorHarmonizerPullWidthMax},
        {"colorHarmonizerSmoothing", &ColorHarmonizerParams::smoothing,
         kColorHarmonizerSmoothingMin, kColorHarmonizerSmoothingMax},
    }};
    return fields;
}

[[nodiscard]] const std::array<std::pair<std::string_view, std::size_t>, 4> &
color_harmonizer_custom_hue_fields() noexcept
{
    static const std::array<std::pair<std::string_view, std::size_t>, 4> fields{{
        {"colorHarmonizerCustomHue0Degrees", 0U},
        {"colorHarmonizerCustomHue1Degrees", 1U},
        {"colorHarmonizerCustomHue2Degrees", 2U},
        {"colorHarmonizerCustomHue3Degrees", 3U},
    }};
    return fields;
}

[[nodiscard]] const std::array<std::pair<std::string_view, std::size_t>, 4> &
color_harmonizer_node_saturation_fields() noexcept
{
    static const std::array<std::pair<std::string_view, std::size_t>, 4> fields{{
        {"colorHarmonizerNodeSaturation0", 0U},
        {"colorHarmonizerNodeSaturation1", 1U},
        {"colorHarmonizerNodeSaturation2", 2U},
        {"colorHarmonizerNodeSaturation3", 3U},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_harmonizer_field(DevelopParams &params, const std::string_view name,
                                                const double value) noexcept
{
    if (name == "colorHarmonizerEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = value == 1.0;
        return true;
    }
    if (name == "colorHarmonizerRuleIndex")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(
                value, 0, static_cast<std::int64_t>(kColorHarmonizerRuleCount - 1U), index))
        {
            return false;
        }
        auto rule = color_harmonizer_rule_from_index(index);
        if (!rule)
        {
            return false;
        }
        params.color_harmonizer.rule = rule.value();
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return true;
    }
    if (name == "colorHarmonizerCustomNodeCount")
    {
        std::int64_t count = 0;
        if (!exact_develop_integer(value, kColorHarmonizerCustomNodesMin,
                                   kColorHarmonizerCustomNodesMax, count))
        {
            return false;
        }
        params.color_harmonizer.num_custom_nodes = count;
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return true;
    }
    if (name == "colorHarmonizerAnchorHueDegrees")
    {
        if (!assign_color_harmonizer_hue_turns(params.color_harmonizer.anchor_hue, value))
        {
            return false;
        }
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return true;
    }
    for (const auto &[field, index] : color_harmonizer_custom_hue_fields())
    {
        if (name == field)
        {
            if (!assign_color_harmonizer_hue_turns(params.color_harmonizer.custom_hue[index],
                                                   value))
            {
                return false;
            }
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = true;
            return true;
        }
    }
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
    {
        return false;
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        if (name == field.develop_name)
        {
            params.color_harmonizer.*(field.member) = value;
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = true;
            return true;
        }
    }
    for (const auto &[field, index] : color_harmonizer_node_saturation_fields())
    {
        if (name == field)
        {
            params.color_harmonizer.node_saturation[index] = value;
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = true;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset_color_harmonizer_field(DevelopParams &params,
                                                const std::string_view name) noexcept
{
    const ColorHarmonizerParams defaults;
    if (name == "colorHarmonizer")
    {
        params.color_harmonizer_enabled = false;
        params.color_harmonizer = defaults;
        return true;
    }
    if (name == "colorHarmonizerEnabled")
    {
        params.color_harmonizer_enabled = false;
        return true;
    }
    if (name == "colorHarmonizerRuleIndex")
    {
        params.color_harmonizer.rule = defaults.rule;
        return true;
    }
    if (name == "colorHarmonizerCustomNodeCount")
    {
        params.color_harmonizer.num_custom_nodes = defaults.num_custom_nodes;
        return true;
    }
    if (name == "colorHarmonizerAnchorHueDegrees")
    {
        params.color_harmonizer.anchor_hue = defaults.anchor_hue;
        return true;
    }
    for (const auto &[field, index] : color_harmonizer_custom_hue_fields())
    {
        if (name == field)
        {
            params.color_harmonizer.custom_hue[index] = defaults.custom_hue[index];
            return true;
        }
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        if (name == field.develop_name)
        {
            params.color_harmonizer.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    for (const auto &[field, index] : color_harmonizer_node_saturation_fields())
    {
        if (name == field)
        {
            params.color_harmonizer.node_saturation[index] = defaults.node_saturation[index];
            return true;
        }
    }
    return false;
}

void clamp_color_harmonizer(ColorHarmonizerParams &params) noexcept
{
    const ColorHarmonizerParams defaults;
    auto rule = color_harmonizer_rule_from_index(color_harmonizer_rule_index(params.rule));
    if (!rule)
    {
        params.rule = defaults.rule;
    }
    const auto clamp_hue = [&](double &value, const double fallback)
    {
        if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
        {
            value = fallback;
            return;
        }
        value = clamp_value(value, kColorHarmonizerHueMin, kColorHarmonizerHueMax);
    };
    clamp_hue(params.anchor_hue, defaults.anchor_hue);
    for (std::size_t index = 0U; index < params.custom_hue.size(); ++index)
    {
        clamp_hue(params.custom_hue[index], defaults.custom_hue[index]);
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        double &value = params.*(field.member);
        value = std::isfinite(value) && std::isfinite(static_cast<float>(value)) ?
                    clamp_value(value, field.minimum, field.maximum) :
                    defaults.*(field.member);
    }
    if (params.num_custom_nodes < kColorHarmonizerCustomNodesMin ||
        params.num_custom_nodes > kColorHarmonizerCustomNodesMax)
    {
        params.num_custom_nodes = defaults.num_custom_nodes;
    }
    for (std::size_t index = 0U; index < params.node_saturation.size(); ++index)
    {
        double &value = params.node_saturation[index];
        value = std::isfinite(value) && std::isfinite(static_cast<float>(value)) ?
                    clamp_value(value, kColorHarmonizerNodeSaturationMin,
                                kColorHarmonizerNodeSaturationMax) :
                    defaults.node_saturation[index];
    }
}

struct ColorBalanceNumericField
{
    std::string_view parameter_name;
    std::string_view develop_name;
    double ColorBalanceRgbParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorBalanceNumericField, 32> &
color_balance_numeric_fields() noexcept
{
    static const std::array<ColorBalanceNumericField, 32> fields{{
        {"shadows_y", "colorBalanceShadowsY", &ColorBalanceRgbParams::shadows_y, -1.0, 1.0},
        {"shadows_chroma", "colorBalanceShadowsChroma", &ColorBalanceRgbParams::shadows_chroma, 0.0,
         1.0},
        {"shadows_hue", "colorBalanceShadowsHue", &ColorBalanceRgbParams::shadows_hue, 0.0, 360.0},
        {"midtones_y", "colorBalanceMidtonesY", &ColorBalanceRgbParams::midtones_y, -1.0, 1.0},
        {"midtones_chroma", "colorBalanceMidtonesChroma", &ColorBalanceRgbParams::midtones_chroma,
         0.0, 1.0},
        {"midtones_hue", "colorBalanceMidtonesHue", &ColorBalanceRgbParams::midtones_hue, 0.0,
         360.0},
        {"highlights_y", "colorBalanceHighlightsY", &ColorBalanceRgbParams::highlights_y, -1.0,
         1.0},
        {"highlights_chroma", "colorBalanceHighlightsChroma",
         &ColorBalanceRgbParams::highlights_chroma, 0.0, 1.0},
        {"highlights_hue", "colorBalanceHighlightsHue", &ColorBalanceRgbParams::highlights_hue, 0.0,
         360.0},
        {"global_y", "colorBalanceGlobalY", &ColorBalanceRgbParams::global_y, -1.0, 1.0},
        {"global_chroma", "colorBalanceGlobalChroma", &ColorBalanceRgbParams::global_chroma, 0.0,
         1.0},
        {"global_hue", "colorBalanceGlobalHue", &ColorBalanceRgbParams::global_hue, 0.0, 360.0},
        {"shadows_falloff", "colorBalanceShadowsFalloff", &ColorBalanceRgbParams::shadows_falloff,
         0.0, 3.0},
        {"white_fulcrum_ev", "colorBalanceWhiteFulcrumEv", &ColorBalanceRgbParams::white_fulcrum_ev,
         -16.0, 16.0},
        {"highlights_falloff", "colorBalanceHighlightsFalloff",
         &ColorBalanceRgbParams::highlights_falloff, 0.0, 3.0},
        {"chroma_shadows", "colorBalanceChromaShadows", &ColorBalanceRgbParams::chroma_shadows,
         -1.0, 1.0},
        {"chroma_highlights", "colorBalanceChromaHighlights",
         &ColorBalanceRgbParams::chroma_highlights, -1.0, 1.0},
        {"chroma_global", "colorBalanceChromaGlobal", &ColorBalanceRgbParams::chroma_global, -1.0,
         1.0},
        {"chroma_midtones", "colorBalanceChromaMidtones", &ColorBalanceRgbParams::chroma_midtones,
         -1.0, 1.0},
        {"saturation_global", "colorBalanceSaturationGlobal",
         &ColorBalanceRgbParams::saturation_global, -1.0, 1.0},
        {"saturation_highlights", "colorBalanceSaturationHighlights",
         &ColorBalanceRgbParams::saturation_highlights, -1.0, 1.0},
        {"saturation_midtones", "colorBalanceSaturationMidtones",
         &ColorBalanceRgbParams::saturation_midtones, -1.0, 1.0},
        {"saturation_shadows", "colorBalanceSaturationShadows",
         &ColorBalanceRgbParams::saturation_shadows, -1.0, 1.0},
        {"hue_rotation", "colorBalanceHueRotation", &ColorBalanceRgbParams::hue_rotation, -180.0,
         180.0},
        {"brilliance_global", "colorBalanceBrillianceGlobal",
         &ColorBalanceRgbParams::brilliance_global, -1.0, 1.0},
        {"brilliance_highlights", "colorBalanceBrillianceHighlights",
         &ColorBalanceRgbParams::brilliance_highlights, -1.0, 1.0},
        {"brilliance_midtones", "colorBalanceBrillianceMidtones",
         &ColorBalanceRgbParams::brilliance_midtones, -1.0, 1.0},
        {"brilliance_shadows", "colorBalanceBrillianceShadows",
         &ColorBalanceRgbParams::brilliance_shadows, -1.0, 1.0},
        {"mask_grey_fulcrum", "colorBalanceMaskGreyFulcrum",
         &ColorBalanceRgbParams::mask_grey_fulcrum, 0.0, 1.0},
        {"vibrance", "colorBalanceVibrance", &ColorBalanceRgbParams::vibrance, -1.0, 1.0},
        {"grey_fulcrum", "colorBalanceGreyFulcrum", &ColorBalanceRgbParams::grey_fulcrum, 0.0, 1.0},
        {"contrast", "colorBalanceContrast", &ColorBalanceRgbParams::contrast, -1.0, 1.0},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_balance_field(ColorBalanceRgbParams &params,
                                             const std::string_view name,
                                             const double value) noexcept
{
    if (name == "colorBalanceFormula")
    {
        params.saturation_formula = value >= 0.5 ? std::string(kColorBalanceRgbFormulaJzAzBz2021) :
                                                   std::string(kColorBalanceRgbFormulaDtUcs2022);
        return true;
    }
    for (const auto &field : color_balance_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.*(field.member) = value;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset_color_balance_field(ColorBalanceRgbParams &params,
                                             const std::string_view name) noexcept
{
    const ColorBalanceRgbParams defaults;
    if (name == "colorBalance" || name == "colorBalanceFormula")
    {
        if (name == "colorBalance")
        {
            params = defaults;
        }
        else
        {
            params.saturation_formula = defaults.saturation_formula;
        }
        return true;
    }
    for (const auto &field : color_balance_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_balance(ColorBalanceRgbParams &params) noexcept
{
    for (const auto &field : color_balance_numeric_fields())
    {
        params.*(field.member) = clamp_value(params.*(field.member), field.minimum, field.maximum);
    }
    params.midtones_y = std::max(params.midtones_y, -0.999999);
    params.mask_grey_fulcrum = std::max(params.mask_grey_fulcrum, 0.000001);
    params.grey_fulcrum = std::max(params.grey_fulcrum, 0.000001);
    if (params.saturation_formula != kColorBalanceRgbFormulaDtUcs2022 &&
        params.saturation_formula != kColorBalanceRgbFormulaJzAzBz2021)
    {
        params.saturation_formula = std::string(kColorBalanceRgbFormulaDtUcs2022);
    }
}

[[nodiscard]] Result<std::array<double, kChannelMixerChannelCount>>
parse_channel_triplet(const ParameterValue &value, const std::string_view name)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color calibration parameter must be an array",
                          {{"parameter", std::string(name)}});
    }
    if (array->size() != kChannelMixerChannelCount)
    {
        return make_error(
            ErrorCode::kValidation, "Color calibration array must have exactly 3 values",
            {{"parameter", std::string(name)}, {"count", std::to_string(array->size())}});
    }
    std::array<double, kChannelMixerChannelCount> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index)
    {
        const double sample = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(sample) || sample < -2.0 || sample > 2.0)
        {
            return make_error(ErrorCode::kValidation,
                              "Color calibration array value must be finite and within [-2, 2]",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        parsed[index] = sample;
    }
    return parsed;
}

[[nodiscard]] Result<std::array<double, kColorEqualizerBandCount>>
parse_band_array(const ParameterValue &value, const std::string_view name)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color equalizer parameter must be an array",
                          {{"parameter", std::string(name)}});
    }
    if (array->size() != kColorEqualizerBandCount)
    {
        return make_error(
            ErrorCode::kValidation, "Color equalizer array must have 8 values",
            {{"parameter", std::string(name)}, {"count", std::to_string(array->size())}});
    }
    std::array<double, kColorEqualizerBandCount> parsed{};
    for (std::size_t index = 0; index < parsed.size(); ++index)
    {
        const double sample = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(sample))
        {
            return make_error(ErrorCode::kValidation, "Color equalizer band must be finite",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        parsed[index] = sample;
    }
    return parsed;
}

[[nodiscard]] bool parse_band_field(const std::string_view name, const std::string_view prefix,
                                    std::size_t &index) noexcept
{
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 1U)
    {
        return false;
    }
    const char digit = name[prefix.size()];
    if (digit < '0' || digit > '7')
    {
        return false;
    }
    index = static_cast<std::size_t>(digit - '0');
    return true;
}

} // namespace

bool ChannelMixerParams::is_identity() const noexcept
{
    const ChannelMixerParams identity;
    return channel_triplet_near(red, identity.red) && channel_triplet_near(green, identity.green) &&
           channel_triplet_near(blue, identity.blue) &&
           channel_triplet_near(saturation, identity.saturation) &&
           channel_triplet_near(lightness, identity.lightness) &&
           channel_triplet_near(grey, identity.grey) && normalize_red == identity.normalize_red &&
           normalize_green == identity.normalize_green &&
           normalize_blue == identity.normalize_blue &&
           normalize_saturation == identity.normalize_saturation &&
           normalize_lightness == identity.normalize_lightness &&
           normalize_grey == identity.normalize_grey && adaptation == identity.adaptation &&
           near(illuminant_x, identity.illuminant_x) && near(illuminant_y, identity.illuminant_y) &&
           near(gamut, identity.gamut) && clip == identity.clip;
}

Result<ChannelMixerParams>
channel_mixer_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto required = [&](const std::string_view name) -> Result<const ParameterValue *>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return make_error(ErrorCode::kValidation, "Color calibration parameter is required",
                              {{"parameter", std::string(name)}});
        }
        return &found->second;
    };
    const auto text = [&](const std::string_view name) -> Result<std::string>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const auto *parsed = std::get_if<std::string>(&value.value()->value);
        if (parsed == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "Color calibration parameter must be a string",
                              {{"parameter", std::string(name)}});
        }
        return *parsed;
    };
    const auto boolean = [&](const std::string_view name) -> Result<bool>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const auto *parsed = std::get_if<bool>(&value.value()->value);
        if (parsed == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Color calibration parameter must be boolean",
                              {{"parameter", std::string(name)}});
        }
        return *parsed;
    };
    const auto number = [&](const std::string_view name) -> Result<double>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const double parsed = as_number(*value.value(), std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(parsed))
        {
            return make_error(ErrorCode::kValidation, "Color calibration parameter must be finite",
                              {{"parameter", std::string(name)}});
        }
        return parsed;
    };
    const auto triplet =
        [&](const std::string_view name) -> Result<std::array<double, kChannelMixerChannelCount>>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        return parse_channel_triplet(*value.value(), name);
    };

    auto working_space = text("working_space");
    auto algorithm = text("algorithm");
    auto adaptation = text("adaptation");
    if (!working_space || !algorithm || !adaptation)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
                                adaptation.error();
    }
    if (working_space.value() != kChannelMixerWorkingSpaceLinearSrgbD50)
    {
        return make_error(ErrorCode::kValidation, "Color calibration working space is unsupported",
                          {{"working_space", working_space.value()}});
    }
    if (algorithm.value() != kChannelMixerAlgorithmV3)
    {
        return make_error(ErrorCode::kValidation, "Color calibration algorithm is unsupported",
                          {{"algorithm", algorithm.value()}});
    }
    if (adaptation.value() != kChannelMixerAdaptationRgb &&
        adaptation.value() != kChannelMixerAdaptationCat16 &&
        adaptation.value() != kChannelMixerAdaptationLinearBradford &&
        adaptation.value() != kChannelMixerAdaptationFullBradford &&
        adaptation.value() != kChannelMixerAdaptationXyz)
    {
        return make_error(ErrorCode::kValidation, "Color calibration adaptation is unsupported",
                          {{"adaptation", adaptation.value()}});
    }

    auto red = triplet("red");
    auto green = triplet("green");
    auto blue = triplet("blue");
    auto saturation = triplet("saturation");
    auto lightness = triplet("lightness");
    auto grey = triplet("grey");
    auto normalize_red = boolean("normalize_red");
    auto normalize_green = boolean("normalize_green");
    auto normalize_blue = boolean("normalize_blue");
    auto normalize_saturation = boolean("normalize_saturation");
    auto normalize_lightness = boolean("normalize_lightness");
    auto normalize_grey = boolean("normalize_grey");
    auto illuminant_x = number("illuminant_x");
    auto illuminant_y = number("illuminant_y");
    auto gamut = number("gamut");
    auto clip = boolean("clip");
    if (!red || !green || !blue || !saturation || !lightness || !grey || !normalize_red ||
        !normalize_green || !normalize_blue || !normalize_saturation || !normalize_lightness ||
        !normalize_grey || !illuminant_x || !illuminant_y || !gamut || !clip)
    {
        return !red                  ? red.error() :
               !green                ? green.error() :
               !blue                 ? blue.error() :
               !saturation           ? saturation.error() :
               !lightness            ? lightness.error() :
               !grey                 ? grey.error() :
               !normalize_red        ? normalize_red.error() :
               !normalize_green      ? normalize_green.error() :
               !normalize_blue       ? normalize_blue.error() :
               !normalize_saturation ? normalize_saturation.error() :
               !normalize_lightness  ? normalize_lightness.error() :
               !normalize_grey       ? normalize_grey.error() :
               !illuminant_x         ? illuminant_x.error() :
               !illuminant_y         ? illuminant_y.error() :
               !gamut                ? gamut.error() :
                                       clip.error();
    }
    if (illuminant_x.value() <= 0.0 || illuminant_y.value() <= 0.0 ||
        illuminant_x.value() + illuminant_y.value() >= 1.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Color calibration illuminant xy is outside the CIE chromaticity domain");
    }
    if (gamut.value() < 0.0 || gamut.value() > 12.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Color calibration gamut compression is outside [0, 12]");
    }
    const auto reject_zero_normalized_row = [](const auto &row, const bool normalize,
                                               const std::string_view name) -> Result<void>
    {
        if (normalize && std::abs(row[0] + row[1] + row[2]) <= kEpsilon)
        {
            return make_error(ErrorCode::kValidation,
                              "Color calibration normalized row must have a non-zero sum",
                              {{"parameter", std::string(name)}});
        }
        return {};
    };
    auto valid_red = reject_zero_normalized_row(red.value(), normalize_red.value(), "red");
    auto valid_green = reject_zero_normalized_row(green.value(), normalize_green.value(), "green");
    auto valid_blue = reject_zero_normalized_row(blue.value(), normalize_blue.value(), "blue");
    if (!valid_red || !valid_green || !valid_blue)
    {
        return !valid_red   ? valid_red.error() :
               !valid_green ? valid_green.error() :
                              valid_blue.error();
    }

    ChannelMixerParams result;
    result.red = red.value();
    result.green = green.value();
    result.blue = blue.value();
    result.saturation = saturation.value();
    result.lightness = lightness.value();
    result.grey = grey.value();
    result.normalize_red = normalize_red.value();
    result.normalize_green = normalize_green.value();
    result.normalize_blue = normalize_blue.value();
    result.normalize_saturation = normalize_saturation.value();
    result.normalize_lightness = normalize_lightness.value();
    result.normalize_grey = normalize_grey.value();
    result.adaptation = adaptation.value();
    result.illuminant_x = illuminant_x.value();
    result.illuminant_y = illuminant_y.value();
    result.gamut = gamut.value();
    result.clip = clip.value();
    return result;
}

Result<void> validate_channel_mixer_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = channel_mixer_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
channel_mixer_to_parameters(const ChannelMixerParams &params)
{
    return {{"working_space", ParameterValue{std::string(kChannelMixerWorkingSpaceLinearSrgbD50)}},
            {"algorithm", ParameterValue{std::string(kChannelMixerAlgorithmV3)}},
            {"adaptation", ParameterValue{params.adaptation}},
            {"red", channel_triplet_parameter(params.red)},
            {"green", channel_triplet_parameter(params.green)},
            {"blue", channel_triplet_parameter(params.blue)},
            {"saturation", channel_triplet_parameter(params.saturation)},
            {"lightness", channel_triplet_parameter(params.lightness)},
            {"grey", channel_triplet_parameter(params.grey)},
            {"normalize_red", ParameterValue{params.normalize_red}},
            {"normalize_green", ParameterValue{params.normalize_green}},
            {"normalize_blue", ParameterValue{params.normalize_blue}},
            {"normalize_saturation", ParameterValue{params.normalize_saturation}},
            {"normalize_lightness", ParameterValue{params.normalize_lightness}},
            {"normalize_grey", ParameterValue{params.normalize_grey}},
            {"illuminant_x", ParameterValue{params.illuminant_x}},
            {"illuminant_y", ParameterValue{params.illuminant_y}},
            {"gamut", ParameterValue{params.gamut}},
            {"clip", ParameterValue{params.clip}}};
}

bool TemperatureParams::is_identity() const noexcept
{
    return mode == kTemperatureModeAsShot && !coefficients.has_value();
}

Result<TemperatureParams>
temperature_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    static const std::set<std::string, std::less<>> allowed{"working_space", "algorithm", "mode",
                                                            "coefficients"};
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (!allowed.contains(name))
        {
            return make_error(ErrorCode::kValidation, "Temperature parameter is unknown",
                              {{"parameter", name}});
        }
    }
    const auto required_text = [&](const std::string_view name) -> Result<std::string>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return make_error(ErrorCode::kValidation, "Temperature parameter is required",
                              {{"parameter", std::string(name)}});
        }
        const auto *text = std::get_if<std::string>(&found->second.value);
        if (text == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Temperature parameter must be a string",
                              {{"parameter", std::string(name)}});
        }
        return *text;
    };
    auto working_space = required_text("working_space");
    auto algorithm = required_text("algorithm");
    auto mode = required_text("mode");
    if (!working_space || !algorithm || !mode)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
                                mode.error();
    }
    if (working_space.value() != kTemperatureWorkingSpaceCameraCfaOrLinearRgb)
    {
        return make_error(ErrorCode::kValidation, "Temperature working space is unsupported",
                          {{"working_space", working_space.value()}});
    }
    if (algorithm.value() != kTemperatureAlgorithmChannelScaleV4)
    {
        return make_error(ErrorCode::kValidation, "Temperature algorithm is unsupported",
                          {{"algorithm", algorithm.value()}});
    }
    if (!temperature_mode_supported(mode.value()))
    {
        return make_error(ErrorCode::kValidation, "Temperature mode is unsupported",
                          {{"mode", mode.value()}});
    }

    TemperatureParams result;
    result.mode = mode.value();
    if (const auto found = parameters.find("coefficients"); found != parameters.end())
    {
        auto coefficients = parse_temperature_coefficients(found->second);
        if (!coefficients)
        {
            auto error = coefficients.error();
            error.context.emplace("parameter", "coefficients");
            return error;
        }
        result.coefficients = coefficients.value();
    }
    if (result.mode == kTemperatureModeManual && !result.coefficients)
    {
        return make_error(ErrorCode::kValidation,
                          "Manual temperature mode requires explicit coefficients",
                          {{"parameter", "coefficients"}});
    }
    return result;
}

Result<void> validate_temperature_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = temperature_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
temperature_to_parameters(const TemperatureParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space",
         ParameterValue{std::string(kTemperatureWorkingSpaceCameraCfaOrLinearRgb)}},
        {"algorithm", ParameterValue{std::string(kTemperatureAlgorithmChannelScaleV4)}},
        {"mode", ParameterValue{params.mode}},
    };
    if (params.coefficients)
    {
        parameters.emplace("coefficients",
                           temperature_coefficients_parameter(*params.coefficients));
    }
    return parameters;
}

bool ColorBalanceParams::is_identity() const noexcept
{
    const ColorBalanceParams defaults;
    if (mode != defaults.mode)
    {
        return false;
    }
    return std::all_of(legacy_color_balance_numeric_fields().begin(),
                       legacy_color_balance_numeric_fields().end(),
                       [&](const LegacyColorBalanceNumericField &field)
                       {
                           return near(legacy_color_balance_value(*this, field),
                                       legacy_color_balance_value(defaults, field));
                       });
}

Result<ColorBalanceParams>
color_balance_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto known_name = [](const std::string_view name)
    {
        if (name == "working_space" || name == "algorithm" || name == "mode")
        {
            return true;
        }
        return std::any_of(legacy_color_balance_numeric_fields().begin(),
                           legacy_color_balance_numeric_fields().end(),
                           [name](const LegacyColorBalanceNumericField &field)
                           { return field.parameter_name == name; });
    };
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (!known_name(name))
        {
            return make_error(ErrorCode::kValidation, "Color Balance parameter is unknown",
                              {{"parameter", name}});
        }
    }

    const auto required = [&](const std::string_view name) -> Result<const ParameterValue *>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return make_error(ErrorCode::kValidation, "Color Balance parameter is required",
                              {{"parameter", std::string(name)}});
        }
        return &found->second;
    };
    const auto text = [&](const std::string_view name) -> Result<std::string>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const auto *parsed = std::get_if<std::string>(&value.value()->value);
        if (parsed == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Color Balance parameter must be a string",
                              {{"parameter", std::string(name)}});
        }
        return *parsed;
    };
    const auto number = [&](const LegacyColorBalanceNumericField &field) -> Result<double>
    {
        auto value = required(field.parameter_name);
        if (!value)
        {
            return value.error();
        }
        double parsed = std::numeric_limits<double>::quiet_NaN();
        if (const auto *floating = std::get_if<double>(&value.value()->value); floating != nullptr)
        {
            parsed = *floating;
        }
        else if (const auto *integer = std::get_if<std::int64_t>(&value.value()->value);
                 integer != nullptr)
        {
            parsed = static_cast<double>(*integer);
        }
        if (!std::isfinite(parsed))
        {
            return make_error(ErrorCode::kValidation, "Color Balance parameter must be finite",
                              {{"parameter", std::string(field.parameter_name)}});
        }
        if (parsed < field.minimum || parsed > field.maximum)
        {
            return make_error(ErrorCode::kValidation,
                              "Color Balance parameter is outside its supported range",
                              {{"parameter", std::string(field.parameter_name)}});
        }
        return parsed;
    };

    auto working_space = text("working_space");
    auto algorithm = text("algorithm");
    auto mode = text("mode");
    if (!working_space || !algorithm || !mode)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
                                mode.error();
    }
    if (working_space.value() != kColorBalanceWorkingSpaceLinearSrgbD50)
    {
        return make_error(ErrorCode::kValidation, "Color Balance working space is unsupported",
                          {{"working_space", working_space.value()}});
    }
    if (algorithm.value() != kColorBalanceAlgorithmLabD50ProPhotoV4)
    {
        return make_error(ErrorCode::kValidation, "Color Balance algorithm is unsupported",
                          {{"algorithm", algorithm.value()}});
    }
    if (mode.value() != kColorBalanceModeLiftGammaGain &&
        mode.value() != kColorBalanceModeSlopeOffsetPower)
    {
        return make_error(ErrorCode::kValidation, "Color Balance mode is unsupported",
                          {{"mode", mode.value()}});
    }

    ColorBalanceParams result;
    result.mode = mode.value();
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        auto parsed = number(field);
        if (!parsed)
        {
            return parsed.error();
        }
        legacy_color_balance_value(result, field) = parsed.value();
    }
    return result;
}

Result<void> validate_color_balance_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_balance_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
color_balance_to_parameters(const ColorBalanceParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorBalanceWorkingSpaceLinearSrgbD50)}},
        {"algorithm", ParameterValue{std::string(kColorBalanceAlgorithmLabD50ProPhotoV4)}},
        {"mode", ParameterValue{params.mode}},
    };
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        parameters.emplace(field.parameter_name,
                           ParameterValue{legacy_color_balance_value(params, field)});
    }
    return parameters;
}

bool ColorBalanceRgbParams::is_identity() const noexcept
{
    const ColorBalanceRgbParams defaults;
    for (const auto &field : color_balance_numeric_fields())
    {
        if (!near(this->*(field.member), defaults.*(field.member)))
        {
            return false;
        }
    }
    return saturation_formula == defaults.saturation_formula;
}

Result<ColorBalanceRgbParams> color_balance_rgb_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto known_name = [](const std::string_view name)
    {
        if (name == "working_space" || name == "algorithm" || name == "saturation_formula")
        {
            return true;
        }
        return std::any_of(
            color_balance_numeric_fields().begin(), color_balance_numeric_fields().end(),
            [name](const ColorBalanceNumericField &field) { return field.parameter_name == name; });
    };
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (!known_name(name))
        {
            return make_error(ErrorCode::kValidation, "Color Balance RGB parameter is unknown",
                              {{"parameter", name}});
        }
    }

    const auto required = [&](const std::string_view name) -> Result<const ParameterValue *>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return make_error(ErrorCode::kValidation, "Color Balance RGB parameter is required",
                              {{"parameter", std::string(name)}});
        }
        return &found->second;
    };
    const auto text = [&](const std::string_view name) -> Result<std::string>
    {
        auto value = required(name);
        if (!value)
        {
            return value.error();
        }
        const auto *parsed = std::get_if<std::string>(&value.value()->value);
        if (parsed == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "Color Balance RGB parameter must be a string",
                              {{"parameter", std::string(name)}});
        }
        return *parsed;
    };
    const auto number = [&](const ColorBalanceNumericField &field) -> Result<double>
    {
        auto value = required(field.parameter_name);
        if (!value)
        {
            return value.error();
        }
        double parsed = std::numeric_limits<double>::quiet_NaN();
        if (const auto *floating = std::get_if<double>(&value.value()->value); floating != nullptr)
        {
            parsed = *floating;
        }
        else if (const auto *integer = std::get_if<std::int64_t>(&value.value()->value);
                 integer != nullptr)
        {
            parsed = static_cast<double>(*integer);
        }
        if (!std::isfinite(parsed))
        {
            return make_error(ErrorCode::kValidation, "Color Balance RGB parameter must be finite",
                              {{"parameter", std::string(field.parameter_name)}});
        }
        if (parsed < field.minimum || parsed > field.maximum)
        {
            return make_error(ErrorCode::kValidation,
                              "Color Balance RGB parameter is outside its supported range",
                              {{"parameter", std::string(field.parameter_name)}});
        }
        return parsed;
    };

    auto working_space = text("working_space");
    auto algorithm = text("algorithm");
    auto formula = text("saturation_formula");
    if (!working_space || !algorithm || !formula)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
                                formula.error();
    }
    if (working_space.value() != kColorBalanceRgbWorkingSpaceLinearSrgbD50)
    {
        return make_error(ErrorCode::kValidation, "Color Balance RGB working space is unsupported",
                          {{"working_space", working_space.value()}});
    }
    if (algorithm.value() != kColorBalanceRgbAlgorithmFilmlightYchV5)
    {
        return make_error(ErrorCode::kValidation, "Color Balance RGB algorithm is unsupported",
                          {{"algorithm", algorithm.value()}});
    }
    if (formula.value() != kColorBalanceRgbFormulaDtUcs2022 &&
        formula.value() != kColorBalanceRgbFormulaJzAzBz2021)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Balance RGB saturation formula is unsupported",
                          {{"saturation_formula", formula.value()}});
    }

    ColorBalanceRgbParams result;
    result.saturation_formula = formula.value();
    for (const auto &field : color_balance_numeric_fields())
    {
        auto parsed = number(field);
        if (!parsed)
        {
            return parsed.error();
        }
        result.*(field.member) = parsed.value();
    }
    if (result.midtones_y <= -1.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Balance RGB midtones power would be singular",
                          {{"parameter", "midtones_y"}});
    }
    if (result.mask_grey_fulcrum <= 0.0 || result.grey_fulcrum <= 0.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Balance RGB fulcrums must be greater than zero",
                          {{"parameter", result.mask_grey_fulcrum <= 0.0 ? "mask_grey_fulcrum" :
                                                                           "grey_fulcrum"}});
    }
    return result;
}

Result<void> validate_color_balance_rgb_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_balance_rgb_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
color_balance_rgb_to_parameters(const ColorBalanceRgbParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorBalanceRgbWorkingSpaceLinearSrgbD50)}},
        {"algorithm", ParameterValue{std::string(kColorBalanceRgbAlgorithmFilmlightYchV5)}},
        {"saturation_formula", ParameterValue{params.saturation_formula}},
    };
    for (const auto &field : color_balance_numeric_fields())
    {
        parameters.emplace(field.parameter_name, ParameterValue{params.*(field.member)});
    }
    return parameters;
}

bool tone_curve_is_identity(const std::vector<ToneCurvePoint> &points) noexcept
{
    if (points.empty())
    {
        return true;
    }
    if (points.size() < kToneCurveMinPoints || !near(points.front().x, 0.0) ||
        !near(points.back().x, 1.0))
    {
        return false;
    }
    for (const auto &point : points)
    {
        if (!near(point.x, point.y))
        {
            return false;
        }
    }
    return true;
}

void clamp_tone_curve(std::vector<ToneCurvePoint> &points) noexcept
{
    if (points.empty())
    {
        return;
    }
    for (auto &point : points)
    {
        point.x = clamp_value(point.x, 0.0, 1.0);
        point.y = clamp_value(point.y, 0.0, 1.0);
    }
    std::sort(points.begin(), points.end(),
              [](const ToneCurvePoint &left, const ToneCurvePoint &right)
              { return left.x < right.x; });
    std::vector<ToneCurvePoint> merged;
    merged.reserve(points.size());
    for (const auto &point : points)
    {
        if (!merged.empty() && point.x - merged.back().x < 1e-4)
        {
            merged.back() = point;
        }
        else
        {
            merged.push_back(point);
        }
    }
    if (merged.size() < kToneCurveMinPoints)
    {
        points.clear();
        return;
    }
    merged.front().x = 0.0;
    merged.back().x = 1.0;
    if (merged.size() >= 3 && merged[1].x < 1e-4)
    {
        merged.erase(merged.begin() + 1);
    }
    if (merged.size() >= 3 && merged[merged.size() - 2U].x > 1.0 - 1e-4)
    {
        merged.erase(merged.end() - 2);
    }
    while (merged.size() > kToneCurveMaxPoints && merged.size() > kToneCurveMinPoints)
    {
        merged.erase(merged.begin() + static_cast<std::ptrdiff_t>(merged.size() / 2));
    }
    if (tone_curve_is_identity(merged))
    {
        points.clear();
        return;
    }
    points = std::move(merged);
}

double evaluate_tone_curve(const std::vector<ToneCurvePoint> &points, const double x) noexcept
{
    if (tone_curve_is_identity(points))
    {
        return std::clamp(x, 0.0, 1.0);
    }
    return evaluate_tone_curve_spline(make_tone_curve_spline(points), x);
}

Result<ToneCurveWorkingSpace> parse_tone_curve_working_space(const std::string_view text)
{
    if (text == kToneCurveWorkingSpaceSrgb)
    {
        return ToneCurveWorkingSpace::kSrgb;
    }
    if (text == kToneCurveWorkingSpaceLinearRgb)
    {
        return ToneCurveWorkingSpace::kLinearRgb;
    }
    if (text == kToneCurveWorkingSpaceRgb)
    {
        return ToneCurveWorkingSpace::kRgb;
    }
    if (text == kToneCurveWorkingSpaceLab)
    {
        return ToneCurveWorkingSpace::kLab;
    }
    if (text == kToneCurveWorkingSpaceXyz)
    {
        return ToneCurveWorkingSpace::kXyz;
    }
    if (text == kToneCurveWorkingSpaceLabIndependent)
    {
        return ToneCurveWorkingSpace::kLabIndependent;
    }
    return make_error(ErrorCode::kValidation, "Tone curve working space is unsupported",
                      {{"working_space", std::string(text)}});
}

std::string_view tone_curve_working_space_name(const ToneCurveWorkingSpace space) noexcept
{
    switch (space)
    {
    case ToneCurveWorkingSpace::kSrgb:
        return kToneCurveWorkingSpaceSrgb;
    case ToneCurveWorkingSpace::kLinearRgb:
        return kToneCurveWorkingSpaceLinearRgb;
    case ToneCurveWorkingSpace::kRgb:
        return kToneCurveWorkingSpaceRgb;
    case ToneCurveWorkingSpace::kLab:
        return kToneCurveWorkingSpaceLab;
    case ToneCurveWorkingSpace::kXyz:
        return kToneCurveWorkingSpaceXyz;
    case ToneCurveWorkingSpace::kLabIndependent:
        return kToneCurveWorkingSpaceLabIndependent;
    }
    return kToneCurveWorkingSpaceRgb;
}

Result<std::vector<ToneCurvePoint>> parse_tone_curve_points(const ParameterValue &value)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Tone curve points must be an array");
    }
    if (array->size() < kToneCurveMinPoints || array->size() > kToneCurveMaxPoints)
    {
        return make_error(ErrorCode::kValidation, "Tone curve must have between 2 and 16 points",
                          {{"point_count", std::to_string(array->size())}});
    }
    std::vector<ToneCurvePoint> points;
    points.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const auto *object = std::get_if<ParameterValue::Object>(&(*array)[index].value);
        if (object == nullptr || object->size() != 2)
        {
            return make_error(ErrorCode::kValidation,
                              "Tone curve point must be an object with only x and y",
                              {{"point_index", std::to_string(index)}});
        }
        const auto x_found = object->find("x");
        const auto y_found = object->find("y");
        if (x_found == object->end() || y_found == object->end())
        {
            return make_error(ErrorCode::kValidation, "Tone curve point is missing x or y",
                              {{"point_index", std::to_string(index)}});
        }
        const double x = as_number(x_found->second, std::numeric_limits<double>::quiet_NaN());
        const double y = as_number(y_found->second, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
        {
            return make_error(ErrorCode::kValidation, "Tone curve point is outside 0..1",
                              {{"point_index", std::to_string(index)}});
        }
        if (!points.empty() && x < points.back().x + 1e-4)
        {
            return make_error(ErrorCode::kValidation, "Tone curve point x values must increase",
                              {{"point_index", std::to_string(index)}});
        }
        points.push_back({x, y});
    }
    if (!near(points.front().x, 0.0) || !near(points.back().x, 1.0))
    {
        return make_error(ErrorCode::kValidation, "Tone curve must start at x=0 and end at x=1");
    }
    return points;
}

Result<std::vector<ToneCurvePoint>> parse_rgb_curve_points(const ParameterValue &value)
{
    const auto *array = std::get_if<ParameterValue::Array>(&value.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "RGB curve points must be an array");
    }
    if (array->size() < kToneCurveMinPoints || array->size() > kToneCurveMaxPoints)
    {
        return make_error(ErrorCode::kValidation, "RGB curve must have between 2 and 20 points",
                          {{"point_count", std::to_string(array->size())}});
    }
    std::vector<ToneCurvePoint> points;
    points.reserve(array->size());
    for (std::size_t index = 0; index < array->size(); ++index)
    {
        const auto *object = std::get_if<ParameterValue::Object>(&(*array)[index].value);
        if (object == nullptr || object->size() != 2)
        {
            return make_error(ErrorCode::kValidation,
                              "RGB curve point must be an object with only x and y",
                              {{"point_index", std::to_string(index)}});
        }
        const auto x_found = object->find("x");
        const auto y_found = object->find("y");
        if (x_found == object->end() || y_found == object->end())
        {
            return make_error(ErrorCode::kValidation, "RGB curve point is missing x or y",
                              {{"point_index", std::to_string(index)}});
        }
        const double x = as_number(x_found->second, std::numeric_limits<double>::quiet_NaN());
        const double y = as_number(y_found->second, std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
        {
            return make_error(ErrorCode::kValidation, "RGB curve point is outside 0..1",
                              {{"point_index", std::to_string(index)}});
        }
        if (!points.empty() && !(x > points.back().x))
        {
            return make_error(ErrorCode::kValidation, "RGB curve point x values must increase",
                              {{"point_index", std::to_string(index)}});
        }
        points.push_back({x, y});
    }
    return points;
}

ParameterValue tone_curve_points_to_parameter(const std::vector<ToneCurvePoint> &points)
{
    ParameterValue::Array array;
    const auto &source = tone_curve_is_identity(points) ?
                             std::vector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}} :
                             points;
    array.reserve(source.size());
    for (const auto &point : source)
    {
        array.push_back(ParameterValue{ParameterValue::Object{
            {"x", ParameterValue{point.x}},
            {"y", ParameterValue{point.y}},
        }});
    }
    return ParameterValue{std::move(array)};
}

Result<void>
validate_tone_curve_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    if (const auto found = parameters.find("working_space"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Tone curve working_space must be a string");
        }
        auto space = parse_tone_curve_working_space(*text);
        if (!space)
        {
            return space.error();
        }
    }
    if (const auto found = parameters.find("interpolation"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr || *text != kToneCurveInterpolationMonotoneHermite)
        {
            return make_error(ErrorCode::kValidation, "Tone curve interpolation is unsupported",
                              {{"interpolation", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("channel_mode"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr ||
            (*text != kToneCurveChannelModeRgb && *text != kToneCurveChannelModeIndependent))
        {
            return make_error(ErrorCode::kValidation, "Tone curve channel_mode is unsupported",
                              {{"channel_mode", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("preserve_colors"); found != parameters.end())
    {
        const auto *text = as_string_if(found->second);
        if (text == nullptr ||
            (*text != kToneCurvePreserveColorsAverage && *text != kToneCurvePreserveColorsNone &&
             *text != kToneCurvePreserveColorsLuminance && *text != kToneCurvePreserveColorsMax &&
             *text != kToneCurvePreserveColorsSum && *text != kToneCurvePreserveColorsNorm &&
             *text != kToneCurvePreserveColorsPower))
        {
            return make_error(ErrorCode::kValidation, "Tone curve preserve_colors is unsupported",
                              {{"preserve_colors", text == nullptr ? std::string() : *text}});
        }
    }
    if (const auto found = parameters.find("points"); found != parameters.end())
    {
        auto points = parse_tone_curve_points(found->second);
        if (!points)
        {
            return points.error();
        }
    }
    return {};
}

Result<void>
validate_sigmoid_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    const auto validate_one_of =
        [&](const std::string_view name,
            const std::initializer_list<std::string_view> allowed) -> Result<void>
    {
        const auto found = parameters.find(std::string(name));
        if (found == parameters.end())
        {
            return {};
        }
        const auto *text = as_string_if(found->second);
        if (text == nullptr)
        {
            return make_error(ErrorCode::kValidation, "Sigmoid parameter value is unsupported",
                              {{"parameter", std::string(name)}});
        }
        for (const auto expected : allowed)
        {
            if (*text == expected)
            {
                return {};
            }
        }
        return make_error(ErrorCode::kValidation, "Sigmoid parameter value is unsupported",
                          {{"parameter", std::string(name)}, {"value", *text}});
    };
    auto working_space = validate_one_of("working_space", {kSigmoidWorkingSpaceLinearSrgb});
    if (!working_space)
    {
        return working_space.error();
    }
    auto color_processing = validate_one_of(
        "color_processing", {kSigmoidColorProcessingPerChannel, kSigmoidColorProcessingRgbRatio});
    if (!color_processing)
    {
        return color_processing.error();
    }
    return {};
}

void clamp_develop(DevelopParams &params) noexcept
{
    clamp_temperature(params.temperature);
    const auto clamp_profile_gamma_value =
        [](double &value, const double default_value, const double minimum, const double maximum)
    { value = std::isfinite(value) ? clamp_value(value, minimum, maximum) : default_value; };
    if (!params.profile_gamma_enabled &&
        params.profile_gamma.mode != kProfileGammaModeLogarithmic &&
        params.profile_gamma.mode != kProfileGammaModeGamma)
    {
        params.profile_gamma.mode = std::string(kProfileGammaModeLogarithmic);
    }
    clamp_profile_gamma_value(params.profile_gamma.linear, kProfileGammaLinearDefault,
                              kProfileGammaLinearMin, kProfileGammaLinearMax);
    clamp_profile_gamma_value(params.profile_gamma.gamma, kProfileGammaGammaDefault,
                              kProfileGammaGammaMin, kProfileGammaGammaMax);
    clamp_profile_gamma_value(params.profile_gamma.dynamic_range, kProfileGammaDynamicRangeDefault,
                              kProfileGammaDynamicRangeMin, kProfileGammaDynamicRangeMax);
    clamp_profile_gamma_value(params.profile_gamma.grey_point, kProfileGammaGreyPointDefault,
                              kProfileGammaGreyPointMin, kProfileGammaGreyPointMax);
    clamp_profile_gamma_value(params.profile_gamma.shadows_range, kProfileGammaShadowsRangeDefault,
                              kProfileGammaShadowsRangeMin, kProfileGammaShadowsRangeMax);
    clamp_profile_gamma_value(params.profile_gamma.security_factor,
                              kProfileGammaSecurityFactorDefault, kProfileGammaSecurityFactorMin,
                              kProfileGammaSecurityFactorMax);
    const auto clamp_primaries_value =
        [](double &value, const double default_value, const double minimum, const double maximum)
    { value = std::isfinite(value) ? clamp_value(value, minimum, maximum) : default_value; };
    clamp_primaries_value(params.primaries.achromatic_tint_hue, 0.0, kPrimariesHueMin,
                          kPrimariesHueMax);
    clamp_primaries_value(params.primaries.achromatic_tint_purity, 0.0,
                          kPrimariesAchromaticTintPurityMin, kPrimariesAchromaticTintPurityMax);
    clamp_primaries_value(params.primaries.red_hue, 0.0, kPrimariesHueMin, kPrimariesHueMax);
    clamp_primaries_value(params.primaries.red_purity, 1.0, kPrimariesPrimaryPurityMin,
                          kPrimariesPrimaryPurityMax);
    clamp_primaries_value(params.primaries.green_hue, 0.0, kPrimariesHueMin, kPrimariesHueMax);
    clamp_primaries_value(params.primaries.green_purity, 1.0, kPrimariesPrimaryPurityMin,
                          kPrimariesPrimaryPurityMax);
    clamp_primaries_value(params.primaries.blue_hue, 0.0, kPrimariesHueMin, kPrimariesHueMax);
    clamp_primaries_value(params.primaries.blue_purity, 1.0, kPrimariesPrimaryPurityMin,
                          kPrimariesPrimaryPurityMax);
    for (auto *channel : {&params.channel_mixer.red, &params.channel_mixer.green,
                          &params.channel_mixer.blue, &params.channel_mixer.saturation,
                          &params.channel_mixer.lightness, &params.channel_mixer.grey})
    {
        for (double &value : *channel)
        {
            value = clamp_value(value, -2.0, 2.0);
        }
    }
    if (params.channel_mixer.adaptation != kChannelMixerAdaptationRgb &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationCat16 &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationLinearBradford &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationFullBradford &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationXyz)
    {
        params.channel_mixer.adaptation = std::string(kChannelMixerAdaptationRgb);
    }
    params.channel_mixer.illuminant_x =
        clamp_value(params.channel_mixer.illuminant_x, 0.000001, 0.999999);
    params.channel_mixer.illuminant_y =
        clamp_value(params.channel_mixer.illuminant_y, 0.000001, 0.999999);
    params.channel_mixer.gamut = clamp_value(params.channel_mixer.gamut, 0.0, 12.0);
    clamp_legacy_color_balance(params.color_balance);
    params.color_checker_patch =
        params.color_checker.patches.empty() ?
            0 :
            std::clamp(params.color_checker_patch, std::int64_t{0},
                       static_cast<std::int64_t>(params.color_checker.patches.size() - 1U));
    clamp_color_balance(params.color_balance_rgb);
    clamp_color_correction(params.color_correction);
    clamp_color_contrast(params.color_contrast);
    clamp_color_reconstruction(params.color_reconstruction);
    clamp_color_harmonizer(params.color_harmonizer);
    // Preserve compatibility with existing typed callers that represented an
    // active operation by setting only the historical enabled/value fields.
    // The added explicit-presence bit is needed for disabled/default masked
    // instances, but must not make a normal active round trip unequal.
    if (params.color_harmonizer_enabled)
    {
        params.color_harmonizer_present = true;
    }
    if (params.exposure_mode != kExposureModeManual &&
        params.exposure_mode != kExposureModeDeflicker)
    {
        params.exposure_mode = std::string(kExposureModeManual);
    }
    params.exposure_black =
        clamp_value(params.exposure_black, kExposureBlackMin, kExposureBlackMax);
    params.exposure_ev = clamp_value(params.exposure_ev, kExposureEvMin, kExposureEvMax);
    params.exposure_deflicker_percentile =
        clamp_value(params.exposure_deflicker_percentile, kExposureDeflickerPercentileMin,
                    kExposureDeflickerPercentileMax);
    params.exposure_deflicker_target_ev =
        clamp_value(params.exposure_deflicker_target_ev, kExposureDeflickerTargetEvMin,
                    kExposureDeflickerTargetEvMax);
    if (params.exposure_mode == kExposureModeManual)
    {
        const double white = std::exp2(-params.exposure_ev);
        if (params.exposure_black >= white)
        {
            params.exposure_black = std::max(kExposureBlackMin, white - 0.01);
        }
    }
    params.contrast = clamp_value(params.contrast, -1.0, 1.0);
    params.highlights = clamp_value(params.highlights, -1.0, 1.0);
    params.shadows = clamp_value(params.shadows, -1.0, 1.0);
    params.whites = clamp_value(params.whites, -1.0, 1.0);
    params.blacks = clamp_value(params.blacks, -1.0, 1.0);
    params.vibrance = clamp_value(params.vibrance, -1.0, 1.0);
    params.saturation = clamp_value(params.saturation, -1.0, 1.0);
    params.rotate_quarters = ((params.rotate_quarters % 4) + 4) % 4;
    params.flip_horizontal = flag01(params.flip_horizontal);
    params.flip_vertical = flag01(params.flip_vertical);
    params.straighten_degrees =
        clamp_value(params.straighten_degrees, kDevelopStraightenMin, kDevelopStraightenMax);
    params.crop_width = clamp_value(params.crop_width, 0.01, 1.0);
    params.crop_height = clamp_value(params.crop_height, 0.01, 1.0);
    params.crop_x = clamp_value(params.crop_x, 0.0, 1.0 - params.crop_width);
    params.crop_y = clamp_value(params.crop_y, 0.0, 1.0 - params.crop_height);
    params.canvas.percent_left =
        clamp_value(params.canvas.percent_left, kCanvasPercentMin, kCanvasPercentMax);
    params.canvas.percent_right =
        clamp_value(params.canvas.percent_right, kCanvasPercentMin, kCanvasPercentMax);
    params.canvas.percent_top =
        clamp_value(params.canvas.percent_top, kCanvasPercentMin, kCanvasPercentMax);
    params.canvas.percent_bottom =
        clamp_value(params.canvas.percent_bottom, kCanvasPercentMin, kCanvasPercentMax);
    if (canvas_color_name(params.canvas.color).empty())
        params.canvas.color = CanvasColor::kGreen;
    params.sharpen = clamp_value(params.sharpen, 0.0, 2.0);
    params.sharpen_radius =
        clamp_value(params.sharpen_radius, kSharpenRadiusMin, kSharpenRadiusMax);
    params.sharpen_threshold =
        clamp_value(params.sharpen_threshold, kSharpenThresholdMin, kSharpenThresholdMax);
    params.retouch.num_scales =
        std::clamp<std::int64_t>(params.retouch.num_scales, 0, kRetouchMaxScales);
    params.retouch.merge_from_scale =
        std::clamp<std::int64_t>(params.retouch.merge_from_scale, 0, params.retouch.num_scales);
    params.retouch.max_heal_iterations =
        std::clamp<std::int64_t>(params.retouch.max_heal_iterations, 1, kRetouchMaxHealIterations);
    if (params.retouch.regions.size() > kRetouchMaxRegions)
    {
        params.retouch.regions.resize(kRetouchMaxRegions);
    }
    for (auto &region : params.retouch.regions)
    {
        region.scale = std::clamp<std::int64_t>(region.scale, 0, params.retouch.num_scales + 1);
        region.opacity = clamp_value(region.opacity, 0.0, 1.0);
        region.source_x = clamp_value(region.source_x, 0.0, 1.0);
        region.source_y = clamp_value(region.source_y, 0.0, 1.0);
        region.blur_radius =
            clamp_value(region.blur_radius, kRetouchBlurRadiusMin, kRetouchBlurRadiusMax);
        for (double &channel : region.fill_color)
        {
            channel = clamp_value(channel, 0.0, 1.0);
        }
        region.fill_brightness = clamp_value(region.fill_brightness, -1.0, 1.0);
    }
    params.clarity = clamp_value(params.clarity, -1.0, 1.0);
    params.vignette = clamp_value(params.vignette, 0.0, 1.0);
    params.grain = clamp_value(params.grain, 0.0, 1.0);
    params.bloom = clamp_value(params.bloom, 0.0, 1.0);
    params.soften = clamp_value(params.soften, 0.0, 1.0);
    params.dehaze = clamp_value(params.dehaze, -1.0, 1.0);
    params.dehaze_distance =
        clamp_value(params.dehaze_distance, kDehazeDistanceMin, kDehazeDistanceMax);
    if (output_dither_method_name(params.output_dither.method).empty())
        params.output_dither.method = OutputDitherMethod::kFloydSteinbergAuto;
    params.output_dither.random_damping_db = clamp_value(
        params.output_dither.random_damping_db, kOutputDitherDampingMin, kOutputDitherDampingMax);
    for (double &channel : params.frame.border_color)
        channel = clamp_value(channel, 0.0, 1.0);
    for (double &channel : params.frame.frame_color)
        channel = clamp_value(channel, 0.0, 1.0);
    params.frame.aspect = clamp_value(params.frame.aspect, -1.0, 3.0);
    if (params.frame.aspect < 0.0 && !near(params.frame.aspect, -1.0))
        params.frame.aspect = -1.0;
    params.frame.size = clamp_value(params.frame.size, 0.0, 0.5);
    params.frame.position_h = clamp_value(params.frame.position_h, 0.0, 1.0);
    params.frame.position_v = clamp_value(params.frame.position_v, 0.0, 1.0);
    params.frame.frame_size = clamp_value(params.frame.frame_size, 0.0, 1.0);
    params.frame.frame_offset = clamp_value(params.frame.frame_offset, 0.0, 1.0);
    if (frame_orientation_name(params.frame.orientation).empty())
        params.frame.orientation = FrameOrientation::kAuto;
    if (frame_basis_name(params.frame.basis).empty())
        params.frame.basis = FrameBasis::kAuto;
    for (double &channel : params.watermark.color)
        channel = clamp_value(channel, 0.0, 1.0);
    params.watermark.opacity = clamp_value(params.watermark.opacity, 0.0, 1.0);
    params.watermark.scale_percent =
        clamp_value(params.watermark.scale_percent, kWatermarkScaleMin, kWatermarkScaleMax);
    params.watermark.x_offset = clamp_value(params.watermark.x_offset, -1.0, 1.0);
    params.watermark.y_offset = clamp_value(params.watermark.y_offset, -1.0, 1.0);
    params.watermark.rotation_degrees =
        clamp_value(params.watermark.rotation_degrees, -180.0, 180.0);
    if (watermark_alignment_name(params.watermark.alignment).empty())
        params.watermark.alignment = WatermarkAlignment::kBottomRight;
    params.velvia = clamp_value(params.velvia, 0.0, 1.0);
    params.monochrome.filter_a = clamp_value(params.monochrome.filter_a, -128.0, 128.0);
    params.monochrome.filter_b = clamp_value(params.monochrome.filter_b, -128.0, 128.0);
    params.monochrome.size = clamp_value(params.monochrome.size, 0.5, 3.0);
    params.monochrome.highlights = clamp_value(params.monochrome.highlights, 0.0, 1.0);
    params.monochrome.mix = clamp_value(params.monochrome.mix, 0.0, 1.0);
    params.split_toning.shadow_hue = clamp_value(params.split_toning.shadow_hue, 0.0, 1.0);
    params.split_toning.shadow_saturation =
        clamp_value(params.split_toning.shadow_saturation, 0.0, 1.0);
    params.split_toning.highlight_hue = clamp_value(params.split_toning.highlight_hue, 0.0, 1.0);
    params.split_toning.highlight_saturation =
        clamp_value(params.split_toning.highlight_saturation, 0.0, 1.0);
    params.split_toning.balance = clamp_value(params.split_toning.balance, 0.0, 1.0);
    params.split_toning.compress = clamp_value(params.split_toning.compress, 0.0, 100.0);
    params.split_toning.mix = clamp_value(params.split_toning.mix, 0.0, 1.0);
    params.gamma = clamp_value(params.gamma, 0.2, 3.0);
    clamp_rgb_levels(params.rgb_levels);
    for (auto &channel : params.rgb_curve.channels)
    {
        for (auto &point : channel)
        {
            point.x = clamp_value(point.x, 0.0, 1.0);
            point.y = clamp_value(point.y, 0.0, 1.0);
        }
        if (channel.size() < kToneCurveMinPoints)
        {
            channel = {{0.0, 0.0}, {1.0, 1.0}};
        }
    }
    if (params.rgb_curve.mode != kRgbLevelsModeIndependent)
    {
        params.rgb_curve.mode = std::string(kRgbLevelsModeLinked);
    }
    if (params.rgb_curve.interpolation != kToneCurveInterpolationMonotoneHermite)
    {
        params.rgb_curve.interpolation = std::string(kToneCurveInterpolationMonotoneHermite);
    }
    params.sigmoid_contrast =
        clamp_value(params.sigmoid_contrast, kSigmoidContrastMin, kSigmoidContrastMax);
    params.sigmoid_skew = clamp_value(params.sigmoid_skew, kSigmoidSkewMin, kSigmoidSkewMax);
    params.sigmoid_display_white =
        clamp_value(params.sigmoid_display_white, kSigmoidDisplayWhiteMin, kSigmoidDisplayWhiteMax);
    params.sigmoid_display_black =
        clamp_value(params.sigmoid_display_black, kSigmoidDisplayBlackMin, kSigmoidDisplayBlackMax);
    params.sigmoid_hue_preservation = clamp_value(params.sigmoid_hue_preservation, 0.0, 1.0);
    params.raw_highlights = clamp_value(params.raw_highlights, 0.0, 1.0);
    params.raw_highlights_clip = clamp_value(params.raw_highlights_clip, 0.5, 1.0);
    if (params.raw_highlights_mode != kRawHighlightsModeClip &&
        params.raw_highlights_mode != kRawHighlightsModeInpaint &&
        params.raw_highlights_mode != kRawHighlightsModeOpposed &&
        params.raw_highlights_mode != kRawHighlightsModeLch)
    {
        params.raw_highlights_mode = std::string(kRawHighlightsModeOpposed);
    }
    params.hot_pixels_strength = clamp_value(params.hot_pixels_strength, 0.0, 1.0);
    params.hot_pixels_threshold = clamp_value(params.hot_pixels_threshold, 0.0, 1.0);
    params.raw_ca_iterations =
        std::clamp(params.raw_ca_iterations, std::int64_t{0}, std::int64_t{5});
    params.raw_denoise_threshold = clamp_value(params.raw_denoise_threshold, 0.0, 1.0);
    for (auto &channel : params.raw_denoise_bands)
    {
        for (double &band : channel)
        {
            band = clamp_value(band, 0.0, 16.0);
        }
    }
    params.denoise = clamp_value(params.denoise, 0.0, 1.0);
    params.denoise_chroma = clamp_value(params.denoise_chroma, 0.0, 1.0);
    params.denoise_radius = clamp_value(params.denoise_radius, 0.5, 8.0);
    params.lens_k1 = clamp_value(params.lens_k1, -2.0, 2.0);
    params.lens_k2 = clamp_value(params.lens_k2, -2.0, 2.0);
    params.lens_tca_r = clamp_value(params.lens_tca_r, 0.9, 1.1);
    params.lens_tca_b = clamp_value(params.lens_tca_b, 0.9, 1.1);
    params.lens_vignetting = clamp_value(params.lens_vignetting, 0.0, 1.0);
    if (params.lens_mode != kLensModeManual && params.lens_mode != kLensModeLookup)
    {
        params.lens_mode = std::string(kLensModeManual);
    }
    params.lens_focal_mm = clamp_value(params.lens_focal_mm, 1.0, 2000.0);
    for (double &value : params.color_eq_hue)
    {
        value = clamp_value(value, -0.5, 0.5);
    }
    for (double &value : params.color_eq_sat)
    {
        value = clamp_value(value, -1.0, 1.0);
    }
    for (double &value : params.color_eq_light)
    {
        value = clamp_value(value, -1.0, 1.0);
    }
    params.color_eq_band = std::clamp(params.color_eq_band, std::int64_t{0},
                                      static_cast<std::int64_t>(kColorEqualizerBandCount - 1U));
    if (color_zones_channel_name(params.color_zones.select_by).empty())
        params.color_zones.select_by = ColorZonesChannel::kHue;
    params.color_zones.strength = clamp_value(params.color_zones.strength, -200.0, 200.0);
    params.color_zones_band = std::clamp(params.color_zones_band, std::int64_t{0},
                                         static_cast<std::int64_t>(kColorEqualizerBandCount - 1U));
    for (auto &curve : params.color_zones.curves)
    {
        if (color_zones_interpolation_name(curve.interpolation).empty())
            curve.interpolation = ColorZonesInterpolation::kCatmullRom;
        for (auto &point : curve.points)
        {
            point.x = clamp_value(point.x, 0.0, 1.0);
            point.y = clamp_value(point.y, 0.0, 1.0);
        }
    }
    params.graduated_density = clamp_value(params.graduated_density, -4.0, 4.0);
    params.graduated_hardness = clamp_value(params.graduated_hardness, 0.0, 1.0);
    params.graduated_rotation = clamp_value(params.graduated_rotation, -180.0, 180.0);
    params.graduated_offset = clamp_value(params.graduated_offset, -1.0, 1.0);
    if (params.graduated_enabled)
    {
        params.graduated_present = true;
    }
    else if (!params.graduated_present && !near(params.graduated_density, 0.0))
    {
        // Preserve compatibility with callers that predate explicit presence,
        // without re-enabling a loaded disabled operation whose stored density
        // is intentionally non-zero.
        params.graduated_present = true;
        params.graduated_enabled = true;
    }
    params.tone_eq_blacks = clamp_value(params.tone_eq_blacks, -4.0, 4.0);
    params.tone_eq_shadows = clamp_value(params.tone_eq_shadows, -4.0, 4.0);
    params.tone_eq_midtones = clamp_value(params.tone_eq_midtones, -4.0, 4.0);
    params.tone_eq_highlights = clamp_value(params.tone_eq_highlights, -4.0, 4.0);
    params.tone_eq_whites = clamp_value(params.tone_eq_whites, -4.0, 4.0);
    if (params.tone_curve_working_space != kToneCurveWorkingSpaceSrgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLinearRgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceRgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLab &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceXyz &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLabIndependent)
    {
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceRgb);
    }
    clamp_tone_curve(params.tone_curve);
}

bool DevelopParams::is_identity() const noexcept
{
    return masks.empty() && !color_harmonizer_present && !color_harmonizer_mask_id.has_value() &&
           !graduated_present && !graduated_enabled && !graduated_mask_id.has_value() &&
           temperature.is_identity() && !profile_gamma_enabled && input_color.is_identity() &&
           output_color.is_identity() && primaries.is_identity() && channel_mixer.is_identity() &&
           exposure_mode == kExposureModeManual && near(exposure_black, 0.0) &&
           near(exposure_ev, 0.0) &&
           near(exposure_deflicker_percentile, kExposureDeflickerPercentileDefault) &&
           near(exposure_deflicker_target_ev, kExposureDeflickerTargetEvDefault) &&
           !exposure_compensate_exposure_bias && !exposure_compensate_highlight_preservation &&
           near(contrast, 0.0) && near(highlights, 0.0) && near(shadows, 0.0) &&
           near(whites, 0.0) && near(blacks, 0.0) && near(vibrance, 0.0) && near(saturation, 0.0) &&
           rotate_quarters % 4 == 0 && flip_horizontal == 0 && flip_vertical == 0 &&
           near(straighten_degrees, 0.0) && near(crop_x, 0.0) && near(crop_y, 0.0) &&
           near(crop_width, 1.0) && near(crop_height, 1.0) && !canvas_present && !canvas_enabled &&
           near(sharpen, 0.0) && near(sharpen_radius, 2.0) && near(sharpen_threshold, 0.5) &&
           retouch.is_identity() && near(clarity, 0.0) && near(vignette, 0.0) && near(grain, 0.0) &&
           near(bloom, 0.0) && near(soften, 0.0) && near(dehaze, 0.0) &&
           near(dehaze_distance, 0.2) && dehaze_adaptive && !output_dither_present &&
           !output_dither_enabled && !frame_present && !frame_enabled && !watermark_present &&
           !watermark_enabled && near(velvia, 0.0) && !color_balance_enabled &&
           !color_checker_enabled && color_balance_rgb.is_identity() && !color_correction_enabled &&
           !color_contrast_enabled && !color_reconstruction_enabled && !color_zones_present &&
           !color_zones_enabled && !color_zones_mask_id.has_value() && !color_harmonizer_enabled &&
           !monochrome_present && !monochrome_enabled && !monochrome_mask_id.has_value() &&
           !split_toning_present && !split_toning_enabled && !split_toning_mask_id.has_value() &&
           near(gamma, kDevelopGammaDefault) && rgb_levels.is_identity() &&
           rgb_curve.is_identity() && tone_curve_is_identity(tone_curve) && !sigmoid_enabled &&
           near(raw_highlights, 0.0) && near(hot_pixels_strength, 0.0) && raw_ca_iterations == 0 &&
           near(raw_denoise_threshold, 0.0) && near(denoise, 0.0) && near(lens_k1, 0.0) &&
           near(lens_k2, 0.0) && near(lens_tca_r, 1.0) && near(lens_tca_b, 1.0) &&
           near(lens_vignetting, 0.0) && lens_mode != kLensModeLookup &&
           bands_near_zero(color_eq_hue) && bands_near_zero(color_eq_sat) &&
           bands_near_zero(color_eq_light) && near(graduated_density, 0.0) &&
           near(tone_eq_blacks, 0.0) && near(tone_eq_shadows, 0.0) && near(tone_eq_midtones, 0.0) &&
           near(tone_eq_highlights, 0.0) && near(tone_eq_whites, 0.0);
}

namespace
{

bool assign_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
    const auto selected = [value](const auto &options) -> std::optional<std::string>
    {
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }
        const auto index = static_cast<std::int64_t>(std::llround(value));
        if (index < 0 || index >= static_cast<std::int64_t>(options.size()))
        {
            return std::nullopt;
        }
        return std::string(options[static_cast<std::size_t>(index)]);
    };
    if (apply_temperature_field(params.temperature, name, value))
    {
        return true;
    }
    if (name == "profileGammaEnabled")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma_enabled = value >= 0.5;

        return true;
    }
    if (name == "profileGammaModeIndex")
    {
        auto mode = selected(kSelectableProfileGammaModes);
        if (!mode)
        {
            return false;
        }
        params.profile_gamma.mode = std::move(*mode);

        return true;
    }
    if (name == "profileGammaLinear")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.linear = value;

        return true;
    }
    if (name == "profileGammaGamma")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.gamma = value;

        return true;
    }
    if (name == "profileGammaDynamicRange")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.dynamic_range = value;

        return true;
    }
    if (name == "profileGammaGreyPoint")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.grey_point = value;

        return true;
    }
    if (name == "profileGammaShadowsRange")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.shadows_range = value;

        return true;
    }
    if (name == "profileGammaSecurityFactor")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.security_factor = value;

        return true;
    }
    if (name == "inputProfile")
    {
        auto profile = selected(kSelectableInputProfiles);
        if (!profile)
        {
            return false;
        }
        params.input_color.input_profile = std::move(*profile);
        params.input_color.input_profile_filename.clear();

        return true;
    }
    if (name == "workingProfile")
    {
        auto profile = selected(kSelectableWorkingProfiles);
        if (!profile)
        {
            return false;
        }
        params.input_color.working_profile = std::move(*profile);
        params.input_color.working_profile_filename.clear();

        return true;
    }
    if (name == "renderingIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.input_color.rendering_intent = std::move(*intent);

        return true;
    }
    if (name == "gamutNormalize")
    {
        auto normalize = selected(kSelectableColorNormalizations);
        if (!normalize)
        {
            return false;
        }
        params.input_color.gamut_normalize = std::move(*normalize);

        return true;
    }
    if (name == "blueMapping")
    {
        params.input_color.blue_mapping = value >= 0.5;

        return true;
    }
    if (name == "outputProfile")
    {
        auto profile = selected(kSelectableOutputProfiles);
        if (!profile)
        {
            return false;
        }
        params.output_color.output_profile = std::move(*profile);
        params.output_color.output_profile_filename.clear();

        return true;
    }
    if (name == "outputRenderingIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.output_color.rendering_intent = std::move(*intent);

        return true;
    }
    if (name == "proofMode")
    {
        auto mode = selected(kSelectableProofModes);
        if (!mode)
        {
            return false;
        }
        params.output_color.proof_mode = std::move(*mode);

        return true;
    }
    if (name == "proofProfile")
    {
        auto profile = selected(kSelectableProofProfiles);
        if (!profile)
        {
            return false;
        }
        params.output_color.proof_profile = std::move(*profile);
        params.output_color.proof_profile_filename.clear();

        return true;
    }
    if (name == "proofIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.output_color.proof_intent = std::move(*intent);

        return true;
    }
    if (name == "outputBlackPointCompensation")
    {
        params.output_color.black_point_compensation = value >= 0.5;

        return true;
    }
    if (name == "primariesAchromaticHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.achromatic_tint_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesAchromaticPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.achromatic_tint_purity = value;

        return true;
    }
    if (name == "primariesRedHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.red_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesRedPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.red_purity = value;

        return true;
    }
    if (name == "primariesGreenHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.green_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesGreenPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.green_purity = value;

        return true;
    }
    if (name == "primariesBlueHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.blue_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesBluePurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.blue_purity = value;

        return true;
    }
    if (name == "channelMixerRR")
    {
        params.channel_mixer.red[0] = value;

        return true;
    }
    if (name == "channelMixerRG")
    {
        params.channel_mixer.red[1] = value;

        return true;
    }
    if (name == "channelMixerRB")
    {
        params.channel_mixer.red[2] = value;

        return true;
    }
    if (name == "channelMixerGR")
    {
        params.channel_mixer.green[0] = value;

        return true;
    }
    if (name == "channelMixerGG")
    {
        params.channel_mixer.green[1] = value;

        return true;
    }
    if (name == "channelMixerGB")
    {
        params.channel_mixer.green[2] = value;

        return true;
    }
    if (name == "channelMixerBR")
    {
        params.channel_mixer.blue[0] = value;

        return true;
    }
    if (name == "channelMixerBG")
    {
        params.channel_mixer.blue[1] = value;

        return true;
    }
    if (name == "channelMixerBB")
    {
        params.channel_mixer.blue[2] = value;

        return true;
    }
    if (name == "exposureMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.exposure_mode =
            value == 0.0 ? std::string(kExposureModeManual) : std::string(kExposureModeDeflicker);

        return true;
    }
    if (name == "exposureBlack")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_black = value;

        return true;
    }
    if (name == "exposure")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_ev = value;
        const double white = std::exp2(-value);
        if (params.exposure_black >= white)
        {
            params.exposure_black = std::max(kExposureBlackMin, white - 0.01);
        }

        return true;
    }
    if (name == "exposureDeflickerPercentile")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_deflicker_percentile = value;

        return true;
    }
    if (name == "exposureDeflickerTarget")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_deflicker_target_ev = value;

        return true;
    }
    if (name == "exposureCompensateBias")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.exposure_compensate_exposure_bias = value == 1.0;

        return true;
    }
    if (name == "exposureCompensateHighlight")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.exposure_compensate_highlight_preservation = value == 1.0;

        return true;
    }
    if (name == "contrast")
    {
        params.contrast = value;

        return true;
    }
    if (name == "highlights")
    {
        params.highlights = value;

        return true;
    }
    if (name == "shadows")
    {
        params.shadows = value;

        return true;
    }
    if (name == "whites")
    {
        params.whites = value;

        return true;
    }
    if (name == "blacks")
    {
        params.blacks = value;

        return true;
    }
    if (name == "vibrance")
    {
        params.vibrance = value;

        return true;
    }
    if (name == "saturation")
    {
        params.saturation = value;

        return true;
    }
    if (name == "straighten")
    {
        params.straighten_degrees = value;

        return true;
    }
    if (name == "cropX")
    {
        params.crop_x = value;

        return true;
    }
    if (name == "cropY")
    {
        params.crop_y = value;

        return true;
    }
    if (name == "cropWidth")
    {
        params.crop_width = value;

        return true;
    }
    if (name == "cropHeight")
    {
        params.crop_height = value;

        return true;
    }
    if (name == "canvasEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.canvas_enabled = value == 1.0;
        if (params.canvas_enabled)
        {
            params.canvas_present = true;
            params.geometry_effect_enabled = true;
        }
        else if (params.canvas.is_identity())
        {
            params.canvas_present = false;
        }

        return true;
    }
    if (name == "canvasLeft" || name == "canvasRight" || name == "canvasTop" ||
             name == "canvasBottom")
    {
        params.canvas_present = true;
        params.canvas_enabled = true;
        params.geometry_effect_enabled = true;
        double *target = name == "canvasLeft"  ? &params.canvas.percent_left :
                         name == "canvasRight" ? &params.canvas.percent_right :
                         name == "canvasTop"   ? &params.canvas.percent_top :
                                                 &params.canvas.percent_bottom;
        *target = value;

        return true;
    }
    if (name == "canvasColorIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 4.0)
            return false;
        params.canvas_present = true;
        params.canvas_enabled = true;
        params.geometry_effect_enabled = true;
        params.canvas.color = static_cast<CanvasColor>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "sharpen")
    {
        params.sharpen = value;

        return true;
    }
    if (name == "sharpenRadius")
    {
        params.sharpen_radius = value;

        return true;
    }
    if (name == "sharpenThreshold")
    {
        params.sharpen_threshold = value;

        return true;
    }
    if (name == "clarity")
    {
        params.clarity = value;

        return true;
    }
    if (name == "vignette")
    {
        params.vignette = value;

        return true;
    }
    if (name == "grain")
    {
        params.grain = value;

        return true;
    }
    if (name == "bloom")
    {
        params.bloom = value;

        return true;
    }
    if (name == "soften")
    {
        params.soften = value;

        return true;
    }
    if (name == "dehaze")
    {
        params.dehaze = value;

        return true;
    }
    if (name == "dehazeDistance")
    {
        params.dehaze_distance = value;

        return true;
    }
    if (name == "dehazeAdaptive")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.dehaze_adaptive = value == 1.0;

        return true;
    }
    if (name == "outputDitherEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.output_dither_present = true;
        params.output_dither_enabled = value == 1.0;
        if (params.output_dither_enabled)
            params.effects_effect_enabled = true;

        return true;
    }
    if (name == "outputDitherMethodIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value)
            return false;
        auto method = output_dither_method_from_index(static_cast<std::int64_t>(value));
        if (!method)
            return false;
        params.output_dither_present = true;
        params.output_dither_enabled = true;
        params.effects_effect_enabled = true;
        params.output_dither.method = method.value();

        return true;
    }
    if (name == "outputDitherDamping")
    {
        if (!std::isfinite(value))
            return false;
        params.output_dither_present = true;
        params.output_dither_enabled = true;
        params.effects_effect_enabled = true;
        params.output_dither.random_damping_db = value;

        return true;
    }
    if (name == "outputFrameEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.frame_present = true;
        params.frame_enabled = value == 1.0;
        if (params.frame_enabled)
            params.effects_effect_enabled = true;

        return true;
    }
    if (name == "outputFrameBorderRed" || name == "outputFrameBorderGreen" ||
             name == "outputFrameBorderBlue" || name == "outputFrameLineRed" ||
             name == "outputFrameLineGreen" || name == "outputFrameLineBlue")
    {
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        const bool line = name.starts_with("outputFrameLine");
        auto &color = line ? params.frame.frame_color : params.frame.border_color;
        const std::size_t channel = name.ends_with("Red") ? 0U : name.ends_with("Green") ? 1U : 2U;
        color[channel] = value;

        return true;
    }
    if (name == "outputFrameAspect")
    {
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        params.frame.aspect = value;

        return true;
    }
    if (name == "outputFrameOrientationIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        params.frame.orientation = static_cast<FrameOrientation>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "outputFrameSize" || name == "outputFramePositionH" ||
             name == "outputFramePositionV" || name == "outputFrameLineSize" ||
             name == "outputFrameLineOffset")
    {
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        double *target = name == "outputFrameSize"      ? &params.frame.size :
                         name == "outputFramePositionH" ? &params.frame.position_h :
                         name == "outputFramePositionV" ? &params.frame.position_v :
                         name == "outputFrameLineSize"  ? &params.frame.frame_size :
                                                          &params.frame.frame_offset;
        *target = value;

        return true;
    }
    if (name == "outputFrameBasisIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 4.0)
            return false;
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        params.frame.basis = static_cast<FrameBasis>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "watermarkEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.watermark_present = true;
        params.watermark_enabled = value == 1.0;
        if (params.watermark_enabled)
            params.effects_effect_enabled = true;

        return true;
    }
    if (name == "watermarkRed" || name == "watermarkGreen" || name == "watermarkBlue")
    {
        params.watermark_present = true;
        params.watermark_enabled = true;
        params.effects_effect_enabled = true;
        const std::size_t channel = name.ends_with("Red") ? 0U : name.ends_with("Green") ? 1U : 2U;
        params.watermark.color[channel] = value;

        return true;
    }
    if (name == "watermarkOpacity" || name == "watermarkScale" || name == "watermarkOffsetX" ||
             name == "watermarkOffsetY" || name == "watermarkRotation")
    {
        params.watermark_present = true;
        params.watermark_enabled = true;
        params.effects_effect_enabled = true;
        double *target = name == "watermarkOpacity" ? &params.watermark.opacity :
                         name == "watermarkScale"   ? &params.watermark.scale_percent :
                         name == "watermarkOffsetX" ? &params.watermark.x_offset :
                         name == "watermarkOffsetY" ? &params.watermark.y_offset :
                                                      &params.watermark.rotation_degrees;
        *target = value;

        return true;
    }
    if (name == "watermarkAlignmentIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 8.0)
            return false;
        params.watermark_present = true;
        params.watermark_enabled = true;
        params.effects_effect_enabled = true;
        params.watermark.alignment =
            static_cast<WatermarkAlignment>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "velvia")
    {
        params.velvia = value;

        return true;
    }
    if (apply_legacy_color_balance_field(params.color_balance, name, value))
    {
        params.color_balance_enabled = true;

        return true;
    }
    if (apply_color_checker_field(params, name, value))
    {

        return true;
    }
    if (apply_color_balance_field(params.color_balance_rgb, name, value))
    {

        return true;
    }
    if (apply_color_correction_field(params, name, value))
    {

        return true;
    }
    if (apply_color_contrast_field(params, name, value))
    {

        return true;
    }
    if (apply_color_reconstruction_field(params, name, value))
    {

        return true;
    }
    if (apply_color_harmonizer_field(params, name, value))
    {

        return true;
    }
    if (name == "monochrome")
    {
        params.monochrome_present = true;
        params.monochrome_enabled = value > 0.0;
        params.monochrome.mix = value;
        if (params.monochrome_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "monochromeEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.monochrome_present = true;
        params.monochrome_enabled = value == 1.0;
        if (params.monochrome_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "monochromeFilterA" || name == "monochromeFilterB" ||
             name == "monochromeSize" || name == "monochromeHighlights" || name == "monochromeMix")
    {
        params.monochrome_present = true;
        params.monochrome_enabled = true;
        params.color_effect_enabled = true;
        double *target = name == "monochromeFilterA"    ? &params.monochrome.filter_a :
                         name == "monochromeFilterB"    ? &params.monochrome.filter_b :
                         name == "monochromeSize"       ? &params.monochrome.size :
                         name == "monochromeHighlights" ? &params.monochrome.highlights :
                                                          &params.monochrome.mix;
        *target = value;

        return true;
    }
    if (name == "splitShadowsHue")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        params.split_toning.shadow_hue = value;

        return true;
    }
    if (name == "splitHighlightsHue")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        params.split_toning.highlight_hue = value;

        return true;
    }
    if (name == "splitBalance")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        params.split_toning.balance = value;

        return true;
    }
    if (name == "splitAmount")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = value > 0.0;
        params.split_toning.mix = value;
        if (params.split_toning_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "splitToningEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.split_toning_present = true;
        params.split_toning_enabled = value == 1.0;
        if (params.split_toning_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "splitShadowSaturation" || name == "splitHighlightSaturation" ||
             name == "splitCompress" || name == "splitMix")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        double *target =
            name == "splitShadowSaturation"    ? &params.split_toning.shadow_saturation :
            name == "splitHighlightSaturation" ? &params.split_toning.highlight_saturation :
            name == "splitCompress"            ? &params.split_toning.compress :
                                                 &params.split_toning.mix;
        *target = value;

        return true;
    }
    if (name == "gamma")
    {
        params.gamma = value;

        return true;
    }
    if (apply_rgb_levels_field(params, name, value))
    {

        return true;
    }
    if (name == "sigmoidContrast")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_contrast = value;

        return true;
    }
    if (name == "sigmoidSkew")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_skew = value;

        return true;
    }
    if (name == "sigmoidHuePreservation")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_hue_preservation = value;

        return true;
    }
    if (name == "rawHighlights")
    {
        params.raw_highlights = value;

        return true;
    }
    if (name == "rawHighlightsClip")
    {
        params.raw_highlights_clip = value;

        return true;
    }
    if (name == "rawHighlightsMode")
    {
        params.raw_highlights_mode = value >= 0.5 ? std::string(kRawHighlightsModeInpaint) :
                                                    std::string(kRawHighlightsModeClip);

        return true;
    }
    if (name == "hotPixelsStrength")
    {
        params.hot_pixels_strength = value;

        return true;
    }
    if (name == "hotPixelsThreshold")
    {
        params.hot_pixels_threshold = value;

        return true;
    }
    if (name == "hotPixelsPermissive")
    {
        params.hot_pixels_permissive = value >= 0.5;

        return true;
    }
    if (name == "rawCaIterations")
    {
        params.raw_ca_iterations = static_cast<std::int64_t>(std::llround(value));

        return true;
    }
    if (name == "rawCaAvoidShift")
    {
        params.raw_ca_avoid_shift = value >= 0.5;

        return true;
    }
    if (name == "denoise")
    {
        params.denoise = value;

        return true;
    }
    if (name == "denoiseChroma")
    {
        params.denoise_chroma = value;

        return true;
    }
    if (name == "denoiseRadius")
    {
        params.denoise_radius = value;

        return true;
    }
    if (name == "lensK1")
    {
        params.lens_k1 = value;

        return true;
    }
    if (name == "lensK2")
    {
        params.lens_k2 = value;

        return true;
    }
    if (name == "lensTcaR")
    {
        params.lens_tca_r = value;

        return true;
    }
    if (name == "lensTcaB")
    {
        params.lens_tca_b = value;

        return true;
    }
    if (name == "lensVignetting")
    {
        params.lens_vignetting = value;

        return true;
    }
    if (name == "lensMode")
    {
        params.lens_mode =
            value >= 0.5 ? std::string(kLensModeLookup) : std::string(kLensModeManual);

        return true;
    }
    if (name == "lensFocal")
    {
        params.lens_focal_mm = value;

        return true;
    }
    if (name == "colorZonesEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        if (!params.color_zones_present && value == 1.0)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = value == 1.0;
        if (params.color_zones_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "colorZonesSelectByIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        params.color_zones.select_by =
            static_cast<ColorZonesChannel>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "colorZonesBandIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 7.0)
            return false;
        params.color_zones_band = static_cast<std::int64_t>(value);

        return true;
    }
    if (name == "colorZonesStrength")
    {
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        params.color_zones.strength = value;

        return true;
    }
    if (name == "colorZonesLightnessInterpolationIndex" ||
             name == "colorZonesChromaInterpolationIndex" ||
             name == "colorZonesHueInterpolationIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        const std::size_t channel = name.starts_with("colorZonesLightness") ? 0U :
                                    name.starts_with("colorZonesChroma")    ? 1U :
                                                                              2U;
        params.color_zones.curves[channel].interpolation =
            static_cast<ColorZonesInterpolation>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "colorZonesLightness" || name == "colorZonesChroma" || name == "colorZonesHue")
    {
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        if (!studio_color_zones_curves(params.color_zones))
            return false;
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        const std::size_t channel = name == "colorZonesLightness" ? 0U :
                                    name == "colorZonesChroma"    ? 1U :
                                                                    2U;
        const auto band = static_cast<std::size_t>(
            std::clamp(params.color_zones_band, std::int64_t{0}, std::int64_t{7}));
        params.color_zones.curves[channel].points[band].y = value;

        return true;
    }
    if (name == "colorEqBand")
    {
        params.color_eq_band = static_cast<std::int64_t>(std::llround(value));

        return true;
    }
    if (name == "colorEqHue")
    {
        params.color_eq_hue[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;

        return true;
    }
    if (name == "colorEqSat")
    {
        params.color_eq_sat[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;

        return true;
    }
    if (name == "colorEqLight")
    {
        params.color_eq_light[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;

        return true;
    }
    if (name == "graduatedDensity")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_density = value;

        return true;
    }
    if (name == "graduatedHardness")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_hardness = value;

        return true;
    }
    if (name == "graduatedRotation")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_rotation = value;

        return true;
    }
    if (name == "graduatedOffset")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_offset = value;

        return true;
    }
    if (name == "toneEqBlacks")
    {
        params.tone_eq_blacks = value;

        return true;
    }
    if (name == "toneEqShadows")
    {
        params.tone_eq_shadows = value;

        return true;
    }
    if (name == "toneEqMidtones")
    {
        params.tone_eq_midtones = value;

        return true;
    }
    if (name == "toneEqHighlights")
    {
        params.tone_eq_highlights = value;

        return true;
    }
    if (name == "toneEqWhites")
    {
        params.tone_eq_whites = value;

        return true;
    }
    std::size_t band = 0;
    if (parse_band_field(name, "colorEqHue", band))
    {
        params.color_eq_hue[band] = value;
        return true;
    }
    if (parse_band_field(name, "colorEqSat", band))
    {
        params.color_eq_sat[band] = value;
        return true;
    }
    if (parse_band_field(name, "colorEqLight", band))
    {
        params.color_eq_light[band] = value;
        return true;
    }
    return false;
}


[[nodiscard]] bool develop_set_field_accepts(const std::string_view name, const double value)
{
    DevelopParams params;
    return static_cast<bool>(apply_develop_field_strict(params, name, value));
}

[[nodiscard]] std::optional<double> first_accepted_develop_set_value(const std::string_view name)
{
    static constexpr double kSeeds[] = {0.0,  1.0,  0.5,  -1.0, 2.0,  -0.5, 0.1,  0.01, 0.2,
                                        3.0,  4.0,  5.0,  8.0,  10.0, 18.0, -18.0, 45.0, -45.0,
                                        90.0, 100.0, 180.0, 360.0, -100.0, 0.25, -0.25, 1.5};
    for (const double seed : kSeeds)
    {
        if (develop_set_field_accepts(name, seed))
        {
            return seed;
        }
    }
    for (int index = 0; index <= 32; ++index)
    {
        const double seed = static_cast<double>(index);
        if (develop_set_field_accepts(name, seed))
        {
            return seed;
        }
    }
    return std::nullopt;
}

[[nodiscard]] double develop_set_field_extreme(const std::string_view name, const double seed,
                                               const double direction, const bool integer)
{
    double accepted = seed;
    double step = integer ? 1.0 : std::max(1.0e-3, std::abs(seed) * 0.25 + 1.0e-3);
    for (int grow = 0; grow < 40; ++grow)
    {
        const double candidate = accepted + direction * step;
        if (develop_set_field_accepts(name, candidate))
        {
            accepted = candidate;
            step *= 2.0;
            if (std::abs(accepted) > 1.0e7)
            {
                return accepted;
            }
            continue;
        }
        double low = direction > 0.0 ? accepted : candidate;
        double high = direction > 0.0 ? candidate : accepted;
        for (int refine = 0; refine < 48; ++refine)
        {
            double mid = (low + high) * 0.5;
            if (integer)
            {
                mid = direction > 0.0 ? std::floor(mid) : std::ceil(mid);
            }
            if (mid == low || mid == high)
            {
                break;
            }
            if (develop_set_field_accepts(name, mid))
            {
                if (direction > 0.0)
                {
                    low = mid;
                }
                else
                {
                    high = mid;
                }
            }
            else if (direction > 0.0)
            {
                high = mid;
            }
            else
            {
                low = mid;
            }
        }
        const double edge = direction > 0.0 ? low : high;
        return develop_set_field_accepts(name, edge) ? edge : accepted;
    }
    return accepted;
}

[[nodiscard]] std::vector<std::string> candidate_develop_set_names()
{
    static constexpr std::string_view kQuoted[] = {
        "blacks",
        "bloom",
        "blueMapping",
        "canvasBottom",
        "canvasColorIndex",
        "canvasEnabled",
        "canvasLeft",
        "canvasRight",
        "canvasTop",
        "channelMixerBB",
        "channelMixerBG",
        "channelMixerBR",
        "channelMixerGB",
        "channelMixerGG",
        "channelMixerGR",
        "channelMixerRB",
        "channelMixerRG",
        "channelMixerRR",
        "clarity",
        "colorBalanceFormula",
        "colorCheckerEnabled",
        "colorCheckerPatch",
        "colorCheckerPreset",
        "colorCheckerSourceA",
        "colorCheckerSourceB",
        "colorCheckerSourceL",
        "colorCheckerTargetA",
        "colorCheckerTargetB",
        "colorCheckerTargetL",
        "colorContrastEnabled",
        "colorContrastUnbound",
        "colorCorrectionEnabled",
        "colorEqBand",
        "colorEqHue",
        "colorEqLight",
        "colorEqSat",
        "colorHarmonizerAnchorHueDegrees",
        "colorHarmonizerCustomNodeCount",
        "colorHarmonizerEnabled",
        "colorHarmonizerRuleIndex",
        "colorReconstructionEnabled",
        "colorReconstructionHueDegrees",
        "colorReconstructionPrecedenceIndex",
        "colorZonesBandIndex",
        "colorZonesChroma",
        "colorZonesChromaInterpolationIndex",
        "colorZonesEnabled",
        "colorZonesHue",
        "colorZonesHueInterpolationIndex",
        "colorZonesLightness",
        "colorZonesLightnessInterpolationIndex",
        "colorZonesSelectByIndex",
        "colorZonesStrength",
        "contrast",
        "cropHeight",
        "cropWidth",
        "cropX",
        "cropY",
        "dehaze",
        "dehazeAdaptive",
        "dehazeDistance",
        "denoise",
        "denoiseChroma",
        "denoiseRadius",
        "exposure",
        "exposureBlack",
        "exposureCompensateBias",
        "exposureCompensateHighlight",
        "exposureDeflickerPercentile",
        "exposureDeflickerTarget",
        "exposureMode",
        "gamma",
        "gamutNormalize",
        "graduatedDensity",
        "graduatedHardness",
        "graduatedOffset",
        "graduatedRotation",
        "grain",
        "highlights",
        "hotPixelsPermissive",
        "hotPixelsStrength",
        "hotPixelsThreshold",
        "inputProfile",
        "legacyColorBalanceMode",
        "lensFocal",
        "lensK1",
        "lensK2",
        "lensMode",
        "lensTcaB",
        "lensTcaR",
        "lensVignetting",
        "monochrome",
        "monochromeEnabled",
        "monochromeFilterA",
        "monochromeFilterB",
        "monochromeHighlights",
        "monochromeMix",
        "monochromeSize",
        "outputBlackPointCompensation",
        "outputDitherDamping",
        "outputDitherEnabled",
        "outputDitherMethodIndex",
        "outputFrameAspect",
        "outputFrameBasisIndex",
        "outputFrameBorderBlue",
        "outputFrameBorderGreen",
        "outputFrameBorderRed",
        "outputFrameEnabled",
        "outputFrameLineBlue",
        "outputFrameLineGreen",
        "outputFrameLineOffset",
        "outputFrameLineRed",
        "outputFrameLineSize",
        "outputFrameOrientationIndex",
        "outputFramePositionH",
        "outputFramePositionV",
        "outputFrameSize",
        "outputProfile",
        "outputRenderingIntent",
        "primariesAchromaticHueDegrees",
        "primariesAchromaticPurity",
        "primariesBlueHueDegrees",
        "primariesBluePurity",
        "primariesGreenHueDegrees",
        "primariesGreenPurity",
        "primariesRedHueDegrees",
        "primariesRedPurity",
        "profileGammaDynamicRange",
        "profileGammaEnabled",
        "profileGammaGamma",
        "profileGammaGreyPoint",
        "profileGammaLinear",
        "profileGammaModeIndex",
        "profileGammaSecurityFactor",
        "profileGammaShadowsRange",
        "proofIntent",
        "proofMode",
        "proofProfile",
        "rawCaAvoidShift",
        "rawCaIterations",
        "rawHighlights",
        "rawHighlightsClip",
        "rawHighlightsMode",
        "renderingIntent",
        "rgbLevelsMode",
        "rgbLevelsPreserve",
        "saturation",
        "shadows",
        "sharpen",
        "sharpenRadius",
        "sharpenThreshold",
        "sigmoidContrast",
        "sigmoidHuePreservation",
        "sigmoidSkew",
        "soften",
        "splitAmount",
        "splitBalance",
        "splitCompress",
        "splitHighlightSaturation",
        "splitHighlightsHue",
        "splitMix",
        "splitShadowSaturation",
        "splitShadowsHue",
        "splitToningEnabled",
        "straighten",
        "toneEqBlacks",
        "toneEqHighlights",
        "toneEqMidtones",
        "toneEqShadows",
        "toneEqWhites",
        "velvia",
        "vibrance",
        "vignette",
        "watermarkAlignmentIndex",
        "watermarkBlue",
        "watermarkEnabled",
        "watermarkGreen",
        "watermarkOffsetX",
        "watermarkOffsetY",
        "watermarkOpacity",
        "watermarkRed",
        "watermarkRotation",
        "watermarkScale",
        "whiteBalanceBlue",
        "whiteBalanceFourth",
        "whiteBalanceGreen",
        "whiteBalanceMode",
        "whiteBalanceRed",
        "whites",
        "workingProfile",
    };
    std::vector<std::string> names;
    names.reserve(std::size(kQuoted) + 80U);
    for (const auto name : kQuoted)
    {
        names.emplace_back(name);
    }
    for (const auto &field : rgb_levels_stop_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_balance_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_correction_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_contrast_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &[field, index] : color_harmonizer_custom_hue_fields())
    {
        static_cast<void>(index);
        names.emplace_back(field);
    }
    for (const auto &[field, index] : color_harmonizer_node_saturation_fields())
    {
        static_cast<void>(index);
        names.emplace_back(field);
    }
    for (int band = 0; band <= 7; ++band)
    {
        names.push_back("colorEqHue" + std::to_string(band));
        names.push_back("colorEqSat" + std::to_string(band));
        names.push_back("colorEqLight" + std::to_string(band));
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

} // namespace

bool apply_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
    if (is_develop_mask_field(name))
    {
        return static_cast<bool>(apply_develop_mask_field_strict(params, name, value));
    }
    if (!assign_develop_field(params, name, value))
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

Result<void> apply_develop_field_strict(DevelopParams &params, const std::string_view name,
                                        const double value)
{
    if (is_develop_mask_field(name))
    {
        return apply_develop_mask_field_strict(params, name, value);
    }
    DevelopParams candidate = params;
    if (!assign_develop_field(candidate, name, value))
    {
        return make_error(ErrorCode::kInvalidArgument, "Develop field or value is unsupported",
                          {{"name", std::string(name)}, {"value", std::to_string(value)}});
    }
    DevelopParams clamped = candidate;
    clamp_develop(clamped);
    if (clamped != candidate)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Develop field value is outside the supported range",
                          {{"name", std::string(name)}, {"value", std::to_string(value)}});
    }
    params = std::move(candidate);
    return {};
}

Result<void> apply_develop_text_field_strict(DevelopParams &params, const std::string_view name,
                                             const std::string_view value)
{
    if (name != "watermarkText")
        return make_error(ErrorCode::kInvalidArgument, "Develop text field is unsupported",
                          {{"name", std::string(name)}});
    DevelopParams candidate = params;
    candidate.watermark_present = true;
    candidate.watermark_enabled = true;
    candidate.effects_effect_enabled = true;
    candidate.watermark.text = std::string(value);
    auto valid = watermark_to_parameters(candidate.watermark);
    if (!valid)
        return valid.error();
    params = std::move(candidate);
    return {};
}

std::string_view develop_set_field_kind_name(const DevelopSetFieldKind kind) noexcept
{
    switch (kind)
    {
    case DevelopSetFieldKind::Number:
        return "number";
    case DevelopSetFieldKind::Integer:
        return "integer";
    case DevelopSetFieldKind::Toggle:
        return "toggle";
    case DevelopSetFieldKind::Text:
        return "text";
    }
    return "number";
}

std::vector<std::string_view> develop_set_field_prefixes() noexcept
{
    return {kColorHarmonizerMaskFieldPrefix, kGraduatedMaskFieldPrefix};
}

std::vector<DevelopSetField> list_develop_set_fields()
{
    std::vector<DevelopSetField> fields;
    for (const auto &name : candidate_develop_set_names())
    {
        const auto seed = first_accepted_develop_set_value(name);
        if (!seed)
        {
            continue;
        }
        const bool half = develop_set_field_accepts(name, *seed + 0.5) ||
                          develop_set_field_accepts(name, *seed - 0.5);
        const bool integer = !half;
        const bool toggle = develop_set_field_accepts(name, 0.0) &&
                            develop_set_field_accepts(name, 1.0) &&
                            !develop_set_field_accepts(name, 0.5) &&
                            !develop_set_field_accepts(name, 2.0) &&
                            !develop_set_field_accepts(name, -1.0);
        DevelopSetField field;
        field.name = name;
        field.kind = toggle  ? DevelopSetFieldKind::Toggle :
                     integer ? DevelopSetFieldKind::Integer :
                               DevelopSetFieldKind::Number;
        field.minimum = develop_set_field_extreme(name, *seed, -1.0, integer || toggle);
        field.maximum = develop_set_field_extreme(name, *seed, 1.0, integer || toggle);
        if (toggle)
        {
            field.minimum = 0.0;
            field.maximum = 1.0;
        }
        fields.push_back(std::move(field));
    }
    DevelopSetField text;
    text.name = "watermarkText";
    text.kind = DevelopSetFieldKind::Text;
    fields.push_back(std::move(text));
    std::sort(fields.begin(), fields.end(),
              [](const DevelopSetField &left, const DevelopSetField &right)
              { return left.name < right.name; });
    return fields;
}

bool reset_develop_field(DevelopParams &params, const std::string_view name)
{
    if (is_develop_mask_field(name))
    {
        return static_cast<bool>(reset_develop_mask_field(params, name));
    }
    DevelopParams identity;
    if (reset_temperature_field(params.temperature, name))
    {
    }
    else if (name == "profileGammaEnabled")
    {
        params.profile_gamma_enabled = identity.profile_gamma_enabled;
    }
    else if (name == "profileGammaModeIndex")
    {
        params.profile_gamma.mode = identity.profile_gamma.mode;
    }
    else if (name == "profileGammaLinear")
    {
        params.profile_gamma.linear = identity.profile_gamma.linear;
    }
    else if (name == "profileGammaGamma")
    {
        params.profile_gamma.gamma = identity.profile_gamma.gamma;
    }
    else if (name == "profileGammaDynamicRange")
    {
        params.profile_gamma.dynamic_range = identity.profile_gamma.dynamic_range;
    }
    else if (name == "profileGammaGreyPoint")
    {
        params.profile_gamma.grey_point = identity.profile_gamma.grey_point;
    }
    else if (name == "profileGammaShadowsRange")
    {
        params.profile_gamma.shadows_range = identity.profile_gamma.shadows_range;
    }
    else if (name == "profileGammaSecurityFactor")
    {
        params.profile_gamma.security_factor = identity.profile_gamma.security_factor;
    }
    else if (name == "inputProfile" || name == "workingProfile" || name == "renderingIntent" ||
             name == "gamutNormalize" || name == "blueMapping")
    {
        params.input_color = identity.input_color;
    }
    else if (name == "outputProfile" || name == "outputRenderingIntent" || name == "proofMode" ||
             name == "proofProfile" || name == "proofIntent" ||
             name == "outputBlackPointCompensation")
    {
        params.output_color = identity.output_color;
    }
    else if (name == "primariesAchromaticHueDegrees")
    {
        params.primaries.achromatic_tint_hue = identity.primaries.achromatic_tint_hue;
    }
    else if (name == "primariesAchromaticPurity")
    {
        params.primaries.achromatic_tint_purity = identity.primaries.achromatic_tint_purity;
    }
    else if (name == "primariesRedHueDegrees")
    {
        params.primaries.red_hue = identity.primaries.red_hue;
    }
    else if (name == "primariesRedPurity")
    {
        params.primaries.red_purity = identity.primaries.red_purity;
    }
    else if (name == "primariesGreenHueDegrees")
    {
        params.primaries.green_hue = identity.primaries.green_hue;
    }
    else if (name == "primariesGreenPurity")
    {
        params.primaries.green_purity = identity.primaries.green_purity;
    }
    else if (name == "primariesBlueHueDegrees")
    {
        params.primaries.blue_hue = identity.primaries.blue_hue;
    }
    else if (name == "primariesBluePurity")
    {
        params.primaries.blue_purity = identity.primaries.blue_purity;
    }
    else if (name == "channelMixerRR" || name == "channelMixerRG" || name == "channelMixerRB" ||
             name == "channelMixerGR" || name == "channelMixerGG" || name == "channelMixerGB" ||
             name == "channelMixerBR" || name == "channelMixerBG" || name == "channelMixerBB")
    {
        params.channel_mixer = identity.channel_mixer;
    }
    else if (name == "exposureMode")
    {
        params.exposure_mode = identity.exposure_mode;
    }
    else if (name == "exposureBlack")
    {
        params.exposure_black = identity.exposure_black;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = identity.exposure_ev;
    }
    else if (name == "exposureDeflickerPercentile")
    {
        params.exposure_deflicker_percentile = identity.exposure_deflicker_percentile;
    }
    else if (name == "exposureDeflickerTarget")
    {
        params.exposure_deflicker_target_ev = identity.exposure_deflicker_target_ev;
    }
    else if (name == "exposureCompensateBias")
    {
        params.exposure_compensate_exposure_bias = identity.exposure_compensate_exposure_bias;
    }
    else if (name == "exposureCompensateHighlight")
    {
        params.exposure_compensate_highlight_preservation =
            identity.exposure_compensate_highlight_preservation;
    }
    else if (name == "contrast")
    {
        params.contrast = identity.contrast;
    }
    else if (name == "highlights")
    {
        params.highlights = identity.highlights;
    }
    else if (name == "shadows")
    {
        params.shadows = identity.shadows;
    }
    else if (name == "whites")
    {
        params.whites = identity.whites;
    }
    else if (name == "blacks")
    {
        params.blacks = identity.blacks;
    }
    else if (name == "vibrance")
    {
        params.vibrance = identity.vibrance;
    }
    else if (name == "saturation")
    {
        params.saturation = identity.saturation;
    }
    else if (name == "rotate")
    {
        params.rotate_quarters = 0;
    }
    else if (name == "flip")
    {
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
    }
    else if (name == "straighten")
    {
        params.straighten_degrees = identity.straighten_degrees;
    }
    else if (name == "crop" || name == "cropX" || name == "cropY" || name == "cropWidth" ||
             name == "cropHeight")
    {
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
        params.canvas_present = identity.canvas_present;
        params.canvas_enabled = identity.canvas_enabled;
        params.canvas = identity.canvas;
    }
    else if (name == "canvas")
    {
        params.canvas_present = identity.canvas_present;
        params.canvas_enabled = identity.canvas_enabled;
        params.canvas = identity.canvas;
    }
    else if (name == "sharpen" || name == "sharpenRadius" || name == "sharpenThreshold")
    {
        params.sharpen = identity.sharpen;
        if (name == "sharpenRadius")
        {
            params.sharpen_radius = identity.sharpen_radius;
        }
        else if (name == "sharpenThreshold")
        {
            params.sharpen_threshold = identity.sharpen_threshold;
        }
    }
    else if (name == "retouch")
    {
        params.retouch = identity.retouch;
    }
    else if (name == "clarity")
    {
        params.clarity = identity.clarity;
    }
    else if (name == "vignette")
    {
        params.vignette = identity.vignette;
    }
    else if (name == "grain")
    {
        params.grain = identity.grain;
    }
    else if (name == "bloom")
    {
        params.bloom = identity.bloom;
    }
    else if (name == "soften")
    {
        params.soften = identity.soften;
    }
    else if (name == "dehaze")
    {
        params.dehaze = identity.dehaze;
    }
    else if (name == "dehazeDistance")
    {
        params.dehaze_distance = identity.dehaze_distance;
    }
    else if (name == "dehazeAdaptive")
    {
        params.dehaze_adaptive = identity.dehaze_adaptive;
    }
    else if (name == "outputDither")
    {
        params.output_dither_present = identity.output_dither_present;
        params.output_dither_enabled = identity.output_dither_enabled;
        params.output_dither = identity.output_dither;
        params.frame_present = identity.frame_present;
        params.frame_enabled = identity.frame_enabled;
        params.frame = identity.frame;
        params.watermark_present = identity.watermark_present;
        params.watermark_enabled = identity.watermark_enabled;
        params.watermark = identity.watermark;
    }
    else if (name == "outputDitherMethodIndex")
    {
        params.output_dither.method = identity.output_dither.method;
    }
    else if (name == "outputDitherDamping")
    {
        params.output_dither.random_damping_db = identity.output_dither.random_damping_db;
    }
    else if (name == "outputFrame")
    {
        params.frame_present = identity.frame_present;
        params.frame_enabled = identity.frame_enabled;
        params.frame = identity.frame;
    }
    else if (name == "watermark")
    {
        params.watermark_present = identity.watermark_present;
        params.watermark_enabled = identity.watermark_enabled;
        params.watermark = identity.watermark;
    }
    else if (name == "velvia")
    {
        params.velvia = identity.velvia;
    }
    else if (reset_legacy_color_balance_field(params.color_balance, name))
    {
        if (name == "legacyColorBalance")
        {
            params.color_balance_enabled = false;
        }
    }
    else if (reset_color_checker_field(params, name))
    {
    }
    else if (reset_color_balance_field(params.color_balance_rgb, name))
    {
    }
    else if (reset_color_correction_field(params, name))
    {
    }
    else if (reset_color_contrast_field(params, name))
    {
    }
    else if (reset_color_reconstruction_field(params, name))
    {
    }
    else if (reset_color_harmonizer_field(params, name))
    {
    }
    else if (name == "monochrome")
    {
        params.monochrome_present = identity.monochrome_present;
        params.monochrome_enabled = identity.monochrome_enabled;
        params.monochrome = identity.monochrome;
        params.monochrome_mask_id = identity.monochrome_mask_id;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_toning.shadow_hue = identity.split_toning.shadow_hue;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_toning.highlight_hue = identity.split_toning.highlight_hue;
    }
    else if (name == "splitBalance")
    {
        params.split_toning.balance = identity.split_toning.balance;
    }
    else if (name == "splitAmount")
    {
        params.split_toning_present = identity.split_toning_present;
        params.split_toning_enabled = identity.split_toning_enabled;
        params.split_toning = identity.split_toning;
        params.split_toning_mask_id = identity.split_toning_mask_id;
    }
    else if (name == "splitToning")
    {
        params.split_toning_present = identity.split_toning_present;
        params.split_toning_enabled = identity.split_toning_enabled;
        params.split_toning = identity.split_toning;
        params.split_toning_mask_id = identity.split_toning_mask_id;
    }
    else if (name == "gamma")
    {
        params.gamma = identity.gamma;
    }
    else if (reset_rgb_levels_field(params, name))
    {
    }
    else if (name == "toneCurve")
    {
        params.tone_curve.clear();
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceSrgb);
    }
    else if (name == "sigmoidContrast")
    {
        params.sigmoid_contrast = identity.sigmoid_contrast;
    }
    else if (name == "sigmoidSkew")
    {
        params.sigmoid_skew = identity.sigmoid_skew;
    }
    else if (name == "sigmoidHuePreservation")
    {
        params.sigmoid_hue_preservation = identity.sigmoid_hue_preservation;
    }
    else if (name == "rawHighlights" || name == "rawHighlightsClip" || name == "rawHighlightsMode")
    {
        params.raw_highlights = identity.raw_highlights;
        params.raw_highlights_clip = identity.raw_highlights_clip;
        params.raw_highlights_mode = identity.raw_highlights_mode;
    }
    else if (name == "hotPixelsStrength" || name == "hotPixelsThreshold" ||
             name == "hotPixelsPermissive")
    {
        params.hot_pixels_strength = identity.hot_pixels_strength;
        params.hot_pixels_threshold = identity.hot_pixels_threshold;
        params.hot_pixels_permissive = identity.hot_pixels_permissive;
    }
    else if (name == "rawCaIterations" || name == "rawCaAvoidShift")
    {
        params.raw_ca_iterations = identity.raw_ca_iterations;
        params.raw_ca_avoid_shift = identity.raw_ca_avoid_shift;
    }
    else if (name == "denoise" || name == "denoiseChroma" || name == "denoiseRadius")
    {
        params.denoise = identity.denoise;
        if (name != "denoise")
        {
            params.denoise_chroma = identity.denoise_chroma;
            params.denoise_radius = identity.denoise_radius;
        }
    }
    else if (name == "lensK1" || name == "lensK2" || name == "lensTcaR" || name == "lensTcaB" ||
             name == "lensVignetting" || name == "lensMode" || name == "lensFocal")
    {
        params.lens_k1 = identity.lens_k1;
        params.lens_k2 = identity.lens_k2;
        params.lens_tca_r = identity.lens_tca_r;
        params.lens_tca_b = identity.lens_tca_b;
        params.lens_vignetting = identity.lens_vignetting;
        params.lens_mode = identity.lens_mode;
        params.lens_focal_mm = identity.lens_focal_mm;
    }
    else if (name == "colorZones")
    {
        params.color_zones_present = identity.color_zones_present;
        params.color_zones_enabled = identity.color_zones_enabled;
        params.color_zones = identity.color_zones;
        params.color_zones_mask_id = identity.color_zones_mask_id;
        params.color_zones_band = identity.color_zones_band;
    }
    else if (name == "colorEqHue" || name == "colorEqSat" || name == "colorEqLight" ||
             name == "colorEqBand")
    {
        const auto band = static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}));
        if (name == "colorEqHue")
        {
            params.color_eq_hue[band] = 0.0;
        }
        else if (name == "colorEqSat")
        {
            params.color_eq_sat[band] = 0.0;
        }
        else if (name == "colorEqLight")
        {
            params.color_eq_light[band] = 0.0;
        }
        else
        {
            params.color_eq_band = 0;
        }
    }
    else if (name == "graduatedDensity" || name == "graduatedHardness" ||
             name == "graduatedRotation" || name == "graduatedOffset")
    {
        params.graduated_density = identity.graduated_density;
        params.graduated_hardness = identity.graduated_hardness;
        params.graduated_rotation = identity.graduated_rotation;
        params.graduated_offset = identity.graduated_offset;
    }
    else if (name == "toneEqBlacks")
    {
        params.tone_eq_blacks = identity.tone_eq_blacks;
    }
    else if (name == "toneEqShadows")
    {
        params.tone_eq_shadows = identity.tone_eq_shadows;
    }
    else if (name == "toneEqMidtones")
    {
        params.tone_eq_midtones = identity.tone_eq_midtones;
    }
    else if (name == "toneEqHighlights")
    {
        params.tone_eq_highlights = identity.tone_eq_highlights;
    }
    else if (name == "toneEqWhites")
    {
        params.tone_eq_whites = identity.tone_eq_whites;
    }
    else
    {
        std::size_t band = 0;
        if (parse_band_field(name, "colorEqHue", band))
        {
            params.color_eq_hue[band] = 0.0;
        }
        else if (parse_band_field(name, "colorEqSat", band))
        {
            params.color_eq_sat[band] = 0.0;
        }
        else if (parse_band_field(name, "colorEqLight", band))
        {
            params.color_eq_light[band] = 0.0;
        }
        else
        {
            return false;
        }
    }
    clamp_develop(params);
    return true;
}

bool reset_develop_section(DevelopParams &params, const std::string_view section)
{
    DevelopParams identity;
    if (section == "geometry")
    {
        params.rotate_quarters = 0;
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
        params.straighten_degrees = 0.0;
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
        params.lens_k1 = identity.lens_k1;
        params.lens_k2 = identity.lens_k2;
        params.lens_tca_r = identity.lens_tca_r;
        params.lens_tca_b = identity.lens_tca_b;
        params.lens_vignetting = identity.lens_vignetting;
        params.lens_mode = identity.lens_mode;
        params.lens_focal_mm = identity.lens_focal_mm;
    }
    else if (section == "whiteBalance")
    {
        params.temperature = identity.temperature;
    }
    else if (section == "profileGamma")
    {
        params.profile_gamma_enabled = identity.profile_gamma_enabled;
        params.profile_gamma = identity.profile_gamma;
    }
    else if (section == "inputProfile")
    {
        params.input_color = identity.input_color;
    }
    else if (section == "outputProfile")
    {
        params.output_color = identity.output_color;
    }
    else if (section == "calibration")
    {
        params.channel_mixer = identity.channel_mixer;
    }
    else if (section == "primaries")
    {
        params.primaries = identity.primaries;
    }
    else if (section == "light")
    {
        params.exposure_mode = identity.exposure_mode;
        params.exposure_black = identity.exposure_black;
        params.exposure_ev = identity.exposure_ev;
        params.exposure_deflicker_percentile = identity.exposure_deflicker_percentile;
        params.exposure_deflicker_target_ev = identity.exposure_deflicker_target_ev;
        params.exposure_compensate_exposure_bias = identity.exposure_compensate_exposure_bias;
        params.exposure_compensate_highlight_preservation =
            identity.exposure_compensate_highlight_preservation;
        params.contrast = identity.contrast;
        params.highlights = identity.highlights;
        params.shadows = identity.shadows;
        params.whites = identity.whites;
        params.blacks = identity.blacks;
        params.gamma = identity.gamma;
        params.rgb_levels = identity.rgb_levels;
        params.rgb_curve = identity.rgb_curve;
        params.tone_curve.clear();
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceSrgb);
        params.sigmoid_contrast = identity.sigmoid_contrast;
        params.sigmoid_skew = identity.sigmoid_skew;
        params.sigmoid_display_white = identity.sigmoid_display_white;
        params.sigmoid_display_black = identity.sigmoid_display_black;
        params.sigmoid_hue_preservation = identity.sigmoid_hue_preservation;
        params.tone_eq_blacks = identity.tone_eq_blacks;
        params.tone_eq_shadows = identity.tone_eq_shadows;
        params.tone_eq_midtones = identity.tone_eq_midtones;
        params.tone_eq_highlights = identity.tone_eq_highlights;
        params.tone_eq_whites = identity.tone_eq_whites;
    }
    else if (section == "color")
    {
        params.vibrance = identity.vibrance;
        params.saturation = identity.saturation;
        params.velvia = identity.velvia;
        params.color_balance_enabled = identity.color_balance_enabled;
        params.color_balance = identity.color_balance;
        params.color_checker_enabled = identity.color_checker_enabled;
        params.color_checker = identity.color_checker;
        params.color_checker_patch = identity.color_checker_patch;
        params.color_balance_rgb = identity.color_balance_rgb;
        params.color_correction_enabled = identity.color_correction_enabled;
        params.color_correction = identity.color_correction;
        params.color_contrast_enabled = identity.color_contrast_enabled;
        params.color_contrast = identity.color_contrast;
        params.color_reconstruction_enabled = identity.color_reconstruction_enabled;
        params.color_reconstruction = identity.color_reconstruction;
        params.color_harmonizer_enabled = identity.color_harmonizer_enabled;
        params.color_harmonizer = identity.color_harmonizer;
        params.monochrome_present = identity.monochrome_present;
        params.monochrome_enabled = identity.monochrome_enabled;
        params.monochrome = identity.monochrome;
        params.monochrome_mask_id = identity.monochrome_mask_id;
        params.split_toning_present = identity.split_toning_present;
        params.split_toning_enabled = identity.split_toning_enabled;
        params.split_toning = identity.split_toning;
        params.split_toning_mask_id = identity.split_toning_mask_id;
        params.color_zones_present = identity.color_zones_present;
        params.color_zones_enabled = identity.color_zones_enabled;
        params.color_zones = identity.color_zones;
        params.color_zones_mask_id = identity.color_zones_mask_id;
        params.color_zones_band = identity.color_zones_band;
        params.color_eq_hue = {};
        params.color_eq_sat = {};
        params.color_eq_light = {};
        params.color_eq_band = 0;
    }
    else if (section == "colorHarmonizer")
    {
        params.color_harmonizer_enabled = identity.color_harmonizer_enabled;
        params.color_harmonizer = identity.color_harmonizer;
    }
    else if (section == "detail")
    {
        params.sharpen = identity.sharpen;
        params.sharpen_radius = identity.sharpen_radius;
        params.sharpen_threshold = identity.sharpen_threshold;
        params.retouch = identity.retouch;
        params.clarity = identity.clarity;
        params.grain = identity.grain;
        params.denoise = identity.denoise;
        params.denoise_chroma = identity.denoise_chroma;
        params.denoise_radius = identity.denoise_radius;
    }
    else if (section == "effects")
    {
        params.vignette = identity.vignette;
        params.bloom = identity.bloom;
        params.soften = identity.soften;
        params.dehaze = identity.dehaze;
        params.dehaze_distance = identity.dehaze_distance;
        params.dehaze_adaptive = identity.dehaze_adaptive;
        params.output_dither_present = identity.output_dither_present;
        params.output_dither_enabled = identity.output_dither_enabled;
        params.output_dither = identity.output_dither;
        params.graduated_density = identity.graduated_density;
        params.graduated_hardness = identity.graduated_hardness;
        params.graduated_rotation = identity.graduated_rotation;
        params.graduated_offset = identity.graduated_offset;
    }
    else if (section == "raw")
    {
        params.raw_highlights = identity.raw_highlights;
        params.raw_highlights_clip = identity.raw_highlights_clip;
        params.raw_highlights_mode = identity.raw_highlights_mode;
        params.hot_pixels_strength = identity.hot_pixels_strength;
        params.hot_pixels_threshold = identity.hot_pixels_threshold;
        params.hot_pixels_permissive = identity.hot_pixels_permissive;
        params.raw_ca_iterations = identity.raw_ca_iterations;
        params.raw_ca_avoid_shift = identity.raw_ca_avoid_shift;
        params.raw_denoise_threshold = identity.raw_denoise_threshold;
        params.raw_denoise_bands = identity.raw_denoise_bands;
        params.denoise = identity.denoise;
        params.denoise_chroma = identity.denoise_chroma;
        params.denoise_radius = identity.denoise_radius;
        params.lens_k1 = identity.lens_k1;
        params.lens_vignetting = identity.lens_vignetting;
    }
    else if (section == "toneEqual")
    {
        params.tone_eq_blacks = identity.tone_eq_blacks;
        params.tone_eq_shadows = identity.tone_eq_shadows;
        params.tone_eq_midtones = identity.tone_eq_midtones;
        params.tone_eq_highlights = identity.tone_eq_highlights;
        params.tone_eq_whites = identity.tone_eq_whites;
    }
    else if (section == "graduated")
    {
        params.graduated_density = identity.graduated_density;
        params.graduated_hardness = identity.graduated_hardness;
        params.graduated_rotation = identity.graduated_rotation;
        params.graduated_offset = identity.graduated_offset;
        params.color_eq_hue = {};
        params.color_eq_sat = {};
        params.color_eq_light = {};
        params.color_eq_band = 0;
    }
    else
    {
        return false;
    }
    if (section != "profileGamma")
    {
        static_cast<void>(set_develop_section_effect_enabled(params, section, true));
    }
    clamp_develop(params);
    return true;
}

bool develop_section_modified(const DevelopParams &params, const std::string_view section)
{
    const DevelopParams identity;
    if (section == "geometry")
    {
        return params.rotate_quarters % 4 != 0 || params.flip_horizontal != 0 ||
               params.flip_vertical != 0 || !near(params.straighten_degrees, 0.0) ||
               !near(params.crop_x, 0.0) || !near(params.crop_y, 0.0) ||
               !near(params.crop_width, 1.0) || !near(params.crop_height, 1.0) ||
               params.canvas_present || params.canvas_enabled;
    }
    if (section == "whiteBalance")
    {
        return !params.temperature.is_identity();
    }
    if (section == "profileGamma")
    {
        return params.profile_gamma_enabled || !params.profile_gamma.is_default();
    }
    if (section == "inputProfile")
    {
        return !params.input_color.is_identity();
    }
    if (section == "outputProfile")
    {
        return !params.output_color.is_identity();
    }
    if (section == "calibration")
    {
        return !params.channel_mixer.is_identity();
    }
    if (section == "primaries")
    {
        return !params.primaries.is_identity();
    }
    if (section == "light")
    {
        const ExposureParams exposure{params.exposure_mode,
                                      params.exposure_black,
                                      params.exposure_ev,
                                      params.exposure_deflicker_percentile,
                                      params.exposure_deflicker_target_ev,
                                      params.exposure_compensate_exposure_bias,
                                      params.exposure_compensate_highlight_preservation};
        return !exposure.is_identity() || !near(params.contrast, 0.0) ||
               !near(params.highlights, 0.0) || !near(params.shadows, 0.0) ||
               !near(params.whites, 0.0) || !near(params.blacks, 0.0) ||
               !near(params.gamma, kDevelopGammaDefault) || !params.rgb_levels.is_identity() ||
               !params.rgb_curve.is_identity() || !tone_curve_is_identity(params.tone_curve) ||
               params.sigmoid_enabled ||
               !near(params.sigmoid_contrast, identity.sigmoid_contrast) ||
               !near(params.sigmoid_skew, identity.sigmoid_skew) ||
               !near(params.sigmoid_display_white, identity.sigmoid_display_white) ||
               !near(params.sigmoid_display_black, identity.sigmoid_display_black) ||
               !near(params.sigmoid_hue_preservation, identity.sigmoid_hue_preservation);
    }
    if (section == "color")
    {
        return !near(params.vibrance, 0.0) || !near(params.saturation, 0.0) ||
               !near(params.velvia, 0.0) || params.color_balance_enabled ||
               !params.color_balance.is_identity() || params.color_checker_enabled ||
               !params.color_balance_rgb.is_identity() || params.color_correction_enabled ||
               params.color_contrast_enabled || params.color_reconstruction_enabled ||
               params.color_zones_present || params.color_zones_enabled ||
               params.color_zones_mask_id.has_value() || params.color_harmonizer_enabled ||
               params.color_harmonizer_present || params.monochrome_present ||
               params.monochrome_enabled || params.monochrome_mask_id.has_value() ||
               params.split_toning_present || params.split_toning_enabled ||
               params.split_toning_mask_id.has_value();
    }
    if (section == "colorHarmonizer")
    {
        return params.color_harmonizer_enabled || params.color_harmonizer_present;
    }
    if (section == "detail")
    {
        return !near(params.sharpen, 0.0) ||
               !near(params.sharpen_radius, identity.sharpen_radius) ||
               !near(params.sharpen_threshold, identity.sharpen_threshold) ||
               !params.retouch.is_identity() || !near(params.clarity, 0.0) ||
               !near(params.grain, 0.0);
    }
    if (section == "effects")
    {
        return !near(params.vignette, 0.0) || !near(params.bloom, 0.0) ||
               !near(params.soften, 0.0) || !near(params.dehaze, 0.0) ||
               !near(params.dehaze_distance, identity.dehaze_distance) ||
               params.dehaze_adaptive != identity.dehaze_adaptive || params.output_dither_present ||
               params.output_dither_enabled || params.frame_present || params.frame_enabled ||
               params.watermark_present || params.watermark_enabled;
    }
    if (section == "raw")
    {
        return !near(params.raw_highlights, 0.0) || !near(params.hot_pixels_strength, 0.0) ||
               params.raw_ca_iterations > 0 || !near(params.raw_denoise_threshold, 0.0) ||
               !near(params.denoise, 0.0) || !near(params.lens_k1, 0.0) ||
               !near(params.lens_vignetting, 0.0);
    }
    if (section == "toneEqual")
    {
        return !near(params.tone_eq_blacks, 0.0) || !near(params.tone_eq_shadows, 0.0) ||
               !near(params.tone_eq_midtones, 0.0) || !near(params.tone_eq_highlights, 0.0) ||
               !near(params.tone_eq_whites, 0.0);
    }
    if (section == "graduated")
    {
        return params.graduated_present || params.graduated_enabled ||
               !near(params.graduated_density, 0.0) || !bands_near_zero(params.color_eq_hue) ||
               !bands_near_zero(params.color_eq_sat) || !bands_near_zero(params.color_eq_light);
    }
    return false;
}

bool develop_section_effect_enabled(const DevelopParams &params, const std::string_view section)
{
    if (section == "geometry")
    {
        return params.geometry_effect_enabled;
    }
    if (section == "whiteBalance")
    {
        return params.white_balance_effect_enabled;
    }
    if (section == "profileGamma")
    {
        return params.profile_gamma_enabled;
    }
    if (section == "inputProfile")
    {
        return params.input_profile_effect_enabled;
    }
    if (section == "outputProfile")
    {
        return params.output_profile_effect_enabled;
    }
    if (section == "calibration")
    {
        return params.calibration_effect_enabled;
    }
    if (section == "primaries")
    {
        return params.primaries_effect_enabled;
    }
    if (section == "light")
    {
        return params.light_effect_enabled;
    }
    if (section == "color" || section == "colorHarmonizer")
    {
        return params.color_effect_enabled;
    }
    if (section == "detail")
    {
        return params.detail_effect_enabled;
    }
    if (section == "effects")
    {
        return params.effects_effect_enabled;
    }
    if (section == "raw")
    {
        return params.raw_effect_enabled;
    }
    if (section == "toneEqual")
    {
        return params.tone_equal_effect_enabled;
    }
    if (section == "graduated")
    {
        return params.graduated_effect_enabled;
    }
    return false;
}

bool set_develop_section_effect_enabled(DevelopParams &params, const std::string_view section,
                                        const bool enabled)
{
    if (section == "geometry")
    {
        params.geometry_effect_enabled = enabled;
    }
    else if (section == "whiteBalance")
    {
        params.white_balance_effect_enabled = enabled;
    }
    else if (section == "profileGamma")
    {
        params.profile_gamma_enabled = enabled;
    }
    else if (section == "inputProfile")
    {
        params.input_profile_effect_enabled = enabled;
    }
    else if (section == "outputProfile")
    {
        params.output_profile_effect_enabled = enabled;
    }
    else if (section == "calibration")
    {
        params.calibration_effect_enabled = enabled;
    }
    else if (section == "primaries")
    {
        params.primaries_effect_enabled = enabled;
    }
    else if (section == "light")
    {
        params.light_effect_enabled = enabled;
    }
    else if (section == "color" || section == "colorHarmonizer")
    {
        params.color_effect_enabled = enabled;
    }
    else if (section == "detail")
    {
        params.detail_effect_enabled = enabled;
    }
    else if (section == "effects")
    {
        params.effects_effect_enabled = enabled;
    }
    else if (section == "raw")
    {
        params.raw_effect_enabled = enabled;
    }
    else if (section == "toneEqual")
    {
        params.tone_equal_effect_enabled = enabled;
    }
    else if (section == "graduated")
    {
        params.graduated_effect_enabled = enabled;
    }
    else
    {
        return false;
    }
    return true;
}

namespace
{

std::string format_signed_amount(const double value)
{
    if (!std::isfinite(value))
    {
        return {};
    }
    const double rounded = std::round(value * 10.0) / 10.0;
    if (std::abs(rounded - std::round(rounded)) < 1e-6)
    {
        const int whole = static_cast<int>(std::round(rounded));
        return (whole > 0 ? "+" : "") + std::to_string(whole);
    }
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << std::showpos << rounded;
    return out.str();
}

void add_scaled_change(std::vector<DevelopChange> &changes, std::string field, const double before,
                       const double after, const double scale)
{
    if (near(before, after))
    {
        return;
    }
    changes.push_back({std::move(field), format_signed_amount((after - before) * scale)});
}

void add_toggle_change(std::vector<DevelopChange> &changes, std::string field, const bool before,
                       const bool after)
{
    if (before == after)
    {
        return;
    }
    changes.push_back({std::move(field), after ? std::string("on") : std::string("off")});
}

void add_named_change(std::vector<DevelopChange> &changes, std::string field, const bool changed)
{
    if (!changed)
    {
        return;
    }
    changes.push_back({std::move(field), {}});
}

} // namespace

std::vector<DevelopChange> develop_change_summary(const DevelopParams &before,
                                                  const DevelopParams &after)
{
    if (after.is_identity() && !before.is_identity())
    {
        return {{"reset", {}}};
    }
    std::vector<DevelopChange> changes;
    add_scaled_change(changes, "exposure", before.exposure_ev, after.exposure_ev, 1.0);
    add_scaled_change(changes, "black", before.exposure_black, after.exposure_black, 10.0);
    add_scaled_change(changes, "contrast", before.contrast, after.contrast, 10.0);
    add_scaled_change(changes, "highlights", before.highlights, after.highlights, 10.0);
    add_scaled_change(changes, "shadows", before.shadows, after.shadows, 10.0);
    add_scaled_change(changes, "whites", before.whites, after.whites, 10.0);
    add_scaled_change(changes, "blacks", before.blacks, after.blacks, 10.0);
    add_scaled_change(changes, "vibrance", before.vibrance, after.vibrance, 10.0);
    add_scaled_change(changes, "saturation", before.saturation, after.saturation, 10.0);
    add_scaled_change(changes, "velvia", before.velvia, after.velvia, 10.0);
    add_scaled_change(changes, "gamma", before.gamma, after.gamma, 10.0);
    add_named_change(changes, "rgbLevels", before.rgb_levels != after.rgb_levels);
    add_named_change(changes, "rgbCurve", before.rgb_curve != after.rgb_curve);
    add_scaled_change(changes, "sharpen", before.sharpen, after.sharpen, 10.0);
    add_scaled_change(changes, "sharpenRadius", before.sharpen_radius, after.sharpen_radius, 1.0);
    add_scaled_change(changes, "sharpenThreshold", before.sharpen_threshold,
                      after.sharpen_threshold, 1.0);
    add_named_change(changes, "retouch", before.retouch != after.retouch);
    add_scaled_change(changes, "clarity", before.clarity, after.clarity, 10.0);
    add_scaled_change(changes, "vignette", before.vignette, after.vignette, 10.0);
    add_scaled_change(changes, "grain", before.grain, after.grain, 10.0);
    add_scaled_change(changes, "bloom", before.bloom, after.bloom, 10.0);
    add_scaled_change(changes, "soften", before.soften, after.soften, 10.0);
    add_scaled_change(changes, "dehaze", before.dehaze, after.dehaze, 10.0);
    add_scaled_change(changes, "dehazeDistance", before.dehaze_distance, after.dehaze_distance,
                      10.0);
    add_toggle_change(changes, "dehazeAdaptive", before.dehaze_adaptive, after.dehaze_adaptive);
    add_named_change(changes, "outputDither",
                     before.output_dither_present != after.output_dither_present ||
                         before.output_dither_enabled != after.output_dither_enabled ||
                         before.output_dither != after.output_dither);
    add_named_change(changes, "outputFrame",
                     before.frame_present != after.frame_present ||
                         before.frame_enabled != after.frame_enabled ||
                         before.frame != after.frame);
    add_named_change(changes, "colorZones",
                     before.color_zones_present != after.color_zones_present ||
                         before.color_zones_enabled != after.color_zones_enabled ||
                         before.color_zones != after.color_zones ||
                         before.color_zones_mask_id != after.color_zones_mask_id);
    add_named_change(changes, "watermark",
                     before.watermark_present != after.watermark_present ||
                         before.watermark_enabled != after.watermark_enabled ||
                         before.watermark != after.watermark);
    add_named_change(changes, "monochrome",
                     before.monochrome_present != after.monochrome_present ||
                         before.monochrome_enabled != after.monochrome_enabled ||
                         before.monochrome != after.monochrome ||
                         before.monochrome_mask_id != after.monochrome_mask_id);
    add_named_change(changes, "splitToning",
                     before.split_toning_present != after.split_toning_present ||
                         before.split_toning_enabled != after.split_toning_enabled ||
                         before.split_toning != after.split_toning ||
                         before.split_toning_mask_id != after.split_toning_mask_id);
    add_scaled_change(changes, "denoise", before.denoise, after.denoise, 10.0);
    add_scaled_change(changes, "straighten", before.straighten_degrees, after.straighten_degrees,
                      1.0);
    add_scaled_change(changes, "toneEqBlacks", before.tone_eq_blacks, after.tone_eq_blacks, 1.0);
    add_scaled_change(changes, "toneEqShadows", before.tone_eq_shadows, after.tone_eq_shadows, 1.0);
    add_scaled_change(changes, "toneEqMidtones", before.tone_eq_midtones, after.tone_eq_midtones,
                      1.0);
    add_scaled_change(changes, "toneEqHighlights", before.tone_eq_highlights,
                      after.tone_eq_highlights, 1.0);
    add_scaled_change(changes, "toneEqWhites", before.tone_eq_whites, after.tone_eq_whites, 1.0);
    add_scaled_change(changes, "graduated", before.graduated_density, after.graduated_density, 1.0);
    if (before.rotate_quarters % 4 != after.rotate_quarters % 4)
    {
        const auto delta = ((after.rotate_quarters - before.rotate_quarters) % 4 + 4) % 4;
        const int degrees = delta == 3 ? -90 : static_cast<int>(delta) * 90;
        changes.push_back({"rotate", format_signed_amount(static_cast<double>(degrees))});
    }
    add_named_change(changes, "flip",
                     before.flip_horizontal != after.flip_horizontal ||
                         before.flip_vertical != after.flip_vertical);
    add_named_change(changes, "canvas",
                     before.canvas_present != after.canvas_present ||
                         before.canvas_enabled != after.canvas_enabled ||
                         before.canvas != after.canvas);
    add_named_change(changes, "crop",
                     !near(before.crop_x, after.crop_x) || !near(before.crop_y, after.crop_y) ||
                         !near(before.crop_width, after.crop_width) ||
                         !near(before.crop_height, after.crop_height));
    add_named_change(changes, "toneCurve", before.tone_curve != after.tone_curve);
    add_named_change(changes, "whiteBalance", before.temperature != after.temperature);
    add_named_change(changes, "inputProfile", before.input_color != after.input_color);
    add_named_change(changes, "outputProfile", before.output_color != after.output_color);
    add_named_change(changes, "primaries", before.primaries != after.primaries);
    add_named_change(changes, "mixer", before.channel_mixer != after.channel_mixer);
    add_named_change(changes, "colorBalance",
                     before.color_balance != after.color_balance ||
                         before.color_balance_enabled != after.color_balance_enabled);
    add_named_change(changes, "colorBalanceRgb",
                     before.color_balance_rgb != after.color_balance_rgb);
    add_named_change(changes, "colorReconstruction",
                     before.color_reconstruction != after.color_reconstruction ||
                         before.color_reconstruction_enabled != after.color_reconstruction_enabled);
    add_toggle_change(changes, "profileGamma", before.profile_gamma_enabled,
                      after.profile_gamma_enabled);
    add_toggle_change(changes, "sigmoid", before.sigmoid_enabled, after.sigmoid_enabled);
    add_toggle_change(changes, "light", before.light_effect_enabled, after.light_effect_enabled);
    add_toggle_change(changes, "color", before.color_effect_enabled, after.color_effect_enabled);
    add_toggle_change(changes, "detail", before.detail_effect_enabled, after.detail_effect_enabled);
    add_toggle_change(changes, "effects", before.effects_effect_enabled,
                      after.effects_effect_enabled);
    add_toggle_change(changes, "geometry", before.geometry_effect_enabled,
                      after.geometry_effect_enabled);
    return changes;
}

bool apply_crop_aspect(DevelopParams &params, const std::string_view aspect)
{
    if (aspect == "free")
    {
        return true;
    }
    double ratio = 0.0;
    if (aspect == "1:1")
    {
        ratio = 1.0;
    }
    else if (aspect == "3:2")
    {
        ratio = 3.0 / 2.0;
    }
    else if (aspect == "4:3")
    {
        ratio = 4.0 / 3.0;
    }
    else if (aspect == "5:4")
    {
        ratio = 5.0 / 4.0;
    }
    else if (aspect == "16:9")
    {
        ratio = 16.0 / 9.0;
    }
    else
    {
        return false;
    }

    const double box_w = params.crop_width;
    const double box_h = params.crop_height;
    const double box_x = params.crop_x;
    const double box_y = params.crop_y;
    double new_w = box_w;
    double new_h = box_h;
    if (box_w / std::max(box_h, kEpsilon) > ratio)
    {
        new_w = box_h * ratio;
        new_h = box_h;
    }
    else
    {
        new_w = box_w;
        new_h = box_w / ratio;
    }
    params.crop_width = new_w;
    params.crop_height = new_h;
    params.crop_x = box_x + (box_w - new_w) * 0.5;
    params.crop_y = box_y + (box_h - new_h) * 0.5;
    clamp_develop(params);
    return true;
}

double develop_crop_min_short_edge_pixels(const double source_width,
                                          const double source_height) noexcept
{
    const double short_edge = std::min(source_width, source_height);
    if (!(short_edge > 0.0))
    {
        return 0.0;
    }
    return std::min(kDevelopCropMinShortEdgePixels, short_edge * kDevelopCropMinShortEdgeFraction);
}

void clamp_develop_crop_min_extent(DevelopParams &params, const double source_width,
                                   const double source_height) noexcept
{
    clamp_develop(params);
    const double min_px = develop_crop_min_short_edge_pixels(source_width, source_height);
    if (!(min_px > 0.0) || !(source_width > 0.0) || !(source_height > 0.0))
    {
        return;
    }
    const double pixel_width = params.crop_width * source_width;
    const double pixel_height = params.crop_height * source_height;
    const double short_px = std::min(pixel_width, pixel_height);
    if (short_px + kEpsilon >= min_px)
    {
        return;
    }
    const double scale = min_px / std::max(short_px, kEpsilon);
    const double center_x = params.crop_x + params.crop_width * 0.5;
    const double center_y = params.crop_y + params.crop_height * 0.5;
    params.crop_width = std::min(1.0, params.crop_width * scale);
    params.crop_height = std::min(1.0, params.crop_height * scale);
    params.crop_x = std::clamp(center_x - params.crop_width * 0.5, 0.0, 1.0 - params.crop_width);
    params.crop_y = std::clamp(center_y - params.crop_height * 0.5, 0.0, 1.0 - params.crop_height);
    const double min_w = std::min(1.0, min_px / source_width);
    const double min_h = std::min(1.0, min_px / source_height);
    if (params.crop_width + kEpsilon < min_w)
    {
        const double cx = params.crop_x + params.crop_width * 0.5;
        params.crop_width = min_w;
        params.crop_x = std::clamp(cx - params.crop_width * 0.5, 0.0, 1.0 - params.crop_width);
    }
    if (params.crop_height + kEpsilon < min_h)
    {
        const double cy = params.crop_y + params.crop_height * 0.5;
        params.crop_height = min_h;
        params.crop_y = std::clamp(cy - params.crop_height * 0.5, 0.0, 1.0 - params.crop_height);
    }
    clamp_develop(params);
}

void transform_crop_for_quarter_turns(DevelopParams &params, int turns_cw) noexcept
{
    turns_cw = ((turns_cw % 4) + 4) % 4;
    for (int turn = 0; turn < turns_cw; ++turn)
    {
        const double x = params.crop_x;
        const double y = params.crop_y;
        const double width = params.crop_width;
        const double height = params.crop_height;
        params.crop_x = 1.0 - y - height;
        params.crop_y = x;
        params.crop_width = height;
        params.crop_height = width;
    }
    clamp_develop(params);
}

void transform_crop_for_flip(DevelopParams &params, const bool horizontal,
                             const bool vertical) noexcept
{
    if (horizontal)
    {
        params.crop_x = 1.0 - params.crop_x - params.crop_width;
    }
    if (vertical)
    {
        params.crop_y = 1.0 - params.crop_y - params.crop_height;
    }
    clamp_develop(params);
}

double working_image_aspect(const std::int64_t rotate_quarters, const double source_aspect) noexcept
{
    const double aspect = source_aspect > kEpsilon ? source_aspect : 1.5;
    if (((rotate_quarters % 4) + 4) % 4 % 2 != 0)
    {
        return 1.0 / aspect;
    }
    return aspect;
}

namespace
{

[[nodiscard]] double max_inscribed_normalized_height(const double degrees,
                                                     const double image_aspect,
                                                     const double crop_aspect) noexcept
{
    const double ratio = std::max(crop_aspect, kEpsilon);
    const double aspect = std::max(image_aspect, kEpsilon);
    double height = std::min(1.0, 1.0 / ratio);
    if (std::abs(degrees) < 1e-4)
    {
        return height;
    }
    const double rad = -degrees * std::numbers::pi / 180.0;
    const double inv_c = std::cos(rad);
    const double inv_s = std::sin(rad);
    const double terms[4] = {
        std::abs(inv_c * ratio - inv_s / aspect), std::abs(inv_c * ratio + inv_s / aspect),
        std::abs(inv_s * ratio * aspect + inv_c), std::abs(inv_s * ratio * aspect - inv_c)};
    for (const double term : terms)
    {
        if (term > kEpsilon)
        {
            height = std::min(height, 1.0 / term);
        }
    }
    return clamp_value(height, 0.01, 1.0);
}

} // namespace

void map_straighten_normalized(const double x, const double y, const double straighten_degrees,
                               const double working_aspect, const bool inverse, double &ox,
                               double &oy) noexcept
{
    const double rad =
        (inverse ? -straighten_degrees : straighten_degrees) * std::numbers::pi / 180.0;
    const double cosine = std::cos(rad);
    const double sine = std::sin(rad);
    const double aspect = std::max(working_aspect, kEpsilon);
    const double dx = (x - 0.5) * aspect;
    const double dy = y - 0.5;
    ox = (cosine * dx - sine * dy) / aspect + 0.5;
    oy = sine * dx + cosine * dy + 0.5;
}

void straightened_source_quad(const double straighten_degrees, const double working_aspect,
                              std::array<double, 8> &corners) noexcept
{
    constexpr double kSource[8] = {0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0};
    for (int index = 0; index < 4; ++index)
    {
        map_straighten_normalized(kSource[index * 2], kSource[index * 2 + 1], straighten_degrees,
                                  working_aspect, false,
                                  corners[static_cast<std::size_t>(index * 2)],
                                  corners[static_cast<std::size_t>(index * 2 + 1)]);
    }
}

void inscribed_crop_for_straighten(const double straighten_degrees, const double working_aspect,
                                   const double crop_aspect_norm, double &x, double &y,
                                   double &width, double &height) noexcept
{
    height = max_inscribed_normalized_height(straighten_degrees, working_aspect, crop_aspect_norm);
    width = crop_aspect_norm * height;
    if (width > 1.0)
    {
        width = 1.0;
        height = width / std::max(crop_aspect_norm, kEpsilon);
    }
    x = (1.0 - width) * 0.5;
    y = (1.0 - height) * 0.5;
}

void constrain_crop_to_straighten(DevelopParams &params, const double working_aspect) noexcept
{
    const double ratio = params.crop_width / std::max(params.crop_height, kEpsilon);
    double limit_x = 0.0;
    double limit_y = 0.0;
    double limit_w = 1.0;
    double limit_h = 1.0;
    inscribed_crop_for_straighten(params.straighten_degrees, working_aspect, ratio, limit_x,
                                  limit_y, limit_w, limit_h);
    double width = std::min(params.crop_width, limit_w);
    double height = std::min(params.crop_height, limit_h);
    if (width / std::max(height, kEpsilon) > ratio)
    {
        width = height * ratio;
    }
    else
    {
        height = width / std::max(ratio, kEpsilon);
    }
    const double center_x = params.crop_x + params.crop_width * 0.5;
    const double center_y = params.crop_y + params.crop_height * 0.5;
    params.crop_width = width;
    params.crop_height = height;
    params.crop_x = clamp_value(center_x - width * 0.5, limit_x, limit_x + limit_w - width);
    params.crop_y = clamp_value(center_y - height * 0.5, limit_y, limit_y + limit_h - height);
    clamp_develop(params);
}

void fit_crop_to_straighten(DevelopParams &params, const double working_aspect) noexcept
{
    const double ratio = params.crop_width / std::max(params.crop_height, kEpsilon);
    inscribed_crop_for_straighten(params.straighten_degrees, working_aspect, ratio, params.crop_x,
                                  params.crop_y, params.crop_width, params.crop_height);
    clamp_develop(params);
}

void strip_crop_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           { return operation.id == "ravo.geometry.crop"; }),
                            recipe.operations.end());
}

void strip_straighten_operations(Recipe &recipe)
{
    recipe.operations.erase(std::remove_if(recipe.operations.begin(), recipe.operations.end(),
                                           [](const OperationInstance &operation)
                                           { return operation.id == "ravo.geometry.straighten"; }),
                            recipe.operations.end());
}

Result<Recipe> recipe_from_develop(AssetDescriptor asset, const DevelopParams &params)
{
    DevelopParams clamped = params;
    clamp_develop(clamped);
    Recipe recipe;
    recipe.asset = std::move(asset);
    recipe.masks = clamped.masks;
    if (!clamped.temperature.is_identity())
    {
        add_operation(recipe, "ravo.color.temperature", "temperature-1",
                      temperature_to_parameters(clamped.temperature), 1, std::nullopt,
                      clamped.white_balance_effect_enabled);
    }
    if (clamped.profile_gamma_enabled)
    {
        auto profile_gamma = profile_gamma_to_parameters(clamped.profile_gamma);
        if (!profile_gamma)
        {
            return profile_gamma.error();
        }
        add_operation(recipe, std::string(kProfileGammaOperationId), "profilegamma-1",
                      std::move(profile_gamma).value(), kProfileGammaOperationSchemaVersion,
                      std::nullopt, true);
    }
    add_operation(recipe, "ravo.color.input", "color-input-1",
                  input_color_to_parameters(clamped.input_color), 1, std::nullopt,
                  clamped.input_profile_effect_enabled);
    if (!clamped.primaries.is_identity())
    {
        add_operation(recipe, std::string(kPrimariesOperationId), "primaries-1",
                      primaries_to_parameters(clamped.primaries), 1, std::nullopt,
                      clamped.primaries_effect_enabled);
    }
    if (!clamped.channel_mixer.is_identity())
    {
        add_operation(recipe, "ravo.color.channelmixerrgb", "channelmixerrgb-1",
                      channel_mixer_to_parameters(clamped.channel_mixer), 1, std::nullopt,
                      clamped.calibration_effect_enabled);
    }
    if (!near(clamped.hot_pixels_strength, 0.0))
    {
        add_operation(recipe, "ravo.raw.hotpixels", "hotpixels-1",
                      {{"strength", ParameterValue{clamped.hot_pixels_strength}},
                       {"threshold", ParameterValue{clamped.hot_pixels_threshold}},
                       {"permissive", ParameterValue{clamped.hot_pixels_permissive}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (!near(clamped.raw_highlights, 0.0))
    {
        add_operation(recipe, "ravo.raw.highlights", "raw-highlights-1",
                      {{"mode", ParameterValue{clamped.raw_highlights_mode}},
                       {"amount", ParameterValue{clamped.raw_highlights}},
                       {"clip", ParameterValue{clamped.raw_highlights_clip}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (clamped.raw_ca_iterations > 0)
    {
        add_operation(recipe, "ravo.raw.cacorrect", "cacorrect-1",
                      {{"iterations", ParameterValue{clamped.raw_ca_iterations}},
                       {"avoid_color_shift", ParameterValue{clamped.raw_ca_avoid_shift}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (!near(clamped.raw_denoise_threshold, 0.0))
    {
        add_operation(
            recipe, "ravo.raw.denoise", "rawdenoise-1",
            raw_denoise_to_parameters(clamped.raw_denoise_threshold, clamped.raw_denoise_bands), 1,
            std::nullopt, clamped.raw_effect_enabled);
    }
    if (!near(clamped.denoise, 0.0))
    {
        add_operation(recipe, "ravo.detail.denoiseprofile", "denoiseprofile-1",
                      {{"strength", ParameterValue{clamped.denoise}},
                       {"chroma", ParameterValue{clamped.denoise_chroma}},
                       {"radius", ParameterValue{clamped.denoise_radius}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (clamped.lens_mode == kLensModeLookup || !near(clamped.lens_k1, 0.0) ||
        !near(clamped.lens_k2, 0.0) || !near(clamped.lens_tca_r, 1.0) ||
        !near(clamped.lens_tca_b, 1.0) || !near(clamped.lens_vignetting, 0.0))
    {
        add_operation(recipe, "ravo.geometry.lens", "lens-1",
                      {{"mode", ParameterValue{clamped.lens_mode}},
                       {"k1", ParameterValue{clamped.lens_k1}},
                       {"k2", ParameterValue{clamped.lens_k2}},
                       {"tca_r", ParameterValue{clamped.lens_tca_r}},
                       {"tca_b", ParameterValue{clamped.lens_tca_b}},
                       {"vignetting", ParameterValue{clamped.lens_vignetting}},
                       {"camera_make", ParameterValue{clamped.lens_make}},
                       {"camera_model", ParameterValue{clamped.lens_model}},
                       {"lens", ParameterValue{clamped.lens_name}},
                       {"focal_mm", ParameterValue{clamped.lens_focal_mm}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (clamped.canvas_present || clamped.canvas_enabled)
    {
        auto canvas = canvas_to_parameters(clamped.canvas);
        if (!canvas)
            return canvas.error();
        add_operation(recipe, std::string(kCanvasOperationId), "canvas-1",
                      std::move(canvas).value(), kCanvasOperationSchemaVersion, std::nullopt,
                      clamped.geometry_effect_enabled && clamped.canvas_enabled);
    }
    const ExposureParams exposure{clamped.exposure_mode,
                                  clamped.exposure_black,
                                  clamped.exposure_ev,
                                  clamped.exposure_deflicker_percentile,
                                  clamped.exposure_deflicker_target_ev,
                                  clamped.exposure_compensate_exposure_bias,
                                  clamped.exposure_compensate_highlight_preservation};
    if (!exposure.is_identity())
    {
        add_operation(recipe, std::string(kExposureOperationId), "exposure-1",
                      exposure_to_parameters(exposure), kExposureOperationSchemaVersion,
                      std::nullopt, clamped.light_effect_enabled);
    }
    if (!near(clamped.tone_eq_blacks, 0.0) || !near(clamped.tone_eq_shadows, 0.0) ||
        !near(clamped.tone_eq_midtones, 0.0) || !near(clamped.tone_eq_highlights, 0.0) ||
        !near(clamped.tone_eq_whites, 0.0))
    {
        add_operation(recipe, "ravo.core.toneequal", "toneequal-1",
                      {{"blacks", ParameterValue{clamped.tone_eq_blacks}},
                       {"shadows", ParameterValue{clamped.tone_eq_shadows}},
                       {"midtones", ParameterValue{clamped.tone_eq_midtones}},
                       {"highlights", ParameterValue{clamped.tone_eq_highlights}},
                       {"whites", ParameterValue{clamped.tone_eq_whites}}},
                      1, std::nullopt, clamped.tone_equal_effect_enabled);
    }
    if (clamped.graduated_present || !near(clamped.graduated_density, 0.0))
    {
        add_operation(recipe, "ravo.effect.graduatednd", "graduatednd-1",
                      {{"density_ev", ParameterValue{clamped.graduated_density}},
                       {"hardness", ParameterValue{clamped.graduated_hardness}},
                       {"rotation_deg", ParameterValue{clamped.graduated_rotation}},
                       {"offset", ParameterValue{clamped.graduated_offset}}},
                      1, clamped.graduated_mask_id,
                      clamped.graduated_effect_enabled &&
                          (clamped.graduated_present ? clamped.graduated_enabled : true));
    }
    if (clamped.color_checker_enabled)
    {
        auto color_checker = color_checker_to_parameters(clamped.color_checker);
        if (!color_checker)
        {
            return color_checker.error();
        }
        add_operation(recipe, std::string(kColorCheckerOperationId), "colorchecker-1",
                      std::move(color_checker).value(), kColorCheckerOperationSchemaVersion,
                      std::nullopt, clamped.color_effect_enabled);
    }
    if (clamped.color_harmonizer_present || clamped.color_harmonizer_enabled)
    {
        auto color_harmonizer = color_harmonizer_to_parameters(clamped.color_harmonizer);
        if (!color_harmonizer)
        {
            return color_harmonizer.error();
        }
        add_operation(recipe, std::string(kColorHarmonizerOperationId), "colorharmonizer-1",
                      std::move(color_harmonizer).value(), kColorHarmonizerOperationSchemaVersion,
                      clamped.color_harmonizer_mask_id,
                      clamped.color_effect_enabled && clamped.color_harmonizer_enabled);
    }
    if (!near(clamped.highlights, 0.0))
    {
        add_operation(recipe, "ravo.core.highlights", "highlights-1",
                      {{"amount", ParameterValue{clamped.highlights}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.shadows, 0.0))
    {
        add_operation(recipe, "ravo.core.shadows", "shadows-1",
                      {{"amount", ParameterValue{clamped.shadows}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.whites, 0.0))
    {
        add_operation(recipe, "ravo.core.whites", "whites-1",
                      {{"amount", ParameterValue{clamped.whites}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.blacks, 0.0))
    {
        add_operation(recipe, "ravo.core.blacks", "blacks-1",
                      {{"amount", ParameterValue{clamped.blacks}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.contrast, 0.0))
    {
        add_operation(recipe, "ravo.core.contrast", "contrast-1",
                      {{"amount", ParameterValue{clamped.contrast}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.gamma, kDevelopGammaDefault))
    {
        add_operation(recipe, "ravo.core.gamma", "gamma-1",
                      {{"gamma", ParameterValue{clamped.gamma}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!clamped.rgb_levels.is_identity())
    {
        add_operation(recipe, "ravo.color.rgblevels", "rgblevels-1",
                      rgb_levels_to_parameters(clamped.rgb_levels), 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!clamped.rgb_curve.is_identity())
    {
        add_operation(recipe, "ravo.color.rgbcurve", "rgbcurve-1",
                      rgb_curve_to_parameters(clamped.rgb_curve), 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!tone_curve_is_identity(clamped.tone_curve))
    {
        add_operation(
            recipe, "ravo.core.tonecurve", "tonecurve-1",
            {{"working_space", ParameterValue{clamped.tone_curve_working_space}},
             {"interpolation", ParameterValue{std::string(kToneCurveInterpolationMonotoneHermite)}},
             {"channel_mode", ParameterValue{std::string(kToneCurveChannelModeRgb)}},
             {"preserve_colors", ParameterValue{std::string(kToneCurvePreserveColorsAverage)}},
             {"points", tone_curve_points_to_parameter(clamped.tone_curve)}},
            1, std::nullopt, clamped.light_effect_enabled);
    }
    if (clamped.color_balance_enabled)
    {
        add_operation(recipe, std::string(kColorBalanceOperationId), "colorbalance-1",
                      color_balance_to_parameters(clamped.color_balance),
                      kColorBalanceOperationSchemaVersion, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (!clamped.color_balance_rgb.is_identity())
    {
        add_operation(recipe, "ravo.color.colorbalancergb", "colorbalancergb-1",
                      color_balance_rgb_to_parameters(clamped.color_balance_rgb), 1, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (clamped.color_correction_enabled)
    {
        auto color_correction = color_correction_to_parameters(clamped.color_correction);
        if (!color_correction)
        {
            return color_correction.error();
        }
        add_operation(recipe, std::string(kColorCorrectionOperationId), "colorcorrection-1",
                      std::move(color_correction).value(), kColorCorrectionOperationSchemaVersion,
                      std::nullopt, clamped.color_effect_enabled);
    }
    if (clamped.color_contrast_enabled)
    {
        auto color_contrast = color_contrast_to_parameters(clamped.color_contrast);
        if (!color_contrast)
        {
            return color_contrast.error();
        }
        add_operation(recipe, std::string(kColorContrastOperationId), "colorcontrast-1",
                      std::move(color_contrast).value(), kColorContrastOperationSchemaVersion,
                      std::nullopt, clamped.color_effect_enabled);
    }
    if (!near(clamped.velvia, 0.0))
    {
        add_operation(recipe, "ravo.color.velvia", "velvia-1",
                      {{"amount", ParameterValue{clamped.velvia}}, {"bias", ParameterValue{1.0}}},
                      1, std::nullopt, clamped.color_effect_enabled);
    }
    if (!near(clamped.vibrance, 0.0))
    {
        add_operation(recipe, "ravo.color.vibrance", "vibrance-1",
                      {{"amount", ParameterValue{clamped.vibrance}}}, 1, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (!near(clamped.saturation, 0.0))
    {
        add_operation(recipe, "ravo.color.saturation", "saturation-1",
                      {{"amount", ParameterValue{clamped.saturation}}}, 1, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (!bands_near_zero(clamped.color_eq_hue) || !bands_near_zero(clamped.color_eq_sat) ||
        !bands_near_zero(clamped.color_eq_light))
    {
        add_operation(recipe, "ravo.color.colorequal", "colorequal-1",
                      {{"hue_shift", band_array_parameter(clamped.color_eq_hue)},
                       {"saturation", band_array_parameter(clamped.color_eq_sat)},
                       {"lightness", band_array_parameter(clamped.color_eq_light)}},
                      1, std::nullopt, clamped.graduated_effect_enabled);
    }
    if (clamped.color_zones_present || clamped.color_zones_enabled ||
        clamped.color_zones_mask_id.has_value())
    {
        auto zones = color_zones_to_parameters(clamped.color_zones);
        if (!zones)
            return zones.error();
        add_operation(recipe, std::string(kColorZonesOperationId), "colorzones-1",
                      std::move(zones).value(), kColorZonesOperationSchemaVersion,
                      clamped.color_zones_mask_id,
                      clamped.color_effect_enabled && clamped.color_zones_enabled);
    }
    if (clamped.monochrome_present || clamped.monochrome_enabled ||
        clamped.monochrome_mask_id.has_value())
    {
        auto monochrome = monochrome_to_parameters(clamped.monochrome);
        if (!monochrome)
            return monochrome.error();
        add_operation(recipe, std::string(kMonochromeOperationId), "monochrome-1",
                      std::move(monochrome).value(), kMonochromeOperationSchemaVersion,
                      clamped.monochrome_mask_id,
                      clamped.color_effect_enabled && clamped.monochrome_enabled);
    }
    if (clamped.split_toning_present || clamped.split_toning_enabled ||
        clamped.split_toning_mask_id.has_value())
    {
        auto split = split_toning_to_parameters(clamped.split_toning);
        if (!split)
            return split.error();
        add_operation(recipe, std::string(kSplitToningOperationId), "splittoning-1",
                      std::move(split).value(), kSplitToningOperationSchemaVersion,
                      clamped.split_toning_mask_id,
                      clamped.color_effect_enabled && clamped.split_toning_enabled);
    }
    if (!near(clamped.sharpen, 0.0))
    {
        auto sharpen = sharpen_to_parameters(
            {clamped.sharpen_radius, clamped.sharpen, clamped.sharpen_threshold});
        if (!sharpen)
        {
            return sharpen.error();
        }
        add_operation(recipe, std::string(kSharpenOperationId), "sharpen-1",
                      std::move(sharpen).value(), kSharpenOperationSchemaVersion, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (!clamped.retouch.is_identity())
    {
        add_operation(recipe, std::string(kRetouchOperationId), "retouch-1",
                      retouch_to_parameters(clamped.retouch), kRetouchOperationSchemaVersion,
                      std::nullopt, clamped.detail_effect_enabled);
    }
    if (!near(clamped.clarity, 0.0))
    {
        add_operation(recipe, "ravo.detail.clarity", "clarity-1",
                      {{"amount", ParameterValue{clamped.clarity}}}, 1, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (!near(clamped.bloom, 0.0))
    {
        add_operation(recipe, "ravo.effect.bloom", "bloom-1",
                      {{"amount", ParameterValue{clamped.bloom}}}, 1, std::nullopt,
                      clamped.effects_effect_enabled);
    }
    if (!near(clamped.soften, 0.0))
    {
        add_operation(recipe, "ravo.effect.soften", "soften-1",
                      {{"amount", ParameterValue{clamped.soften}}}, 1, std::nullopt,
                      clamped.effects_effect_enabled);
    }
    if (!near(clamped.dehaze, 0.0))
    {
        auto dehaze = dehaze_to_parameters(
            {clamped.dehaze, clamped.dehaze_distance, clamped.dehaze_adaptive});
        if (!dehaze)
        {
            return dehaze.error();
        }
        add_operation(recipe, std::string(kDehazeOperationId), "dehaze-1",
                      std::move(dehaze).value(), kDehazeOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled);
    }
    if (!near(clamped.vignette, 0.0))
    {
        add_operation(recipe, "ravo.effect.vignette", "vignette-1",
                      {{"amount", ParameterValue{clamped.vignette}},
                       {"midpoint", ParameterValue{0.8}},
                       {"falloff", ParameterValue{0.5}}},
                      1, std::nullopt, clamped.effects_effect_enabled);
    }
    if (!near(clamped.grain, 0.0))
    {
        add_operation(recipe, "ravo.effect.grain", "grain-1",
                      {{"amount", ParameterValue{clamped.grain}}}, 1, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (clamped.rotate_quarters % 4 != 0)
    {
        add_operation(recipe, "ravo.geometry.rotate", "rotate-1",
                      {{"quarters", ParameterValue{clamped.rotate_quarters % 4}}}, 1, std::nullopt,
                      clamped.geometry_effect_enabled);
    }
    if (clamped.flip_horizontal != 0 || clamped.flip_vertical != 0)
    {
        add_operation(recipe, "ravo.geometry.flip", "flip-1",
                      {{"horizontal", ParameterValue{clamped.flip_horizontal}},
                       {"vertical", ParameterValue{clamped.flip_vertical}}},
                      1, std::nullopt, clamped.geometry_effect_enabled);
    }
    if (!near(clamped.straighten_degrees, 0.0))
    {
        add_operation(recipe, "ravo.geometry.straighten", "straighten-1",
                      {{"degrees", ParameterValue{clamped.straighten_degrees}}}, 1, std::nullopt,
                      clamped.geometry_effect_enabled);
    }
    if (!near(clamped.crop_x, 0.0) || !near(clamped.crop_y, 0.0) ||
        !near(clamped.crop_width, 1.0) || !near(clamped.crop_height, 1.0))
    {
        add_operation(recipe, "ravo.geometry.crop", "crop-1",
                      {{"x", ParameterValue{clamped.crop_x}},
                       {"y", ParameterValue{clamped.crop_y}},
                       {"width", ParameterValue{clamped.crop_width}},
                       {"height", ParameterValue{clamped.crop_height}}},
                      1, std::nullopt, clamped.geometry_effect_enabled);
    }
    if (clamped.sigmoid_enabled)
    {
        add_operation(
            recipe, "ravo.display.sigmoid", "sigmoid-1",
            {{"working_space", ParameterValue{std::string(kSigmoidWorkingSpaceLinearSrgb)}},
             {"color_processing", ParameterValue{std::string(kSigmoidColorProcessingPerChannel)}},
             {"middle_grey_contrast", ParameterValue{clamped.sigmoid_contrast}},
             {"contrast_skewness", ParameterValue{clamped.sigmoid_skew}},
             {"display_white_target", ParameterValue{clamped.sigmoid_display_white}},
             {"display_black_target", ParameterValue{clamped.sigmoid_display_black}},
             {"hue_preservation", ParameterValue{clamped.sigmoid_hue_preservation}}},
            1, std::nullopt, clamped.light_effect_enabled);
    }
    if (clamped.color_reconstruction_enabled)
    {
        auto color_reconstruction =
            color_reconstruction_to_parameters(clamped.color_reconstruction);
        if (!color_reconstruction)
        {
            return color_reconstruction.error();
        }
        add_operation(recipe, std::string(kColorReconstructionOperationId), "colorreconstruct-1",
                      std::move(color_reconstruction).value(),
                      kColorReconstructionOperationSchemaVersion, std::nullopt,
                      clamped.color_effect_enabled);
    }
    add_operation(recipe, "ravo.color.output", "color-output-1",
                  output_color_to_parameters(clamped.output_color), 1, std::nullopt,
                  clamped.output_profile_effect_enabled);
    if (clamped.output_dither_present || clamped.output_dither_enabled)
    {
        auto dither = output_dither_to_parameters(clamped.output_dither);
        if (!dither)
            return dither.error();
        add_operation(recipe, std::string(kOutputDitherOperationId), "output-dither-1",
                      std::move(dither).value(), kOutputDitherOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled && clamped.output_dither_enabled);
    }
    if (clamped.frame_present || clamped.frame_enabled)
    {
        auto frame = frame_to_parameters(clamped.frame);
        if (!frame)
            return frame.error();
        add_operation(recipe, std::string(kFrameOperationId), "frame-1", std::move(frame).value(),
                      kFrameOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled && clamped.frame_enabled);
    }
    if (clamped.watermark_present || clamped.watermark_enabled)
    {
        auto watermark = watermark_to_parameters(clamped.watermark);
        if (!watermark)
            return watermark.error();
        add_operation(recipe, std::string(kWatermarkOperationId), "watermark-1",
                      std::move(watermark).value(), kWatermarkOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled && clamped.watermark_enabled);
    }
    return recipe;
}

Result<DevelopParams> develop_from_recipe(const Recipe &recipe)
{
    DevelopParams params;
    params.masks = recipe.masks;
    std::map<std::string, std::pair<bool, bool>, std::less<>> section_flags;
    const auto note_section = [&](const std::string_view section, const bool enabled)
    {
        auto &entry = section_flags[std::string(section)];
        entry.first = true;
        entry.second = entry.second || enabled;
    };
    for (const auto &operation : recipe.operations)
    {
        const auto number = [&](const std::string_view name, const double fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_number(found->second, fallback);
        };
        const auto integer = [&](const std::string_view name, const std::int64_t fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_integer(found->second, fallback);
        };
        if (operation.id == "ravo.color.temperature")
        {
            auto temperature = temperature_from_parameters(operation.parameters);
            if (!temperature)
            {
                return temperature.error();
            }
            params.temperature = std::move(temperature).value();
            note_section("whiteBalance", operation.enabled);
        }
        else if (operation.id == kProfileGammaOperationId)
        {
            auto profile_gamma = profile_gamma_from_parameters(operation.parameters);
            if (!profile_gamma)
            {
                return profile_gamma.error();
            }
            if (operation.enabled)
            {
                params.profile_gamma_enabled = true;
                params.profile_gamma = std::move(profile_gamma).value();
            }
        }
        else if (operation.id == "ravo.color.input")
        {
            auto input_color = input_color_from_parameters(operation.parameters);
            if (!input_color)
            {
                return input_color.error();
            }
            params.input_color = std::move(input_color).value();
            note_section("inputProfile", operation.enabled);
        }
        else if (operation.id == kPrimariesOperationId)
        {
            auto primaries = primaries_from_parameters(operation.parameters);
            if (!primaries)
            {
                return primaries.error();
            }
            params.primaries = std::move(primaries).value();
            note_section("primaries", operation.enabled);
        }
        else if (operation.id == "ravo.color.output")
        {
            auto output_color = output_color_from_parameters(operation.parameters);
            if (!output_color)
            {
                return output_color.error();
            }
            params.output_color = std::move(output_color).value();
            note_section("outputProfile", operation.enabled);
        }
        else if (operation.id == kOutputDitherOperationId)
        {
            if (params.output_dither_present)
            {
                return make_error(
                    ErrorCode::kValidation, "Develop contains duplicate Output Dither operations",
                    {{"operation_id", operation.id}, {"reason", "duplicate_output_dither"}});
            }
            if (operation.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Output Dither masks are unsupported",
                    {{"operation_id", operation.id}, {"reason", "unsupported_output_dither_mask"}});
            }
            auto dither = output_dither_from_parameters(operation.parameters);
            if (!dither)
                return dither.error();
            params.output_dither_present = true;
            params.output_dither_enabled = operation.enabled;
            params.output_dither = dither.value();
            note_section("effects", operation.enabled);
        }
        else if (operation.id == kFrameOperationId)
        {
            if (params.frame_present)
                return make_error(ErrorCode::kValidation, "Develop contains duplicate Frames",
                                  {{"reason", "duplicate_output_frame"}});
            if (operation.mask_id.has_value())
                return make_error(ErrorCode::kUnsupported, "Develop Frame masks are unsupported",
                                  {{"reason", "unsupported_frame_mask"}});
            auto frame = frame_from_parameters(operation.parameters);
            if (!frame)
                return frame.error();
            params.frame_present = true;
            params.frame_enabled = operation.enabled;
            params.frame = frame.value();
            note_section("effects", operation.enabled);
        }
        else if (operation.id == kWatermarkOperationId)
        {
            if (params.watermark_present)
                return make_error(ErrorCode::kValidation, "Develop contains duplicate Watermarks",
                                  {{"reason", "duplicate_watermark"}});
            if (operation.mask_id.has_value())
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Watermark masks are unsupported",
                                  {{"reason", "unsupported_watermark_mask"}});
            auto watermark = watermark_from_parameters(operation.parameters);
            if (!watermark)
                return watermark.error();
            params.watermark_present = true;
            params.watermark_enabled = operation.enabled;
            params.watermark = watermark.value();
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.color.channelmixerrgb")
        {
            auto mixer = channel_mixer_from_parameters(operation.parameters);
            if (!mixer)
            {
                return mixer.error();
            }
            params.channel_mixer = std::move(mixer).value();
            note_section("calibration", operation.enabled);
        }
        else if (operation.id == kExposureOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_exposure_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            auto exposure = exposure_from_parameters(canonical.parameters);
            if (!exposure)
            {
                return exposure.error();
            }
            params.exposure_mode = exposure.value().mode;
            params.exposure_black = exposure.value().black;
            params.exposure_ev = exposure.value().exposure_ev;
            params.exposure_deflicker_percentile = exposure.value().deflicker_percentile;
            params.exposure_deflicker_target_ev = exposure.value().deflicker_target_ev;
            params.exposure_compensate_exposure_bias = exposure.value().compensate_exposure_bias;
            params.exposure_compensate_highlight_preservation =
                exposure.value().compensate_highlight_preservation;
            note_section("light", operation.enabled);
        }
        else if (operation.id == kColorCheckerOperationId)
        {
            auto color_checker = color_checker_from_parameters(operation.parameters);
            if (!color_checker)
            {
                return color_checker.error();
            }
            params.color_checker_enabled = true;
            params.color_checker = std::move(color_checker).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorHarmonizerOperationId)
        {
            if (params.color_harmonizer_present)
            {
                return make_error(ErrorCode::kConflict,
                                  "Develop Color Harmonizer does not allow duplicate operations",
                                  {{"operation_id", operation.id},
                                   {"reason", "duplicate_colorharmonizer_operation"}});
            }
            if (operation.schema_version != kColorHarmonizerOperationSchemaVersion)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Color Harmonizer schema version is unsupported",
                                  {{"operation_id", operation.id},
                                   {"schema_version", std::to_string(operation.schema_version)},
                                   {"reason", "unsupported_colorharmonizer_schema"}});
            }
            auto color_harmonizer = color_harmonizer_from_parameters(operation.parameters);
            if (!color_harmonizer)
            {
                return color_harmonizer.error();
            }
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = operation.enabled;
            params.color_harmonizer = std::move(color_harmonizer).value();
            params.color_harmonizer_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.core.contrast")
        {
            params.contrast = number("amount", params.contrast);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.highlights")
        {
            params.highlights = number("amount", params.highlights);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.shadows")
        {
            params.shadows = number("amount", params.shadows);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.whites")
        {
            params.whites = number("amount", params.whites);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.blacks")
        {
            params.blacks = number("amount", params.blacks);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.gamma")
        {
            params.gamma = number("gamma", params.gamma);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.color.rgblevels")
        {
            const auto take_text = [&](const char *name, std::string &target)
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    if (const auto *text = as_string_if(found->second); text != nullptr)
                    {
                        target = *text;
                    }
                }
            };
            take_text("mode", params.rgb_levels.mode);
            take_text("preserve_colors", params.rgb_levels.preserve_colors);
            if (params.rgb_levels.mode != kRgbLevelsModeLinked &&
                params.rgb_levels.mode != kRgbLevelsModeIndependent)
            {
                return make_error(ErrorCode::kValidation, "RGB levels mode is unsupported",
                                  {{"mode", params.rgb_levels.mode}});
            }
            const auto preserve_names = rgb_levels_preserve_names();
            if (std::find(preserve_names.begin(), preserve_names.end(),
                          params.rgb_levels.preserve_colors) == preserve_names.end())
            {
                return make_error(ErrorCode::kValidation,
                                  "RGB levels preserve-colors is unsupported",
                                  {{"preserve_colors", params.rgb_levels.preserve_colors}});
            }
            params.rgb_levels.levels[0][0] = number("black", params.rgb_levels.levels[0][0]);
            params.rgb_levels.levels[0][1] = number("grey", params.rgb_levels.levels[0][1]);
            params.rgb_levels.levels[0][2] = number("white", params.rgb_levels.levels[0][2]);
            params.rgb_levels.levels[1][0] = number("black_g", params.rgb_levels.levels[1][0]);
            params.rgb_levels.levels[1][1] = number("grey_g", params.rgb_levels.levels[1][1]);
            params.rgb_levels.levels[1][2] = number("white_g", params.rgb_levels.levels[1][2]);
            params.rgb_levels.levels[2][0] = number("black_b", params.rgb_levels.levels[2][0]);
            params.rgb_levels.levels[2][1] = number("grey_b", params.rgb_levels.levels[2][1]);
            params.rgb_levels.levels[2][2] = number("white_b", params.rgb_levels.levels[2][2]);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.color.rgbcurve")
        {
            const auto take_text = [&](const char *name, std::string &target)
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    if (const auto *text = as_string_if(found->second); text != nullptr)
                    {
                        target = *text;
                    }
                }
            };
            take_text("mode", params.rgb_curve.mode);
            take_text("preserve_colors", params.rgb_curve.preserve_colors);
            take_text("interpolation", params.rgb_curve.interpolation);
            if (const auto found = operation.parameters.find("compensate_middle_grey");
                found != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
                {
                    params.rgb_curve.compensate_middle_grey = *flag;
                }
                else
                {
                    params.rgb_curve.compensate_middle_grey =
                        number("compensate_middle_grey", 0.0) != 0.0;
                }
            }
            const auto take_points = [&](const char *name,
                                         std::vector<ToneCurvePoint> &target) -> Result<void>
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    auto points = parse_rgb_curve_points(found->second);
                    if (!points)
                    {
                        return points.error();
                    }
                    target = std::move(points).value();
                }
                return {};
            };
            if (auto red = take_points("points", params.rgb_curve.channels[0]); !red)
            {
                return red.error();
            }
            if (auto green = take_points("points_g", params.rgb_curve.channels[1]); !green)
            {
                return green.error();
            }
            if (auto blue = take_points("points_b", params.rgb_curve.channels[2]); !blue)
            {
                return blue.error();
            }
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.tonecurve")
        {
            if (const auto found = operation.parameters.find("working_space");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.tone_curve_working_space = *text;
                }
            }
            if (const auto found = operation.parameters.find("points");
                found != operation.parameters.end())
            {
                auto points = parse_tone_curve_points(found->second);
                if (!points)
                {
                    return points.error();
                }
                params.tone_curve = std::move(points).value();
            }
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.color.vibrance")
        {
            params.vibrance = number("amount", params.vibrance);
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.saturation")
        {
            params.saturation = number("amount", params.saturation);
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.velvia")
        {
            params.velvia = number("amount", params.velvia);
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.colorbalancergb")
        {
            auto color_balance = color_balance_rgb_from_parameters(operation.parameters);
            if (!color_balance)
            {
                return color_balance.error();
            }
            params.color_balance_rgb = std::move(color_balance).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorCorrectionOperationId)
        {
            auto color_correction = color_correction_from_parameters(operation.parameters);
            if (!color_correction)
            {
                return color_correction.error();
            }
            params.color_correction_enabled = true;
            params.color_correction = std::move(color_correction).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorBalanceOperationId)
        {
            auto color_balance = color_balance_from_parameters(operation.parameters);
            if (!color_balance)
            {
                return color_balance.error();
            }
            params.color_balance = std::move(color_balance).value();
            params.color_balance_enabled = true;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorContrastOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_color_contrast_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            if (!canonical.enabled)
            {
                note_section("color", false);
                continue;
            }
            if (canonical.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Color Contrast masks are unsupported",
                    {{"operation_id", canonical.id}, {"reason", "unsupported_colorcontrast_mask"}});
            }
            auto color_contrast = color_contrast_from_parameters(canonical.parameters);
            if (!color_contrast)
            {
                return color_contrast.error();
            }
            params.color_contrast_enabled = true;
            params.color_contrast = std::move(color_contrast).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorReconstructionOperationId)
        {
            if (operation.mask_id.has_value())
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Color Reconstruction masks are unsupported",
                                  {{"operation_id", operation.id},
                                   {"reason", "unsupported_colorreconstruct_mask"}});
            }
            auto color_reconstruction = color_reconstruction_from_parameters(operation.parameters);
            if (!color_reconstruction)
            {
                return color_reconstruction.error();
            }
            params.color_reconstruction_enabled = true;
            params.color_reconstruction = std::move(color_reconstruction).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kMonochromeOperationId)
        {
            if (params.monochrome_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Monochrome operations",
                                  {{"reason", "duplicate_monochrome"}});
            OperationInstance canonical = operation;
            auto upgraded = upgrade_monochrome_operation(canonical);
            if (!upgraded)
                return upgraded.error();
            auto monochrome = monochrome_from_parameters(canonical.parameters);
            if (!monochrome)
                return monochrome.error();
            params.monochrome_present = true;
            params.monochrome_enabled = operation.enabled;
            params.monochrome = monochrome.value();
            params.monochrome_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kSplitToningOperationId)
        {
            if (params.split_toning_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Split Toning operations",
                                  {{"reason", "duplicate_split_toning"}});
            OperationInstance canonical = operation;
            auto upgraded = upgrade_split_toning_operation(canonical);
            if (!upgraded)
                return upgraded.error();
            auto split = split_toning_from_parameters(canonical.parameters);
            if (!split)
                return split.error();
            params.split_toning_present = true;
            params.split_toning_enabled = operation.enabled;
            params.split_toning = split.value();
            params.split_toning_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kSharpenOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_sharpen_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            if (canonical.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Sharpen masks are unsupported",
                    {{"operation_id", canonical.id}, {"reason", "unsupported_sharpen_mask"}});
            }
            auto sharpen = sharpen_from_parameters(canonical.parameters);
            if (!sharpen)
            {
                return sharpen.error();
            }
            params.sharpen = sharpen.value().amount;
            params.sharpen_radius = sharpen.value().radius;
            params.sharpen_threshold = sharpen.value().threshold;
            note_section("detail", operation.enabled);
        }
        else if (operation.id == kRetouchOperationId)
        {
            auto retouch = retouch_from_parameters(operation.parameters);
            if (!retouch)
            {
                return retouch.error();
            }
            params.retouch = std::move(retouch).value();
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.detail.clarity")
        {
            params.clarity = number("amount", params.clarity);
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.effect.vignette")
        {
            params.vignette = number("amount", params.vignette);
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.effect.grain")
        {
            params.grain = number("amount", params.grain);
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.effect.bloom")
        {
            params.bloom = number("amount", params.bloom);
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.effect.soften")
        {
            params.soften = number("amount", params.soften);
            note_section("effects", operation.enabled);
        }
        else if (operation.id == kDehazeOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_dehaze_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            if (canonical.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Dehaze masks are unsupported",
                    {{"operation_id", canonical.id}, {"reason", "unsupported_dehaze_mask"}});
            }
            auto dehaze = dehaze_from_parameters(canonical.parameters);
            if (!dehaze)
            {
                return dehaze.error();
            }
            params.dehaze = dehaze.value().strength;
            params.dehaze_distance = dehaze.value().distance;
            params.dehaze_adaptive = dehaze.value().adaptive;
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            params.rotate_quarters = integer("quarters", 0) % 4;
            if (params.rotate_quarters < 0)
            {
                params.rotate_quarters += 4;
            }
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.flip")
        {
            params.flip_horizontal = flag01(integer("horizontal", 0));
            params.flip_vertical = flag01(integer("vertical", 0));
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.straighten")
        {
            params.straighten_degrees = number("degrees", params.straighten_degrees);
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            params.crop_x = number("x", params.crop_x);
            params.crop_y = number("y", params.crop_y);
            params.crop_width = number("width", params.crop_width);
            params.crop_height = number("height", params.crop_height);
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.display.sigmoid")
        {
            auto validated = validate_sigmoid_parameters(operation.parameters);
            if (!validated)
            {
                return validated.error();
            }
            params.sigmoid_enabled = true;
            params.sigmoid_contrast = number("middle_grey_contrast", params.sigmoid_contrast);
            params.sigmoid_skew = number("contrast_skewness", params.sigmoid_skew);
            params.sigmoid_display_white =
                number("display_white_target", params.sigmoid_display_white);
            params.sigmoid_display_black =
                number("display_black_target", params.sigmoid_display_black);
            params.sigmoid_hue_preservation =
                number("hue_preservation", params.sigmoid_hue_preservation);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.raw.highlights")
        {
            params.raw_highlights = number("amount", params.raw_highlights);
            params.raw_highlights_clip = number("clip", params.raw_highlights_clip);
            if (const auto found = operation.parameters.find("mode");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.raw_highlights_mode = *text;
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.raw.hotpixels")
        {
            params.hot_pixels_strength = number("strength", 0.25);
            params.hot_pixels_threshold = number("threshold", 0.05);
            if (const auto found = operation.parameters.find("permissive");
                found != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
                {
                    params.hot_pixels_permissive = *flag;
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.raw.denoise")
        {
            params.raw_denoise_threshold = number("threshold", params.raw_denoise_threshold);
            const char *names[4] = {"all", "red", "green", "blue"};
            for (int channel = 0; channel < 4; ++channel)
            {
                for (int band = 0; band < 5; ++band)
                {
                    const std::string key =
                        std::string("y_") + names[channel] + std::to_string(band);
                    params.raw_denoise_bands[static_cast<std::size_t>(channel)]
                                            [static_cast<std::size_t>(band)] =
                        number(key, params.raw_denoise_bands[static_cast<std::size_t>(channel)]
                                                            [static_cast<std::size_t>(band)]);
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.raw.cacorrect")
        {
            params.raw_ca_iterations = integer("iterations", 2);
            if (const auto found = operation.parameters.find("avoid_color_shift");
                found != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
                {
                    params.raw_ca_avoid_shift = *flag;
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.detail.denoiseprofile")
        {
            params.denoise = number("strength", params.denoise);
            params.denoise_chroma = number("chroma", params.denoise_chroma);
            params.denoise_radius = number("radius", params.denoise_radius);
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.lens")
        {
            params.lens_k1 = number("k1", params.lens_k1);
            params.lens_k2 = number("k2", params.lens_k2);
            params.lens_tca_r = number("tca_r", params.lens_tca_r);
            params.lens_tca_b = number("tca_b", params.lens_tca_b);
            params.lens_vignetting = number("vignetting", params.lens_vignetting);
            params.lens_focal_mm = number("focal_mm", params.lens_focal_mm);
            if (const auto found = operation.parameters.find("mode");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.lens_mode = *text;
                }
            }
            const auto take_text = [&](const char *name, std::string &target)
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    if (const auto *text = as_string_if(found->second); text != nullptr)
                    {
                        target = *text;
                    }
                }
            };
            take_text("camera_make", params.lens_make);
            take_text("camera_model", params.lens_model);
            take_text("lens", params.lens_name);
            note_section("raw", operation.enabled);
        }
        else if (operation.id == kCanvasOperationId)
        {
            if (params.canvas_present)
                return make_error(ErrorCode::kValidation, "Develop contains duplicate Canvases",
                                  {{"reason", "duplicate_canvas"}});
            if (operation.mask_id.has_value())
                return make_error(ErrorCode::kUnsupported, "Develop Canvas masks are unsupported",
                                  {{"reason", "unsupported_canvas_mask"}});
            auto canvas = canvas_from_parameters(operation.parameters);
            if (!canvas)
                return canvas.error();
            params.canvas_present = true;
            params.canvas_enabled = operation.enabled;
            params.canvas = canvas.value();
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == kColorZonesOperationId)
        {
            if (params.color_zones_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Color Zones operations",
                                  {{"reason", "duplicate_color_zones"}});
            auto zones = color_zones_from_parameters(operation.parameters);
            if (!zones)
                return zones.error();
            params.color_zones_present = true;
            params.color_zones_enabled = operation.enabled;
            params.color_zones = std::move(zones).value();
            params.color_zones_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.colorequal")
        {
            if (const auto found = operation.parameters.find("hue_shift");
                found != operation.parameters.end())
            {
                auto parsed = parse_band_array(found->second, "hue_shift");
                if (!parsed)
                {
                    return parsed.error();
                }
                params.color_eq_hue = parsed.value();
            }
            if (const auto found = operation.parameters.find("saturation");
                found != operation.parameters.end())
            {
                auto parsed = parse_band_array(found->second, "saturation");
                if (!parsed)
                {
                    return parsed.error();
                }
                params.color_eq_sat = parsed.value();
            }
            if (const auto found = operation.parameters.find("lightness");
                found != operation.parameters.end())
            {
                auto parsed = parse_band_array(found->second, "lightness");
                if (!parsed)
                {
                    return parsed.error();
                }
                params.color_eq_light = parsed.value();
            }
            note_section("graduated", operation.enabled);
        }
        else if (operation.id == "ravo.effect.graduatednd")
        {
            params.graduated_present = true;
            params.graduated_enabled = operation.enabled;
            params.graduated_density = number("density_ev", params.graduated_density);
            params.graduated_hardness = number("hardness", params.graduated_hardness);
            params.graduated_rotation = number("rotation_deg", params.graduated_rotation);
            params.graduated_offset = number("offset", params.graduated_offset);
            params.graduated_mask_id = operation.mask_id;
            note_section("graduated", operation.enabled);
        }
        else if (operation.id == "ravo.core.toneequal")
        {
            params.tone_eq_blacks = number("blacks", params.tone_eq_blacks);
            params.tone_eq_shadows = number("shadows", params.tone_eq_shadows);
            params.tone_eq_midtones = number("midtones", params.tone_eq_midtones);
            params.tone_eq_highlights = number("highlights", params.tone_eq_highlights);
            params.tone_eq_whites = number("whites", params.tone_eq_whites);
            note_section("toneEqual", operation.enabled);
        }
    }
    for (const auto &[section, flags] : section_flags)
    {
        if (flags.first && !flags.second)
        {
            static_cast<void>(set_develop_section_effect_enabled(params, section, false));
        }
    }
    clamp_develop(params);
    return params;
}

Result<LeftoverFlipGeometry> leftover_flip_orientation_to_geometry(const std::int32_t orientation)
{
    LeftoverFlipGeometry geometry;
    switch (orientation)
    {
    case -1:
    case 0:
        return geometry;
    case 1:
        geometry.flip_vertical = 1;
        return geometry;
    case 2:
        geometry.flip_horizontal = 1;
        return geometry;
    case 3:
        geometry.rotate_quarters = 2;
        return geometry;
    case 4:
        geometry.rotate_quarters = 1;
        geometry.flip_horizontal = 1;
        return geometry;
    case 5:
        geometry.rotate_quarters = 1;
        return geometry;
    case 6:
        geometry.rotate_quarters = 3;
        return geometry;
    case 7:
        geometry.rotate_quarters = 1;
        geometry.flip_vertical = 1;
        return geometry;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy flip orientation is outside the frozen bit contract",
                          {{"legacy_operation", "flip"},
                           {"orientation", std::to_string(orientation)},
                           {"reason", "unsupported_legacy_flip_orientation"}});
    }
}

bool LeftoverCropBox::is_identity() const noexcept
{
    return near(x, 0.0) && near(y, 0.0) && near(width, 1.0) && near(height, 1.0);
}

Result<LeftoverCropBox> leftover_crop_box_to_geometry(const float left, const float top,
                                                      const float right, const float bottom)
{
    if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(right) ||
        !std::isfinite(bottom))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy crop box contains a non-finite edge",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_box"}});
    }
    constexpr float kMin = 0.01F;
    const float cx = std::clamp(left, 0.0F, 1.0F - kMin);
    const float cy = std::clamp(top, 0.0F, 1.0F - kMin);
    const float cw = std::clamp(right, kMin, 1.0F);
    const float ch = std::clamp(bottom, kMin, 1.0F);
    const float width = cw - cx;
    const float height = ch - cy;
    if (width < kMin || height < kMin)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy crop box is empty after leftover clamp",
            {{"legacy_operation", "crop"}, {"reason", "unsupported_legacy_crop_box"}});
    }
    LeftoverCropBox box;
    box.x = cx;
    box.y = cy;
    box.width = width;
    box.height = height;
    return box;
}

Result<double> leftover_ashift_rotation_to_straighten(const float rotation, const float lensshift_v,
                                                      const float lensshift_h, const float shear)
{
    if (!std::isfinite(rotation) || !std::isfinite(lensshift_v) || !std::isfinite(lensshift_h) ||
        !std::isfinite(shear))
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy ashift head contains a non-finite value",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_head"}});
    }
    constexpr float kNearZero = 1.0e-4F;
    if (std::fabs(lensshift_v) > kNearZero || std::fabs(lensshift_h) > kNearZero ||
        std::fabs(shear) > kNearZero)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Legacy ashift perspective is outside the rotation-only straighten contract",
            {{"legacy_operation", "ashift"}, {"reason", "unsupported_legacy_ashift_perspective"}});
    }
    if (std::fabs(rotation) > static_cast<float>(kDevelopStraightenMax))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy ashift rotation exceeds canonical straighten range",
                          {{"legacy_operation", "ashift"},
                           {"reason", "unsupported_legacy_ashift_rotation_range"}});
    }
    return static_cast<double>(rotation);
}

bool RgbLevelsParams::is_identity() const noexcept
{
    const auto identity_channel = [](const std::array<double, 3> &channel)
    { return near(channel[0], 0.0) && near(channel[1], 0.5) && near(channel[2], 1.0); };
    if (mode == kRgbLevelsModeIndependent)
    {
        return identity_channel(levels[0]) && identity_channel(levels[1]) &&
               identity_channel(levels[2]);
    }
    return identity_channel(levels[0]);
}

Result<RgbLevelsParams> leftover_rgblevels_from_v1(const std::int32_t autoscale,
                                                   const std::int32_t preserve_colors,
                                                   const std::array<float, 9> &levels)
{
    RgbLevelsParams result;
    if (autoscale == 0)
    {
        result.mode = std::string(kRgbLevelsModeLinked);
    }
    else if (autoscale == 1)
    {
        result.mode = std::string(kRgbLevelsModeIndependent);
    }
    else
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB levels mode is unsupported",
            {{"legacy_operation", "rgblevels"}, {"reason", "unsupported_legacy_rgblevels_mode"}});
    }
    switch (preserve_colors)
    {
    case 0:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNone);
        break;
    case 1:
        result.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
        break;
    case 2:
        result.preserve_colors = std::string(kToneCurvePreserveColorsMax);
        break;
    case 3:
        result.preserve_colors = std::string(kToneCurvePreserveColorsAverage);
        break;
    case 4:
        result.preserve_colors = std::string(kToneCurvePreserveColorsSum);
        break;
    case 5:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNorm);
        break;
    case 6:
        result.preserve_colors = std::string(kToneCurvePreserveColorsPower);
        break;
    default:
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB levels preserve-colors is unsupported",
                          {{"legacy_operation", "rgblevels"},
                           {"reason", "unsupported_legacy_rgblevels_preserve"}});
    }
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        for (std::size_t stop = 0; stop < 3; ++stop)
        {
            const float value = levels[channel * 3U + stop];
            if (!std::isfinite(value))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RGB levels contain a non-finite stop",
                                  {{"legacy_operation", "rgblevels"},
                                   {"reason", "unsupported_legacy_rgblevels_levels"}});
            }
            result.levels[channel][stop] = value;
        }
        if (!(result.levels[channel][2] > result.levels[channel][0]))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB levels white must be greater than black",
                              {{"legacy_operation", "rgblevels"},
                               {"reason", "unsupported_legacy_rgblevels_levels"}});
        }
    }
    return result;
}

std::map<std::string, ParameterValue, std::less<>>
rgb_levels_to_parameters(const RgbLevelsParams &params)
{
    return {{"mode", ParameterValue{params.mode}},
            {"preserve_colors", ParameterValue{params.preserve_colors}},
            {"black", ParameterValue{params.levels[0][0]}},
            {"grey", ParameterValue{params.levels[0][1]}},
            {"white", ParameterValue{params.levels[0][2]}},
            {"black_g", ParameterValue{params.levels[1][0]}},
            {"grey_g", ParameterValue{params.levels[1][1]}},
            {"white_g", ParameterValue{params.levels[1][2]}},
            {"black_b", ParameterValue{params.levels[2][0]}},
            {"grey_b", ParameterValue{params.levels[2][1]}},
            {"white_b", ParameterValue{params.levels[2][2]}}};
}

bool RgbCurveParams::is_identity() const noexcept
{
    if (mode == kRgbLevelsModeIndependent)
    {
        return tone_curve_is_identity(channels[0]) && tone_curve_is_identity(channels[1]) &&
               tone_curve_is_identity(channels[2]);
    }
    return tone_curve_is_identity(channels[0]);
}

namespace
{

[[nodiscard]] std::int32_t rgb_curve_read_i32(const std::vector<std::uint8_t> &payload,
                                              const std::size_t offset) noexcept
{
    std::int32_t value = 0;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    return value;
}

[[nodiscard]] float rgb_curve_read_f32(const std::vector<std::uint8_t> &payload,
                                       const std::size_t offset) noexcept
{
    float value = 0.0F;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    return value;
}

} // namespace

Result<RgbCurveParams> leftover_rgbcurve_from_v1(const std::vector<std::uint8_t> &payload)
{
    constexpr std::size_t kPayloadSize = 516;
    constexpr std::size_t kMaxNodes = 20;
    constexpr std::int32_t kMonotoneHermite = 2;
    if (payload.size() != kPayloadSize)
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve payload size is unsupported",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_payload"}});
    }
    RgbCurveParams result;
    const auto autoscale = rgb_curve_read_i32(payload, 504);
    const auto compensate = rgb_curve_read_i32(payload, 508);
    const auto preserve = rgb_curve_read_i32(payload, 512);
    if (autoscale == 0)
    {
        result.mode = std::string(kRgbLevelsModeLinked);
    }
    else if (autoscale == 1)
    {
        result.mode = std::string(kRgbLevelsModeIndependent);
    }
    else
    {
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve mode is unsupported",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_mode"}});
    }
    if (compensate != 0 && compensate != 1)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Legacy RGB curve middle-grey flag is unsupported",
                          {{"legacy_operation", "rgbcurve"},
                           {"reason", "unsupported_legacy_rgbcurve_middle_grey"}});
    }
    result.compensate_middle_grey = compensate == 1;
    switch (preserve)
    {
    case 0:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNone);
        break;
    case 1:
        result.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
        break;
    case 2:
        result.preserve_colors = std::string(kToneCurvePreserveColorsMax);
        break;
    case 3:
        result.preserve_colors = std::string(kToneCurvePreserveColorsAverage);
        break;
    case 4:
        result.preserve_colors = std::string(kToneCurvePreserveColorsSum);
        break;
    case 5:
        result.preserve_colors = std::string(kToneCurvePreserveColorsNorm);
        break;
    case 6:
        result.preserve_colors = std::string(kToneCurvePreserveColorsPower);
        break;
    default:
        return make_error(
            ErrorCode::kUnsupported, "Legacy RGB curve preserve-colors is unsupported",
            {{"legacy_operation", "rgbcurve"}, {"reason", "unsupported_legacy_rgbcurve_preserve"}});
    }
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const auto count = rgb_curve_read_i32(payload, 480 + channel * 4U);
        const auto type = rgb_curve_read_i32(payload, 492 + channel * 4U);
        if (count < 2 || static_cast<std::size_t>(count) > kMaxNodes)
        {
            return make_error(ErrorCode::kUnsupported, "Legacy RGB curve node count is unsupported",
                              {{"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_nodes"}});
        }
        if (type != kMonotoneHermite)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Legacy RGB curve interpolation is unsupported",
                              {{"legacy_operation", "rgbcurve"},
                               {"reason", "unsupported_legacy_rgbcurve_interpolation"}});
        }
        std::vector<ToneCurvePoint> points;
        points.reserve(static_cast<std::size_t>(count));
        for (std::int32_t index = 0; index < count; ++index)
        {
            const std::size_t offset = (channel * kMaxNodes + static_cast<std::size_t>(index)) * 8U;
            const float x = rgb_curve_read_f32(payload, offset);
            const float y = rgb_curve_read_f32(payload, offset + 4U);
            if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || x > 1.0F || y < 0.0F ||
                y > 1.0F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RGB curve nodes are outside the unit interval",
                                  {{"legacy_operation", "rgbcurve"},
                                   {"reason", "unsupported_legacy_rgbcurve_nodes"}});
            }
            if (!points.empty() && !(x > points.back().x))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RGB curve nodes must be strictly increasing",
                                  {{"legacy_operation", "rgbcurve"},
                                   {"reason", "unsupported_legacy_rgbcurve_nodes"}});
            }
            points.push_back({x, y});
        }
        result.channels[channel] = std::move(points);
    }
    result.interpolation = std::string(kToneCurveInterpolationMonotoneHermite);
    return result;
}

std::map<std::string, ParameterValue, std::less<>>
rgb_curve_to_parameters(const RgbCurveParams &params)
{
    return {{"mode", ParameterValue{params.mode}},
            {"preserve_colors", ParameterValue{params.preserve_colors}},
            {"interpolation", ParameterValue{params.interpolation}},
            {"compensate_middle_grey", ParameterValue{params.compensate_middle_grey}},
            {"points", tone_curve_points_to_parameter(params.channels[0])},
            {"points_g", tone_curve_points_to_parameter(params.channels[1])},
            {"points_b", tone_curve_points_to_parameter(params.channels[2])}};
}

Result<void> leftover_rawdenoise_from_v2(const std::vector<std::uint8_t> &payload,
                                         double &threshold,
                                         std::array<std::array<double, 5>, 4> &bands)
{
    constexpr std::size_t kPayloadSize = 164;
    if (payload.size() != kPayloadSize)
    {
        return make_error(ErrorCode::kUnsupported, "Legacy RAW denoise payload size is unsupported",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_payload"}});
    }
    float threshold_f = 0.0F;
    std::memcpy(&threshold_f, payload.data(), sizeof(threshold_f));
    if (!std::isfinite(threshold_f) || threshold_f < 0.0F || threshold_f > 1.0F)
    {
        return make_error(ErrorCode::kUnsupported, "Legacy RAW denoise threshold is unsupported",
                          {{"legacy_operation", "rawdenoise"},
                           {"reason", "unsupported_legacy_rawdenoise_threshold"}});
    }
    threshold = threshold_f;
    constexpr float kExpectedX[5] = {0.0F, 0.25F, 0.5F, 0.75F, 1.0F};
    for (std::size_t channel = 0; channel < 4; ++channel)
    {
        for (std::size_t band = 0; band < 5; ++band)
        {
            float x = 0.0F;
            float y = 0.0F;
            std::memcpy(&x, payload.data() + 4U + (channel * 5U + band) * 4U, sizeof(x));
            std::memcpy(&y, payload.data() + 84U + (channel * 5U + band) * 4U, sizeof(y));
            if (!std::isfinite(x) || std::abs(x - kExpectedX[band]) > 1.0e-5F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RAW denoise band positions are unsupported",
                                  {{"legacy_operation", "rawdenoise"},
                                   {"reason", "unsupported_legacy_rawdenoise_bands"}});
            }
            if (!std::isfinite(y) || y < 0.0F || y > 16.0F)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Legacy RAW denoise band values are unsupported",
                                  {{"legacy_operation", "rawdenoise"},
                                   {"reason", "unsupported_legacy_rawdenoise_bands"}});
            }
            bands[channel][band] = y;
        }
    }
    return {};
}

std::map<std::string, ParameterValue, std::less<>>
raw_denoise_to_parameters(const double threshold, const std::array<std::array<double, 5>, 4> &bands)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"threshold", ParameterValue{threshold}}};
    const char *names[4] = {"all", "red", "green", "blue"};
    for (int channel = 0; channel < 4; ++channel)
    {
        for (int band = 0; band < 5; ++band)
        {
            parameters.emplace(
                std::string("y_") + names[channel] + std::to_string(band),
                ParameterValue{
                    bands[static_cast<std::size_t>(channel)][static_cast<std::size_t>(band)]});
        }
    }
    return parameters;
}

} // namespace ravo
