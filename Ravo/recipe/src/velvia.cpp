#include "ravo/recipe/velvia.h"

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
[[nodiscard]] TaskError invalid(const std::string_view message,
                                const std::string_view parameter = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"reason", "invalid_velvia_parameters"}};
    if (!parameter.empty())
        context.emplace("parameter", parameter);
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    return found == parameters.end() ?
               Result<const ParameterValue *>{invalid("Required Velvia field is missing", name)} :
               Result<const ParameterValue *>{&found->second};
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    return parsed == nullptr ? Result<std::string>{invalid("Velvia field must be text", name)} :
                               Result<std::string>{*parsed};
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name, const double minimum, const double maximum)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    double parsed = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&value.value()->value); floating != nullptr)
        parsed = *floating;
    else if (const auto *integer = std::get_if<std::int64_t>(&value.value()->value);
             integer != nullptr)
        parsed = static_cast<double>(*integer);
    if (!std::isfinite(parsed) || !std::isfinite(static_cast<float>(parsed)) || parsed < minimum ||
        parsed > maximum)
        return invalid("Velvia numeric field is outside its supported range", name);
    return parsed;
}
} // namespace

Result<VelviaParams>
velvia_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 4> names{"working_space", "algorithm", "strength",
                                                    "bias"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("Velvia parameters must contain exactly four known fields");
    auto working = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto strength = number(parameters, "strength", 0.0, 100.0);
    auto bias = number(parameters, "bias", 0.0, 1.0);
    if (!working || !algorithm || !strength || !bias)
        return !working ? working.error() :
               !algorithm ? algorithm.error() :
               !strength ? strength.error() :
                           bias.error();
    if (working.value() != kVelviaWorkingSpace)
        return invalid("Velvia working space is unsupported", "working_space");
    if (algorithm.value() != kVelviaAlgorithm)
        return invalid("Velvia algorithm is unsupported", "algorithm");
    return VelviaParams{strength.value(), bias.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
velvia_to_parameters(const VelviaParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kVelviaWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kVelviaAlgorithm)}},
        {"strength", ParameterValue{params.strength}},
        {"bias", ParameterValue{params.bias}},
    };
    auto valid = velvia_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

Result<void> upgrade_velvia_operation(OperationInstance &operation)
{
    if (operation.id != kVelviaOperationId)
        return make_error(ErrorCode::kInvalidArgument, "Operation is not Velvia");
    if (operation.schema_version == kVelviaOperationSchemaVersion)
        return {};
    if (operation.schema_version != 1)
        return make_error(ErrorCode::kUnsupported, "Velvia schema version is unsupported",
                          {{"reason", "unsupported_velvia_schema"}});
    if (operation.parameters.size() != 2U || !operation.parameters.contains("amount") ||
        !operation.parameters.contains("bias"))
        return invalid("Legacy Ravo Velvia must contain amount and bias");
    const auto read = [&](const std::string_view name) -> Result<double>
    {
        const auto &value = operation.parameters.at(std::string(name)).value;
        if (const auto *floating = std::get_if<double>(&value); floating != nullptr)
            return *floating;
        if (const auto *integer = std::get_if<std::int64_t>(&value); integer != nullptr)
            return static_cast<double>(*integer);
        return invalid("Legacy Ravo Velvia field is not numeric", name);
    };
    auto amount = read("amount");
    auto bias = read("bias");
    if (!amount || !bias)
        return !amount ? amount.error() : bias.error();
    auto upgraded = velvia_to_parameters({amount.value() * 100.0, bias.value()});
    if (!upgraded)
        return upgraded.error();
    operation.schema_version = kVelviaOperationSchemaVersion;
    operation.parameters = std::move(upgraded).value();
    return {};
}

} // namespace ravo
