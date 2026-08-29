#include "ravo/recipe/watermark.h"

#include <algorithm>
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
        {"reason", "invalid_watermark_parameters"}};
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
                                           "Required watermark parameter is missing", name)} :
                                       Result<const ParameterValue *>{&found->second};
}

[[nodiscard]] Result<std::string>
text_value(const std::map<std::string, ParameterValue, std::less<>> &parameters,
           const std::string_view name)
{
    auto value = required(parameters, name);
    if (!value)
        return value.error();
    const auto *text = std::get_if<std::string>(&value.value()->value);
    return text == nullptr ?
               Result<std::string>{invalid("Watermark parameter must be text", name)} :
               Result<std::string>{*text};
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
        return invalid("Watermark numeric parameter is outside its supported range", name);
    return parsed;
}

[[nodiscard]] Result<std::array<double, 3>>
color_value(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    auto value = required(parameters, "color");
    if (!value)
        return value.error();
    const auto *array = std::get_if<ParameterValue::Array>(&value.value()->value);
    if (array == nullptr || array->size() != 3U)
        return invalid("Watermark color must contain three channels", "color");
    std::array<double, 3> result{};
    for (std::size_t index = 0U; index < result.size(); ++index)
    {
        const auto *floating = std::get_if<double>(&(*array)[index].value);
        const auto *integer = std::get_if<std::int64_t>(&(*array)[index].value);
        if (floating == nullptr && integer == nullptr)
            return invalid("Watermark color channel must be numeric", "color");
        result[index] = floating != nullptr ? *floating : static_cast<double>(*integer);
        if (!std::isfinite(result[index]) || !std::isfinite(static_cast<float>(result[index])) ||
            result[index] < 0.0 || result[index] > 1.0)
            return invalid("Watermark color channel is outside [0,1]", "color");
    }
    return result;
}

[[nodiscard]] Result<void> validate_template(const std::string_view value)
{
    if (value.empty() || value.size() > kWatermarkTextMaxBytes)
        return invalid("Watermark text length is unsupported", "text");
    std::size_t lines = 1U;
    std::size_t characters = 0U;
    for (std::size_t index = 0U; index < value.size();)
    {
        if (value[index] == '\n')
        {
            if (characters == 0U || ++lines > kWatermarkMaxLines)
                return invalid("Watermark text line layout is unsupported", "text");
            characters = 0U;
            ++index;
            continue;
        }
        if (value[index] == '{')
        {
            const std::string_view remaining = value.substr(index);
            const std::string_view token = remaining.starts_with("{stem}")     ? "{stem}" :
                                           remaining.starts_with("{asset_id}") ? "{asset_id}" :
                                                                                 std::string_view{};
            if (token.empty())
                return invalid("Watermark text contains an unknown token", "text");
            index += token.size();
            if (++characters > kWatermarkLineMaxCharacters)
                return invalid("Watermark text line is too long", "text");
            continue;
        }
        if (value[index] == '}' || !watermark_character_supported(value[index]))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Watermark text contains an unsupported character",
                              {{"byte_index", std::to_string(index)},
                               {"reason", "unsupported_watermark_character"}});
        }
        ++index;
        if (++characters > kWatermarkLineMaxCharacters)
            return invalid("Watermark text line is too long", "text");
    }
    if (characters == 0U)
        return invalid("Watermark text cannot end with an empty line", "text");
    return {};
}

[[nodiscard]] std::string source_stem(const std::string_view uri)
{
    const std::size_t slash = uri.find_last_of("/\\");
    std::string value(uri.substr(slash == std::string_view::npos ? 0U : slash + 1U));
    const std::size_t suffix = value.find_first_of("?#");
    if (suffix != std::string::npos)
        value.erase(suffix);
    const std::size_t dot = value.find_last_of('.');
    if (dot != std::string::npos && dot != 0U)
        value.erase(dot);
    return value;
}

void replace_all(std::string &value, const std::string_view token,
                 const std::string_view replacement)
{
    for (std::size_t offset = 0U; (offset = value.find(token, offset)) != std::string::npos;)
    {
        value.replace(offset, token.size(), replacement);
        offset += replacement.size();
    }
}

} // namespace

std::string_view watermark_alignment_name(const WatermarkAlignment alignment) noexcept
{
    switch (alignment)
    {
    case WatermarkAlignment::kTopLeft:
        return "top_left";
    case WatermarkAlignment::kTopCenter:
        return "top_center";
    case WatermarkAlignment::kTopRight:
        return "top_right";
    case WatermarkAlignment::kCenterLeft:
        return "center_left";
    case WatermarkAlignment::kCenter:
        return "center";
    case WatermarkAlignment::kCenterRight:
        return "center_right";
    case WatermarkAlignment::kBottomLeft:
        return "bottom_left";
    case WatermarkAlignment::kBottomCenter:
        return "bottom_center";
    case WatermarkAlignment::kBottomRight:
        return "bottom_right";
    }
    return {};
}

Result<WatermarkAlignment> parse_watermark_alignment(const std::string_view name)
{
    for (std::uint8_t index = 0U; index < 9U; ++index)
    {
        const auto alignment = static_cast<WatermarkAlignment>(index);
        if (watermark_alignment_name(alignment) == name)
            return alignment;
    }
    return make_error(
        ErrorCode::kValidation, "Watermark alignment is unsupported",
        {{"alignment", std::string(name)}, {"reason", "invalid_watermark_alignment"}});
}

