#include "ravo/recipe/sharpen.h"

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
        {"reason", "invalid_sharpen_parameters"}};
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
        return invalid_parameter("Sharpen parameter is required", name);
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
        return invalid_parameter("Sharpen parameter must be a string", name);
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
        return invalid_parameter("Sharpen parameter must be finite and representable as float",
                                 name);
    }
    if (parsed < minimum || parsed > maximum)
    {
        return invalid_parameter("Sharpen parameter is outside its supported range", name);
    }
    return parsed;
}

[[nodiscard]] Result<double>
optional_v1_number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                   const std::string_view name, const double fallback, const double minimum,
                   const double maximum)
{
    if (!parameters.contains(std::string(name)))
    {
        return fallback;
    }
    return number(parameters, name, minimum, maximum);
}

} // namespace

Result<SharpenParams>
sharpen_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 5> names{"working_space", "algorithm", "radius",
                                                    "amount", "threshold"};
    if (parameters.size() != names.size())
    {
        return invalid_parameter("Sharpen parameters must contain exactly five fields");
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Sharpen parameter is unknown", name);
        }
    }
    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto radius = number(parameters, "radius", kSharpenRadiusMin, kSharpenRadiusMax);
    auto amount = number(parameters, "amount", kSharpenAmountMin, kSharpenAmountMax);
    auto threshold = number(parameters, "threshold", kSharpenThresholdMin, kSharpenThresholdMax);
    if (!working_space || !algorithm || !radius || !amount || !threshold)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
               !radius        ? radius.error() :
               !amount        ? amount.error() :
                                threshold.error();
    }
    if (working_space.value() != kSharpenWorkingSpaceLabD50)
    {
        return invalid_parameter("Sharpen working space is unsupported", "working_space");
    }
    if (algorithm.value() != kSharpenAlgorithmSeparableGaussianUsmV1)
    {
        return invalid_parameter("Sharpen algorithm is unsupported", "algorithm");
    }
    return SharpenParams{radius.value(), amount.value(), threshold.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
sharpen_to_parameters(const SharpenParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kSharpenWorkingSpaceLabD50)}},
        {"algorithm", ParameterValue{std::string(kSharpenAlgorithmSeparableGaussianUsmV1)}},
        {"radius", ParameterValue{params.radius}},
        {"amount", ParameterValue{params.amount}},
        {"threshold", ParameterValue{params.threshold}},
    };
    auto validated = sharpen_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void>
validate_sharpen_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = sharpen_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

Result<void> upgrade_sharpen_operation(OperationInstance &operation)
{
    if (operation.id != kSharpenOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Sharpen",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version == kSharpenOperationSchemaVersion)
    {
        return {};
    }
    if (operation.schema_version != 1)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Sharpen operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    constexpr std::array<std::string_view, 3> names{"radius", "amount", "threshold"};
    for (const auto &[name, ignored] : operation.parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Sharpen v1 parameter is unknown", name);
        }
    }
    auto radius = optional_v1_number(operation.parameters, "radius", 2.0, 0.0, 12.0);
    auto amount = optional_v1_number(operation.parameters, "amount", 0.0, 0.0, 2.0);
    auto threshold = optional_v1_number(operation.parameters, "threshold", 0.5, 0.0, 100.0);
    if (!radius || !amount || !threshold)
    {
        return !radius ? radius.error() : !amount ? amount.error() : threshold.error();
    }
    auto canonical = sharpen_to_parameters({radius.value(), amount.value(), threshold.value()});
    if (!canonical)
    {
        return canonical.error();
    }
    operation.schema_version = kSharpenOperationSchemaVersion;
    operation.parameters = std::move(canonical).value();
    return {};
}

} // namespace ravo
