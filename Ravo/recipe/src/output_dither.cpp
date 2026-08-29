#include "ravo/recipe/output_dither.h"

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

struct MethodName
{
    OutputDitherMethod method;
    std::string_view name;
};

constexpr std::array<MethodName, kOutputDitherMethodCount> kMethodNames{{
    {OutputDitherMethod::kRandom, "random"},
    {OutputDitherMethod::kFloydSteinberg1BitGray, "floyd_steinberg_1bit_gray"},
    {OutputDitherMethod::kFloydSteinberg1BitRgb, "floyd_steinberg_1bit_rgb"},
    {OutputDitherMethod::kFloydSteinberg2BitGray, "floyd_steinberg_2bit_gray"},
    {OutputDitherMethod::kFloydSteinberg2BitRgb, "floyd_steinberg_2bit_rgb"},
    {OutputDitherMethod::kFloydSteinberg4BitGray, "floyd_steinberg_4bit_gray"},
    {OutputDitherMethod::kFloydSteinberg4BitRgb, "floyd_steinberg_4bit_rgb"},
    {OutputDitherMethod::kFloydSteinberg6BitGray, "floyd_steinberg_6bit_gray"},
    {OutputDitherMethod::kFloydSteinberg8BitRgb, "floyd_steinberg_8bit_rgb"},
    {OutputDitherMethod::kFloydSteinberg16BitRgb, "floyd_steinberg_16bit_rgb"},
    {OutputDitherMethod::kFloydSteinbergAuto, "floyd_steinberg_auto"},
    {OutputDitherMethod::kPosterize2, "posterize_2"},
    {OutputDitherMethod::kPosterize3, "posterize_3"},
    {OutputDitherMethod::kPosterize4, "posterize_4"},
    {OutputDitherMethod::kPosterize5, "posterize_5"},
    {OutputDitherMethod::kPosterize6, "posterize_6"},
    {OutputDitherMethod::kPosterize7, "posterize_7"},
    {OutputDitherMethod::kPosterize8, "posterize_8"},
}};

[[nodiscard]] TaskError invalid_parameter(const std::string_view message,
                                          const std::string_view parameter = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"reason", "invalid_output_dither_parameters"}};
    if (!parameter.empty())
        context.emplace("parameter", parameter);
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
        return invalid_parameter("Output dither parameter is required", name);
    return &found->second;
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    if (parsed == nullptr)
        return invalid_parameter("Output dither parameter must be a string", name);
    return *parsed;
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name)
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
    if (!std::isfinite(parsed) || !std::isfinite(static_cast<float>(parsed)) ||
        parsed < kOutputDitherDampingMin || parsed > kOutputDitherDampingMax)
    {
        return invalid_parameter("Output dither damping is outside its supported range", name);
    }
    return parsed;
}

} // namespace

std::string_view output_dither_method_name(const OutputDitherMethod method) noexcept
{
    const auto found =
        std::find_if(kMethodNames.begin(), kMethodNames.end(),
                     [method](const MethodName &entry) { return entry.method == method; });
    return found == kMethodNames.end() ? std::string_view{} : found->name;
}

Result<OutputDitherMethod> parse_output_dither_method(const std::string_view name)
{
    const auto found = std::find_if(kMethodNames.begin(), kMethodNames.end(),
                                    [name](const MethodName &entry) { return entry.name == name; });
    if (found == kMethodNames.end())
    {
        return make_error(
            ErrorCode::kValidation, "Output dither method is unsupported",
            {{"method", std::string(name)}, {"reason", "invalid_output_dither_method"}});
    }
    return found->method;
}

Result<OutputDitherMethod> output_dither_method_from_index(const std::int64_t index)
{
    if (index < 0 || index >= static_cast<std::int64_t>(kMethodNames.size()))
    {
        return make_error(
            ErrorCode::kValidation, "Output dither method index is invalid",
            {{"index", std::to_string(index)}, {"reason", "invalid_output_dither_method_index"}});
    }
    return kMethodNames[static_cast<std::size_t>(index)].method;
}

std::int64_t output_dither_method_index(const OutputDitherMethod method) noexcept
{
    const auto found =
        std::find_if(kMethodNames.begin(), kMethodNames.end(),
                     [method](const MethodName &entry) { return entry.method == method; });
    return found == kMethodNames.end() ? -1 : std::distance(kMethodNames.begin(), found);
}

Result<OutputDitherParams>
output_dither_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 4> names{"working_space", "algorithm", "method",
                                                    "random_damping_db"};
    if (parameters.size() != names.size())
        return invalid_parameter("Output dither parameters must contain exactly four fields");
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
            return invalid_parameter("Output dither parameter is unknown", name);
    }
    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto method_name = text(parameters, "method");
    auto damping = number(parameters, "random_damping_db");
    if (!working_space || !algorithm || !method_name || !damping)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
               !method_name   ? method_name.error() :
                                damping.error();
    }
    if (working_space.value() != kOutputDitherWorkingSpace)
        return invalid_parameter("Output dither working space is unsupported", "working_space");
    if (algorithm.value() != kOutputDitherAlgorithm)
        return invalid_parameter("Output dither algorithm is unsupported", "algorithm");
    auto method = parse_output_dither_method(method_name.value());
    if (!method)
        return method.error();
    return OutputDitherParams{method.value(), damping.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
output_dither_to_parameters(const OutputDitherParams &params)
{
    const auto method = output_dither_method_name(params.method);
    if (method.empty())
    {
        return make_error(ErrorCode::kValidation, "Output dither method is invalid",
                          {{"reason", "invalid_output_dither_method"}});
    }
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kOutputDitherWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kOutputDitherAlgorithm)}},
        {"method", ParameterValue{std::string(method)}},
        {"random_damping_db", ParameterValue{params.random_damping_db}},
    };
    auto validated = output_dither_from_parameters(parameters);
    if (!validated)
        return validated.error();
    return parameters;
}

Result<void> validate_output_dither_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = output_dither_from_parameters(parameters);
    return parsed ? Result<void>{} : Result<void>{parsed.error()};
}

} // namespace ravo
