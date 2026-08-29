#include "ravo/recipe/canvas_frame.h"

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

[[nodiscard]] TaskError invalid(const std::string_view owner, const std::string_view message,
                                const std::string_view parameter = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"reason", owner == "canvas" ? "invalid_canvas_parameters" : "invalid_frame_parameters"}};
    if (!parameter.empty())
        context.emplace("parameter", parameter);
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<const ParameterValue *>
required(const std::map<std::string, ParameterValue, std::less<>> &parameters,
         const std::string_view owner, const std::string_view name)
{
    const auto found = parameters.find(std::string(name));
    if (found == parameters.end())
        return invalid(owner, "Required parameter is missing", name);
    return &found->second;
}

[[nodiscard]] Result<std::string>
text(const std::map<std::string, ParameterValue, std::less<>> &parameters,
     const std::string_view owner, const std::string_view name)
{
    auto value = required(parameters, owner, name);
    if (!value)
        return value.error();
    const auto *parsed = std::get_if<std::string>(&value.value()->value);
    if (parsed == nullptr)
        return invalid(owner, "Parameter must be text", name);
    return *parsed;
}

[[nodiscard]] Result<double>
number(const std::map<std::string, ParameterValue, std::less<>> &parameters,
       const std::string_view owner, const std::string_view name, const double minimum,
       const double maximum)
{
    auto value = required(parameters, owner, name);
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
        return invalid(owner, "Numeric parameter is outside its supported range", name);
    return parsed;
}

[[nodiscard]] Result<std::array<double, 3>>
color(const std::map<std::string, ParameterValue, std::less<>> &parameters,
      const std::string_view owner, const std::string_view name)
{
    auto value = required(parameters, owner, name);
    if (!value)
        return value.error();
    const auto *values = std::get_if<ParameterValue::Array>(&value.value()->value);
    if (values == nullptr || values->size() != 3U)
        return invalid(owner, "Color parameter must contain exactly three channels", name);
    std::array<double, 3> result{};
    for (std::size_t index = 0U; index < result.size(); ++index)
    {
        const auto *floating = std::get_if<double>(&(*values)[index].value);
        const auto *integer = std::get_if<std::int64_t>(&(*values)[index].value);
        if (floating == nullptr && integer == nullptr)
            return invalid(owner, "Color channel must be numeric", name);
        result[index] = floating != nullptr ? *floating : static_cast<double>(*integer);
        if (!std::isfinite(result[index]) || !std::isfinite(static_cast<float>(result[index])) ||
            result[index] < 0.0 || result[index] > 1.0)
            return invalid(owner, "Color channel is outside [0,1]", name);
    }
    return result;
}

[[nodiscard]] ParameterValue color_value(const std::array<double, 3> &value)
{
    ParameterValue::Array array;
    array.reserve(3U);
    for (const double channel : value)
        array.emplace_back(channel);
    return ParameterValue{std::move(array)};
}

template <std::size_t N>
[[nodiscard]] bool only_names(const std::map<std::string, ParameterValue, std::less<>> &parameters,
                              const std::array<std::string_view, N> &names) noexcept
{
    if (parameters.size() != names.size())
        return false;
    return std::all_of(
        parameters.begin(), parameters.end(), [&](const auto &entry)
        { return std::find(names.begin(), names.end(), entry.first) != names.end(); });
}

} // namespace

bool CanvasParams::is_identity() const noexcept
{
    return percent_left == 0.0 && percent_right == 0.0 && percent_top == 0.0 &&
           percent_bottom == 0.0;
}

std::string_view canvas_color_name(const CanvasColor color) noexcept
{
    switch (color)
    {
    case CanvasColor::kGreen:
        return "green";
    case CanvasColor::kRed:
        return "red";
    case CanvasColor::kBlue:
        return "blue";
    case CanvasColor::kBlack:
        return "black";
    case CanvasColor::kWhite:
        return "white";
    }
    return {};
}

