#include "ravo/domain/types.h"

#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ravo/foundation/json.h"

namespace ravo
{
namespace
{

[[nodiscard]] TaskError preset_error(const std::string_view message, const std::string_view reason,
                                     const std::string_view field = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!field.empty())
        context.emplace("field", std::string(field));
    return make_error(ErrorCode::kValidation, std::string(message), std::move(context));
}

[[nodiscard]] Result<std::int64_t> require_int64(const JsonValue &object,
                                                 const std::string_view key)
{
    const auto *root = object.object_if();
    if (root == nullptr)
        return preset_error("Export JSON root must be an object", "invalid_export_json_root");
    const auto found = root->find(std::string(key));
    if (found == root->end())
        return preset_error("Export JSON is missing a required field", "missing_export_json_field",
                            key);
    const auto *number = found->second.number_if();
    if (number == nullptr)
        return preset_error("Export JSON field must be a number", "invalid_export_json_number",
                            key);
    std::int64_t value = 0;
    const auto *begin = number->text.data();
    const auto *end = begin + number->text.size();
    const auto converted = std::from_chars(begin, end, value);
    if (converted.ec != std::errc{} || converted.ptr != end)
        return preset_error("Export JSON number is invalid", "invalid_export_json_number", key);
    return value;
}

[[nodiscard]] Result<std::string> require_string(const JsonValue &object,
                                                 const std::string_view key)
{
    const auto *root = object.object_if();
    if (root == nullptr)
        return preset_error("Export JSON root must be an object", "invalid_export_json_root");
    const auto found = root->find(std::string(key));
    if (found == root->end())
        return preset_error("Export JSON is missing a required field", "missing_export_json_field",
                            key);
    const auto *value = found->second.string_if();
    if (value == nullptr)
        return preset_error("Export JSON field must be a string", "invalid_export_json_string",
                            key);
    return *value;
}

[[nodiscard]] Result<bool> require_bool(const JsonValue &object, const std::string_view key)
{
    const auto *root = object.object_if();
    if (root == nullptr)
        return preset_error("Export JSON root must be an object", "invalid_export_json_root");
    const auto found = root->find(std::string(key));
    if (found == root->end())
        return preset_error("Export JSON is missing a required field", "missing_export_json_field",
                            key);
    const auto *value = found->second.boolean_if();
    if (value == nullptr)
        return preset_error("Export JSON field must be a boolean", "invalid_export_json_boolean",
                            key);
    return *value;
}

[[nodiscard]] Result<double> require_double(const JsonValue &object, const std::string_view key)
{
    const auto *root = object.object_if();
    if (root == nullptr)
        return preset_error("Export JSON root must be an object", "invalid_export_json_root");
    const auto found = root->find(std::string(key));
    if (found == root->end())
        return preset_error("Export JSON is missing a required field", "missing_export_json_field",
                            key);
    const auto *number = found->second.number_if();
    if (number == nullptr)
        return preset_error("Export JSON field must be a number", "invalid_export_json_number",
                            key);
    char *end = nullptr;
    const double value = std::strtod(number->text.c_str(), &end);
    if (end != number->text.c_str() + number->text.size() || !std::isfinite(value))
        return preset_error("Export JSON number is invalid", "invalid_export_json_number", key);
    return value;
}

[[nodiscard]] Result<std::uint32_t> require_uint32(const JsonValue &object,
                                                   const std::string_view key)
{
    auto parsed = require_int64(object, key);
    if (!parsed)
        return parsed.error();
    if (parsed.value() < 0 ||
        static_cast<std::uint64_t>(parsed.value()) > std::numeric_limits<std::uint32_t>::max())
        return preset_error("Export JSON number is out of range", "invalid_export_json_number",
                            key);
    return static_cast<std::uint32_t>(parsed.value());
}

[[nodiscard]] JsonValue number_u32(const std::uint32_t value)
{
    return JsonValue::number(std::to_string(value));
}

[[nodiscard]] JsonValue number_i64(const std::int64_t value)
{
    return JsonValue::number(std::to_string(value));
}

[[nodiscard]] JsonValue number_f(const double value)
{
    return JsonValue::number(std::to_string(value));
}

[[nodiscard]] Result<void> validate_box_limit(const std::uint32_t value,
                                              const std::string_view field)
{
    if (value > kExportBoxLimitMax)
    {
        return make_error(ErrorCode::kValidation, "Export box limit is out of range",
                          {{"field", std::string(field)},
                           {"value", std::to_string(value)},
                           {"reason", "invalid_export_box_limit"}});
    }
    return {};
}

[[nodiscard]] Result<ExportOutputSharpenOptions> parse_output_sharpen(const JsonValue &object)
{
    ExportOutputSharpenOptions sharpen;
    auto enabled = require_bool(object, "enabled");
    if (!enabled)
        return enabled.error();
    sharpen.enabled = enabled.value();
    auto amount = require_double(object, "amount");
    auto radius = require_double(object, "radius");
    auto threshold = require_double(object, "threshold");
    if (!amount || !radius || !threshold)
        return !amount ? amount.error() : !radius ? radius.error() : threshold.error();
    sharpen.amount = amount.value();
    sharpen.radius = radius.value();
    sharpen.threshold = threshold.value();
    auto valid = validate_export_output_sharpen_options(sharpen);
    if (!valid)
        return valid.error();
    return sharpen;
}

[[nodiscard]] Result<JsonValue> serialize_output_sharpen(const ExportOutputSharpenOptions &sharpen)
{
    auto valid = validate_export_output_sharpen_options(sharpen);
    if (!valid)
        return valid.error();
    JsonValue::Object object;
    object.emplace("enabled", sharpen.enabled);
    object.emplace("amount", number_f(sharpen.amount));
    object.emplace("radius", number_f(sharpen.radius));
    object.emplace("threshold", number_f(sharpen.threshold));
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<ExportWatermarkOptions> parse_watermark(const JsonValue &object)
{
    ExportWatermarkOptions watermark;
    auto enabled = require_bool(object, "enabled");
    if (!enabled)
        return enabled.error();
    watermark.enabled = enabled.value();
    auto text = require_string(object, "text");
    if (!text)
        return text.error();
    watermark.text = text.value();
    const auto *root = object.object_if();
    if (root == nullptr)
        return preset_error("Export watermark must be an object", "invalid_export_json_object",
                            "watermark");
    const auto color_found = root->find("color");
    if (color_found == root->end())
        return preset_error("Export watermark requires color", "missing_export_json_field",
                            "color");
    const auto *color = color_found->second.array_if();
    if (color == nullptr || color->size() != 3U)
    {
        return make_error(ErrorCode::kValidation,
                          "Export watermark color must contain three channels",
                          {{"reason", "invalid_export_watermark_color"}, {"parameter", "color"}});
    }
    for (std::size_t index = 0U; index < 3U; ++index)
    {
        const auto *number = (*color)[index].number_if();
        if (number == nullptr)
        {
            return make_error(
                ErrorCode::kValidation, "Export watermark color channel must be numeric",
                {{"reason", "invalid_export_watermark_color"}, {"parameter", "color"}});
        }
        char *end = nullptr;
        const std::string owned(number->text);
        const double value = std::strtod(owned.c_str(), &end);
        if (end != owned.c_str() + owned.size() || !std::isfinite(value))
        {
            return make_error(
                ErrorCode::kValidation, "Export watermark color channel is invalid",
                {{"reason", "invalid_export_watermark_color"}, {"parameter", "color"}});
        }
        watermark.color[index] = value;
    }
    auto opacity = require_double(object, "opacity");
    auto scale = require_double(object, "scale_percent");
    auto x = require_double(object, "x_offset");
    auto y = require_double(object, "y_offset");
    auto alignment = require_string(object, "alignment");
    auto rotation = require_double(object, "rotation_degrees");
    if (!opacity || !scale || !x || !y || !alignment || !rotation)
        return !opacity   ? opacity.error() :
               !scale     ? scale.error() :
               !x         ? x.error() :
               !y         ? y.error() :
               !alignment ? alignment.error() :
                            rotation.error();
    watermark.opacity = opacity.value();
    watermark.scale_percent = scale.value();
    watermark.x_offset = x.value();
    watermark.y_offset = y.value();
    watermark.alignment = alignment.value();
    watermark.rotation_degrees = rotation.value();
    auto valid = validate_export_watermark_options(watermark);
    if (!valid)
        return valid.error();
    return watermark;
}

[[nodiscard]] Result<JsonValue> serialize_watermark(const ExportWatermarkOptions &watermark)
{
    auto valid = validate_export_watermark_options(watermark);
    if (!valid)
        return valid.error();
    JsonValue::Array color;
    color.reserve(3U);
    for (const double channel : watermark.color)
        color.emplace_back(number_f(channel));
    JsonValue::Object object;
    object.emplace("enabled", watermark.enabled);
    object.emplace("text", watermark.text);
    object.emplace("color", JsonValue{std::move(color)});
    object.emplace("opacity", number_f(watermark.opacity));
    object.emplace("scale_percent", number_f(watermark.scale_percent));
    object.emplace("x_offset", number_f(watermark.x_offset));
    object.emplace("y_offset", number_f(watermark.y_offset));
    object.emplace("alignment", watermark.alignment);
    object.emplace("rotation_degrees", number_f(watermark.rotation_degrees));
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<std::array<double, 3>> parse_rgb_color(const JsonValue &object,
                                                            const std::string_view field)
{
    const auto *root = object.object_if();
    if (root == nullptr)
        return preset_error("Export colour object required", "invalid_export_json_object", field);
    const auto found = root->find(std::string(field));
    if (found == root->end())
        return preset_error("Export colour field missing", "missing_export_json_field", field);
    const auto *color = found->second.array_if();
    if (color == nullptr || color->size() != 3U)
    {
        return make_error(ErrorCode::kValidation, "Export colour must contain three channels",
                          {{"reason", "invalid_export_color"}, {"parameter", std::string(field)}});
    }
    std::array<double, 3> result{};
    for (std::size_t index = 0U; index < 3U; ++index)
    {
        const auto *number = (*color)[index].number_if();
        if (number == nullptr)
        {
            return make_error(
                ErrorCode::kValidation, "Export colour channel must be numeric",
                {{"reason", "invalid_export_color"}, {"parameter", std::string(field)}});
        }
        char *end = nullptr;
        const std::string owned(number->text);
        const double value = std::strtod(owned.c_str(), &end);
        if (end != owned.c_str() + owned.size() || !std::isfinite(value) || value < 0.0 ||
            value > 1.0)
        {
            return make_error(
                ErrorCode::kValidation, "Export colour channel is invalid",
                {{"reason", "invalid_export_color"}, {"parameter", std::string(field)}});
        }
        result[index] = value;
    }
    return result;
}

[[nodiscard]] Result<ExportColorOptions> parse_output_color(const JsonValue &object)
{
    ExportColorOptions color;
    auto enabled = require_bool(object, "enabled");
    if (!enabled)
        return enabled.error();
    color.enabled = enabled.value();
    auto profile = require_string(object, "output_profile");
    if (!profile)
        return profile.error();
    color.output_profile = profile.value();
    auto filename = require_string(object, "output_profile_filename");
    if (!filename)
        return filename.error();
    color.output_profile_filename = filename.value();
    auto intent = require_string(object, "rendering_intent");
    if (!intent)
        return intent.error();
    color.rendering_intent = intent.value();
    auto bpc = require_bool(object, "black_point_compensation");
    if (!bpc)
        return bpc.error();
    color.black_point_compensation = bpc.value();
    auto valid = validate_export_color_options(color);
    if (!valid)
        return valid.error();
    return color;
}

[[nodiscard]] Result<JsonValue> serialize_output_color(const ExportColorOptions &color)
{
    auto valid = validate_export_color_options(color);
    if (!valid)
        return valid.error();
    JsonValue::Object object;
    object.emplace("enabled", color.enabled);
    object.emplace("output_profile", color.output_profile);
    object.emplace("output_profile_filename", color.output_profile_filename);
    object.emplace("rendering_intent", color.rendering_intent);
    object.emplace("black_point_compensation", color.black_point_compensation);
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<ExportFrameOptions> parse_frame(const JsonValue &object)
{
    ExportFrameOptions frame;
    auto enabled = require_bool(object, "enabled");
    if (!enabled)
        return enabled.error();
    frame.enabled = enabled.value();
    auto border = parse_rgb_color(object, "border_color");
    if (!border)
        return border.error();
    frame.border_color = border.value();
    auto aspect = require_double(object, "aspect");
    if (!aspect)
        return aspect.error();
    frame.aspect = aspect.value();
    auto orientation = require_string(object, "orientation");
    if (!orientation)
        return orientation.error();
    frame.orientation = orientation.value();
    auto size = require_double(object, "size");
    if (!size)
        return size.error();
    frame.size = size.value();
    auto position_h = require_double(object, "position_h");
    if (!position_h)
        return position_h.error();
    frame.position_h = position_h.value();
    auto position_v = require_double(object, "position_v");
    if (!position_v)
        return position_v.error();
    frame.position_v = position_v.value();
    auto frame_size = require_double(object, "frame_size");
    if (!frame_size)
        return frame_size.error();
    frame.frame_size = frame_size.value();
    auto frame_offset = require_double(object, "frame_offset");
    if (!frame_offset)
        return frame_offset.error();
    frame.frame_offset = frame_offset.value();
    auto frame_color = parse_rgb_color(object, "frame_color");
    if (!frame_color)
        return frame_color.error();
    frame.frame_color = frame_color.value();
    auto basis = require_string(object, "basis");
    if (!basis)
        return basis.error();
    frame.basis = basis.value();
    auto valid = validate_export_frame_options(frame);
    if (!valid)
        return valid.error();
    return frame;
}

[[nodiscard]] Result<JsonValue> serialize_frame(const ExportFrameOptions &frame)
{
    auto valid = validate_export_frame_options(frame);
    if (!valid)
        return valid.error();
    JsonValue::Array border;
    border.reserve(3U);
    for (const double channel : frame.border_color)
        border.emplace_back(number_f(channel));
    JsonValue::Array line;
    line.reserve(3U);
    for (const double channel : frame.frame_color)
        line.emplace_back(number_f(channel));
    JsonValue::Object object;
    object.emplace("enabled", frame.enabled);
    object.emplace("border_color", JsonValue{std::move(border)});
    object.emplace("aspect", number_f(frame.aspect));
    object.emplace("orientation", frame.orientation);
    object.emplace("size", number_f(frame.size));
    object.emplace("position_h", number_f(frame.position_h));
    object.emplace("position_v", number_f(frame.position_v));
    object.emplace("frame_size", number_f(frame.frame_size));
    object.emplace("frame_offset", number_f(frame.frame_offset));
    object.emplace("frame_color", JsonValue{std::move(line)});
    object.emplace("basis", frame.basis);
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<ExportOptions> parse_export_options_object(const JsonValue &value)
{
    const auto *object = value.object_if();
    if (object == nullptr)
        return preset_error("Export options must be an object", "invalid_export_options_object",
                            "options");
    ExportOptions options;
    auto format_name = require_string(value, "format");
    if (!format_name)
        return format_name.error();
    auto format = parse_export_format(format_name.value());
    if (!format)
        return format.error();
    options.format = format.value();

    auto metadata_name = require_string(value, "metadata_mode");
    if (!metadata_name)
        return metadata_name.error();
    auto metadata = parse_export_metadata_mode(metadata_name.value());
    if (!metadata)
        return metadata.error();
    options.metadata_mode = metadata.value();

    auto max_edge = require_uint32(value, "max_edge");
    auto max_width = require_uint32(value, "max_width");
    auto max_height = require_uint32(value, "max_height");
    if (!max_edge || !max_width || !max_height)
        return !max_edge ? max_edge.error() : !max_width ? max_width.error() : max_height.error();
    options.max_edge = max_edge.value();
    options.max_width = max_width.value();
    options.max_height = max_height.value();

    const auto sharpen_found = object->find("output_sharpen");
    if (sharpen_found == object->end())
        return preset_error("Export options require output_sharpen", "missing_export_json_field",
                            "output_sharpen");
    auto sharpen = parse_output_sharpen(sharpen_found->second);
    if (!sharpen)
        return sharpen.error();
    options.output_sharpen = sharpen.value();

    const auto color_found = object->find("output_color");
    if (color_found == object->end())
        return preset_error("Export options require output_color", "missing_export_json_field",
                            "output_color");
    auto output_color = parse_output_color(color_found->second);
    if (!output_color)
        return output_color.error();
    options.output_color = output_color.value();

    const auto frame_found = object->find("frame");
    if (frame_found == object->end())
        return preset_error("Export options require frame", "missing_export_json_field", "frame");
    auto frame = parse_frame(frame_found->second);
    if (!frame)
        return frame.error();
    options.frame = frame.value();

    const auto watermark_found = object->find("watermark");
    if (watermark_found == object->end())
        return preset_error("Export options require watermark", "missing_export_json_field",
                            "watermark");
    auto watermark = parse_watermark(watermark_found->second);
    if (!watermark)
        return watermark.error();
    options.watermark = watermark.value();

    const auto jpeg_found = object->find("jpeg");
    const auto png_found = object->find("png");
    const auto tiff_found = object->find("tiff");
    if (jpeg_found == object->end() || png_found == object->end() || tiff_found == object->end())
        return preset_error("Export options require jpeg/png/tiff objects",
                            "missing_export_json_field", "jpeg");

    {
        auto quality = require_int64(jpeg_found->second, "quality");
        auto subsampling_name = require_string(jpeg_found->second, "subsampling");
        if (!quality || !subsampling_name)
            return !quality ? quality.error() : subsampling_name.error();
        if (quality.value() < kJpegQualityMin || quality.value() > kJpegQualityMax)
            return make_error(ErrorCode::kValidation, "JPEG quality is out of range",
                              {{"reason", "invalid_jpeg_quality"}});
        options.jpeg_options.quality = static_cast<int>(quality.value());
        auto subsampling = parse_jpeg_subsampling(subsampling_name.value());
        if (!subsampling)
            return subsampling.error();
        options.jpeg_options.subsampling = subsampling.value();
    }
    {
        auto bit_depth_name = require_string(png_found->second, "bit_depth");
        auto compression = require_int64(png_found->second, "compression");
        if (!bit_depth_name || !compression)
            return !bit_depth_name ? bit_depth_name.error() : compression.error();
        auto bit_depth = parse_png_bit_depth(bit_depth_name.value());
        if (!bit_depth)
            return bit_depth.error();
        options.png_options.bit_depth = bit_depth.value();
        options.png_options.compression = static_cast<int>(compression.value());
    }
    {
        auto sample_type_name = require_string(tiff_found->second, "sample_type");
        auto compression_name = require_string(tiff_found->second, "compression");
        auto compression_level = require_int64(tiff_found->second, "compression_level");
        auto grayscale = require_bool(tiff_found->second, "grayscale_if_neutral");
        auto dpi = require_int64(tiff_found->second, "resolution_dpi");
        if (!sample_type_name || !compression_name || !compression_level || !grayscale || !dpi)
            return !sample_type_name  ? sample_type_name.error() :
                   !compression_name  ? compression_name.error() :
                   !compression_level ? compression_level.error() :
                   !grayscale         ? grayscale.error() :
                                        dpi.error();
        auto sample_type = parse_tiff_sample_type(sample_type_name.value());
        auto compression = parse_tiff_compression(compression_name.value());
        if (!sample_type || !compression)
            return !sample_type ? sample_type.error() : compression.error();
        options.tiff_options.sample_type = sample_type.value();
        options.tiff_options.compression = compression.value();
        options.tiff_options.compression_level = static_cast<int>(compression_level.value());
        options.tiff_options.grayscale_if_neutral = grayscale.value();
        options.tiff_options.resolution_dpi = static_cast<int>(dpi.value());
    }

    // Reject unknown keys fail-closed.
    static constexpr std::string_view kAllowed[] = {
        "format",       "metadata_mode", "max_edge",  "max_width", "max_height", "output_sharpen",
        "output_color", "frame",         "watermark", "jpeg",      "png",        "tiff"};
    for (const auto &[key, _] : *object)
    {
        bool allowed = false;
        for (const auto name : kAllowed)
        {
            if (key == name)
            {
                allowed = true;
                break;
            }
        }
        if (!allowed)
            return preset_error("Export options contain an unknown field",
                                "unknown_export_options_field", key);
    }

    auto valid = validate_export_options(options);
    if (!valid)
        return valid.error();
    return options;
}

[[nodiscard]] Result<JsonValue> serialize_export_options_object(const ExportOptions &options)
{
    auto valid = validate_export_options(options);
    if (!valid)
        return valid.error();
    JsonValue::Object jpeg{
        {"quality", number_i64(options.jpeg_options.quality)},
        {"subsampling", std::string(jpeg_subsampling_name(options.jpeg_options.subsampling))}};
    JsonValue::Object png{
        {"bit_depth", std::string(png_bit_depth_name(options.png_options.bit_depth))},
        {"compression", number_i64(options.png_options.compression)}};
    JsonValue::Object tiff{
        {"sample_type", std::string(tiff_sample_type_name(options.tiff_options.sample_type))},
        {"compression", std::string(tiff_compression_name(options.tiff_options.compression))},
        {"compression_level", number_i64(options.tiff_options.compression_level)},
        {"grayscale_if_neutral", options.tiff_options.grayscale_if_neutral},
        {"resolution_dpi", number_i64(options.tiff_options.resolution_dpi)}};
    auto sharpen = serialize_output_sharpen(options.output_sharpen);
    if (!sharpen)
        return sharpen.error();
    auto output_color = serialize_output_color(options.output_color);
    if (!output_color)
        return output_color.error();
    auto frame = serialize_frame(options.frame);
    if (!frame)
        return frame.error();
    auto watermark = serialize_watermark(options.watermark);
    if (!watermark)
        return watermark.error();
    JsonValue::Object object;
    object.emplace("format", std::string(export_format_name(options.format)));
    object.emplace("metadata_mode", std::string(export_metadata_mode_name(options.metadata_mode)));
    object.emplace("max_edge", number_u32(options.max_edge));
    object.emplace("max_width", number_u32(options.max_width));
    object.emplace("max_height", number_u32(options.max_height));
    object.emplace("output_sharpen", std::move(sharpen).value());
    object.emplace("output_color", std::move(output_color).value());
    object.emplace("frame", std::move(frame).value());
    object.emplace("watermark", std::move(watermark).value());
    object.emplace("jpeg", JsonValue{std::move(jpeg)});
    object.emplace("png", JsonValue{std::move(png)});
    object.emplace("tiff", JsonValue{std::move(tiff)});
    return JsonValue{std::move(object)};
}

} // namespace

Result<void> validate_export_output_sharpen_options(const ExportOutputSharpenOptions &options)
{
    if (!std::isfinite(options.amount) || options.amount < kExportOutputSharpenAmountMin ||
        options.amount > kExportOutputSharpenAmountMax)
    {
        return make_error(ErrorCode::kValidation, "Export output sharpen amount is out of range",
                          {{"reason", "invalid_export_output_sharpen_amount"}});
    }
    if (!std::isfinite(options.radius) || options.radius < kExportOutputSharpenRadiusMin ||
        options.radius > kExportOutputSharpenRadiusMax)
    {
        return make_error(ErrorCode::kValidation, "Export output sharpen radius is out of range",
                          {{"reason", "invalid_export_output_sharpen_radius"}});
    }
    if (!std::isfinite(options.threshold) || options.threshold < kExportOutputSharpenThresholdMin ||
        options.threshold > kExportOutputSharpenThresholdMax)
    {
        return make_error(ErrorCode::kValidation, "Export output sharpen threshold is out of range",
                          {{"reason", "invalid_export_output_sharpen_threshold"}});
    }
    return {};
}

namespace
{

[[nodiscard]] bool export_watermark_character_ok(const char character) noexcept
{
    if (character == ' ' || (character >= '0' && character <= '9') ||
        (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z'))
        return true;
    constexpr std::string_view punctuation = ".,:;!?-_/+()[]#@&%'";
    return punctuation.find(character) != std::string_view::npos;
}

[[nodiscard]] Result<void> validate_export_watermark_text(const std::string_view value)
{
    if (value.empty() || value.size() > kExportWatermarkTextMaxBytes)
    {
        return make_error(ErrorCode::kValidation, "Export watermark text length is unsupported",
                          {{"reason", "invalid_export_watermark_text"}, {"parameter", "text"}});
    }
    std::size_t lines = 1U;
    std::size_t characters = 0U;
    for (std::size_t index = 0U; index < value.size();)
    {
        if (value[index] == '\n')
        {
            if (characters == 0U || ++lines > 8U)
            {
                return make_error(
                    ErrorCode::kValidation, "Export watermark text line layout is unsupported",
                    {{"reason", "invalid_export_watermark_text"}, {"parameter", "text"}});
            }
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
            {
                return make_error(
                    ErrorCode::kValidation, "Export watermark text contains an unknown token",
                    {{"reason", "invalid_export_watermark_text"}, {"parameter", "text"}});
            }
            index += token.size();
            if (++characters > 64U)
            {
                return make_error(
                    ErrorCode::kValidation, "Export watermark text line is too long",
                    {{"reason", "invalid_export_watermark_text"}, {"parameter", "text"}});
            }
            continue;
        }
        if (value[index] == '}' || !export_watermark_character_ok(value[index]))
        {
            return make_error(ErrorCode::kUnsupported,
                              "Export watermark text contains an unsupported character",
                              {{"byte_index", std::to_string(index)},
                               {"reason", "unsupported_watermark_character"}});
        }
        ++index;
        if (++characters > 64U)
        {
            return make_error(ErrorCode::kValidation, "Export watermark text line is too long",
                              {{"reason", "invalid_export_watermark_text"}, {"parameter", "text"}});
        }
    }
    if (characters == 0U)
    {
        return make_error(ErrorCode::kValidation,
                          "Export watermark text cannot end with an empty line",
                          {{"reason", "invalid_export_watermark_text"}, {"parameter", "text"}});
    }
    return {};
}

} // namespace

Result<void> validate_export_watermark_alignment(const std::string_view alignment)
{
    static constexpr std::string_view kNames[] = {"top_left",    "top_center",    "top_right",
                                                  "center_left", "center",        "center_right",
                                                  "bottom_left", "bottom_center", "bottom_right"};
    for (const auto name : kNames)
    {
        if (alignment == name)
            return {};
    }
    return make_error(
        ErrorCode::kValidation, "Export watermark alignment is unsupported",
        {{"alignment", std::string(alignment)}, {"reason", "invalid_export_watermark_alignment"}});
}

Result<void> validate_export_watermark_options(const ExportWatermarkOptions &options)
{
    auto text = validate_export_watermark_text(options.text);
    if (!text)
        return text.error();
    for (const double channel : options.color)
    {
        if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0)
        {
            return make_error(
                ErrorCode::kValidation, "Export watermark color channel is outside [0,1]",
                {{"reason", "invalid_export_watermark_color"}, {"parameter", "color"}});
        }
    }
    if (!std::isfinite(options.opacity) || options.opacity < kExportWatermarkOpacityMin ||
        options.opacity > kExportWatermarkOpacityMax)
    {
        return make_error(
            ErrorCode::kValidation, "Export watermark opacity is out of range",
            {{"reason", "invalid_export_watermark_opacity"}, {"parameter", "opacity"}});
    }
    if (!std::isfinite(options.scale_percent) || options.scale_percent < kExportWatermarkScaleMin ||
        options.scale_percent > kExportWatermarkScaleMax)
    {
        return make_error(
            ErrorCode::kValidation, "Export watermark scale is out of range",
            {{"reason", "invalid_export_watermark_scale"}, {"parameter", "scale_percent"}});
    }
    if (!std::isfinite(options.x_offset) || options.x_offset < kExportWatermarkOffsetMin ||
        options.x_offset > kExportWatermarkOffsetMax)
    {
        return make_error(
            ErrorCode::kValidation, "Export watermark x offset is out of range",
            {{"reason", "invalid_export_watermark_offset"}, {"parameter", "x_offset"}});
    }
    if (!std::isfinite(options.y_offset) || options.y_offset < kExportWatermarkOffsetMin ||
        options.y_offset > kExportWatermarkOffsetMax)
    {
        return make_error(
            ErrorCode::kValidation, "Export watermark y offset is out of range",
            {{"reason", "invalid_export_watermark_offset"}, {"parameter", "y_offset"}});
    }
    auto alignment = validate_export_watermark_alignment(options.alignment);
    if (!alignment)
        return alignment.error();
    if (!std::isfinite(options.rotation_degrees) ||
        options.rotation_degrees < kExportWatermarkRotationMin ||
        options.rotation_degrees > kExportWatermarkRotationMax)
    {
        return make_error(
            ErrorCode::kValidation, "Export watermark rotation is out of range",
            {{"reason", "invalid_export_watermark_rotation"}, {"parameter", "rotation_degrees"}});
    }
    return {};
}

Result<void> validate_export_color_options(const ExportColorOptions &options)
{
    static constexpr std::array<std::string_view, 12> kProfiles{
        "srgb",   "adobe_rgb",    "linear_rec709", "linear_rec2020",
        "rec709", "prophoto_rgb", "pq_rec2020",    "hlg_rec2020",
        "pq_p3",  "hlg_p3",       "display_p3",    "file_icc"};
    static constexpr std::array<std::string_view, 4> kIntents{
        "perceptual", "relative_colorimetric", "saturation", "absolute_colorimetric"};
    bool profile_ok = false;
    for (const auto name : kProfiles)
    {
        if (options.output_profile == name)
        {
            profile_ok = true;
            break;
        }
    }
    if (!profile_ok)
    {
        return make_error(
            ErrorCode::kValidation, "Export output profile is unsupported",
            {{"reason", "invalid_export_output_profile"}, {"profile", options.output_profile}});
    }
    const bool file_icc = options.output_profile == "file_icc";
    if (file_icc == options.output_profile_filename.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Export output profile filename must match file_icc selection",
                          {{"reason", "invalid_export_output_profile_filename"}});
    }
    bool intent_ok = false;
    for (const auto name : kIntents)
    {
        if (options.rendering_intent == name)
        {
            intent_ok = true;
            break;
        }
    }
    if (!intent_ok)
    {
        return make_error(
            ErrorCode::kValidation, "Export rendering intent is unsupported",
            {{"reason", "invalid_export_rendering_intent"}, {"intent", options.rendering_intent}});
    }
    return {};
}

Result<void> validate_export_frame_options(const ExportFrameOptions &options)
{
    for (const double channel : options.border_color)
    {
        if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0)
        {
            return make_error(ErrorCode::kValidation, "Export frame border colour is invalid",
                              {{"reason", "invalid_export_frame_border_color"}});
        }
    }
    for (const double channel : options.frame_color)
    {
        if (!std::isfinite(channel) || channel < 0.0 || channel > 1.0)
        {
            return make_error(ErrorCode::kValidation, "Export frame line colour is invalid",
                              {{"reason", "invalid_export_frame_color"}});
        }
    }
    if (!std::isfinite(options.aspect) || (options.aspect < 0.0 && options.aspect != -1.0) ||
        options.aspect > 3.0 ||
        (options.aspect > 0.0 && static_cast<float>(options.aspect) <= 0.0F))
    {
        return make_error(ErrorCode::kValidation, "Export frame aspect is unsupported",
                          {{"reason", "invalid_export_frame_aspect"}});
    }
    static constexpr std::array<std::string_view, 3> kOrientations{"auto", "portrait", "landscape"};
    bool orientation_ok = false;
    for (const auto name : kOrientations)
    {
        if (options.orientation == name)
        {
            orientation_ok = true;
            break;
        }
    }
    if (!orientation_ok)
    {
        return make_error(
            ErrorCode::kValidation, "Export frame orientation is unsupported",
            {{"reason", "invalid_export_frame_orientation"}, {"orientation", options.orientation}});
    }
    if (!std::isfinite(options.size) || options.size < 0.0 || options.size > 0.5)
    {
        return make_error(ErrorCode::kValidation, "Export frame size is out of range",
                          {{"reason", "invalid_export_frame_size"}});
    }
    if (!std::isfinite(options.position_h) || options.position_h < 0.0 ||
        options.position_h > 1.0 || !std::isfinite(options.position_v) ||
        options.position_v < 0.0 || options.position_v > 1.0)
    {
        return make_error(ErrorCode::kValidation, "Export frame position is out of range",
                          {{"reason", "invalid_export_frame_position"}});
    }
    if (!std::isfinite(options.frame_size) || options.frame_size < 0.0 ||
        options.frame_size > 1.0 || !std::isfinite(options.frame_offset) ||
        options.frame_offset < 0.0 || options.frame_offset > 1.0)
    {
        return make_error(ErrorCode::kValidation, "Export frame line geometry is out of range",
                          {{"reason", "invalid_export_frame_line"}});
    }
    static constexpr std::array<std::string_view, 5> kBases{"auto", "width", "height", "shorter",
                                                            "longer"};
    bool basis_ok = false;
    for (const auto name : kBases)
    {
        if (options.basis == name)
        {
            basis_ok = true;
            break;
        }
    }
    if (!basis_ok)
    {
        return make_error(ErrorCode::kValidation, "Export frame basis is unsupported",
                          {{"reason", "invalid_export_frame_basis"}, {"basis", options.basis}});
    }
    return {};
}

Result<void> validate_export_options(const ExportOptions &options)
{
    if (options.max_edge > kExportMaxEdgeMax)
    {
        return make_error(ErrorCode::kValidation, "Export long edge is out of range",
                          {{"reason", "invalid_export_max_edge"},
                           {"max_edge", std::to_string(options.max_edge)}});
    }
    auto width = validate_box_limit(options.max_width, "max_width");
    if (!width)
        return width.error();
    auto height = validate_box_limit(options.max_height, "max_height");
    if (!height)
        return height.error();
    auto sharpen = validate_export_output_sharpen_options(options.output_sharpen);
    if (!sharpen)
        return sharpen.error();
    auto output_color = validate_export_color_options(options.output_color);
    if (!output_color)
        return output_color.error();
    auto frame = validate_export_frame_options(options.frame);
    if (!frame)
        return frame.error();
    auto watermark = validate_export_watermark_options(options.watermark);
    if (!watermark)
        return watermark.error();

    Result<void> format_valid;
    switch (options.format)
    {
    case ExportFormat::kJpeg:
        format_valid = validate_jpeg_export_options(options.jpeg_options);
        break;
    case ExportFormat::kPng:
        format_valid = validate_png_export_options(options.png_options);
        break;
    case ExportFormat::kTiff:
        format_valid = validate_tiff_export_options(options.tiff_options);
        break;
    case ExportFormat::kOriginalCopy:
        if (options.metadata_mode != ExportMetadataMode::kFull)
        {
            return make_error(ErrorCode::kValidation,
                              "Metadata privacy mode does not apply to original copy",
                              {{"format", "original"}, {"reason", "metadata_mode_not_applicable"}});
        }
        if (export_options_request_resize(options) ||
            export_options_request_output_sharpen(options) ||
            export_options_request_output_color(options) || export_options_request_frame(options) ||
            export_options_request_watermark(options))
        {
            return make_error(
                ErrorCode::kValidation,
                "Original copy rejects resize, colour, frame, sharpen, and watermark fields",
                {{"format", "original"}, {"reason", "original_copy_resize_not_applicable"}});
        }
        return {};
    }
    return format_valid;
}

Result<ExportPreset> parse_export_preset_json(const std::string_view text)
{
    if (text.size() > kExportPresetFileMaxBytes)
    {
        return make_error(
            ErrorCode::kValidation, "Export preset file is too large",
            {{"reason", "export_preset_too_large"}, {"bytes", std::to_string(text.size())}});
    }
    auto parsed = parse_json(text);
    if (!parsed)
        return make_error(
            ErrorCode::kValidation, "Export preset JSON is corrupt",
            {{"reason", "corrupt_export_preset"}, {"detail", parsed.error().message}});
    auto schema = require_string(parsed.value(), "schema");
    if (!schema)
        return schema.error();
    if (schema.value() != kExportPresetSchema)
    {
        return make_error(ErrorCode::kUnsupported, "Export preset schema is unknown",
                          {{"schema", schema.value()}, {"reason", "unknown_export_preset_schema"}});
    }
    auto version = require_int64(parsed.value(), "schema_version");
    if (!version)
        return version.error();
    if (version.value() != kExportPresetSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Export preset schema version is unsupported",
                          {{"schema_version", std::to_string(version.value())},
                           {"reason", "unsupported_export_preset_schema"}});
    }
    const auto *root = parsed.value().object_if();
    if (root == nullptr)
        return preset_error("Export preset root must be an object", "invalid_export_json_root");
    const auto options_found = root->find("options");
    if (options_found == root->end())
        return preset_error("Export preset requires options", "missing_export_json_field",
                            "options");
    // Fail closed on unexpected top-level keys.
    for (const auto &[key, _] : *root)
    {
        if (key != "schema" && key != "schema_version" && key != "options")
            return preset_error("Export preset contains an unknown field",
                                "unknown_export_preset_field", key);
    }
    auto options = parse_export_options_object(options_found->second);
    if (!options)
        return options.error();
    ExportPreset preset;
    preset.schema_version = version.value();
    preset.options = std::move(options).value();
    return preset;
}

Result<std::string> serialize_export_preset(const ExportPreset &preset)
{
    if (preset.schema_version != kExportPresetSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Export preset schema version is unsupported",
                          {{"schema_version", std::to_string(preset.schema_version)},
                           {"reason", "unsupported_export_preset_schema"}});
    }
    auto options = serialize_export_options_object(preset.options);
    if (!options)
        return options.error();
    JsonValue::Object root;
    root.emplace("schema", std::string(kExportPresetSchema));
    root.emplace("schema_version", number_i64(preset.schema_version));
    root.emplace("options", std::move(options).value());
    return serialize_json(JsonValue{std::move(root)});
}

Result<ExportOptions> apply_export_preset(const ExportPreset &preset)
{
    if (preset.schema_version != kExportPresetSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Export preset schema version is unsupported",
                          {{"schema_version", std::to_string(preset.schema_version)},
                           {"reason", "unsupported_export_preset_schema"}});
    }
    auto valid = validate_export_options(preset.options);
    if (!valid)
        return valid.error();
    return preset.options;
}

