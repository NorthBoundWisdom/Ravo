#include "ravo/cli/application.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/style.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

namespace
{

struct ParsedArguments
{
    bool json = false;
    std::vector<std::string_view> positional;
};

[[nodiscard]] Result<ParsedArguments>
parse_arguments(const std::span<const std::string_view> arguments)
{
    ParsedArguments parsed;
    for (const auto argument : arguments)
    {
        if (argument == "--json")
        {
            if (parsed.json)
            {
                return make_error(ErrorCode::kInvalidArgument, "--json can only be specified once");
            }
            parsed.json = true;
            continue;
        }
        if (argument == "--version")
        {
            parsed.positional.push_back(argument);
            continue;
        }
        parsed.positional.push_back(argument);
    }
    return parsed;
}

[[nodiscard]] JsonValue error_object(const TaskError &error)
{
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
    {
        context.emplace(key, value);
    }
    return JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                             {"context", std::move(context)},
                             {"message", error.message}};
}

[[nodiscard]] JsonValue success_envelope(JsonValue data)
{
    return JsonValue::Object{{"data", std::move(data)},
                             {"diagnostics", JsonValue::Array{}},
                             {"ok", true},
                             {"type", "ravo.cli.result"},
                             {"version", JsonValue::number("1")}};
}

[[nodiscard]] JsonValue failure_envelope(const TaskError &error)
{
    return JsonValue::Object{{"diagnostics", JsonValue::Array{}},
                             {"error", error_object(error)},
                             {"ok", false},
                             {"type", "ravo.cli.result"},
                             {"version", JsonValue::number("1")}};
}

struct RenderCliArguments
{
    std::string_view input;
    std::string_view recipe;
    std::string_view output;
    std::string_view backend = "cpu";
    bool backend_specified = false;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
};

[[nodiscard]] Result<std::uint32_t> parse_dimension(const std::string_view text,
                                                    const std::string_view option)
{
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Render dimension must be a positive integer",
                          {{"option", std::string(option)}, {"value", std::string(text)}});
    }
    return value;
}

[[nodiscard]] Result<RenderCliArguments>
parse_render_arguments(const std::span<const std::string_view> positional)
{
    if (positional.size() < 2 || positional[0] != "render" || positional[1].starts_with("--"))
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo render <input> --recipe <recipe> --output <png> [--backend cpu]");
    }

    RenderCliArguments result{positional[1], {}, {}, "cpu", false, std::nullopt, std::nullopt};
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (option != "--recipe" && option != "--output" && option != "--backend" &&
            option != "--width" && option != "--height")
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown render option",
                              {{"option", std::string(option)}});
        }
        if (index + 1 >= positional.size() || positional[index + 1].starts_with("--"))
        {
            return make_error(ErrorCode::kInvalidArgument, "Render option requires a value",
                              {{"option", std::string(option)}});
        }
        const auto value = positional[++index];
        if (option == "--recipe")
        {
            if (!result.recipe.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Render recipe was specified twice");
            }
            result.recipe = value;
        }
        else if (option == "--output")
        {
            if (!result.output.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Render output was specified twice");
            }
            result.output = value;
        }
        else if (option == "--backend")
        {
            if (result.backend_specified)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Render backend was specified twice");
            }
            result.backend = value;
            result.backend_specified = true;
        }
        else
        {
            auto dimension = parse_dimension(value, option);
            if (!dimension)
            {
                return dimension.error();
            }
            auto &target = option == "--width" ? result.width : result.height;
            if (target.has_value())
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Render dimension was specified twice",
                                  {{"option", std::string(option)}});
            }
            target = dimension.value();
        }
    }
    if (result.recipe.empty() || result.output.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Render requires --recipe and --output options");
    }
    if (result.backend != "cpu")
    {
        return make_error(ErrorCode::kUnsupported, "Only the CPU render backend is available",
                          {{"backend", std::string(result.backend)}});
    }
    return result;
}

struct CatalogCliArguments
{
    bool baseline = false;
    std::string_view catalog;
    std::vector<std::string_view> inputs;
    std::string_view asset_id;
    std::vector<std::string_view> asset_ids;
    std::optional<int> rating;
    std::optional<double> exposure_ev;
    std::optional<double> saturation;
    std::optional<double> contrast;
    std::optional<std::uint32_t> max_edge;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::optional<std::pair<double, double>> pick_white;
    std::optional<std::string_view> watermark_text;
    std::string_view output;
    std::string_view output_directory;
    std::string_view filename_template;
    std::string_view format;
    std::string_view metadata_mode;
    std::string_view quality;
    std::string_view jpeg_subsampling;
    std::string_view tiff_sample_type;
    std::string_view tiff_compression;
    std::string_view tiff_compression_level;
    std::string_view tiff_resolution_dpi;
    bool tiff_grayscale_if_neutral = false;
    std::string_view png_bit_depth;
    std::string_view png_compression;
    std::string_view tag;
    std::string_view add;
    std::string_view remove;
    std::string_view title;
    std::string_view description;
    std::string_view creator;
    std::string_view copyright;
    std::string_view label;
    std::optional<std::int64_t> history_id;
};

[[nodiscard]] Result<int> parse_int_flag(const std::string_view text, const std::string_view option)
{
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "Option requires an integer",
                          {{"option", std::string(option)}, {"value", std::string(text)}});
    }
    return value;
}

[[nodiscard]] TaskError annotate_export_option_error(TaskError error, const std::string_view option,
                                                     const std::string_view value,
                                                     const std::string_view format,
                                                     const std::string_view reason)
{
    error.context.insert_or_assign("format", std::string(format));
    error.context.insert_or_assign("option", std::string(option));
    error.context.insert_or_assign("reason", std::string(reason));
    error.context.insert_or_assign("value", std::string(value));
    return error;
}

[[nodiscard]] Result<int> parse_export_int_flag(const std::string_view text,
                                                const std::string_view option,
                                                const std::string_view format,
                                                const std::string_view reason)
{
    auto parsed = parse_int_flag(text, option);
    if (!parsed)
    {
        return annotate_export_option_error(parsed.error(), option, text, format, reason);
    }
    return parsed.value();
}

[[nodiscard]] Result<double> parse_double_flag(const std::string_view text,
                                               const std::string_view option)
{
    const std::string owned(text);
    char *end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size() || !std::isfinite(value))
    {
        return make_error(ErrorCode::kInvalidArgument, "Option requires a finite number",
                          {{"option", std::string(option)}, {"value", std::string(text)}});
    }
    return value;
}

