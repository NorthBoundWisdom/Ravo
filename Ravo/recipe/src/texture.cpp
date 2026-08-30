#include "ravo/recipe/texture.h"

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
        {"reason", "invalid_texture_parameters"}};
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
        return invalid_parameter("Texture parameter is required", name);
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
        return invalid_parameter("Texture parameter must be a string", name);
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
    if (!std::isfinite(parsed) || !std::isfinite(static_cast<float>(parsed)))
    {
        return invalid_parameter("Texture parameter must be finite and representable as float",
                                 name);
    }
    if (parsed < minimum || parsed > maximum)
    {
        return invalid_parameter("Texture parameter is outside its supported range", name);
    }
    return parsed;
}

[[nodiscard]] Result<std::int64_t>
integer(const std::map<std::string, ParameterValue, std::less<>> &parameters,
        const std::string_view name, const std::int64_t minimum, const std::int64_t maximum)
{
    auto value = required(parameters, name);
    if (!value)
    {
        return value.error();
    }
    const auto *parsed = std::get_if<std::int64_t>(&value.value()->value);
    if (parsed == nullptr)
    {
        return invalid_parameter("Texture parameter must be an integer", name);
    }
    if (*parsed < minimum || *parsed > maximum)
    {
        return invalid_parameter("Texture parameter is outside its supported range", name);
    }
    return *parsed;
}

} // namespace

Result<TextureParams>
texture_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 5> names{"working_space", "algorithm", "strength",
                                                    "detail_threshold", "iterations"};
    if (parameters.size() != names.size())
    {
        return invalid_parameter("Texture parameters must contain exactly five fields");
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Texture parameter is unknown", name);
        }
    }
    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto strength = number(parameters, "strength", kTextureStrengthMin, kTextureStrengthMax);
    auto detail_threshold = number(parameters, "detail_threshold", kTextureDetailThresholdMin,
                                   kTextureDetailThresholdMax);
    auto iterations =
        integer(parameters, "iterations", kTextureIterationsMin, kTextureIterationsMax);
    if (!working_space || !algorithm || !strength || !detail_threshold || !iterations)
    {
        return !working_space    ? working_space.error() :
               !algorithm        ? algorithm.error() :
               !strength         ? strength.error() :
               !detail_threshold ? detail_threshold.error() :
                                   iterations.error();
    }
    if (working_space.value() != kTextureWorkingSpaceLinearRec709)
    {
        return invalid_parameter("Texture working space is unsupported", "working_space");
    }
    if (algorithm.value() != kTextureAlgorithmGuidedLuminanceTwoBandV1)
    {
        return invalid_parameter("Texture algorithm is unsupported", "algorithm");
    }
    return TextureParams{strength.value(), detail_threshold.value(), iterations.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
texture_to_parameters(const TextureParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kTextureWorkingSpaceLinearRec709)}},
        {"algorithm", ParameterValue{std::string(kTextureAlgorithmGuidedLuminanceTwoBandV1)}},
        {"strength", ParameterValue{params.strength}},
        {"detail_threshold", ParameterValue{params.detail_threshold}},
        {"iterations", ParameterValue{params.iterations}},
    };
    auto validated = texture_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void>
validate_texture_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = texture_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

} // namespace ravo
