#include "ravo/recipe/color_reconstruction.h"

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
        {"reason", "invalid_colorreconstruct_parameters"}};
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
        return invalid_parameter("Color Reconstruction parameter is required", name);
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
        return invalid_parameter("Color Reconstruction parameter must be a string", name);
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
    const float narrowed = static_cast<float>(parsed);
    if (!std::isfinite(parsed) || !std::isfinite(narrowed))
    {
        return invalid_parameter(
            "Color Reconstruction parameter must be finite and representable as float", name);
    }
    if (parsed < minimum || parsed > maximum)
    {
        return invalid_parameter("Color Reconstruction parameter is outside its supported range",
                                 name);
    }
    return parsed;
}

} // namespace

std::string_view
color_reconstruction_precedence_name(const ColorReconstructionPrecedence precedence) noexcept
{
    switch (precedence)
    {
    case ColorReconstructionPrecedence::kNone:
        return "none";
    case ColorReconstructionPrecedence::kChroma:
        return "chroma";
    case ColorReconstructionPrecedence::kHue:
        return "hue";
    }
    return "";
}

Result<ColorReconstructionPrecedence>
parse_color_reconstruction_precedence(const std::string_view name)
{
    if (name == "none")
    {
        return ColorReconstructionPrecedence::kNone;
    }
    if (name == "chroma")
    {
        return ColorReconstructionPrecedence::kChroma;
    }
    if (name == "hue")
    {
        return ColorReconstructionPrecedence::kHue;
    }
    return invalid_parameter("Color Reconstruction precedence is unsupported", "precedence");
}

Result<ColorReconstructionParams> color_reconstruction_from_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 7> names{
        "working_space", "algorithm", "threshold", "spatial", "range", "hue", "precedence"};
    if (parameters.size() != names.size())
    {
        return invalid_parameter(
            "Color Reconstruction parameters must contain exactly seven fields");
    }
    for (const auto &[name, ignored] : parameters)
    {
        static_cast<void>(ignored);
        if (std::find(names.begin(), names.end(), name) == names.end())
        {
            return invalid_parameter("Color Reconstruction parameter is unknown", name);
        }
    }

    auto working_space = text(parameters, "working_space");
    auto algorithm = text(parameters, "algorithm");
    auto threshold = number(parameters, "threshold", kColorReconstructionThresholdMin,
                            kColorReconstructionThresholdMax);
    auto spatial = number(parameters, "spatial", kColorReconstructionSpatialMin,
                          kColorReconstructionSpatialMax);
    auto range =
        number(parameters, "range", kColorReconstructionRangeMin, kColorReconstructionRangeMax);
    auto hue = number(parameters, "hue", kColorReconstructionHueMin, kColorReconstructionHueMax);
    auto precedence_text = text(parameters, "precedence");
    if (!working_space || !algorithm || !threshold || !spatial || !range || !hue ||
        !precedence_text)
    {
        return !working_space ? working_space.error() :
               !algorithm     ? algorithm.error() :
               !threshold     ? threshold.error() :
               !spatial       ? spatial.error() :
               !range         ? range.error() :
               !hue           ? hue.error() :
                                precedence_text.error();
    }
    if (working_space.value() != kColorReconstructionWorkingSpaceLabD50)
    {
        return invalid_parameter("Color Reconstruction working space is unsupported",
                                 "working_space");
    }
    if (algorithm.value() != kColorReconstructionAlgorithmBilateralGridV3)
    {
        return invalid_parameter("Color Reconstruction algorithm is unsupported", "algorithm");
    }
    auto precedence = parse_color_reconstruction_precedence(precedence_text.value());
    if (!precedence)
    {
        return precedence.error();
    }
    return ColorReconstructionParams{threshold.value(), spatial.value(), range.value(), hue.value(),
                                     precedence.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
color_reconstruction_to_parameters(const ColorReconstructionParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> parameters{
        {"working_space", ParameterValue{std::string(kColorReconstructionWorkingSpaceLabD50)}},
        {"algorithm", ParameterValue{std::string(kColorReconstructionAlgorithmBilateralGridV3)}},
        {"threshold", ParameterValue{params.threshold}},
        {"spatial", ParameterValue{params.spatial}},
        {"range", ParameterValue{params.range}},
        {"hue", ParameterValue{params.hue}},
        {"precedence",
         ParameterValue{std::string(color_reconstruction_precedence_name(params.precedence))}},
    };
    auto validated = color_reconstruction_from_parameters(parameters);
    if (!validated)
    {
        return validated.error();
    }
    return parameters;
}

Result<void> validate_color_reconstruction_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto parsed = color_reconstruction_from_parameters(parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    return {};
}

} // namespace ravo
