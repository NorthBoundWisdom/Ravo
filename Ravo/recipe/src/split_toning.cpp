#include "ravo/recipe/split_toning.h"

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
        {"reason", "invalid_split_toning_parameters"}};
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
                                           "Required Split Toning field is missing", name)} :
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
    return parsed == nullptr ?
               Result<std::string>{invalid("Split Toning field must be text", name)} :
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
        return invalid("Split Toning numeric field is outside its supported range", name);
    return parsed;
}

} // namespace

Result<SplitToningParams>
split_toning_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 9> names{
        "working_space",     "algorithm",     "shadow_hue",
        "shadow_saturation", "highlight_hue", "highlight_saturation",
        "balance",           "compress",      "mix"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("Split Toning parameters must contain exactly nine known fields");
    auto working = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto shadow_hue = number(parameters, "shadow_hue", 0.0, 1.0);
    auto shadow_saturation = number(parameters, "shadow_saturation", 0.0, 1.0);
    auto highlight_hue = number(parameters, "highlight_hue", 0.0, 1.0);
    auto highlight_saturation = number(parameters, "highlight_saturation", 0.0, 1.0);
    auto balance = number(parameters, "balance", 0.0, 1.0);
    auto compress = number(parameters, "compress", 0.0, 100.0);
    auto mix = number(parameters, "mix", 0.0, 1.0);
    if (!working || !algorithm || !shadow_hue || !shadow_saturation || !highlight_hue ||
        !highlight_saturation || !balance || !compress || !mix)
        return !working              ? working.error() :
               !algorithm            ? algorithm.error() :
               !shadow_hue           ? shadow_hue.error() :
               !shadow_saturation    ? shadow_saturation.error() :
               !highlight_hue        ? highlight_hue.error() :
               !highlight_saturation ? highlight_saturation.error() :
               !balance              ? balance.error() :
               !compress             ? compress.error() :
                                       mix.error();
    if (working.value() != kSplitToningWorkingSpace)
        return invalid("Split Toning working space is unsupported", "working_space");
    if (algorithm.value() != kSplitToningAlgorithm)
        return invalid("Split Toning algorithm is unsupported", "algorithm");
    return SplitToningParams{shadow_hue.value(),
                             shadow_saturation.value(),
                             highlight_hue.value(),
                             highlight_saturation.value(),
                             balance.value(),
                             compress.value(),
                             mix.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
split_toning_to_parameters(const SplitToningParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kSplitToningWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kSplitToningAlgorithm)}},
        {"shadow_hue", ParameterValue{params.shadow_hue}},
        {"shadow_saturation", ParameterValue{params.shadow_saturation}},
        {"highlight_hue", ParameterValue{params.highlight_hue}},
        {"highlight_saturation", ParameterValue{params.highlight_saturation}},
        {"balance", ParameterValue{params.balance}},
        {"compress", ParameterValue{params.compress}},
        {"mix", ParameterValue{params.mix}},
    };
    auto valid = split_toning_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

Result<void> upgrade_split_toning_operation(OperationInstance &operation)
{
    if (operation.id != kSplitToningOperationId)
        return make_error(ErrorCode::kInvalidArgument, "Operation is not Split Toning");
    if (operation.schema_version == kSplitToningOperationSchemaVersion)
        return {};
    if (operation.schema_version != 1)
        return make_error(ErrorCode::kUnsupported,
                          "Split Toning operation schema version is unsupported",
                          {{"schema_version", std::to_string(operation.schema_version)},
                           {"reason", "unsupported_split_toning_schema"}});
    const auto get = [&](const std::string_view name, const double fallback) -> Result<double>
    {
        const auto found = operation.parameters.find(std::string(name));
        if (found == operation.parameters.end())
            return fallback;
        const auto *floating = std::get_if<double>(&found->second.value);
        const auto *integer = std::get_if<std::int64_t>(&found->second.value);
        return floating != nullptr ?
                   Result<double>{*floating} :
               integer != nullptr ?
                   Result<double>{static_cast<double>(*integer)} :
                   Result<double>{invalid("Legacy Ravo Split Toning field is invalid", name)};
    };
    if (operation.parameters.size() != 4U)
        return invalid("Legacy Ravo Split Toning must contain exactly four fields");
    auto shadow = get("shadows_hue", 0.0);
    auto highlight = get("highlights_hue", 0.2);
    auto balance = get("balance", 0.5);
    auto amount = get("amount", 0.0);
    if (!shadow || !highlight || !balance || !amount)
        return !shadow    ? shadow.error() :
               !highlight ? highlight.error() :
               !balance   ? balance.error() :
                            amount.error();
    SplitToningParams params;
    params.shadow_hue = shadow.value();
    params.highlight_hue = highlight.value();
    params.balance = balance.value();
    params.mix = amount.value();
    auto upgraded = split_toning_to_parameters(params);
    if (!upgraded)
        return upgraded.error();
    operation.schema_version = kSplitToningOperationSchemaVersion;
    operation.parameters = std::move(upgraded).value();
    return {};
}

} // namespace ravo
