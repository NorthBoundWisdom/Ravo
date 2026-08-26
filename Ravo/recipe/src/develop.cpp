#include "ravo/recipe/develop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <utility>

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
                   const std::int64_t schema_version = 1)
{
    recipe.operations.push_back({std::move(id), schema_version, std::move(instance_id), true,
                                 std::move(parameters), std::nullopt});
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
    clamp_color_balance(params.color_balance_rgb);
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
    params.sharpen = clamp_value(params.sharpen, 0.0, 2.0);
    params.sharpen_radius = clamp_value(params.sharpen_radius, 0.0, 12.0);
    params.clarity = clamp_value(params.clarity, -1.0, 1.0);
    params.vignette = clamp_value(params.vignette, 0.0, 1.0);
    params.grain = clamp_value(params.grain, 0.0, 1.0);
    params.bloom = clamp_value(params.bloom, 0.0, 1.0);
    params.soften = clamp_value(params.soften, 0.0, 1.0);
    params.dehaze = clamp_value(params.dehaze, -1.0, 1.0);
    params.velvia = clamp_value(params.velvia, 0.0, 1.0);
    params.color_contrast = clamp_value(params.color_contrast, -1.0, 1.0);
    params.monochrome = clamp_value(params.monochrome, 0.0, 1.0);
    params.split_shadows_hue = clamp_value(params.split_shadows_hue, 0.0, 1.0);
    params.split_highlights_hue = clamp_value(params.split_highlights_hue, 0.0, 1.0);
    params.split_balance = clamp_value(params.split_balance, 0.0, 1.0);
    params.split_amount = clamp_value(params.split_amount, 0.0, 1.0);
    params.gamma = clamp_value(params.gamma, 0.2, 3.0);
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
    params.graduated_density = clamp_value(params.graduated_density, -4.0, 4.0);
    params.graduated_hardness = clamp_value(params.graduated_hardness, 0.0, 1.0);
    params.graduated_rotation = clamp_value(params.graduated_rotation, -180.0, 180.0);
    params.graduated_offset = clamp_value(params.graduated_offset, -1.0, 1.0);
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
    return temperature.is_identity() && !profile_gamma_enabled && input_color.is_identity() &&
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
           near(crop_width, 1.0) && near(crop_height, 1.0) && near(sharpen, 0.0) &&
           near(clarity, 0.0) && near(vignette, 0.0) && near(grain, 0.0) && near(bloom, 0.0) &&
           near(soften, 0.0) && near(dehaze, 0.0) && near(velvia, 0.0) &&
           color_balance_rgb.is_identity() && near(color_contrast, 0.0) && near(monochrome, 0.0) &&
           near(split_amount, 0.0) && near(gamma, kDevelopGammaDefault) &&
           tone_curve_is_identity(tone_curve) && !sigmoid_enabled && near(raw_highlights, 0.0) &&
           near(hot_pixels_strength, 0.0) && raw_ca_iterations == 0 && near(denoise, 0.0) &&
           near(lens_k1, 0.0) && near(lens_k2, 0.0) && near(lens_tca_r, 1.0) &&
           near(lens_tca_b, 1.0) && near(lens_vignetting, 0.0) && lens_mode != kLensModeLookup &&
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
    }
    else if (name == "profileGammaEnabled")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma_enabled = value >= 0.5;
    }
    else if (name == "profileGammaModeIndex")
    {
        auto mode = selected(kSelectableProfileGammaModes);
        if (!mode)
        {
            return false;
        }
        params.profile_gamma.mode = std::move(*mode);
    }
    else if (name == "profileGammaLinear")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.linear = value;
    }
    else if (name == "profileGammaGamma")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.gamma = value;
    }
    else if (name == "profileGammaDynamicRange")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.dynamic_range = value;
    }
    else if (name == "profileGammaGreyPoint")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.grey_point = value;
    }
    else if (name == "profileGammaShadowsRange")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.shadows_range = value;
    }
    else if (name == "profileGammaSecurityFactor")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.security_factor = value;
    }
    else if (name == "inputProfile")
    {
        auto profile = selected(kSelectableInputProfiles);
        if (!profile)
        {
            return false;
        }
        params.input_color.input_profile = std::move(*profile);
        params.input_color.input_profile_filename.clear();
    }
    else if (name == "workingProfile")
    {
        auto profile = selected(kSelectableWorkingProfiles);
        if (!profile)
        {
            return false;
        }
        params.input_color.working_profile = std::move(*profile);
        params.input_color.working_profile_filename.clear();
    }
    else if (name == "renderingIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.input_color.rendering_intent = std::move(*intent);
    }
    else if (name == "gamutNormalize")
    {
        auto normalize = selected(kSelectableColorNormalizations);
        if (!normalize)
        {
            return false;
        }
        params.input_color.gamut_normalize = std::move(*normalize);
    }
    else if (name == "blueMapping")
    {
        params.input_color.blue_mapping = value >= 0.5;
    }
    else if (name == "outputProfile")
    {
        auto profile = selected(kSelectableOutputProfiles);
        if (!profile)
        {
            return false;
        }
        params.output_color.output_profile = std::move(*profile);
        params.output_color.output_profile_filename.clear();
    }
    else if (name == "outputRenderingIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.output_color.rendering_intent = std::move(*intent);
    }
    else if (name == "proofMode")
    {
        auto mode = selected(kSelectableProofModes);
        if (!mode)
        {
            return false;
        }
        params.output_color.proof_mode = std::move(*mode);
    }
    else if (name == "proofProfile")
    {
        auto profile = selected(kSelectableProofProfiles);
        if (!profile)
        {
            return false;
        }
        params.output_color.proof_profile = std::move(*profile);
        params.output_color.proof_profile_filename.clear();
    }
    else if (name == "proofIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.output_color.proof_intent = std::move(*intent);
    }
    else if (name == "outputBlackPointCompensation")
    {
        params.output_color.black_point_compensation = value >= 0.5;
    }
    else if (name == "primariesAchromaticHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.achromatic_tint_hue = value * std::numbers::pi / 180.0;
    }
    else if (name == "primariesAchromaticPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.achromatic_tint_purity = value;
    }
    else if (name == "primariesRedHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.red_hue = value * std::numbers::pi / 180.0;
    }
    else if (name == "primariesRedPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.red_purity = value;
    }
    else if (name == "primariesGreenHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.green_hue = value * std::numbers::pi / 180.0;
    }
    else if (name == "primariesGreenPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.green_purity = value;
    }
    else if (name == "primariesBlueHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.blue_hue = value * std::numbers::pi / 180.0;
    }
    else if (name == "primariesBluePurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.blue_purity = value;
    }
    else if (name == "channelMixerRR")
    {
        params.channel_mixer.red[0] = value;
    }
    else if (name == "channelMixerRG")
    {
        params.channel_mixer.red[1] = value;
    }
    else if (name == "channelMixerRB")
    {
        params.channel_mixer.red[2] = value;
    }
    else if (name == "channelMixerGR")
    {
        params.channel_mixer.green[0] = value;
    }
    else if (name == "channelMixerGG")
    {
        params.channel_mixer.green[1] = value;
    }
    else if (name == "channelMixerGB")
    {
        params.channel_mixer.green[2] = value;
    }
    else if (name == "channelMixerBR")
    {
        params.channel_mixer.blue[0] = value;
    }
    else if (name == "channelMixerBG")
    {
        params.channel_mixer.blue[1] = value;
    }
    else if (name == "channelMixerBB")
    {
        params.channel_mixer.blue[2] = value;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = value;
        if (std::isfinite(value))
        {
            const double white = std::exp2(-value);
            if (params.exposure_black >= white)
            {
                params.exposure_black = std::max(kExposureBlackMin, white - 0.01);
            }
        }
    }
    else if (name == "contrast")
    {
        params.contrast = value;
    }
    else if (name == "highlights")
    {
        params.highlights = value;
    }
    else if (name == "shadows")
    {
        params.shadows = value;
    }
    else if (name == "whites")
    {
        params.whites = value;
    }
    else if (name == "blacks")
    {
        params.blacks = value;
    }
    else if (name == "vibrance")
    {
        params.vibrance = value;
    }
    else if (name == "saturation")
    {
        params.saturation = value;
    }
    else if (name == "straighten")
    {
        params.straighten_degrees = value;
    }
    else if (name == "cropX")
    {
        params.crop_x = value;
    }
    else if (name == "cropY")
    {
        params.crop_y = value;
    }
    else if (name == "cropWidth")
    {
        params.crop_width = value;
    }
    else if (name == "cropHeight")
    {
        params.crop_height = value;
    }
    else if (name == "sharpen")
    {
        params.sharpen = value;
    }
    else if (name == "sharpenRadius")
    {
        params.sharpen_radius = value;
    }
    else if (name == "clarity")
    {
        params.clarity = value;
    }
    else if (name == "vignette")
    {
        params.vignette = value;
    }
    else if (name == "grain")
    {
        params.grain = value;
    }
    else if (name == "bloom")
    {
        params.bloom = value;
    }
    else if (name == "soften")
    {
        params.soften = value;
    }
    else if (name == "dehaze")
    {
        params.dehaze = value;
    }
    else if (name == "velvia")
    {
        params.velvia = value;
    }
    else if (apply_color_balance_field(params.color_balance_rgb, name, value))
    {
    }
    else if (name == "colorContrast")
    {
        params.color_contrast = value;
    }
    else if (name == "monochrome")
    {
        params.monochrome = value;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_shadows_hue = value;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_highlights_hue = value;
    }
    else if (name == "splitBalance")
    {
        params.split_balance = value;
    }
    else if (name == "splitAmount")
    {
        params.split_amount = value;
    }
    else if (name == "gamma")
    {
        params.gamma = value;
    }
    else if (name == "sigmoidContrast")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_contrast = value;
    }
    else if (name == "sigmoidSkew")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_skew = value;
    }
    else if (name == "sigmoidHuePreservation")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_hue_preservation = value;
    }
    else if (name == "rawHighlights")
    {
        params.raw_highlights = value;
    }
    else if (name == "rawHighlightsClip")
    {
        params.raw_highlights_clip = value;
    }
    else if (name == "rawHighlightsMode")
    {
        params.raw_highlights_mode = value >= 0.5 ? std::string(kRawHighlightsModeInpaint) :
                                                    std::string(kRawHighlightsModeClip);
    }
    else if (name == "hotPixelsStrength")
    {
        params.hot_pixels_strength = value;
    }
    else if (name == "hotPixelsThreshold")
    {
        params.hot_pixels_threshold = value;
    }
    else if (name == "hotPixelsPermissive")
    {
        params.hot_pixels_permissive = value >= 0.5;
    }
    else if (name == "rawCaIterations")
    {
        params.raw_ca_iterations = static_cast<std::int64_t>(std::llround(value));
    }
    else if (name == "rawCaAvoidShift")
    {
        params.raw_ca_avoid_shift = value >= 0.5;
    }
    else if (name == "denoise")
    {
        params.denoise = value;
    }
    else if (name == "denoiseChroma")
    {
        params.denoise_chroma = value;
    }
    else if (name == "denoiseRadius")
    {
        params.denoise_radius = value;
    }
    else if (name == "lensK1")
    {
        params.lens_k1 = value;
    }
    else if (name == "lensK2")
    {
        params.lens_k2 = value;
    }
    else if (name == "lensTcaR")
    {
        params.lens_tca_r = value;
    }
    else if (name == "lensTcaB")
    {
        params.lens_tca_b = value;
    }
    else if (name == "lensVignetting")
    {
        params.lens_vignetting = value;
    }
    else if (name == "lensMode")
    {
        params.lens_mode =
            value >= 0.5 ? std::string(kLensModeLookup) : std::string(kLensModeManual);
    }
    else if (name == "lensFocal")
    {
        params.lens_focal_mm = value;
    }
    else if (name == "colorEqBand")
    {
        params.color_eq_band = static_cast<std::int64_t>(std::llround(value));
    }
    else if (name == "colorEqHue")
    {
        params.color_eq_hue[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;
    }
    else if (name == "colorEqSat")
    {
        params.color_eq_sat[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;
    }
    else if (name == "colorEqLight")
    {
        params.color_eq_light[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;
    }
    else if (name == "graduatedDensity")
    {
        params.graduated_density = value;
    }
    else if (name == "graduatedHardness")
    {
        params.graduated_hardness = value;
    }
    else if (name == "graduatedRotation")
    {
        params.graduated_rotation = value;
    }
    else if (name == "graduatedOffset")
    {
        params.graduated_offset = value;
    }
    else if (name == "toneEqBlacks")
    {
        params.tone_eq_blacks = value;
    }
    else if (name == "toneEqShadows")
    {
        params.tone_eq_shadows = value;
    }
    else if (name == "toneEqMidtones")
    {
        params.tone_eq_midtones = value;
    }
    else if (name == "toneEqHighlights")
    {
        params.tone_eq_highlights = value;
    }
    else if (name == "toneEqWhites")
    {
        params.tone_eq_whites = value;
    }
    else
    {
        std::size_t band = 0;
        if (parse_band_field(name, "colorEqHue", band))
        {
            params.color_eq_hue[band] = value;
        }
        else if (parse_band_field(name, "colorEqSat", band))
        {
            params.color_eq_sat[band] = value;
        }
        else if (parse_band_field(name, "colorEqLight", band))
        {
            params.color_eq_light[band] = value;
        }
        else
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool apply_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
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

bool reset_develop_field(DevelopParams &params, const std::string_view name)
{
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
    else if (name == "exposure")
    {
        params.exposure_ev = identity.exposure_ev;
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
    }
    else if (name == "sharpen" || name == "sharpenRadius")
    {
        params.sharpen = identity.sharpen;
        if (name == "sharpenRadius")
        {
            params.sharpen_radius = identity.sharpen_radius;
        }
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
    else if (name == "velvia")
    {
        params.velvia = identity.velvia;
    }
    else if (reset_color_balance_field(params.color_balance_rgb, name))
    {
    }
    else if (name == "colorContrast")
    {
        params.color_contrast = identity.color_contrast;
    }
    else if (name == "monochrome")
    {
        params.monochrome = identity.monochrome;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_shadows_hue = identity.split_shadows_hue;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_highlights_hue = identity.split_highlights_hue;
    }
    else if (name == "splitBalance")
    {
        params.split_balance = identity.split_balance;
    }
    else if (name == "splitAmount")
    {
        params.split_amount = identity.split_amount;
    }
    else if (name == "gamma")
    {
        params.gamma = identity.gamma;
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
        params.color_balance_rgb = identity.color_balance_rgb;
        params.color_contrast = identity.color_contrast;
        params.monochrome = identity.monochrome;
        params.split_shadows_hue = identity.split_shadows_hue;
        params.split_highlights_hue = identity.split_highlights_hue;
        params.split_balance = identity.split_balance;
        params.split_amount = identity.split_amount;
        params.color_eq_hue = {};
        params.color_eq_sat = {};
        params.color_eq_light = {};
        params.color_eq_band = 0;
    }
    else if (section == "detail")
    {
        params.sharpen = identity.sharpen;
        params.sharpen_radius = identity.sharpen_radius;
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
    clamp_develop(params);
    return true;
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
    if (!clamped.temperature.is_identity())
    {
        add_operation(recipe, "ravo.color.temperature", "temperature-1",
                      temperature_to_parameters(clamped.temperature));
    }
    if (clamped.profile_gamma_enabled)
    {
        auto profile_gamma = profile_gamma_to_parameters(clamped.profile_gamma);
        if (!profile_gamma)
        {
            return profile_gamma.error();
        }
        add_operation(recipe, std::string(kProfileGammaOperationId), "profilegamma-1",
                      std::move(profile_gamma).value());
    }
    add_operation(recipe, "ravo.color.input", "color-input-1",
                  input_color_to_parameters(clamped.input_color));
    if (!clamped.primaries.is_identity())
    {
        add_operation(recipe, std::string(kPrimariesOperationId), "primaries-1",
                      primaries_to_parameters(clamped.primaries));
    }
    if (!clamped.channel_mixer.is_identity())
    {
        add_operation(recipe, "ravo.color.channelmixerrgb", "channelmixerrgb-1",
                      channel_mixer_to_parameters(clamped.channel_mixer));
    }
    if (!near(clamped.hot_pixels_strength, 0.0))
    {
        add_operation(recipe, "ravo.raw.hotpixels", "hotpixels-1",
                      {{"strength", ParameterValue{clamped.hot_pixels_strength}},
                       {"threshold", ParameterValue{clamped.hot_pixels_threshold}},
                       {"permissive", ParameterValue{clamped.hot_pixels_permissive}}});
    }
    if (!near(clamped.raw_highlights, 0.0))
    {
        add_operation(recipe, "ravo.raw.highlights", "raw-highlights-1",
                      {{"mode", ParameterValue{clamped.raw_highlights_mode}},
                       {"amount", ParameterValue{clamped.raw_highlights}},
                       {"clip", ParameterValue{clamped.raw_highlights_clip}}});
    }
    if (clamped.raw_ca_iterations > 0)
    {
        add_operation(recipe, "ravo.raw.cacorrect", "cacorrect-1",
                      {{"iterations", ParameterValue{clamped.raw_ca_iterations}},
                       {"avoid_color_shift", ParameterValue{clamped.raw_ca_avoid_shift}}});
    }
    if (!near(clamped.denoise, 0.0))
    {
        add_operation(recipe, "ravo.detail.denoiseprofile", "denoiseprofile-1",
                      {{"strength", ParameterValue{clamped.denoise}},
                       {"chroma", ParameterValue{clamped.denoise_chroma}},
                       {"radius", ParameterValue{clamped.denoise_radius}}});
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
                       {"focal_mm", ParameterValue{clamped.lens_focal_mm}}});
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
                      exposure_to_parameters(exposure), kExposureOperationSchemaVersion);
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
                       {"whites", ParameterValue{clamped.tone_eq_whites}}});
    }
    if (!near(clamped.graduated_density, 0.0))
    {
        add_operation(recipe, "ravo.effect.graduatednd", "graduatednd-1",
                      {{"density_ev", ParameterValue{clamped.graduated_density}},
                       {"hardness", ParameterValue{clamped.graduated_hardness}},
                       {"rotation_deg", ParameterValue{clamped.graduated_rotation}},
                       {"offset", ParameterValue{clamped.graduated_offset}}});
    }
    if (!near(clamped.highlights, 0.0))
    {
        add_operation(recipe, "ravo.core.highlights", "highlights-1",
                      {{"amount", ParameterValue{clamped.highlights}}});
    }
    if (!near(clamped.shadows, 0.0))
    {
        add_operation(recipe, "ravo.core.shadows", "shadows-1",
                      {{"amount", ParameterValue{clamped.shadows}}});
    }
    if (!near(clamped.whites, 0.0))
    {
        add_operation(recipe, "ravo.core.whites", "whites-1",
                      {{"amount", ParameterValue{clamped.whites}}});
    }
    if (!near(clamped.blacks, 0.0))
    {
        add_operation(recipe, "ravo.core.blacks", "blacks-1",
                      {{"amount", ParameterValue{clamped.blacks}}});
    }
    if (!near(clamped.contrast, 0.0))
    {
        add_operation(recipe, "ravo.core.contrast", "contrast-1",
                      {{"amount", ParameterValue{clamped.contrast}}});
    }
    if (!near(clamped.gamma, kDevelopGammaDefault))
    {
        add_operation(recipe, "ravo.core.gamma", "gamma-1",
                      {{"gamma", ParameterValue{clamped.gamma}}});
    }
    if (!tone_curve_is_identity(clamped.tone_curve))
    {
        add_operation(
            recipe, "ravo.core.tonecurve", "tonecurve-1",
            {{"working_space", ParameterValue{clamped.tone_curve_working_space}},
             {"interpolation", ParameterValue{std::string(kToneCurveInterpolationMonotoneHermite)}},
             {"channel_mode", ParameterValue{std::string(kToneCurveChannelModeRgb)}},
             {"preserve_colors", ParameterValue{std::string(kToneCurvePreserveColorsAverage)}},
             {"points", tone_curve_points_to_parameter(clamped.tone_curve)}});
    }
    if (!clamped.color_balance_rgb.is_identity())
    {
        add_operation(recipe, "ravo.color.colorbalancergb", "colorbalancergb-1",
                      color_balance_rgb_to_parameters(clamped.color_balance_rgb));
    }
    if (!near(clamped.color_contrast, 0.0))
    {
        add_operation(recipe, "ravo.color.colorcontrast", "colorcontrast-1",
                      {{"amount", ParameterValue{clamped.color_contrast}}});
    }
    if (!near(clamped.velvia, 0.0))
    {
        add_operation(recipe, "ravo.color.velvia", "velvia-1",
                      {{"amount", ParameterValue{clamped.velvia}}, {"bias", ParameterValue{1.0}}});
    }
    if (!near(clamped.vibrance, 0.0))
    {
        add_operation(recipe, "ravo.color.vibrance", "vibrance-1",
                      {{"amount", ParameterValue{clamped.vibrance}}});
    }
    if (!near(clamped.saturation, 0.0))
    {
        add_operation(recipe, "ravo.color.saturation", "saturation-1",
                      {{"amount", ParameterValue{clamped.saturation}}});
    }
    if (!bands_near_zero(clamped.color_eq_hue) || !bands_near_zero(clamped.color_eq_sat) ||
        !bands_near_zero(clamped.color_eq_light))
    {
        add_operation(recipe, "ravo.color.colorequal", "colorequal-1",
                      {{"hue_shift", band_array_parameter(clamped.color_eq_hue)},
                       {"saturation", band_array_parameter(clamped.color_eq_sat)},
                       {"lightness", band_array_parameter(clamped.color_eq_light)}});
    }
    if (!near(clamped.monochrome, 0.0))
    {
        add_operation(recipe, "ravo.color.monochrome", "monochrome-1",
                      {{"amount", ParameterValue{clamped.monochrome}}});
    }
    if (!near(clamped.split_amount, 0.0))
    {
        add_operation(recipe, "ravo.color.splittoning", "splittoning-1",
                      {{"shadows_hue", ParameterValue{clamped.split_shadows_hue}},
                       {"highlights_hue", ParameterValue{clamped.split_highlights_hue}},
                       {"balance", ParameterValue{clamped.split_balance}},
                       {"amount", ParameterValue{clamped.split_amount}}});
    }
    if (!near(clamped.sharpen, 0.0))
    {
        add_operation(recipe, "ravo.detail.sharpen", "sharpen-1",
                      {{"amount", ParameterValue{clamped.sharpen}},
                       {"radius", ParameterValue{clamped.sharpen_radius}},
                       {"threshold", ParameterValue{0.5}}});
    }
    if (!near(clamped.clarity, 0.0))
    {
        add_operation(recipe, "ravo.detail.clarity", "clarity-1",
                      {{"amount", ParameterValue{clamped.clarity}}});
    }
    if (!near(clamped.bloom, 0.0))
    {
        add_operation(recipe, "ravo.effect.bloom", "bloom-1",
                      {{"amount", ParameterValue{clamped.bloom}}});
    }
    if (!near(clamped.soften, 0.0))
    {
        add_operation(recipe, "ravo.effect.soften", "soften-1",
                      {{"amount", ParameterValue{clamped.soften}}});
    }
    if (!near(clamped.dehaze, 0.0))
    {
        add_operation(recipe, "ravo.effect.dehaze", "dehaze-1",
                      {{"amount", ParameterValue{clamped.dehaze}}});
    }
    if (!near(clamped.vignette, 0.0))
    {
        add_operation(recipe, "ravo.effect.vignette", "vignette-1",
                      {{"amount", ParameterValue{clamped.vignette}},
                       {"midpoint", ParameterValue{0.8}},
                       {"falloff", ParameterValue{0.5}}});
    }
    if (!near(clamped.grain, 0.0))
    {
        add_operation(recipe, "ravo.effect.grain", "grain-1",
                      {{"amount", ParameterValue{clamped.grain}}});
    }
    if (clamped.rotate_quarters % 4 != 0)
    {
        add_operation(recipe, "ravo.geometry.rotate", "rotate-1",
                      {{"quarters", ParameterValue{clamped.rotate_quarters % 4}}});
    }
    if (clamped.flip_horizontal != 0 || clamped.flip_vertical != 0)
    {
        add_operation(recipe, "ravo.geometry.flip", "flip-1",
                      {{"horizontal", ParameterValue{clamped.flip_horizontal}},
                       {"vertical", ParameterValue{clamped.flip_vertical}}});
    }
    if (!near(clamped.straighten_degrees, 0.0))
    {
        add_operation(recipe, "ravo.geometry.straighten", "straighten-1",
                      {{"degrees", ParameterValue{clamped.straighten_degrees}}});
    }
    if (!near(clamped.crop_x, 0.0) || !near(clamped.crop_y, 0.0) ||
        !near(clamped.crop_width, 1.0) || !near(clamped.crop_height, 1.0))
    {
        add_operation(recipe, "ravo.geometry.crop", "crop-1",
                      {{"x", ParameterValue{clamped.crop_x}},
                       {"y", ParameterValue{clamped.crop_y}},
                       {"width", ParameterValue{clamped.crop_width}},
                       {"height", ParameterValue{clamped.crop_height}}});
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
             {"hue_preservation", ParameterValue{clamped.sigmoid_hue_preservation}}});
    }
    add_operation(recipe, "ravo.color.output", "color-output-1",
                  output_color_to_parameters(clamped.output_color));
    return recipe;
}