bool watermark_character_supported(const char character) noexcept
{
    if (character == ' ' || (character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z'))
        return true;
    constexpr std::string_view punctuation = ".,:;!?-_/+()[]#@&%'";
    return punctuation.find(character) != std::string_view::npos;
}

Result<std::string> expand_watermark_text(const std::string_view text, const AssetDescriptor &asset)
{
    auto valid = validate_template(text);
    if (!valid)
        return valid.error();
    std::string expanded(text);
    replace_all(expanded, "{stem}", source_stem(asset.input_uri));
    replace_all(expanded, "{asset_id}", asset.id);
    if (expanded.empty() || expanded.size() > kWatermarkTextMaxBytes)
        return make_error(ErrorCode::kValidation, "Expanded watermark text length is unsupported",
                          {{"reason", "invalid_expanded_watermark_text"}});
    std::size_t line_characters = 0U;
    std::size_t lines = 1U;
    for (std::size_t index = 0U; index < expanded.size(); ++index)
    {
        if (expanded[index] == '\n')
        {
            if (line_characters == 0U || ++lines > kWatermarkMaxLines)
                return make_error(ErrorCode::kValidation,
                                  "Expanded watermark line layout is unsupported",
                                  {{"reason", "invalid_expanded_watermark_text"}});
            line_characters = 0U;
            continue;
        }
        if (!watermark_character_supported(expanded[index]))
            return make_error(ErrorCode::kUnsupported,
                              "Expanded watermark text contains an unsupported character",
                              {{"byte_index", std::to_string(index)},
                               {"reason", "unsupported_watermark_character"}});
        if (++line_characters > kWatermarkLineMaxCharacters)
            return make_error(ErrorCode::kValidation, "Expanded watermark line is too long",
                              {{"reason", "invalid_expanded_watermark_text"}});
    }
    if (line_characters == 0U)
        return make_error(ErrorCode::kValidation, "Expanded watermark ends with an empty line",
                          {{"reason", "invalid_expanded_watermark_text"}});
    return expanded;
}

Result<WatermarkParams>
watermark_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters)
{
    constexpr std::array<std::string_view, 10> names{
        "working_space", "algorithm", "text",     "color",     "opacity",
        "scale_percent", "x_offset",  "y_offset", "alignment", "rotation_degrees"};
    if (parameters.size() != names.size() ||
        !std::all_of(parameters.begin(), parameters.end(), [&](const auto &entry)
                     { return std::find(names.begin(), names.end(), entry.first) != names.end(); }))
        return invalid("Watermark parameters must contain exactly ten known fields");
    auto working = text_value(parameters, "working_space");
    auto algorithm = text_value(parameters, "algorithm");
    auto text = text_value(parameters, "text");
    auto color = color_value(parameters);
    auto opacity = number(parameters, "opacity", 0.0, 1.0);
    auto scale = number(parameters, "scale_percent", kWatermarkScaleMin, kWatermarkScaleMax);
    auto x = number(parameters, "x_offset", -1.0, 1.0);
    auto y = number(parameters, "y_offset", -1.0, 1.0);
    auto alignment = text_value(parameters, "alignment");
    auto rotation = number(parameters, "rotation_degrees", -180.0, 180.0);
    if (!working || !algorithm || !text || !color || !opacity || !scale || !x || !y || !alignment ||
        !rotation)
        return !working   ? working.error() :
               !algorithm ? algorithm.error() :
               !text      ? text.error() :
               !color     ? color.error() :
               !opacity   ? opacity.error() :
               !scale     ? scale.error() :
               !x         ? x.error() :
               !y         ? y.error() :
               !alignment ? alignment.error() :
                            rotation.error();
    if (working.value() != kWatermarkWorkingSpace)
        return invalid("Watermark working space is unsupported", "working_space");
    if (algorithm.value() != kWatermarkAlgorithm)
        return invalid("Watermark algorithm is unsupported", "algorithm");
    auto valid_text = validate_template(text.value());
    if (!valid_text)
        return valid_text.error();
    auto parsed_alignment = parse_watermark_alignment(alignment.value());
    if (!parsed_alignment)
        return parsed_alignment.error();
    return WatermarkParams{text.value(), color.value(), opacity.value(),          scale.value(),
                           x.value(),    y.value(),     parsed_alignment.value(), rotation.value()};
}

Result<std::map<std::string, ParameterValue, std::less<>>>
watermark_to_parameters(const WatermarkParams &params)
{
    ParameterValue::Array color;
    color.reserve(3U);
    for (const double value : params.color)
        color.emplace_back(value);
    std::map<std::string, ParameterValue, std::less<>> result{
        {"working_space", ParameterValue{std::string(kWatermarkWorkingSpace)}},
        {"algorithm", ParameterValue{std::string(kWatermarkAlgorithm)}},
        {"text", ParameterValue{params.text}},
        {"color", ParameterValue{std::move(color)}},
        {"opacity", ParameterValue{params.opacity}},
        {"scale_percent", ParameterValue{params.scale_percent}},
        {"x_offset", ParameterValue{params.x_offset}},
        {"y_offset", ParameterValue{params.y_offset}},
        {"alignment", ParameterValue{std::string(watermark_alignment_name(params.alignment))}},
        {"rotation_degrees", ParameterValue{params.rotation_degrees}},
    };
    auto valid = watermark_from_parameters(result);
    return valid ? Result<decltype(result)>{std::move(result)} : valid.error();
}

} // namespace ravo
