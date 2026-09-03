#include "ravo/domain/types.h"

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
    static constexpr std::string_view kAllowed[] = {"format",    "metadata_mode", "max_edge",
                                                    "max_width", "max_height",    "output_sharpen",
                                                    "jpeg",      "png",           "tiff"};
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
    JsonValue::Object object;
    object.emplace("format", std::string(export_format_name(options.format)));
    object.emplace("metadata_mode", std::string(export_metadata_mode_name(options.metadata_mode)));
    object.emplace("max_edge", number_u32(options.max_edge));
    object.emplace("max_width", number_u32(options.max_width));
    object.emplace("max_height", number_u32(options.max_height));
    object.emplace("output_sharpen", std::move(sharpen).value());
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
            export_options_request_output_sharpen(options))
        {
            return make_error(
                ErrorCode::kValidation, "Original copy rejects resize and output sharpen fields",
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