[[nodiscard]] Result<CatalogCliArguments>
parse_catalog_flags(const std::span<const std::string_view> positional)
{
    CatalogCliArguments result;
    const bool batch_export = positional.size() > 1U && positional[1] == "export-batch";
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (option == "--baseline")
        {
            if (result.baseline)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "--baseline can only be specified once");
            }
            result.baseline = true;
            continue;
        }
        if (option == "--tiff-grayscale-if-neutral")
        {
            if (result.tiff_grayscale_if_neutral)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "--tiff-grayscale-if-neutral can only be specified once");
            }
            result.tiff_grayscale_if_neutral = true;
            continue;
        }
        if (index + 1 >= positional.size() || positional[index + 1].starts_with("--"))
        {
            return make_error(ErrorCode::kInvalidArgument, "Catalog option requires a value",
                              {{"option", std::string(option)}});
        }
        const auto value = positional[++index];
        if (option == "--path" || option == "--catalog")
        {
            if (!result.catalog.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Catalog path was specified twice");
            }
            result.catalog = value;
        }
        else if (option == "--input")
        {
            result.inputs.push_back(value);
        }
        else if (option == "--asset-id")
        {
            if (batch_export)
            {
                result.asset_ids.push_back(value);
            }
            else if (!result.asset_id.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Asset ID was specified twice");
            }
            else
            {
                result.asset_id = value;
            }
        }
        else if (option == "--rating")
        {
            auto rating = parse_int_flag(value, option);
            if (!rating)
            {
                return rating.error();
            }
            result.rating = rating.value();
        }
        else if (option == "--exposure-ev")
        {
            auto parsed = parse_double_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.exposure_ev = parsed.value();
        }
        else if (option == "--saturation")
        {
            auto parsed = parse_double_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.saturation = parsed.value();
        }
        else if (option == "--contrast")
        {
            auto parsed = parse_double_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.contrast = parsed.value();
        }
        else if (option == "--pick-white")
        {
            const auto owned = std::string(value);
            const auto split = owned.find(',');
            if (split == std::string::npos || split == 0 || split + 1 == owned.size())
            {
                return make_error(ErrorCode::kInvalidArgument, "--pick-white requires x,y",
                                  {{"value", owned}});
            }
            auto x = parse_double_flag(owned.substr(0, split), option);
            if (!x)
            {
                return x.error();
            }
            auto y = parse_double_flag(owned.substr(split + 1), option);
            if (!y)
            {
                return y.error();
            }
            result.pick_white = std::pair<double, double>{x.value(), y.value()};
        }
        else if (option == "--set")
        {
            const auto owned = std::string(value);
            const auto split = owned.find('=');
            if (split == std::string::npos || split == 0 || split + 1 == owned.size())
            {
                return make_error(ErrorCode::kInvalidArgument, "--set requires name=value",
                                  {{"value", owned}});
            }
            auto parsed = parse_double_flag(owned.substr(split + 1), option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.develop_sets.emplace_back(owned.substr(0, split), parsed.value());
        }
        else if (option == "--watermark-text")
        {
            if (result.watermark_text.has_value())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--watermark-text was specified more than once");
            result.watermark_text = value;
        }
        else if (option == "--max-edge")
        {
            auto dimension = parse_dimension(value, option);
            if (!dimension)
            {
                return dimension.error();
            }
            result.max_edge = dimension.value();
        }
        else if (option == "--output")
        {
            if (batch_export)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Batch export uses --output-dir, not --output");
            }
            if (!result.output.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Output path was specified twice");
            }
            result.output = value;
        }
        else if (batch_export && option == "--output-dir")
        {
            if (!result.output_directory.empty())
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Output directory was specified twice");
            }
            result.output_directory = value;
        }
        else if (batch_export && option == "--filename-template")
        {
            if (!result.filename_template.empty())
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Filename template was specified twice");
            }
            result.filename_template = value;
        }
        else if (option == "--format")
        {
            if (!result.format.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Export format was specified twice");
            }
            result.format = value;
        }
        else if (option == "--metadata")
        {
            result.metadata_mode = value;
        }
        else if (option == "--quality")
        {
            result.quality = value;
        }
        else if (option == "--jpeg-subsampling")
        {
            result.jpeg_subsampling = value;
        }
        else if (option == "--tiff-sample-type")
        {
            result.tiff_sample_type = value;
        }
        else if (option == "--tiff-compression")
        {
            result.tiff_compression = value;
        }
        else if (option == "--tiff-compression-level")
        {
            result.tiff_compression_level = value;
        }
        else if (option == "--tiff-resolution-dpi")
        {
            result.tiff_resolution_dpi = value;
        }
        else if (option == "--png-bit-depth")
        {
            result.png_bit_depth = value;
        }
        else if (option == "--png-compression")
        {
            result.png_compression = value;
        }
        else if (option == "--tag")
        {
            result.tag = value;
        }
        else if (option == "--add")
        {
            result.add = value;
        }
        else if (option == "--remove")
        {
            result.remove = value;
        }
        else if (option == "--title")
        {
            result.title = value;
        }
        else if (option == "--description")
        {
            result.description = value;
        }
        else if (option == "--creator")
        {
            result.creator = value;
        }
        else if (option == "--copyright")
        {
            result.copyright = value;
        }
        else if (option == "--label")
        {
            result.label = value;
        }
        else if (option == "--history-id")
        {
            auto parsed = parse_int_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.history_id = parsed.value();
        }
        else
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown catalog option",
                              {{"option", std::string(option)}});
        }
    }
    return result;
}

[[nodiscard]] bool has_explicit_tiff_options(const CatalogCliArguments &flags) noexcept
{
    return !flags.tiff_sample_type.empty() || !flags.tiff_compression.empty() ||
           !flags.tiff_compression_level.empty() || !flags.tiff_resolution_dpi.empty() ||
           flags.tiff_grayscale_if_neutral;
}

[[nodiscard]] bool has_explicit_png_options(const CatalogCliArguments &flags) noexcept
{
    return !flags.png_bit_depth.empty() || !flags.png_compression.empty();
}

[[nodiscard]] bool has_explicit_jpeg_options(const CatalogCliArguments &flags) noexcept
{
    return !flags.quality.empty() || !flags.jpeg_subsampling.empty();
}

[[nodiscard]] Result<void> validate_cli_export_options(const ExportRequest &request,
                                                       const CatalogCliArguments &flags)
{
    Result<void> valid;
    switch (request.format)
    {
    case ExportFormat::kJpeg:
        valid = validate_jpeg_export_options(request.jpeg_options);
        break;
    case ExportFormat::kPng:
        valid = validate_png_export_options(request.png_options);
        break;
    case ExportFormat::kTiff:
        valid = validate_tiff_export_options(request.tiff_options);
        break;
    case ExportFormat::kOriginalCopy:
        return {};
    }
    if (valid)
    {
        return {};
    }

    TaskError error = valid.error();
    const auto found = error.context.find("reason");
    if (found == error.context.end())
    {
        return error;
    }
    const std::string reason = found->second;
    const auto annotate = [&](const std::string_view option, const std::string_view value,
                              const std::string_view format)
    {
        return Result<void>{
            annotate_export_option_error(std::move(error), option, value, format, reason)};
    };
    if (reason == "invalid_jpeg_quality" && !flags.quality.empty())
    {
        return annotate("--quality", flags.quality, "jpeg");
    }
    if (reason == "invalid_png_compression" && !flags.png_compression.empty())
    {
        return annotate("--png-compression", flags.png_compression, "png");
    }
    if (reason == "invalid_tiff_compression_level" && !flags.tiff_compression_level.empty())
    {
        return annotate("--tiff-compression-level", flags.tiff_compression_level, "tiff");
    }
    if (reason == "invalid_tiff_resolution" && !flags.tiff_resolution_dpi.empty())
    {
        return annotate("--tiff-resolution-dpi", flags.tiff_resolution_dpi, "tiff");
    }
    return error;
}

