#include "ravo/recipe/color_contrast.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
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
        {"reason", "invalid_colorcontrast_parameters"}};
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
        return invalid_parameter("Color Contrast parameter is required", name);
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
        return invalid_parameter("Color Contrast parameter must be a string", name);
    }
    return *parsed;
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name, const std::optional<double> minimum = std::nullopt,
       const std::optional<double> maximum = std::nullopt)
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
            "Color Contrast parameter must be finite and representable as float", name);
    }
    if ((minimum && parsed < *minimum) || (maximum && parsed > *maximum))
    {
        return invalid_parameter("Color Contrast parameter is outside its supported range", name);
    }
    return parsed;
}

[[nodiscard]] Result<bool>
boolean(const std::map<std::string, ParameterValue, std::less<>> &parameters,
        const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
    {
        return value.error();
    }
    const auto *parsed = std::get_if<bool>(&value.value()->value);
    if (parsed == nullptr)
    {
        return invalid_parameter("Color Contrast parameter must be boolean", name);
    }
    return *parsed;
}

} // namespace

Result<ColorContrastParams>
color_contrast_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 7> names{"working_space", "algorithm",   "a_steepness",
                                                    "a_offset",      "b_steepness", "b_offset",
                                                    "unbound"};
    if (parameters.size() != names.size())
    {
        return invalid_parameter("Color Contrast parameters must contain exactly seven fields");
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Color Contrast parameter is unknown", name);
        }
    }

    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto a_steepness =
        number(parameters, "a_steepness", kColorContrastSteepnessMin, kColorContrastSteepnessMax);
    auto a_offset = number(parameters, "a_offset");
    auto b_steepness =
        number(parameters, "b_steepness", kColorContrastSteepnessMin, kColorContrastSteepnessMax);
    auto b_offset = number(parameters, "b_offset");
    auto unbound = boolean(parameters, "unbound");
    if (!working_space || !algorithm || !a_steepness || !a_offset || !b_steepness || !b_offset ||
        !unbound)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
               !a_steepness   ? a_steepness.error() :
               !a_offset      ? a_offset.error() :
               !b_steepness   ? b_steepness.error() :
               !b_offset      ? b_offset.error() :
                                unbound.error();
    }
    if (working_space.value() != kColorContrastWorkingSpaceLabD50)
    {
        return invalid_parameter("Color Contrast working space is unsupported", "working_space");
    }
    if (algorithm.value() != kColorContrastAlgorithmAxisAffineV2)
    {
        return invalid_parameter("Color Contrast algorithm is unsupported", "algorithm");
    }
    return ColorContrastParams{a_steepness.value(), a_offset.value(), b_steepness.value(),
                               b_offset.value(), unbound.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
color_contrast_to_parameters(const ColorContrastParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorContrastWorkingSpaceLabD50)}},
        {"algorithm", ParameterValue{std::string(kColorContrastAlgorithmAxisAffineV2)}},
        {"a_steepness", ParameterValue{params.a_steepness}},
        {"a_offset", ParameterValue{params.a_offset}},
        {"b_steepness", ParameterValue{params.b_steepness}},
        {"b_offset", ParameterValue{params.b_offset}},
        {"unbound", ParameterValue{params.unbound}},
    };
    auto validated = color_contrast_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void> validate_color_contrast_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_contrast_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

Result<void> upgrade_color_contrast_operation(OperationInstance &operation)
{
    if (operation.id != kColorContrastOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Color Contrast",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version == kColorContrastOperationSchemaVersion)
    {
        return {};
    }
    if (operation.schema_version != 1)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Contrast operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.parameters.size() > 1U ||
        (operation.parameters.size() == 1U && !operation.parameters.contains("amount")))
    {
        return invalid_parameter("Color Contrast v1 contains an unknown parameter");
    }

    double amount = 0.0;
    if (operation.parameters.contains("amount"))
    {
        auto parsed = number(operation.parameters, "amount", -1.0, 1.0);
        if (!parsed)
        {
            auto error = parsed.error();
            error.context.emplace("operation_id", operation.id);
            return error;
        }
        amount = parsed.value();
    }
    const float slope = 1.0F + static_cast<float>(amount);
    const ColorContrastParams params{static_cast<double>(slope), 0.0, static_cast<double>(slope),
                                     0.0, true};
    auto parameters = color_contrast_to_parameters(params);
    if (!parameters)
    {
        return parameters.error();
    }
    operation.schema_version = kColorContrastOperationSchemaVersion;
    operation.parameters = std::move(parameters).value();
    operation.enabled = operation.enabled && amount != 0.0;
    return {};
}

} // namespace ravo
