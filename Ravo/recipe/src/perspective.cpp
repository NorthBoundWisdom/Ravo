#include "ravo/recipe/perspective.h"

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
        {"reason", "invalid_perspective_parameters"}};
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
               Result<const ParameterValue *>{invalid("Required Perspective field is missing", name)} :
               Result<const ParameterValue *>{&found->second};
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
        return invalid("Perspective numeric field is outside its supported range", name);
    return parsed;
}

[[nodiscard]] Result<bool>
boolean(const std::map<std::string, ParameterValue, std::less<>> &parameters,
        const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    const auto *parsed = std::get_if<bool>(&value.value()->value);
    return parsed == nullptr ? Result<bool>{invalid("Perspective field must be boolean", name)} :
                               Result<bool>{*parsed};
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    return parsed == nullptr ? Result<std::string>{invalid("Perspective field must be text", name)} :
                               Result<std::string>{*parsed};
}
} // namespace

bool PerspectiveParams::is_identity() const noexcept
{
    constexpr double epsilon = 1.0e-9;
    return std::abs(rotation_degrees) <= epsilon && std::abs(vertical_shift) <= epsilon &&
           std::abs(horizontal_shift) <= epsilon && std::abs(shear) <= epsilon;
}

bool perspective_interpolation_is_supported(const std::string_view interpolation) noexcept
{
    return interpolation == kPerspectiveInterpolationBilinear ||
           interpolation == kPerspectiveInterpolationLanczos2 ||
           interpolation == kPerspectiveInterpolationLanczos3;
}

Result<PerspectiveParams>
perspective_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 8> names{
        "working_space", "algorithm", "rotation_degrees", "vertical_shift",
        "horizontal_shift", "shear", "constrain_crop", "interpolation"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("Perspective parameters must contain exactly eight known fields");
    auto working = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto rotation = number(parameters, "rotation_degrees", kPerspectiveRotationMin,
                           kPerspectiveRotationMax);
    auto vertical = number(parameters, "vertical_shift", kPerspectiveShiftMin,
                           kPerspectiveShiftMax);
    auto horizontal = number(parameters, "horizontal_shift", kPerspectiveShiftMin,
                             kPerspectiveShiftMax);
    auto shear = number(parameters, "shear", kPerspectiveShearMin, kPerspectiveShearMax);
    auto constrain = boolean(parameters, "constrain_crop");
    auto interpolation = text(parameters, "interpolation");
    if (!working || !algorithm || !rotation || !vertical || !horizontal || !shear || !constrain ||
        !interpolation)
        return !working       ? working.error() :
               !algorithm     ? algorithm.error() :
               !rotation      ? rotation.error() :
               !vertical      ? vertical.error() :
               !horizontal    ? horizontal.error() :
               !shear         ? shear.error() :
               !constrain     ? constrain.error() :
                                interpolation.error();
    if (working.value() != kPerspectiveWorkingSpace)
        return invalid("Perspective working space is unsupported", "working_space");
    if (algorithm.value() != kPerspectiveAlgorithm)
        return invalid("Perspective algorithm is unsupported", "algorithm");
    if (!perspective_interpolation_is_supported(interpolation.value()))
        return invalid("Perspective interpolation is unsupported", "interpolation");
    return PerspectiveParams{rotation.value(), vertical.value(), horizontal.value(), shear.value(),
                             constrain.value(), std::move(interpolation).value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
perspective_to_parameters(const PerspectiveParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kPerspectiveWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kPerspectiveAlgorithm)}},
        {"rotation_degrees", ParameterValue{params.rotation_degrees}},
        {"vertical_shift", ParameterValue{params.vertical_shift}},
        {"horizontal_shift", ParameterValue{params.horizontal_shift}},
        {"shear", ParameterValue{params.shear}},
        {"constrain_crop", ParameterValue{params.constrain_crop}},
        {"interpolation", ParameterValue{params.interpolation}},
    };
    auto valid = perspective_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

} // namespace ravo