[[nodiscard]] Result<ExportFormat> resolved_export_format(const CatalogCliArguments &flags)
{
    if (flags.format.empty())
    {
        return ExportFormat::kPng;
    }
    return parse_export_format(flags.format);
}

[[nodiscard]] Result<ExportOptions> resolved_export_options(const CatalogCliArguments &flags)
{
    ExportRequest request;
    auto format = resolved_export_format(flags);
    if (!format)
        return format.error();
    request.format = format.value();
    if (!flags.metadata_mode.empty())
    {
        auto metadata_mode = parse_export_metadata_mode(flags.metadata_mode);
        if (!metadata_mode)
        {
            auto error = metadata_mode.error();
            error.context.emplace("option", "--metadata");
            return error;
        }
        request.metadata_mode = metadata_mode.value();
    }
    if (!flags.quality.empty())
    {
        auto quality =
            parse_export_int_flag(flags.quality, "--quality", "jpeg", "invalid_jpeg_quality");
        if (!quality)
            return quality.error();
        request.jpeg_options.quality = quality.value();
    }
    if (!flags.jpeg_subsampling.empty())
    {
        auto subsampling = parse_jpeg_subsampling(flags.jpeg_subsampling);
        if (!subsampling)
        {
            return annotate_export_option_error(subsampling.error(), "--jpeg-subsampling",
                                                flags.jpeg_subsampling, "jpeg",
                                                "invalid_jpeg_subsampling");
        }
        request.jpeg_options.subsampling = subsampling.value();
    }
    if (!flags.tiff_sample_type.empty())
    {
        auto sample_type = parse_tiff_sample_type(flags.tiff_sample_type);
        if (!sample_type)
        {
            return annotate_export_option_error(sample_type.error(), "--tiff-sample-type",
                                                flags.tiff_sample_type, "tiff",
                                                "invalid_tiff_sample_type");
        }
        request.tiff_options.sample_type = sample_type.value();
    }
    if (!flags.tiff_compression.empty())
    {
        auto compression = parse_tiff_compression(flags.tiff_compression);
        if (!compression)
        {
            return annotate_export_option_error(compression.error(), "--tiff-compression",
                                                flags.tiff_compression, "tiff",
                                                "invalid_tiff_compression");
        }
        request.tiff_options.compression = compression.value();
    }
    if (!flags.tiff_compression_level.empty())
    {
        auto compression_level =
            parse_export_int_flag(flags.tiff_compression_level, "--tiff-compression-level", "tiff",
                                  "invalid_tiff_compression_level");
        if (!compression_level)
            return compression_level.error();
        request.tiff_options.compression_level = compression_level.value();
    }
    if (!flags.tiff_resolution_dpi.empty())
    {
        auto resolution = parse_export_int_flag(flags.tiff_resolution_dpi, "--tiff-resolution-dpi",
                                                "tiff", "invalid_tiff_resolution");
        if (!resolution)
            return resolution.error();
        request.tiff_options.resolution_dpi = resolution.value();
    }
    request.tiff_options.grayscale_if_neutral = flags.tiff_grayscale_if_neutral;
    if (!flags.png_bit_depth.empty())
    {
        auto bit_depth = parse_png_bit_depth(flags.png_bit_depth);
        if (!bit_depth)
        {
            return annotate_export_option_error(bit_depth.error(), "--png-bit-depth",
                                                flags.png_bit_depth, "png",
                                                "invalid_png_bit_depth");
        }
        request.png_options.bit_depth = bit_depth.value();
    }
    if (!flags.png_compression.empty())
    {
        auto compression = parse_export_int_flag(flags.png_compression, "--png-compression", "png",
                                                 "invalid_png_compression");
        if (!compression)
            return compression.error();
        request.png_options.compression = compression.value();
    }
    if (flags.max_edge)
        request.max_edge = *flags.max_edge;
    auto valid = validate_cli_export_options(request, flags);
    if (!valid)
        return valid.error();
    return static_cast<ExportOptions>(request);
}

[[nodiscard]] Result<void> reject_scoped_export_options(const CatalogCliArguments &flags,
                                                        const std::string_view subcommand)
{
    const bool is_export = subcommand == "export" || subcommand == "export-batch";
    if (is_export)
    {
        auto format = resolved_export_format(flags);
        if (!format)
        {
            return format.error();
        }
        if (has_explicit_jpeg_options(flags) && format.value() != ExportFormat::kJpeg)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "JPEG options require catalog export with --format jpeg",
                              {{"format", std::string(export_format_name(format.value()))},
                               {"reason", "jpeg_options_require_jpeg_export"}});
        }
        if (has_explicit_png_options(flags) && format.value() != ExportFormat::kPng)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "PNG options require catalog export with --format png",
                              {{"format", std::string(export_format_name(format.value()))},
                               {"reason", "png_options_require_png_export"}});
        }
        if (has_explicit_tiff_options(flags) && format.value() != ExportFormat::kTiff)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "TIFF options require catalog export with --format tiff",
                              {{"format", std::string(export_format_name(format.value()))},
                               {"reason", "tiff_options_require_tiff_export"}});
        }
        return {};
    }
    if (has_explicit_jpeg_options(flags))
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "JPEG options require catalog export with --format jpeg",
                          {{"reason", "jpeg_options_require_jpeg_export"},
                           {"subcommand", std::string(subcommand)}});
    }
    if (has_explicit_png_options(flags))
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "PNG options require catalog export with --format png",
                          {{"reason", "png_options_require_png_export"},
                           {"subcommand", std::string(subcommand)}});
    }
    if (has_explicit_tiff_options(flags))
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "TIFF options require catalog export with --format tiff",
                          {{"reason", "tiff_options_require_tiff_export"},
                           {"subcommand", std::string(subcommand)}});
    }
    if (!flags.metadata_mode.empty())
    {
        return make_error(
            ErrorCode::kInvalidArgument, "Metadata privacy mode requires catalog export",
            {{"reason", "metadata_mode_requires_export"}, {"subcommand", std::string(subcommand)}});
    }
    return {};
}

struct AppliedDevelopOverride
{
    std::string name;
    std::variant<double, std::string> value;
};

