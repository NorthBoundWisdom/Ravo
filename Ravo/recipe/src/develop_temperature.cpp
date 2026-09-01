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

} // namespace develop_internal

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


} // namespace ravo
