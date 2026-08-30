#include "ravo/recipe/lut3d.h"

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
    std::map<std::string, std::string, std::less<>> context{{"reason", "invalid_lut3d_parameters"}};
    if (!parameter.empty())
        context.emplace("parameter", parameter);
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
        return invalid("Required 3D LUT field is missing", name);
    const auto *value = std::get_if<std::string>(&found->second.value);
    return value == nullptr ? Result<std::string>{invalid("3D LUT field must be text", name)} :
                              Result<std::string>{*value};
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
        return invalid("Required 3D LUT field is missing", name);
    double value = std::numeric_limits<double>::quiet_NaN();
    if (const auto *floating = std::get_if<double>(&found->second.value); floating != nullptr)
        value = *floating;
    else if (const auto *integer = std::get_if<std::int64_t>(&found->second.value);
             integer != nullptr)
        value = static_cast<double>(*integer);
    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        return invalid("3D LUT strength is outside its supported range", name);
    return value;
}
} // namespace

bool lut3d_space_supported(const std::string_view space) noexcept
{
    return std::find(kLut3dSelectableSpaces.begin(), kLut3dSelectableSpaces.end(), space) !=
           kLut3dSelectableSpaces.end();
}

bool lut3d_interpolation_supported(const std::string_view interpolation) noexcept
{
    return std::find(kLut3dSelectableInterpolations.begin(),
                     kLut3dSelectableInterpolations.end(), interpolation) !=
           kLut3dSelectableInterpolations.end();
}

Result<Lut3dParams>
lut3d_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 5> names{"file_path", "input_space", "output_space",
                                                    "interpolation", "strength"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("3D LUT parameters must contain exactly five known fields");

    auto path = text(parameters, "file_path");
    auto input = text(parameters, "input_space");
    auto output = text(parameters, "output_space");
    auto interpolation = text(parameters, "interpolation");
    auto strength = number(parameters, "strength");
    if (!path || !input || !output || !interpolation || !strength)
        return !path            ? path.error() :
               !input           ? input.error() :
               !output          ? output.error() :
               !interpolation   ? interpolation.error() :
                                  strength.error();
    if (path.value().empty() || path.value().size() > kLut3dPathMaximumBytes ||
        path.value().find('\0') != std::string::npos)
        return invalid("3D LUT path is empty or too long", "file_path");
    if (!lut3d_space_supported(input.value()))
        return invalid("3D LUT input colour space is unsupported", "input_space");
    if (!lut3d_space_supported(output.value()))
        return invalid("3D LUT output colour space is unsupported", "output_space");
    if (!lut3d_interpolation_supported(interpolation.value()))
        return invalid("3D LUT interpolation is unsupported", "interpolation");
    return Lut3dParams{std::move(path).value(), std::move(input).value(),
                       std::move(output).value(), std::move(interpolation).value(),
                       strength.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
lut3d_to_parameters(const Lut3dParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> values{
        {"file_path", ParameterValue{params.file_path}},
        {"input_space", ParameterValue{params.input_space}},
        {"output_space", ParameterValue{params.output_space}},
        {"interpolation", ParameterValue{params.interpolation}},
        {"strength", ParameterValue{params.strength}},
    };
    auto valid = lut3d_from_parameters(values);
    return valid ? Result<decltype(values)>{std::move(values)} : valid.error();
}

} // namespace ravo