[[nodiscard]] Result<std::vector<AppliedDevelopOverride>>
apply_develop_overrides(DevelopParams &params, const CatalogCliArguments &flags)
{
    std::vector<AppliedDevelopOverride> applied;
    std::set<std::string, std::less<>> names;
    const auto apply_one = [&](const std::string_view name, const double value) -> Result<void>
    {
        if (!names.emplace(name).second)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Develop field was specified more than once",
                              {{"name", std::string(name)}});
        }
        DevelopParams candidate = params;
        auto assigned = apply_develop_field_strict(candidate, name, value);
        if (!assigned)
        {
            return assigned.error();
        }
        params = std::move(candidate);
        applied.push_back({std::string(name), value});
        return {};
    };

    if (flags.exposure_ev)
    {
        auto result = apply_one("exposure", *flags.exposure_ev);
        if (!result)
        {
            return result.error();
        }
    }
    if (flags.saturation)
    {
        auto result = apply_one("saturation", *flags.saturation);
        if (!result)
        {
            return result.error();
        }
    }
    if (flags.contrast)
    {
        auto result = apply_one("contrast", *flags.contrast);
        if (!result)
        {
            return result.error();
        }
    }
    for (const auto &[name, value] : flags.develop_sets)
    {
        auto result = apply_one(name, value);
        if (!result)
        {
            return result.error();
        }
    }
    if (flags.watermark_text.has_value())
    {
        constexpr std::string_view name = "watermarkText";
        if (!names.emplace(name).second)
            return make_error(ErrorCode::kInvalidArgument,
                              "Develop field was specified more than once",
                              {{"name", std::string(name)}});
        DevelopParams candidate = params;
        auto assigned = apply_develop_text_field_strict(candidate, name, *flags.watermark_text);
        if (!assigned)
            return assigned.error();
        params = std::move(candidate);
        applied.push_back({std::string(name), std::string(*flags.watermark_text)});
    }
    clamp_develop(params);
    return applied;
}