std::string_view export_job_item_status_name(const ExportJobItemStatus status) noexcept
{
    switch (status)
    {
    case ExportJobItemStatus::kPending:
        return "pending";
    case ExportJobItemStatus::kDelivered:
        return "delivered";
    case ExportJobItemStatus::kFailed:
        return "failed";
    }
    return "pending";
}

Result<ExportJobItemStatus> parse_export_job_item_status(const std::string_view name)
{
    if (name == "pending")
        return ExportJobItemStatus::kPending;
    if (name == "delivered")
        return ExportJobItemStatus::kDelivered;
    if (name == "failed")
        return ExportJobItemStatus::kFailed;
    return make_error(ErrorCode::kValidation, "Export job item status is unknown",
                      {{"value", std::string(name)}, {"reason", "invalid_export_job_item_status"}});
}

Result<ExportJob> parse_export_job_json(const std::string_view text)
{
    if (text.size() > kExportJobFileMaxBytes)
    {
        return make_error(
            ErrorCode::kValidation, "Export job file is too large",
            {{"reason", "export_job_too_large"}, {"bytes", std::to_string(text.size())}});
    }
    auto parsed = parse_json(text);
    if (!parsed)
        return make_error(ErrorCode::kValidation, "Export job JSON is corrupt",
                          {{"reason", "corrupt_export_job"}, {"detail", parsed.error().message}});
    auto schema = require_string(parsed.value(), "schema");
    if (!schema)
        return schema.error();
    if (schema.value() != kExportJobSchema)
    {
        return make_error(ErrorCode::kUnsupported, "Export job schema is unknown",
                          {{"schema", schema.value()}, {"reason", "unknown_export_job_schema"}});
    }
    auto version = require_int64(parsed.value(), "schema_version");
    if (!version)
        return version.error();
    if (version.value() != kExportJobSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Export job schema version is unsupported",
                          {{"schema_version", std::to_string(version.value())},
                           {"reason", "unsupported_export_job_schema"}});
    }
    auto job_id = require_string(parsed.value(), "job_id");
    auto output_directory = require_string(parsed.value(), "output_directory");
    auto filename_template = require_string(parsed.value(), "filename_template");
    if (!job_id || !output_directory || !filename_template)
        return !job_id           ? job_id.error() :
               !output_directory ? output_directory.error() :
                                   filename_template.error();
    const auto *root = parsed.value().object_if();
    if (root == nullptr)
        return preset_error("Export job root must be an object", "invalid_export_json_root");
    for (const auto &[key, _] : *root)
    {
        if (key != "schema" && key != "schema_version" && key != "job_id" && key != "asset_ids" &&
            key != "options" && key != "output_directory" && key != "filename_template" &&
            key != "items")
            return preset_error("Export job contains an unknown field", "unknown_export_job_field",
                                key);
    }
    const auto options_found = root->find("options");
    const auto assets_found = root->find("asset_ids");
    const auto items_found = root->find("items");
    if (options_found == root->end() || assets_found == root->end() || items_found == root->end())
        return preset_error("Export job is missing required fields", "missing_export_json_field",
                            "items");
    auto options = parse_export_options_object(options_found->second);
    if (!options)
        return options.error();
    const auto *assets = assets_found->second.array_if();
    if (assets == nullptr)
        return preset_error("Export job asset_ids must be an array", "invalid_export_job_assets",
                            "asset_ids");
    const auto *items = items_found->second.array_if();
    if (items == nullptr)
        return preset_error("Export job items must be an array", "invalid_export_job_items",
                            "items");
    ExportJob job;
    job.schema_version = version.value();
    job.job_id = std::move(job_id).value();
    job.output_directory = std::move(output_directory).value();
    job.filename_template = std::move(filename_template).value();
    job.options = std::move(options).value();
    job.asset_ids.reserve(assets->size());
    for (const auto &entry : *assets)
    {
        const auto *id = entry.string_if();
        if (id == nullptr || id->empty())
            return preset_error("Export job asset id is invalid", "invalid_export_job_asset_id",
                                "asset_ids");
        job.asset_ids.push_back(*id);
    }
    job.items.reserve(items->size());
    for (const auto &entry : *items)
    {
        const auto *item_object = entry.object_if();
        if (item_object == nullptr)
            return preset_error("Export job item must be an object", "invalid_export_job_item");
        ExportJobItem item;
        auto asset_id = require_string(entry, "asset_id");
        auto status_name = require_string(entry, "status");
        auto output_path = require_string(entry, "output_path");
        if (!asset_id || !status_name || !output_path)
            return !asset_id    ? asset_id.error() :
                   !status_name ? status_name.error() :
                                  output_path.error();
        auto status = parse_export_job_item_status(status_name.value());
        if (!status)
            return status.error();
        item.asset_id = std::move(asset_id).value();
        item.status = status.value();
        item.output_path = std::move(output_path).value();
        if (const auto error_reason = item_object->find("error_reason");
            error_reason != item_object->end())
        {
            const auto *text_value = error_reason->second.string_if();
            if (text_value == nullptr)
                return preset_error("Export job item error_reason must be a string",
                                    "invalid_export_job_item", "error_reason");
            item.error_reason = *text_value;
        }
        if (const auto error_message = item_object->find("error_message");
            error_message != item_object->end())
        {
            const auto *text_value = error_message->second.string_if();
            if (text_value == nullptr)
                return preset_error("Export job item error_message must be a string",
                                    "invalid_export_job_item", "error_message");
            item.error_message = *text_value;
        }
        job.items.push_back(std::move(item));
    }
    if (job.asset_ids.size() != job.items.size())
    {
        return make_error(ErrorCode::kValidation,
                          "Export job asset list and item outcomes must match",
                          {{"reason", "export_job_item_count_mismatch"},
                           {"asset_count", std::to_string(job.asset_ids.size())},
                           {"item_count", std::to_string(job.items.size())}});
    }
    for (std::size_t index = 0; index < job.asset_ids.size(); ++index)
    {
        if (job.asset_ids[index] != job.items[index].asset_id)
        {
            return make_error(ErrorCode::kValidation, "Export job item order must match asset_ids",
                              {{"reason", "export_job_item_order_mismatch"},
                               {"batch_index", std::to_string(index + 1U)}});
        }
    }
    return job;
}