Result<CanvasColor> parse_canvas_color(const std::string_view name)
{
    for (int index = 0; index < 5; ++index)
    {
        const auto value = static_cast<CanvasColor>(index);
        if (canvas_color_name(value) == name)
            return value;
    }
    return make_error(ErrorCode::kValidation, "Canvas color is unsupported",
                      {{"color", std::string(name)}, {"reason", "invalid_canvas_color"}});
}

std::string_view frame_orientation_name(const FrameOrientation orientation) noexcept
{
    switch (orientation)
    {
    case FrameOrientation::kAuto:
        return "auto";
    case FrameOrientation::kPortrait:
        return "portrait";
    case FrameOrientation::kLandscape:
        return "landscape";
    }
    return {};
}

Result<FrameOrientation> parse_frame_orientation(const std::string_view name)
{
    for (int index = 0; index < 3; ++index)
    {
        const auto value = static_cast<FrameOrientation>(index);
        if (frame_orientation_name(value) == name)
            return value;
    }
    return make_error(
        ErrorCode::kValidation, "Frame orientation is unsupported",
        {{"orientation", std::string(name)}, {"reason", "invalid_frame_orientation"}});
}

std::string_view frame_basis_name(const FrameBasis basis) noexcept
{
    switch (basis)
    {
    case FrameBasis::kAuto:
        return "auto";
    case FrameBasis::kWidth:
        return "width";
    case FrameBasis::kHeight:
        return "height";
    case FrameBasis::kShorter:
        return "shorter";
    case FrameBasis::kLonger:
        return "longer";
    }
    return {};
}

Result<FrameBasis> parse_frame_basis(const std::string_view name)
{
    for (int index = 0; index < 5; ++index)
    {
        const auto value = static_cast<FrameBasis>(index);
        if (frame_basis_name(value) == name)
            return value;
    }
    return make_error(ErrorCode::kValidation, "Frame basis is unsupported",
                      {{"basis", std::string(name)}, {"reason", "invalid_frame_basis"}});
}

Result<CanvasParams>
canvas_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 7> names{
        "working_space", "algorithm",      "percent_left", "percent_right",
        "percent_top",   "percent_bottom", "color"};
    if (!only_names(parameters, names))
        return invalid("canvas", "Canvas parameters must contain exactly seven known fields");
    auto working = text(parameters, "canvas", "working_space");
    auto algorithm = text(parameters, "canvas", "algorithm");
    auto left = number(parameters, "canvas", "percent_left", 0.0, 100.0);
    auto right = number(parameters, "canvas", "percent_right", 0.0, 100.0);
    auto top = number(parameters, "canvas", "percent_top", 0.0, 100.0);
    auto bottom = number(parameters, "canvas", "percent_bottom", 0.0, 100.0);
    auto color_name = text(parameters, "canvas", "color");
    if (!working || !algorithm || !left || !right || !top || !bottom || !color_name)
    {
        return !working   ? working.error() :
               !algorithm ? algorithm.error() :
               !left      ? left.error() :
               !right     ? right.error() :
               !top       ? top.error() :
               !bottom    ? bottom.error() :
                            color_name.error();
    }
    if (working.value() != kCanvasWorkingSpace)
        return invalid("canvas", "Canvas working space is unsupported", "working_space");
    if (algorithm.value() != kCanvasAlgorithm)
        return invalid("canvas", "Canvas algorithm is unsupported", "algorithm");
    auto parsed_color = parse_canvas_color(color_name.value());
    if (!parsed_color)
        return parsed_color.error();
    return CanvasParams{left.value(), right.value(), top.value(), bottom.value(),
                        parsed_color.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
canvas_to_parameters(const CanvasParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kCanvasWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kCanvasAlgorithm)}},
        {"percent_left", ParameterValue{params.percent_left}},
        {"percent_right", ParameterValue{params.percent_right}},
        {"percent_top", ParameterValue{params.percent_top}},
        {"percent_bottom", ParameterValue{params.percent_bottom}},
        {"color", ParameterValue{std::string(canvas_color_name(params.color))}},
    };
    auto valid = canvas_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

