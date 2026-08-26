#include "ravo/recipe/color_correction.h"

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

[[nodiscard]] TaskError invalid_parameter(const std::string_view message,
                                          const std::string_view parameter = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"reason", "invalid_colorcorrection_parameters"}};
    if (!parameter.empty())
    {
        context.emplace("parameter", parameter);
    }
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
    {
        return invalid_parameter("Color Correction parameter is required", name);
    }
    return &found->second;
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
    {
        return value.error();
    }
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    if (parsed == nullptr)
    {
        return invalid_parameter("Color Correction parameter must be a string", name);
    }
    return *parsed;
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name, const double minimum, const double maximum)
{
    auto value = required(parameters, name);
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
    const float narrowed = static_cast<float>(parsed);
    if (!std::isfinite(parsed) || !std::isfinite(narrowed))
    {
        return invalid_parameter(
            "Color Correction parameter must be finite and representable as float", name);
    }
    if (parsed < minimum || parsed > maximum)
    {
        return invalid_parameter("Color Correction parameter is outside its supported range", name);
    }
    return parsed;
}

} // namespace

Result<ColorCorrectionParams> color_correction_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 7> names{"working_space", "algorithm", "highlight_a",
                                                    "highlight_b",   "shadow_a",  "shadow_b",
                                                    "saturation"};
    if (parameters.size() != names.size())
    {
        return invalid_parameter("Color Correction parameters must contain exactly seven fields");
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Color Correction parameter is unknown", name);
        }
    }

    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto highlight_a =
        number(parameters, "highlight_a", kColorCorrectionEndpointMin, kColorCorrectionEndpointMax);
    auto highlight_b =
        number(parameters, "highlight_b", kColorCorrectionEndpointMin, kColorCorrectionEndpointMax);
    auto shadow_a =
        number(parameters, "shadow_a", kColorCorrectionEndpointMin, kColorCorrectionEndpointMax);
    auto shadow_b =
        number(parameters, "shadow_b", kColorCorrectionEndpointMin, kColorCorrectionEndpointMax);
    auto saturation = number(parameters, "saturation", kColorCorrectionSaturationMin,
                             kColorCorrectionSaturationMax);
    if (!working_space || !algorithm || !highlight_a || !highlight_b || !shadow_a || !shadow_b ||
        !saturation)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
               !highlight_a   ? highlight_a.error() :
               !highlight_b   ? highlight_b.error() :
               !shadow_a      ? shadow_a.error() :
               !shadow_b      ? shadow_b.error() :
                                saturation.error();
    }
    if (working_space.value() != kColorCorrectionWorkingSpaceLabD50)
    {
        return invalid_parameter("Color Correction working space is unsupported", "working_space");
    }
    if (algorithm.value() != kColorCorrectionAlgorithmAffineLabV1)
    {
        return invalid_parameter("Color Correction algorithm is unsupported", "algorithm");
    }
    return ColorCorrectionParams{highlight_a.value(), highlight_b.value(), shadow_a.value(),
                                 shadow_b.value(), saturation.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
color_correction_to_parameters(const ColorCorrectionParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorCorrectionWorkingSpaceLabD50)}},
        {"algorithm", ParameterValue{std::string(kColorCorrectionAlgorithmAffineLabV1)}},
        {"highlight_a", ParameterValue{params.highlight_a}},
        {"highlight_b", ParameterValue{params.highlight_b}},
        {"shadow_a", ParameterValue{params.shadow_a}},
        {"shadow_b", ParameterValue{params.shadow_b}},
        {"saturation", ParameterValue{params.saturation}},
    };
    auto validated = color_correction_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void> validate_color_correction_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_correction_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

} // namespace ravo