Result<std::string> serialize_export_job(const ExportJob &job)
{
    if (job.schema_version != kExportJobSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported, "Export job schema version is unsupported",
                          {{"schema_version", std::to_string(job.schema_version)},
                           {"reason", "unsupported_export_job_schema"}});
    }
    auto options = serialize_export_options_object(job.options);
    if (!options)
        return options.error();
    JsonValue::Array assets;
    assets.reserve(job.asset_ids.size());
    for (const auto &id : job.asset_ids)
        assets.emplace_back(id);
    JsonValue::Array items;
    items.reserve(job.items.size());
    for (const auto &item : job.items)
    {
        JsonValue::Object object;
        object.emplace("asset_id", item.asset_id);
        object.emplace("status", std::string(export_job_item_status_name(item.status)));
        object.emplace("output_path", item.output_path);
        if (item.error_reason)
            object.emplace("error_reason", *item.error_reason);
        if (item.error_message)
            object.emplace("error_message", *item.error_message);
        items.emplace_back(std::move(object));
    }
    JsonValue::Object root;
    root.emplace("schema", std::string(kExportJobSchema));
    root.emplace("schema_version", number_i64(job.schema_version));
    root.emplace("job_id", job.job_id);
    root.emplace("asset_ids", JsonValue{std::move(assets)});
    root.emplace("options", std::move(options).value());
    root.emplace("output_directory", job.output_directory);
    root.emplace("filename_template", job.filename_template);
    root.emplace("items", JsonValue{std::move(items)});
    return serialize_json(JsonValue{std::move(root)});
}

} // namespace ravo
