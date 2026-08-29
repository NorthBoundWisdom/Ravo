#include "ravo/recipe/monochrome.h"

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
        {"reason", "invalid_monochrome_parameters"}};
    if (!parameter.empty())
        context.emplace("parameter", parameter);
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    return found == parameters.end() ? Result<const ParameterValue *>{invalid(
                                           "Required Monochrome field is missing", name)} :
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
    return parsed == nullptr ? Result<std::string>{invalid("Monochrome field must be text", name)} :
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
        return invalid("Monochrome numeric field is outside its supported range", name);
    return parsed;
}

} // namespace

Result<MonochromeParams>
monochrome_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 7> names{
        "working_space", "algorithm", "filter_a", "filter_b", "size", "highlights", "mix"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("Monochrome parameters must contain exactly seven known fields");
    auto working = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto filter_a = number(parameters, "filter_a", -128.0, 128.0);
    auto filter_b = number(parameters, "filter_b", -128.0, 128.0);
    auto size = number(parameters, "size", 0.5, 3.0);
    auto highlights = number(parameters, "highlights", 0.0, 1.0);
    auto mix = number(parameters, "mix", 0.0, 1.0);
    if (!working || !algorithm || !filter_a || !filter_b || !size || !highlights || !mix)
        return !working    ? working.error() :
               !algorithm  ? algorithm.error() :
               !filter_a   ? filter_a.error() :
               !filter_b   ? filter_b.error() :
               !size       ? size.error() :
               !highlights ? highlights.error() :
                             mix.error();
    if (working.value() != kMonochromeWorkingSpace)
        return invalid("Monochrome working space is unsupported", "working_space");
    if (algorithm.value() != kMonochromeAlgorithm)
        return invalid("Monochrome algorithm is unsupported", "algorithm");
    return MonochromeParams{filter_a.value(), filter_b.value(), size.value(), highlights.value(),
                            mix.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
monochrome_to_parameters(const MonochromeParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kMonochromeWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kMonochromeAlgorithm)}},
        {"filter_a", ParameterValue{params.filter_a}},
        {"filter_b", ParameterValue{params.filter_b}},
        {"size", ParameterValue{params.size}},
        {"highlights", ParameterValue{params.highlights}},
        {"mix", ParameterValue{params.mix}},
    };
    auto valid = monochrome_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

Result<void> upgrade_monochrome_operation(OperationInstance &operation)
{
    if (operation.id != kMonochromeOperationId)
        return make_error(ErrorCode::kInvalidArgument, "Operation is not Monochrome");
    if (operation.schema_version == kMonochromeOperationSchemaVersion)
        return {};
    if (operation.schema_version != 1)
        return make_error(ErrorCode::kUnsupported,
                          "Monochrome operation schema version is unsupported",
                          {{"schema_version", std::to_string(operation.schema_version)},
                           {"reason", "unsupported_monochrome_schema"}});
    if (operation.parameters.size() != 1U || !operation.parameters.contains("amount"))
        return invalid("Legacy Ravo Monochrome must contain only amount");
    const auto *floating = std::get_if<double>(&operation.parameters.at("amount").value);
    const auto *integer = std::get_if<std::int64_t>(&operation.parameters.at("amount").value);
    const double amount = floating != nullptr ? *floating :
                          integer != nullptr  ? static_cast<double>(*integer) :
                                                std::numeric_limits<double>::quiet_NaN();
    MonochromeParams params;
    params.mix = amount;
    auto upgraded = monochrome_to_parameters(params);
    if (!upgraded)
        return upgraded.error();
    operation.schema_version = kMonochromeOperationSchemaVersion;
    operation.parameters = std::move(upgraded).value();
    return {};
}

} // namespace ravo