Result<FrameParams>
frame_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 12> names{
        "working_space", "algorithm",  "border_color", "aspect",       "orientation", "size",
        "position_h",    "position_v", "frame_size",   "frame_offset", "frame_color", "basis"};
    if (!only_names(parameters, names))
        return invalid("frame", "Frame parameters must contain exactly twelve known fields");
    auto working = text(parameters, "frame", "working_space");
    auto algorithm = text(parameters, "frame", "algorithm");
    auto border = color(parameters, "frame", "border_color");
    auto aspect = number(parameters, "frame", "aspect", -1.0, 3.0);
    auto orientation = text(parameters, "frame", "orientation");
    auto size = number(parameters, "frame", "size", 0.0, 0.5);
    auto position_h = number(parameters, "frame", "position_h", 0.0, 1.0);
    auto position_v = number(parameters, "frame", "position_v", 0.0, 1.0);
    auto frame_size = number(parameters, "frame", "frame_size", 0.0, 1.0);
    auto frame_offset = number(parameters, "frame", "frame_offset", 0.0, 1.0);
    auto frame_color = color(parameters, "frame", "frame_color");
    auto basis = text(parameters, "frame", "basis");
    if (!working || !algorithm || !border || !aspect || !orientation || !size || !position_h ||
        !position_v || !frame_size || !frame_offset || !frame_color || !basis)
    {
        return !working      ? working.error() :
               !algorithm    ? algorithm.error() :
               !border       ? border.error() :
               !aspect       ? aspect.error() :
               !orientation  ? orientation.error() :
               !size         ? size.error() :
               !position_h   ? position_h.error() :
               !position_v   ? position_v.error() :
               !frame_size   ? frame_size.error() :
               !frame_offset ? frame_offset.error() :
               !frame_color  ? frame_color.error() :
                               basis.error();
    }
    if (working.value() != kFrameWorkingSpace)
        return invalid("frame", "Frame working space is unsupported", "working_space");
    if (algorithm.value() != kFrameAlgorithm)
        return invalid("frame", "Frame algorithm is unsupported", "algorithm");
    if ((aspect.value() < 0.0 && aspect.value() != -1.0) ||
        (aspect.value() > 0.0 && static_cast<float>(aspect.value()) <= 0.0F))
        return invalid("frame", "Frame aspect is unsupported", "aspect");
    auto parsed_orientation = parse_frame_orientation(orientation.value());
    if (!parsed_orientation)
        return parsed_orientation.error();
    auto parsed_basis = parse_frame_basis(basis.value());
    if (!parsed_basis)
        return parsed_basis.error();
    FrameParams result;
    result.border_color = border.value();
    result.aspect = aspect.value();
    result.orientation = parsed_orientation.value();
    result.size = size.value();
    result.position_h = position_h.value();
    result.position_v = position_v.value();
    result.frame_size = frame_size.value();
    result.frame_offset = frame_offset.value();
    result.frame_color = frame_color.value();
    result.basis = parsed_basis.value();
    return result;
}

Result<std::map<std::string, ParameterValue, std::less<>>>
frame_to_parameters(const FrameParams &params)
{
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kFrameWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kFrameAlgorithm)}},
        {"border_color", color_value(params.border_color)},
        {"aspect", ParameterValue{params.aspect}},
        {"orientation", ParameterValue{std::string(frame_orientation_name(params.orientation))}},
        {"size", ParameterValue{params.size}},
        {"position_h", ParameterValue{params.position_h}},
        {"position_v", ParameterValue{params.position_v}},
        {"frame_size", ParameterValue{params.frame_size}},
        {"frame_offset", ParameterValue{params.frame_offset}},
        {"frame_color", color_value(params.frame_color)},
        {"basis", ParameterValue{std::string(frame_basis_name(params.basis))}},
    };
    auto valid = frame_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

} // namespace ravo
