#include "ravo/recipe/dehaze.h"

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
        {"reason", "invalid_dehaze_parameters"}};
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
        return invalid_parameter("Dehaze parameter is required", name);
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
        return invalid_parameter("Dehaze parameter must be a string", name);
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
        return invalid_parameter("Dehaze parameter must be finite and representable as float",
                                 name);
    }
    if (parsed < minimum || parsed > maximum)
    {
        return invalid_parameter("Dehaze parameter is outside its supported range", name);
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
        return invalid_parameter("Dehaze parameter must be boolean", name);
    }
    return *parsed;
}

} // namespace

Result<DehazeParams>
dehaze_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 5> names{"working_space", "algorithm", "strength",
                                                    "distance", "adaptive"};
    if (parameters.size() != names.size())
    {
        return invalid_parameter("Dehaze parameters must contain exactly five fields");
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Dehaze parameter is unknown", name);
        }
    }
    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto strength = number(parameters, "strength", kDehazeStrengthMin, kDehazeStrengthMax);
    auto distance = number(parameters, "distance", kDehazeDistanceMin, kDehazeDistanceMax);
    auto adaptive = boolean(parameters, "adaptive");
    if (!working_space || !algorithm || !strength || !distance || !adaptive)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
               !strength      ? strength.error() :
               !distance      ? distance.error() :
                                adaptive.error();
    }
    if (working_space.value() != kDehazeWorkingSpaceSourceLinearRgb)
    {
        return invalid_parameter("Dehaze working space is unsupported", "working_space");
    }
    if (algorithm.value() != kDehazeAlgorithmDarkChannelGuidedV4)
    {
        return invalid_parameter("Dehaze algorithm is unsupported", "algorithm");
    }
    return DehazeParams{strength.value(), distance.value(), adaptive.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
dehaze_to_parameters(const DehazeParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kDehazeWorkingSpaceSourceLinearRgb)}},
        {"algorithm", ParameterValue{std::string(kDehazeAlgorithmDarkChannelGuidedV4)}},
        {"strength", ParameterValue{params.strength}},
        {"distance", ParameterValue{params.distance}},
        {"adaptive", ParameterValue{params.adaptive}},
    };
    auto validated = dehaze_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void>
validate_dehaze_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = dehaze_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

Result<void> upgrade_dehaze_operation(OperationInstance &operation)
{
    if (operation.id != kDehazeOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Dehaze",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version == kDehazeOperationSchemaVersion)
    {
        return {};
    }
    if (operation.schema_version != 1)
    {
        return make_error(ErrorCode::kUnsupported, "Dehaze operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.parameters.size() > 1U ||
        (operation.parameters.size() == 1U && !operation.parameters.contains("amount")))
    {
        return invalid_parameter("Dehaze v1 contains an unknown parameter");
    }
    double amount = 0.0;
    if (operation.parameters.contains("amount"))
    {
        auto parsed =
            number(operation.parameters, "amount", kDehazeStrengthMin, kDehazeStrengthMax);
        if (!parsed)
        {
            return parsed.error();
        }
        amount = parsed.value();
    }
    auto canonical = dehaze_to_parameters({amount, 0.2, true});
    if (!canonical)
    {
        return canonical.error();
    }
    operation.schema_version = kDehazeOperationSchemaVersion;
    operation.parameters = std::move(canonical).value();
    return {};
}

} // namespace ravo