[[nodiscard]] Result<std::unique_ptr<CatalogService>>
open_catalog_session(const EngineFacade &engine, const std::string_view path, const bool create)
{
    auto repository =
        create ? SqliteCatalogRepository::create(path) : SqliteCatalogRepository::open(path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(std::string(path) + ".preview");
    if (!cache)
    {
        return cache.error();
    }
    return std::make_unique<CatalogService>(engine, std::move(repository).value(),
                                            std::make_unique<QtRasterDecoder>(),
                                            std::move(cache).value());
}

[[nodiscard]] JsonValue optional_string_json(const std::optional<std::string> &value)
{
    if (!value)
    {
        return nullptr;
    }
    return *value;
}

[[nodiscard]] JsonValue asset_to_json(const AssetRecord &asset)
{
    JsonValue::Array tags;
    for (const auto &tag : asset.tags)
    {
        tags.push_back(tag);
    }
    JsonValue::Object metadata{
        {"copyright", optional_string_json(asset.metadata.copyright)},
        {"creator", optional_string_json(asset.metadata.creator)},
        {"description", optional_string_json(asset.metadata.description)},
        {"title", optional_string_json(asset.metadata.title)},
    };
    JsonValue captured_at{nullptr};
    if (asset.capture.captured_datetime)
    {
        captured_at = format_capture_datetime_iso(*asset.capture.captured_datetime);
    }
    JsonValue gps{nullptr};
    if (asset.capture.location)
    {
        JsonValue::Object gps_object{
            {"latitude",
             JsonValue::number(format_scaled_decimal(asset.capture.location->latitude_e6, 6))},
            {"longitude",
             JsonValue::number(format_scaled_decimal(asset.capture.location->longitude_e6, 6))},
        };
        if (asset.capture.location->altitude)
        {
            const auto &altitude = *asset.capture.location->altitude;
            std::int64_t signed_mm = static_cast<std::int64_t>(altitude.magnitude_mm);
            if (altitude.reference == CaptureAltitudeReference::kBelowSeaLevel)
            {
                signed_mm = -signed_mm;
            }
            gps_object.emplace("altitude_m",
                               JsonValue::number(format_scaled_decimal(signed_mm, 3)));
        }
        gps = std::move(gps_object);
    }
    JsonValue::Object capture{
        {"aperture", asset.capture.aperture ?
                         JsonValue::number(std::to_string(*asset.capture.aperture)) :
                         JsonValue{nullptr}},
        {"camera_make", optional_string_json(asset.capture.camera_make)},
        {"camera_model", optional_string_json(asset.capture.camera_model)},
        {"captured_at", std::move(captured_at)},
        {"focal_length_mm", asset.capture.focal_length_mm ?
                                JsonValue::number(std::to_string(*asset.capture.focal_length_mm)) :
                                JsonValue{nullptr}},
        {"gps", std::move(gps)},
        {"iso", asset.capture.iso ? JsonValue::number(std::to_string(*asset.capture.iso)) :
                                    JsonValue{nullptr}},
        {"shutter_s", asset.capture.shutter_s ?
                          JsonValue::number(std::to_string(*asset.capture.shutter_s)) :
                          JsonValue{nullptr}},
    };
    return JsonValue::Object{
        {"capture", std::move(capture)},
        {"color_label", std::string(color_label_name(asset.review.color_label))},
        {"has_edits", asset.has_edits},
        {"id", asset.id},
        {"import_state", asset.import_state},
        {"media_type", asset.media_type},
        {"metadata", std::move(metadata)},
        {"rating", JsonValue::number(std::to_string(asset.review.rating))},
        {"rejected", asset.review.rejected},
        {"tags", std::move(tags)},
        {"uri", asset.normalized_uri},
    };
}

[[nodiscard]] Result<JsonValue> probe_statistics_json(const PreviewResult &preview)
{
    RasterBuffer raster;
    raster.width = preview.width;
    raster.height = preview.height;
    raster.srgb = preview.rgb;
    raster.color_profile = preview.color_profile;
    auto histogram = collect_rgb_histogram(raster);
    if (!histogram)
    {
        return histogram.error();
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(preview.width) * preview.height;
    const std::array<const std::array<std::uint32_t, kRgbHistogramBins> *, 3> channels{
        &histogram.value().red,
        &histogram.value().green,
        &histogram.value().blue,
    };
    std::array<std::uint64_t, 3> sums{};
    std::array<std::uint32_t, 3> minima{};
    std::array<std::uint32_t, 3> maxima{};
    std::array<std::uint32_t, 3> zeros{};
    std::array<std::uint32_t, 3> full{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        bool found = false;
        for (std::uint32_t bin = 0; bin < kRgbHistogramBins; ++bin)
        {
            const auto count = (*channels[channel])[bin];
            sums[channel] += static_cast<std::uint64_t>(bin) * count;
            if (count != 0)
            {
                if (!found)
                {
                    minima[channel] = bin;
                    found = true;
                }
                maxima[channel] = bin;
            }
        }
        zeros[channel] = (*channels[channel])[0];
        full[channel] = (*channels[channel])[kRgbHistogramBins - 1U];
    }

    const auto integer_array = [](const auto &values)
    {
        JsonValue::Array result;
        result.reserve(values.size());
        for (const auto value : values)
        {
            result.push_back(JsonValue::number(std::to_string(value)));
        }
        return result;
    };
    JsonValue::Array means;
    means.reserve(sums.size());
    for (const auto sum : sums)
    {
        means.push_back(JsonValue::number(
            std::to_string(static_cast<double>(sum) / static_cast<double>(pixels))));
    }
    const double luma_mean =
        (0.2126 * static_cast<double>(sums[0]) + 0.7152 * static_cast<double>(sums[1]) +
         0.0722 * static_cast<double>(sums[2])) /
        static_cast<double>(pixels);
    return JsonValue{JsonValue::Object{
        {"channel_full_counts", integer_array(full)},
        {"channel_maxima", integer_array(maxima)},
        {"channel_means", std::move(means)},
        {"channel_minima", integer_array(minima)},
        {"channel_sums", integer_array(sums)},
        {"channel_zero_counts", integer_array(zeros)},
        {"display_luma_mean", JsonValue::number(std::to_string(luma_mean))},
        {"histogram_peak", JsonValue::number(std::to_string(histogram.value().max_count))},
        {"pixels", JsonValue::number(std::to_string(pixels))},
    }};
}

[[nodiscard]] JsonValue develop_fields_json()
{
    JsonValue::Array fields;
    for (const auto &field : list_develop_set_fields())
    {
        JsonValue::Object item{{"kind", std::string(develop_set_field_kind_name(field.kind))},
                               {"name", field.name}};
        if (field.minimum)
        {
            item.emplace("minimum", JsonValue::number(std::to_string(*field.minimum)));
        }
        if (field.maximum)
        {
            item.emplace("maximum", JsonValue::number(std::to_string(*field.maximum)));
        }
        fields.push_back(JsonValue{std::move(item)});
    }
    JsonValue::Array prefixes;
    for (const auto prefix : develop_set_field_prefixes())
    {
        prefixes.push_back(JsonValue{JsonValue::Object{
            {"kind", "number"},
            {"prefix", std::string(prefix)},
        }});
    }
    return JsonValue{JsonValue::Object{
        {"fields", std::move(fields)},
        {"prefixes", std::move(prefixes)},
        {"set", "--set name=value"},
        {"text", "--watermark-text"},
    }};
}

[[nodiscard]] bool ends_with_png(const std::string_view path) noexcept
{
    if (path.size() < 4U)
    {
        return false;
    }
    const auto suffix = path.substr(path.size() - 4U);
    return (suffix[0] == '.' && (suffix[1] == 'p' || suffix[1] == 'P') &&
            (suffix[2] == 'n' || suffix[2] == 'N') && (suffix[3] == 'g' || suffix[3] == 'G'));
}

[[nodiscard]] Result<JsonValue>
run_catalog_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo catalog <create|import|list|preview|probe|recipe|develop|fields|rate|"
            "export|export-batch|tag|metadata|refresh-metadata|history|snapshot|restore> "
            "--catalog <path>");
    }
    const auto subcommand = positional[1];
    auto flags = parse_catalog_flags(positional);
    if (!flags)
    {
        return flags.error();
    }
    if (subcommand == "fields")
    {
        return develop_fields_json();
    }
    if (flags.value().catalog.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog commands require --catalog or --path");
    }
    if (!flags.value().output.empty() && subcommand != "export" && subcommand != "probe")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--output is only valid for catalog export or catalog probe",
                          {{"subcommand", std::string(subcommand)}});
    }
    if (flags.value().baseline && subcommand != "probe")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--baseline is only valid for catalog probe");
    }
    auto scoped = reject_scoped_export_options(flags.value(), subcommand);
    if (!scoped)
    {
        return scoped.error();
    }

    if (subcommand == "create")
    {
        auto session = open_catalog_session(engine, flags.value().catalog, true);
        if (!session)
        {
            return session.error();
        }
        auto snapshot = session.value()->snapshot();
        if (!snapshot)
        {
            return snapshot.error();
        }
        return JsonValue{JsonValue::Object{
            {"catalog_id", snapshot.value().catalog_id},
            {"path", snapshot.value().database_path},
            {"schema_version", JsonValue::number(std::to_string(snapshot.value().schema_version))},
        }};
    }

    auto session = open_catalog_session(engine, flags.value().catalog, false);
    if (!session)
    {
        return session.error();
    }
    auto &service = *session.value();

    if (subcommand == "import")
    {
        if (flags.value().inputs.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog import requires --input");
        }
        std::vector<std::string> inputs;
        inputs.reserve(flags.value().inputs.size());
        for (const auto input : flags.value().inputs)
        {
            inputs.emplace_back(input);
        }
        auto imported = service.import_inputs(inputs, CancellationToken{});
        if (!imported)
        {
            return imported.error();
        }
        JsonValue::Array items;
        int imported_count = 0;
        for (const auto &item : imported.value())
        {
            JsonValue::Object row{
                {"input", item.input_path},
                {"status",
                 std::string(item.status == ImportItemStatus::kImported    ? "imported" :
                             item.status == ImportItemStatus::kDuplicate   ? "duplicate" :
                             item.status == ImportItemStatus::kUnsupported ? "unsupported" :
                                                                             "failed")}};
            if (item.asset)
            {
                row.emplace("asset", asset_to_json(*item.asset));
            }
            if (item.error)
            {
                row.emplace("error", error_object(*item.error));
            }
            if (item.status == ImportItemStatus::kImported)
            {
                ++imported_count;
            }
            items.emplace_back(std::move(row));
        }
        return JsonValue{JsonValue::Object{
            {"imported", JsonValue::number(std::to_string(imported_count))},
            {"items", std::move(items)},
        }};
    }
    if (subcommand == "list")
    {
        auto snapshot = service.snapshot();
        if (!snapshot)
        {
            return snapshot.error();
        }
        LibraryQuery query;
        if (!flags.value().tag.empty())
        {
            auto tag = normalize_tag_name(flags.value().tag);
            if (!tag)
            {
                return tag.error();
            }
            query.tag = tag.value();
        }
        auto listed = service.list_assets(query);
        if (!listed)
        {
            return listed.error();
        }
        JsonValue::Array assets;
        for (const auto &asset : listed.value())
        {
            assets.push_back(asset_to_json(asset));
        }
        return JsonValue{JsonValue::Object{
            {"assets", std::move(assets)},
            {"catalog_id", snapshot.value().catalog_id},
            {"schema_version", JsonValue::number(std::to_string(snapshot.value().schema_version))},
        }};
    }
    if (subcommand == "preview")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog preview requires --asset-id");
        }
        PreviewRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.max_edge = flags.value().max_edge.value_or(kDefaultPreviewMaxEdge);
        auto previewed = service.request_preview(request);
        if (!previewed)
        {
            return previewed.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", previewed.value().asset_id},
            {"cache_path", previewed.value().cache_path},
            {"height", JsonValue::number(std::to_string(previewed.value().height))},
            {"original_missing", previewed.value().original_missing},
            {"width", JsonValue::number(std::to_string(previewed.value().width))},
        }};
    }
    if (subcommand == "probe")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog probe requires --asset-id");
        }
        if (!flags.value().output.empty())
        {
            if (!ends_with_png(flags.value().output))
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "catalog probe --output must be a .png path",
                                  {{"path", std::string(flags.value().output)}});
            }
            if (std::filesystem::exists(std::filesystem::path(std::string(flags.value().output))))
            {
                return make_error(ErrorCode::kConflict, "Output path already exists",
                                  {{"path", std::string(flags.value().output)}});
            }
        }
        auto stored_before = service.load_recipe(flags.value().asset_id);
        if (!stored_before)
        {
            return stored_before.error();
        }
        auto serialized_before = serialize_recipe(stored_before.value());
        if (!serialized_before)
        {
            return serialized_before.error();
        }
        auto previews_before = service.list_previews();
        if (!previews_before)
        {
            return previews_before.error();
        }
        auto source = flags.value().baseline ?
                          service.load_baseline_recipe(flags.value().asset_id) :
                          stored_before;
        if (!source)
        {
            return source.error();
        }
        auto params = develop_from_recipe(source.value());
        if (!params)
        {
            return params.error();
        }
        auto applied = apply_develop_overrides(params.value(), flags.value());
        if (!applied)
        {
            return applied.error();
        }

        PreviewRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.max_edge = flags.value().max_edge.value_or(512U);
        request.prefer_embedded_preview = false;
        request.persist_preview_record = false;
        auto previewed = service.request_preview(request, params.value());
        if (!previewed)
        {
            return previewed.error();
        }
        if (!previewed.value().cache_path.empty() || previewed.value().rgb.empty())
        {
            return make_error(ErrorCode::kIo,
                              "Develop probe did not return a non-persistent memory preview");
        }
        auto statistics = probe_statistics_json(previewed.value());
        if (!statistics)
        {
            return statistics.error();
        }
        auto stored_after = service.load_recipe(flags.value().asset_id);
        if (!stored_after)
        {
            return stored_after.error();
        }
        auto serialized_after = serialize_recipe(stored_after.value());
        if (!serialized_after)
        {
            return serialized_after.error();
        }
        if (serialized_before.value() != serialized_after.value())
        {
            return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed the recipe");
        }
        auto previews_after = service.list_previews();
        if (!previews_after)
        {
            return previews_after.error();
        }
        if (previews_before.value() != previews_after.value())
        {
            return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed preview records");
        }

        JsonValue::Object overrides;
        for (const auto &item : applied.value())
        {
            if (const auto *number = std::get_if<double>(&item.value); number != nullptr)
                overrides.emplace(item.name, JsonValue::number(std::to_string(*number)));
            else
                overrides.emplace(item.name, JsonValue{std::get<std::string>(item.value)});
        }
        JsonValue::Object payload{
            {"asset_id", previewed.value().asset_id},
            {"baseline", flags.value().baseline},
            {"color_profile", previewed.value().color_profile.identifier},
            {"height", JsonValue::number(std::to_string(previewed.value().height))},
            {"original_missing", previewed.value().original_missing},
            {"overrides", std::move(overrides)},
            {"preview_records_unchanged", true},
            {"recipe_unchanged", true},
            {"statistics", std::move(statistics).value()},
            {"width", JsonValue::number(std::to_string(previewed.value().width))},
        };
        if (!flags.value().output.empty())
        {
            RenderedImage image;
            image.width = previewed.value().width;
            image.height = previewed.value().height;
            image.rgb = previewed.value().rgb;
            image.color_profile = previewed.value().color_profile;
            auto encoded = engine.encode_png(image);
            if (!encoded)
            {
                return encoded.error();
            }
            auto written = write_file_bytes_atomically(flags.value().output, encoded.value());
            if (!written)
            {
                return written.error();
            }
            payload.emplace("output", std::string(flags.value().output));
        }
        return JsonValue{std::move(payload)};
    }
    if (subcommand == "recipe")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog recipe requires --asset-id");
        }
        auto recipe = service.load_recipe(flags.value().asset_id);
        if (!recipe)
        {
            return recipe.error();
        }
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
        {
            return serialized.error();
        }
        auto parsed = parse_json(serialized.value());
        if (!parsed)
        {
            return parsed.error();
        }
        auto has_edits = service.asset_has_edits(flags.value().asset_id);
        if (!has_edits)
        {
            return has_edits.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", std::string(flags.value().asset_id)},
            {"has_edits", has_edits.value()},
            {"recipe", std::move(parsed).value()},
        }};
    }
    if (subcommand == "develop")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog develop requires --asset-id");
        }
        auto loaded = service.load_recipe(flags.value().asset_id);
        if (!loaded)
        {
            return loaded.error();
        }
        auto params = develop_from_recipe(loaded.value());
        if (!params)
        {
            return params.error();
        }
        if (flags.value().pick_white)
        {
            if (std::abs(params.value().straighten_degrees) > 1.0e-4 ||
                params.value().canvas_enabled)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "White-balance pick is unavailable with straighten or Canvas");
            }
            WhiteBalancePickRequest request;
            request.preview_x = flags.value().pick_white->first;
            request.preview_y = flags.value().pick_white->second;
            request.crop_x = params.value().crop_x;
            request.crop_y = params.value().crop_y;
            request.crop_width = params.value().crop_width;
            request.crop_height = params.value().crop_height;
            request.rotate_quarters = static_cast<int>(params.value().rotate_quarters);
            request.flip_horizontal = params.value().flip_horizontal != 0;
            request.flip_vertical = params.value().flip_vertical != 0;
            auto sampled = service.sample_white_balance(flags.value().asset_id, request,
                                                        CancellationToken{});
            if (!sampled)
            {
                return sampled.error();
            }
            params.value().temperature.mode = std::string(kTemperatureModeManual);
            params.value().temperature.coefficients = sampled.value();
        }
        auto applied = apply_develop_overrides(params.value(), flags.value());
        if (!applied)
        {
            return applied.error();
        }
        auto saved = service.save_develop(flags.value().asset_id, params.value());
        if (!saved)
        {
            return saved.error();
        }
        return asset_to_json(saved.value());
    }
    if (subcommand == "rate")
    {
        if (flags.value().asset_id.empty() || !flags.value().rating)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog rate requires --asset-id and --rating");
        }
        auto rated = service.set_rating(flags.value().asset_id, *flags.value().rating);
        if (!rated)
        {
            return rated.error();
        }
        return asset_to_json(rated.value());
    }
    if (subcommand == "refresh-metadata")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog refresh-metadata requires --asset-id");
        }
        auto refreshed =
            service.refresh_capture_metadata(flags.value().asset_id, CancellationToken{});
        if (!refreshed)
        {
            return refreshed.error();
        }
        return asset_to_json(refreshed.value());
    }
    if (subcommand == "export-batch")
    {
        if (flags.value().asset_ids.empty() || flags.value().output_directory.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export-batch requires --asset-id and --output-dir");
        }
        auto options = resolved_export_options(flags.value());
        if (!options)
            return options.error();
        ExportBatchRequest request;
        request.asset_ids.reserve(flags.value().asset_ids.size());
        for (const auto asset_id : flags.value().asset_ids)
            request.asset_ids.emplace_back(asset_id);
        request.output_directory = std::string(flags.value().output_directory);
        if (!flags.value().filename_template.empty())
            request.filename_template = std::string(flags.value().filename_template);
        request.options = std::move(options).value();
        auto exported = service.export_assets(request);
        if (!exported)
            return exported.error();
        JsonValue::Array items;
        items.reserve(exported.value().size());
        for (const auto &item : exported.value())
        {
            items.emplace_back(JsonValue::Object{
                {"asset_id", item.asset_id},
                {"bytes", JsonValue::number(std::to_string(item.bytes_written))},
                {"height", JsonValue::number(std::to_string(item.height))},
                {"output", item.output_path},
                {"width", JsonValue::number(std::to_string(item.width))},
            });
        }
        return JsonValue{JsonValue::Object{
            {"exported", JsonValue::number(std::to_string(exported.value().size()))},
            {"filename_template", request.filename_template},
            {"format", std::string(export_format_name(request.options.format))},
            {"items", std::move(items)},
            {"metadata_mode",
             std::string(export_metadata_mode_name(request.options.metadata_mode))},
            {"output_directory", request.output_directory},
        }};
    }
    if (subcommand == "export")
    {
        if (flags.value().asset_id.empty() || flags.value().output.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export requires --asset-id and --output");
        }
        ExportRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.output_path = std::string(flags.value().output);
        auto options = resolved_export_options(flags.value());
        if (!options)
            return options.error();
        static_cast<ExportOptions &>(request) = std::move(options).value();
        auto exported = service.export_asset(request);
        if (!exported)
        {
            return exported.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", exported.value().asset_id},
            {"bytes", JsonValue::number(std::to_string(exported.value().bytes_written))},
            {"format", std::string(export_format_name(exported.value().format))},
            {"height", JsonValue::number(std::to_string(exported.value().height))},
            {"metadata_mode", std::string(export_metadata_mode_name(request.metadata_mode))},
            {"output", exported.value().output_path},
            {"width", JsonValue::number(std::to_string(exported.value().width))},
        }};
    }
    if (subcommand == "tag")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog tag requires --asset-id");
        }
        auto asset = service.list_assets();
        if (!asset)
        {
            return asset.error();
        }
        const AssetRecord *selected = nullptr;
        for (const auto &item : asset.value())
        {
            if (item.id == flags.value().asset_id)
            {
                selected = &item;
                break;
            }
        }
        if (selected == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "Asset does not exist",
                              {{"asset_id", std::string(flags.value().asset_id)}});
        }
        std::vector<std::string> tags = selected->tags;
        if (!flags.value().add.empty())
        {
            auto parsed = parse_tag_list(flags.value().add);
            if (!parsed)
            {
                return parsed.error();
            }
            for (auto &tag : parsed.value())
            {
                if (std::find(tags.begin(), tags.end(), tag) == tags.end())
                {
                    tags.push_back(std::move(tag));
                }
            }
        }
        if (!flags.value().remove.empty())
        {
            auto parsed = parse_tag_list(flags.value().remove);
            if (!parsed)
            {
                return parsed.error();
            }
            tags.erase(std::remove_if(tags.begin(), tags.end(),
                                      [&](const std::string &tag)
                                      {
                                          return std::find(parsed.value().begin(),
                                                           parsed.value().end(),
                                                           tag) != parsed.value().end();
                                      }),
                       tags.end());
        }
        if (!flags.value().add.empty() || !flags.value().remove.empty())
        {
            auto saved = service.set_tags(flags.value().asset_id, tags);
            if (!saved)
            {
                return saved.error();
            }
            return asset_to_json(saved.value());
        }
        return asset_to_json(*selected);
    }
    if (subcommand == "metadata")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog metadata requires --asset-id");
        }
        auto loaded = service.list_assets();
        if (!loaded)
        {
            return loaded.error();
        }
        const AssetRecord *selected = nullptr;
        for (const auto &item : loaded.value())
        {
            if (item.id == flags.value().asset_id)
            {
                selected = &item;
                break;
            }
        }
        if (selected == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "Asset does not exist",
                              {{"asset_id", std::string(flags.value().asset_id)}});
        }
        WritableMetadata metadata = selected->metadata;
        bool write = false;
        const auto assign = [&](const std::string_view text, std::optional<std::string> &field)
        {
            if (!text.empty())
            {
                field = std::string(text);
                write = true;
            }
        };
        assign(flags.value().title, metadata.title);
        assign(flags.value().description, metadata.description);
        assign(flags.value().creator, metadata.creator);
        assign(flags.value().copyright, metadata.copyright);
        if (write)
        {
            auto saved = service.set_writable_metadata(flags.value().asset_id, metadata);
            if (!saved)
            {
                return saved.error();
            }
            return asset_to_json(saved.value());
        }
        return asset_to_json(*selected);
    }
    if (subcommand == "history")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog history requires --asset-id");
        }
        auto history = service.list_recipe_history(flags.value().asset_id);
        if (!history)
        {
            return history.error();
        }
        JsonValue::Array entries;
        for (const auto &entry : history.value())
        {
            entries.push_back(JsonValue::Object{
                {"id", JsonValue::number(std::to_string(entry.id))},
                {"kind", entry.kind},
                {"label", entry.label ? JsonValue{*entry.label} : JsonValue{nullptr}},
                {"seq", JsonValue::number(std::to_string(entry.seq))},
            });
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", std::string(flags.value().asset_id)},
            {"history", std::move(entries)},
        }};
    }
    if (subcommand == "snapshot")
    {
        if (flags.value().asset_id.empty() || flags.value().label.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog snapshot requires --asset-id and --label");
        }
        auto saved = service.create_recipe_snapshot(flags.value().asset_id, flags.value().label);
        if (!saved)
        {
            return saved.error();
        }
        return asset_to_json(saved.value());
    }
    if (subcommand == "restore")
    {
        if (flags.value().asset_id.empty() || !flags.value().history_id)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog restore requires --asset-id and --history-id");
        }
        auto restored =
            service.restore_recipe_history(flags.value().asset_id, *flags.value().history_id);
        if (!restored)
        {
            return restored.error();
        }
        return asset_to_json(restored.value());
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

