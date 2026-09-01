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

void append_legacy_color_balance_develop_names(std::vector<std::string> &names)
{
    for (const auto &field : legacy_color_balance_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
}

} // namespace develop_internal

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


} // namespace ravo