Result<DevelopParams> develop_from_recipe(const Recipe &recipe)
{
    DevelopParams params;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
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
        }
        else if (operation.id == kProfileGammaOperationId)
        {
            auto profile_gamma = profile_gamma_from_parameters(operation.parameters);
            if (!profile_gamma)
            {
                return profile_gamma.error();
            }
            params.profile_gamma_enabled = true;
            params.profile_gamma = std::move(profile_gamma).value();
        }
        else if (operation.id == "ravo.color.input")
        {
            auto input_color = input_color_from_parameters(operation.parameters);
            if (!input_color)
            {
                return input_color.error();
            }
            params.input_color = std::move(input_color).value();
        }
        else if (operation.id == kPrimariesOperationId)
        {
            auto primaries = primaries_from_parameters(operation.parameters);
            if (!primaries)
            {
                return primaries.error();
            }
            params.primaries = std::move(primaries).value();
        }
        else if (operation.id == "ravo.color.output")
        {
            auto output_color = output_color_from_parameters(operation.parameters);
            if (!output_color)
            {
                return output_color.error();
            }
            params.output_color = std::move(output_color).value();
        }
        else if (operation.id == "ravo.color.channelmixerrgb")
        {
            auto mixer = channel_mixer_from_parameters(operation.parameters);
            if (!mixer)
            {
                return mixer.error();
            }
            params.channel_mixer = std::move(mixer).value();
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
        }
        else if (operation.id == "ravo.core.contrast")
        {
            params.contrast = number("amount", params.contrast);
        }
        else if (operation.id == "ravo.core.highlights")
        {
            params.highlights = number("amount", params.highlights);
        }
        else if (operation.id == "ravo.core.shadows")
        {
            params.shadows = number("amount", params.shadows);
        }
        else if (operation.id == "ravo.core.whites")
        {
            params.whites = number("amount", params.whites);
        }
        else if (operation.id == "ravo.core.blacks")
        {
            params.blacks = number("amount", params.blacks);
        }
        else if (operation.id == "ravo.core.gamma")
        {
            params.gamma = number("gamma", params.gamma);
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
        }
        else if (operation.id == "ravo.color.vibrance")
        {
            params.vibrance = number("amount", params.vibrance);
        }
        else if (operation.id == "ravo.color.saturation")
        {
            params.saturation = number("amount", params.saturation);
        }
        else if (operation.id == "ravo.color.velvia")
        {
            params.velvia = number("amount", params.velvia);
        }
        else if (operation.id == "ravo.color.colorbalancergb")
        {
            auto color_balance = color_balance_rgb_from_parameters(operation.parameters);
            if (!color_balance)
            {
                return color_balance.error();
            }
            params.color_balance_rgb = std::move(color_balance).value();
        }
        else if (operation.id == "ravo.color.colorcontrast")
        {
            params.color_contrast = number("amount", params.color_contrast);
        }
        else if (operation.id == "ravo.color.monochrome")
        {
            params.monochrome = number("amount", params.monochrome);
        }
        else if (operation.id == "ravo.color.splittoning")
        {
            params.split_shadows_hue = number("shadows_hue", params.split_shadows_hue);
            params.split_highlights_hue = number("highlights_hue", params.split_highlights_hue);
            params.split_balance = number("balance", params.split_balance);
            params.split_amount = number("amount", params.split_amount);
        }
        else if (operation.id == "ravo.detail.sharpen")
        {
            params.sharpen = number("amount", params.sharpen);
            params.sharpen_radius = number("radius", params.sharpen_radius);
        }
        else if (operation.id == "ravo.detail.clarity")
        {
            params.clarity = number("amount", params.clarity);
        }
        else if (operation.id == "ravo.effect.vignette")
        {
            params.vignette = number("amount", params.vignette);
        }
        else if (operation.id == "ravo.effect.grain")
        {
            params.grain = number("amount", params.grain);
        }
        else if (operation.id == "ravo.effect.bloom")
        {
            params.bloom = number("amount", params.bloom);
        }
        else if (operation.id == "ravo.effect.soften")
        {
            params.soften = number("amount", params.soften);
        }
        else if (operation.id == "ravo.effect.dehaze")
        {
            params.dehaze = number("amount", params.dehaze);
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            params.rotate_quarters = integer("quarters", 0) % 4;
            if (params.rotate_quarters < 0)
            {
                params.rotate_quarters += 4;
            }
        }
        else if (operation.id == "ravo.geometry.flip")
        {
            params.flip_horizontal = flag01(integer("horizontal", 0));
            params.flip_vertical = flag01(integer("vertical", 0));
        }
        else if (operation.id == "ravo.geometry.straighten")
        {
            params.straighten_degrees = number("degrees", params.straighten_degrees);
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            params.crop_x = number("x", params.crop_x);
            params.crop_y = number("y", params.crop_y);
            params.crop_width = number("width", params.crop_width);
            params.crop_height = number("height", params.crop_height);
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
        }
        else if (operation.id == "ravo.detail.denoiseprofile")
        {
            params.denoise = number("strength", params.denoise);
            params.denoise_chroma = number("chroma", params.denoise_chroma);
            params.denoise_radius = number("radius", params.denoise_radius);
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
        }
        else if (operation.id == "ravo.effect.graduatednd")
        {
            params.graduated_density = number("density_ev", params.graduated_density);
            params.graduated_hardness = number("hardness", params.graduated_hardness);
            params.graduated_rotation = number("rotation_deg", params.graduated_rotation);
            params.graduated_offset = number("offset", params.graduated_offset);
        }
        else if (operation.id == "ravo.core.toneequal")
        {
            params.tone_eq_blacks = number("blacks", params.tone_eq_blacks);
            params.tone_eq_shadows = number("shadows", params.tone_eq_shadows);
            params.tone_eq_midtones = number("midtones", params.tone_eq_midtones);
            params.tone_eq_highlights = number("highlights", params.tone_eq_highlights);
            params.tone_eq_whites = number("whites", params.tone_eq_whites);
        }
    }
    clamp_develop(params);
    return params;
}

} // namespace ravo
