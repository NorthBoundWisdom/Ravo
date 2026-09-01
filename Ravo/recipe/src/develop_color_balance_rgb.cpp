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
#include <new>
#include <numbers>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>


#include "develop_internal.h"

namespace ravo
{
using namespace develop_internal;

namespace develop_internal
{
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

void append_color_balance_develop_names(std::vector<std::string> &names)
{
    for (const auto &field : color_balance_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
}

} // namespace develop_internal

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


} // namespace ravo