void write_human_error(std::ostream &stream, const TaskError &error)
{
    stream << "ravo: " << error_code_name(error.code) << ": " << error.message;
    for (const auto &[key, value] : error.context)
    {
        stream << " (" << key << "=" << value << ")";
    }
    stream << '\n';
}

} // namespace

CliApplication::CliApplication(const EngineFacade &engine, std::ostream &stdout_stream,
                               std::ostream &stderr_stream)
    : engine_(engine)
    , stdout_stream_(stdout_stream)
    , stderr_stream_(stderr_stream)
{
}

int CliApplication::run(const std::span<const std::string_view> arguments) const
{
    const auto emit = [this](const Result<JsonValue> &result, const bool json)
    {
        if (result)
        {
            if (json)
            {
                stdout_stream_ << serialize_json(success_envelope(result.value())) << '\n';
            }
            else
            {
                stdout_stream_ << serialize_json(result.value()) << '\n';
            }
            return 0;
        }
        if (json)
        {
            stdout_stream_ << serialize_json(failure_envelope(result.error())) << '\n';
        }
        else
        {
            write_human_error(stderr_stream_, result.error());
        }
        return cli_exit_code(result.error().code);
    };

    auto parsed = parse_arguments(arguments);
    if (!parsed)
    {
        return emit(parsed.error(), false);
    }
    if (parsed.value().positional.empty())
    {
        return emit(make_error(ErrorCode::kInvalidArgument, "A Ravo command is required"),
                    parsed.value().json);
    }
    const auto &positional = parsed.value().positional;
    const bool json = parsed.value().json;

    if (positional.size() == 1 && positional.front() == "--version")
    {
        return emit(JsonValue{JsonValue::Object{
                        {"name", "Ravo"}, {"protocol", "ravo-cli/v1"}, {"version", RAVO_VERSION}}},
                    json);
    }
    if (positional.size() == 1 && positional.front() == "develop-fields")
    {
        return emit(develop_fields_json(), json);
    }
    if (positional.size() == 1 && positional.front() == "operations")
    {
        JsonValue::Array operations;
        for (const auto &descriptor : engine_.operations())
        {
            auto operation = operation_descriptor_to_json(descriptor);
            if (!operation)
            {
                return emit(operation.error(), json);
            }
            operations.push_back(std::move(operation).value());
        }
        return emit(JsonValue{JsonValue::Object{{"operations", std::move(operations)}}}, json);
    }
    if (positional.size() == 3 && positional[0] == "recipe" && positional[1] == "validate")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
        {
            return emit(text.error(), json);
        }
        auto recipe = parse_recipe_json(text.value());
        if (!recipe)
        {
            return emit(recipe.error(), json);
        }
        auto valid = engine_.validate(recipe.value());
        if (!valid)
        {
            return emit(valid.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"schema_version",
                         JsonValue::number(std::to_string(recipe.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 9 && positional[0] == "recipe" && positional[1] == "import-xmp" &&
        positional[3] == "--asset-id" && positional[5] == "--input" && positional[7] == "--output")
    {
        auto xmp = read_utf8_text_file(positional[2]);
        if (!xmp)
        {
            return emit(xmp.error(), json);
        }
        LegacyXmpImportRequest request{
            xmp.value(), {std::string(positional[4]), std::string(positional[6]), std::nullopt}};
        auto recipe = import_legacy_xmp(request);
        if (!recipe)
        {
            return emit(recipe.error(), json);
        }
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
        {
            return emit(serialized.error(), json);
        }
        auto written = write_utf8_text_file_atomically(positional[8], serialized.value());
        if (!written)
        {
            return emit(written.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"output", std::string(positional[8])},
                        {"schema_version",
                         JsonValue::number(std::to_string(recipe.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 3 && positional[0] == "recipe" && positional[1] == "style-validate")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto style = parse_recipe_style_json(text.value());
        if (!style)
            return emit(style.error(), json);
        auto valid = engine_.validate(style.value().recipe);
        if (!valid)
            return emit(valid.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(style.value().recipe.operations.size()))},
                        {"schema_version",
                         JsonValue::number(std::to_string(style.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 7 && positional[0] == "recipe" && positional[1] == "style-create" &&
        positional[3] == "--name" && positional[5] == "--output")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto recipe = parse_recipe_json(text.value());
        if (!recipe)
            return emit(recipe.error(), json);
        auto valid = engine_.validate(recipe.value());
        if (!valid)
            return emit(valid.error(), json);
        auto style = recipe_style_from_recipe(std::string(positional[4]), {}, recipe.value());
        if (!style)
            return emit(style.error(), json);
        auto serialized = serialize_recipe_style(style.value());
        if (!serialized)
            return emit(serialized.error(), json);
        auto written = write_utf8_text_file_atomically(positional[6], serialized.value());
        if (!written)
            return emit(written.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(style.value().recipe.operations.size()))},
                        {"output", std::string(positional[6])},
                        {"schema_version",
                         JsonValue::number(std::to_string(style.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 9 && positional[0] == "recipe" && positional[1] == "style-apply" &&
        positional[3] == "--asset-id" && positional[5] == "--input" && positional[7] == "--output")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto style = parse_recipe_style_json(text.value());
        if (!style)
            return emit(style.error(), json);
        auto recipe = apply_recipe_style(
            style.value(), {std::string(positional[4]), std::string(positional[6]), std::nullopt});
        if (!recipe)
            return emit(recipe.error(), json);
        auto valid = engine_.validate(recipe.value());
        if (!valid)
            return emit(valid.error(), json);
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
            return emit(serialized.error(), json);
        auto written = write_utf8_text_file_atomically(positional[8], serialized.value());
        if (!written)
            return emit(written.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"output", std::string(positional[8])}}},
                    json);
    }
    if (positional.front() == "catalog")
    {
        return emit(run_catalog_command(engine_, positional), json);
    }
    if (positional.front() == "inspect")
    {
        if (positional.size() != 2)
        {
            return emit(make_error(ErrorCode::kInvalidArgument, "Usage: ravo inspect <input>"),
                        json);
        }
        auto inspected = engine_.inspect(positional[1], CancellationToken{});
        if (!inspected)
        {
            return emit(inspected.error(), json);
        }
        JsonValue::Object data{{"format", inspected.value().format},
                               {"height", JsonValue::number(std::to_string(inspected.value().height))},
                               {"input_uri", inspected.value().input_uri},
                               {"is_raw", inspected.value().is_raw},
                               {"make", inspected.value().make},
                               {"model", inspected.value().model},
                               {"width", JsonValue::number(std::to_string(inspected.value().width))}};
        if (inspected.value().is_raw)
        {
            const auto coeffs = [](const std::array<double, 4> &values)
            {
                JsonValue::Array items;
                for (const double value : values)
                {
                    items.push_back(JsonValue::number(std::to_string(value)));
                }
                return items;
            };
            data.emplace("has_as_shot_white_balance", inspected.value().has_as_shot_white_balance);
            data.emplace("as_shot_white_balance", coeffs(inspected.value().as_shot_white_balance));
            data.emplace("has_camera_reference_white_balance",
                         inspected.value().has_camera_reference_white_balance);
            data.emplace("camera_reference_white_balance",
                         coeffs(inspected.value().camera_reference_white_balance));
        }
        return emit(JsonValue{std::move(data)}, json);
    }
    if (positional.front() == "render")
    {
        auto parsed_render = parse_render_arguments(positional);
        if (!parsed_render)
        {
            return emit(parsed_render.error(), json);
        }
        auto recipe_text = read_utf8_text_file(parsed_render.value().recipe);
        if (!recipe_text)
        {
            return emit(recipe_text.error(), json);
        }
        auto recipe = parse_recipe_json(recipe_text.value());
        if (!recipe)
        {
            return emit(recipe.error(), json);
        }
        RenderRequest request;
        request.recipe = std::move(recipe).value();
        request.recipe.asset.input_uri = std::string(parsed_render.value().input);
        request.asset = request.recipe.asset;
        request.output_uri = std::string(parsed_render.value().output);
        request.output_width = parsed_render.value().width;
        request.output_height = parsed_render.value().height;
        request.correlation_id = "cli-render";
        const auto rendered = engine_.render(request);
        if (!rendered)
        {
            return emit(rendered.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"correlation_id", rendered.value().correlation_id},
                        {"height", JsonValue::number(std::to_string(rendered.value().height))},
                        {"output", rendered.value().output_uri},
                        {"width", JsonValue::number(std::to_string(rendered.value().width))}}},
                    json);
    }
    return emit(make_error(ErrorCode::kInvalidArgument, "Invalid Ravo command"), json);
}

} // namespace ravo
