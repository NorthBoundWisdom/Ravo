#include "ravo/cli/application.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArrayView>
#include <QCryptographicHash>

#ifdef emit
#undef emit
#endif

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/camera_noise_profile.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/control/live_control.h"
#include "ravo/foundation/json.h"
#include "ravo/engine/noise_calibration.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/style.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/artifact_publication.h"

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
    std::string_view import_mode;
    std::string_view import_destination;
    std::string_view import_organization;
    std::string_view import_preview;
    std::string_view import_filename_template;
    std::string_view import_second_copy;
    bool import_recursive = true;
    std::string_view asset_id;
    std::vector<std::string_view> asset_ids;
    std::optional<int> rating;
    std::optional<double> exposure_ev;
    std::optional<double> saturation;
    std::optional<double> contrast;
    std::optional<std::uint32_t> max_edge;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::vector<std::pair<std::string, std::string>> develop_text_sets;
    std::optional<std::pair<double, double>> pick_white;
    std::optional<std::string_view> watermark_text;
    std::string_view from_xmp;
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
    std::string_view backup;
    std::string_view schedule_directory;
    std::string_view schedule_interval_minutes;
    std::string_view schedule_retention_count;
    std::string_view schedule_enabled;
    std::string_view folder_id;
    std::string_view replacement_directory;
    std::string_view set_id;
    std::string_view set_name;
    std::string_view set_kind;
    std::string_view query_json;
    std::optional<std::int64_t> expected_revision;
    std::optional<std::int64_t> history_id;
};

struct StudioCliArguments
{
    std::string_view session_id;
    std::string_view workspace_root;
    std::string_view asset_id;
    std::optional<std::uint64_t> expected_session_revision;
    std::optional<std::uint64_t> expected_selection_revision;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::string_view output;
    std::optional<std::uint32_t> max_edge;
    int timeout_ms = 120000;
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

[[nodiscard]] Result<std::uint64_t> parse_uint64_flag(const std::string_view text,
                                                      const std::string_view option)
{
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "Option requires an unsigned integer",
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

[[nodiscard]] Result<StudioCliArguments>
parse_studio_flags(const std::span<const std::string_view> positional)
{
    StudioCliArguments result;
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (index + 1 >= positional.size() || positional[index + 1].starts_with("--"))
        {
            return make_error(ErrorCode::kInvalidArgument, "Studio option requires a value",
                              {{"option", std::string(option)}});
        }
        const auto value = positional[++index];
        if (option == "--session-id")
        {
            if (!result.session_id.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio session ID was specified twice");
            result.session_id = value;
        }
        else if (option == "--workspace-root")
        {
            if (!result.workspace_root.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio workspace root was specified twice");
            result.workspace_root = value;
        }
        else if (option == "--asset-id")
        {
            if (!result.asset_id.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio asset ID was specified twice");
            result.asset_id = value;
        }
        else if (option == "--expect-session-revision")
        {
            if (result.expected_session_revision)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Expected session revision was specified twice");
            auto parsed = parse_uint64_flag(value, option);
            if (!parsed)
                return parsed.error();
            result.expected_session_revision = parsed.value();
        }
        else if (option == "--expect-selection-revision")
        {
            if (result.expected_selection_revision)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Expected selection revision was specified twice");
            auto parsed = parse_uint64_flag(value, option);
            if (!parsed)
                return parsed.error();
            result.expected_selection_revision = parsed.value();
        }
        else if (option == "--set")
        {
            const auto owned = std::string(value);
            const auto split = owned.find('=');
            if (split == std::string::npos || split == 0 || split + 1 == owned.size())
                return make_error(ErrorCode::kInvalidArgument, "--set requires name=value",
                                  {{"value", owned}});
            auto parsed = parse_double_flag(owned.substr(split + 1), option);
            if (!parsed)
                return parsed.error();
            if (std::find_if(
                    result.develop_sets.begin(), result.develop_sets.end(), [&](const auto &item)
                    { return item.first == owned.substr(0, split); }) != result.develop_sets.end())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio Develop field was specified twice",
                                  {{"name", owned.substr(0, split)}});
            result.develop_sets.emplace_back(owned.substr(0, split), parsed.value());
        }
        else if (option == "--output")
        {
            if (!result.output.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio preview output was specified twice");
            result.output = value;
        }
        else if (option == "--max-edge")
        {
            if (result.max_edge)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio preview size was specified twice");
            auto parsed = parse_dimension(value, option);
            if (!parsed)
                return parsed.error();
            result.max_edge = parsed.value();
        }
        else if (option == "--timeout-ms")
        {
            auto parsed = parse_int_flag(value, option);
            if (!parsed)
                return parsed.error();
            if (parsed.value() < 100 || parsed.value() > 120000)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Studio timeout must be between 100 and 120000 milliseconds",
                                  {{"value", std::string(value)}});
            result.timeout_ms = parsed.value();
        }
        else
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown Studio option",
                              {{"option", std::string(option)}});
        }
    }
    return result;
}

[[nodiscard]] Result<CatalogCliArguments>
parse_catalog_flags(const std::span<const std::string_view> positional)
{
    CatalogCliArguments result;
    const bool batch_export = positional.size() > 1U && positional[1] == "export-batch";
    const bool multi_asset =
        batch_export ||
        (positional.size() > 1U &&
         (positional[1] == "preview-rebuild" || positional[1] == "set-create" ||
          positional[1] == "set-add" || positional[1] == "set-remove"));
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
        if (option == "--no-recursive")
        {
            if (!result.import_recursive)
                return make_error(ErrorCode::kInvalidArgument,
                                  "--no-recursive can only be specified once");
            result.import_recursive = false;
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
        else if (option == "--mode")
        {
            if (!result.import_mode.empty())
                return make_error(ErrorCode::kInvalidArgument, "Import mode was specified twice");
            result.import_mode = value;
        }
        else if (option == "--destination")
        {
            if (!result.import_destination.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Import destination was specified twice");
            result.import_destination = value;
        }
        else if (option == "--organize")
        {
            if (!result.import_organization.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Import organization was specified twice");
            result.import_organization = value;
        }
        else if (option == "--preview")
        {
            if (!result.import_preview.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Import preview policy was specified twice");
            result.import_preview = value;
        }
        else if (option == "--rename-template")
        {
            if (!result.import_filename_template.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Import rename template was specified twice");
            result.import_filename_template = value;
        }
        else if (option == "--second-copy")
        {
            if (!result.import_second_copy.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Import second-copy destination was specified twice");
            result.import_second_copy = value;
        }
        else if (option == "--asset-id")
        {
            if (multi_asset)
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
        else if (option == "--set-text")
        {
            const auto owned = std::string(value);
            const auto split = owned.find('=');
            if (split == std::string::npos || split == 0U)
            {
                return make_error(ErrorCode::kInvalidArgument, "--set-text requires name=value",
                                  {{"value", owned}});
            }
            result.develop_text_sets.emplace_back(owned.substr(0U, split),
                                                  owned.substr(split + 1U));
        }
        else if (option == "--watermark-text")
        {
            if (result.watermark_text.has_value())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--watermark-text was specified more than once");
            result.watermark_text = value;
        }
        else if (option == "--from-xmp")
        {
            if (!result.from_xmp.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "--from-xmp was specified twice");
            }
            result.from_xmp = value;
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
        else if (option == "--backup")
        {
            if (!result.backup.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Backup path was specified twice");
            }
            result.backup = value;
        }
        else if (option == "--schedule-dir")
        {
            if (!result.schedule_directory.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Scheduled backup directory was specified twice");
            result.schedule_directory = value;
        }
        else if (option == "--interval-minutes")
        {
            if (!result.schedule_interval_minutes.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Backup interval was specified twice");
            result.schedule_interval_minutes = value;
        }
        else if (option == "--retention-count")
        {
            if (!result.schedule_retention_count.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Backup retention was specified twice");
            result.schedule_retention_count = value;
        }
        else if (option == "--enabled")
        {
            if (!result.schedule_enabled.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Backup enabled state was specified twice");
            result.schedule_enabled = value;
        }
        else if (option == "--folder-id")
        {
            if (!result.folder_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "Folder ID was specified twice");
            result.folder_id = value;
        }
        else if (option == "--replacement")
        {
            if (!result.replacement_directory.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Replacement folder was specified twice");
            result.replacement_directory = value;
        }
        else if (option == "--set-id")
        {
            if (!result.set_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "Library set ID was specified twice");
            result.set_id = value;
        }
        else if (option == "--name")
        {
            if (!result.set_name.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Library set name was specified twice");
            result.set_name = value;
        }
        else if (option == "--kind")
        {
            if (!result.set_kind.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Library set kind was specified twice");
            result.set_kind = value;
        }
        else if (option == "--query")
        {
            if (!result.query_json.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Library query document was specified twice");
            result.query_json = value;
        }
        else if (option == "--revision")
        {
            if (result.expected_revision)
                return make_error(ErrorCode::kInvalidArgument,
                                  "Catalog revision was specified twice");
            std::int64_t parsed = 0;
            const auto converted =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
                parsed < 0)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "--revision requires a non-negative integer",
                                  {{"value", std::string(value)}});
            }
            result.expected_revision = parsed;
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
    for (const auto &[name, value] : flags.develop_text_sets)
    {
        if (!names.emplace(name).second)
            return make_error(ErrorCode::kInvalidArgument,
                              "Develop field was specified more than once", {{"name", name}});
        DevelopParams candidate = params;
        auto assigned = apply_develop_text_field_strict(candidate, name, value);
        if (!assigned)
            return assigned.error();
        params = std::move(candidate);
        applied.push_back({name, value});
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
    auto recovery = FilesystemRecoveryStore::create_for_catalog(path);
    if (!recovery)
    {
        return recovery.error();
    }
    auto service = std::make_unique<CatalogService>(
        engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(), std::move(recovery).value());
    auto resumed = service->sync_recovery(std::nullopt);
    if (!resumed)
    {
        return resumed.error();
    }
    return service;
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

[[nodiscard]] JsonValue recovery_state_to_json(const AssetRecoveryState &state)
{
    return JsonValue::Object{
        {"asset_id", state.asset_id},
        {"generation", JsonValue::number(std::to_string(state.generation))},
        {"pending", state.pending()},
        {"synchronized_generation",
         JsonValue::number(std::to_string(state.synchronized_generation))},
    };
}

[[nodiscard]] JsonValue recovery_artifact_to_json(const RecoveryArtifact &artifact)
{
    return JsonValue::Object{
        {"asset_id", artifact.asset_id},
        {"bytes", JsonValue::number(std::to_string(artifact.bytes))},
        {"generation", JsonValue::number(std::to_string(artifact.generation))},
        {"path", artifact.path},
        {"sha256", artifact.sha256},
    };
}

[[nodiscard]] JsonValue backup_artifact_to_json(const CatalogBackupArtifact &artifact,
                                                const bool verified)
{
    return JsonValue::Object{
        {"catalog",
         JsonValue::Object{
             {"bytes", JsonValue::number(std::to_string(artifact.catalog.bytes))},
             {"catalog_id", artifact.catalog.catalog_id},
             {"path", artifact.catalog.path},
             {"revision", JsonValue::number(std::to_string(artifact.catalog.revision))},
             {"schema_version", JsonValue::number(std::to_string(artifact.catalog.schema_version))},
             {"sha256", artifact.catalog.sha256},
         }},
        {"created_unix_ms", JsonValue::number(std::to_string(artifact.created_unix_ms))},
        {"excludes", JsonValue::Array{JsonValue{"originals"}, JsonValue{"previews"}}},
        {"format_version", JsonValue::number(std::to_string(kCatalogBackupFormatVersion))},
        {"manifest", artifact.manifest_path},
        {"path", artifact.path},
        {"sidecar_bytes", JsonValue::number(std::to_string(artifact.sidecar_bytes))},
        {"sidecar_count", JsonValue::number(std::to_string(artifact.sidecar_count))},
        {"verified", verified},
    };
}

[[nodiscard]] JsonValue restore_result_to_json(const CatalogRestoreResult &result)
{
    return JsonValue::Object{
        {"backup", backup_artifact_to_json(result.source_backup, true)},
        {"catalog",
         JsonValue::Object{
             {"catalog_id", result.catalog.catalog_id},
             {"path", result.catalog.database_path},
             {"revision", JsonValue::number(std::to_string(result.catalog.revision))},
             {"schema_version", JsonValue::number(std::to_string(result.catalog.schema_version))},
         }},
        {"previews_rebuild_required", result.previews_rebuild_required},
        {"published", result.published},
        {"support_root", result.support_root},
    };
}

[[nodiscard]] JsonValue preview_rebuild_to_json(const PreviewRebuildResult &result)
{
    JsonValue::Array items;
    items.reserve(result.items.size());
    for (const auto &item : result.items)
    {
        JsonValue::Object value{
            {"asset_id", item.asset_id},
            {"browse_cache_path",
             item.browse_cache_path ? JsonValue{*item.browse_cache_path} : JsonValue{nullptr}},
            {"develop_cache_path",
             item.develop_cache_path ? JsonValue{*item.develop_cache_path} : JsonValue{nullptr}},
            {"status", item.error ? JsonValue{"failed"} : JsonValue{"rebuilt"}},
        };
        if (item.error)
            value.emplace("error", error_object(*item.error));
        else
            value.emplace("error", nullptr);
        items.emplace_back(std::move(value));
    }
    return JsonValue::Object{
        {"completed", JsonValue::number(std::to_string(result.completed))},
        {"failed", JsonValue::number(std::to_string(result.failed))},
        {"items", std::move(items)},
        {"succeeded", JsonValue::number(std::to_string(result.succeeded))},
        {"total", JsonValue::number(std::to_string(result.total))},
    };
}

[[nodiscard]] JsonValue backup_policy_to_json(const CatalogBackupPolicy &policy)
{
    return JsonValue::Object{
        {"destination_directory", policy.destination_directory},
        {"enabled", policy.enabled},
        {"interval_minutes", JsonValue::number(std::to_string(policy.interval_minutes))},
        {"last_backup_bytes", JsonValue::number(std::to_string(policy.last_backup_bytes))},
        {"last_error", policy.last_error ? JsonValue{*policy.last_error} : JsonValue{nullptr}},
        {"last_success_unix_ms", policy.last_success_unix_ms ? JsonValue::number(std::to_string(
                                                                   *policy.last_success_unix_ms)) :
                                                               JsonValue{nullptr}},
        {"next_run_unix_ms", policy.next_run_unix_ms ?
                                 JsonValue::number(std::to_string(*policy.next_run_unix_ms)) :
                                 JsonValue{nullptr}},
        {"retention_count", JsonValue::number(std::to_string(policy.retention_count))},
    };
}

[[nodiscard]] JsonValue backup_schedule_to_json(const CatalogBackupScheduleResult &result)
{
    JsonValue::Array removed;
    for (const auto &path : result.removed_backups)
        removed.emplace_back(path);
    JsonValue::Array retained;
    for (const auto &path : result.retained_unverified_paths)
        retained.emplace_back(path);
    return JsonValue::Object{
        {"backup",
         result.backup ? backup_artifact_to_json(*result.backup, true) : JsonValue{nullptr}},
        {"policy", backup_policy_to_json(result.policy)},
        {"ran", result.ran},
        {"removed_backups", std::move(removed)},
        {"retained_unverified_paths", std::move(retained)},
    };
}

[[nodiscard]] JsonValue folder_to_json(const FolderRecord &folder)
{
    return JsonValue::Object{
        {"asset_count", JsonValue::number(std::to_string(folder.asset_count))},
        {"depth", JsonValue::number(std::to_string(folder.depth))},
        {"display_name", folder.display_name},
        {"folder_id", folder.id.empty() ? JsonValue{nullptr} : JsonValue{folder.id}},
        {"missing", folder.missing},
        {"uri", folder.uri},
    };
}

[[nodiscard]] Result<JsonValue> library_set_to_json(const LibrarySetRecord &set)
{
    JsonValue::Object object{
        {"asset_count", JsonValue::number(std::to_string(set.asset_count))},
        {"created_unix_ms", JsonValue::number(std::to_string(set.created_unix_ms))},
        {"id", set.id},
        {"kind", std::string(library_set_kind_name(set.kind))},
        {"name", set.name},
        {"updated_unix_ms", JsonValue::number(std::to_string(set.updated_unix_ms))},
    };
    if (set.query)
    {
        auto serialized = serialize_library_query_document(*set.query);
        if (!serialized)
            return serialized.error();
        auto parsed = parse_json(serialized.value());
        if (!parsed)
            return parsed.error();
        object.emplace("query", std::move(parsed).value());
    }
    else
        object.emplace("query", JsonValue{nullptr});
    return JsonValue{std::move(object)};
}

[[nodiscard]] Result<JsonValue> library_set_mutation_to_json(const LibrarySetMutation &mutation)
{
    auto set = library_set_to_json(mutation.set);
    if (!set)
        return set.error();
    return JsonValue{JsonValue::Object{
        {"revision", JsonValue::number(std::to_string(mutation.revision))},
        {"set", std::move(set).value()},
    }};
}

[[nodiscard]] JsonValue folder_relink_to_json(const FolderRelinkResult &result)
{
    return JsonValue::Object{
        {"asset_count", JsonValue::number(std::to_string(result.asset_count))},
        {"folder_id", result.folder_id},
        {"previous_uri", result.previous_uri},
        {"recovery_pending", JsonValue::number(std::to_string(result.recovery_pending))},
        {"replacement_uri", result.replacement_uri},
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

[[nodiscard]] JsonValue crs_omissions_json(const std::vector<CrsOmission> &omitted)
{
    JsonValue::Array items;
    for (const auto &item : omitted)
    {
        items.push_back(JsonValue{JsonValue::Object{
            {"key", item.key},
            {"reason", item.reason},
            {"value", item.value},
        }});
    }
    return JsonValue{std::move(items)};
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
        {"set_text", "--set-text name=value"},
        {"watermark_text", "--watermark-text"},
    }};
}

[[nodiscard]] Result<PerspectiveAnalysisMode>
perspective_analysis_mode(const std::string_view value)
{
    if (value == "vertical")
        return PerspectiveAnalysisMode::kVertical;
    if (value == "horizontal")
        return PerspectiveAnalysisMode::kHorizontal;
    if (value == "full")
        return PerspectiveAnalysisMode::kFull;
    return make_error(ErrorCode::kInvalidArgument, "Perspective analysis mode is unsupported",
                      {{"mode", std::string(value)}});
}

[[nodiscard]] Result<JsonValue>
run_perspective_analysis(const EngineFacade &engine,
                         const std::span<const std::string_view> positional)
{
    if (positional.size() != 3U && positional.size() != 5U)
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo perspective analyze <input> [--mode vertical|horizontal|full]");
    if (positional[0] != "perspective" || positional[1] != "analyze")
        return make_error(ErrorCode::kInvalidArgument, "Unknown Perspective command");
    std::string_view mode_name = "full";
    if (positional.size() == 5U)
    {
        if (positional[3] != "--mode")
            return make_error(ErrorCode::kInvalidArgument, "Unknown Perspective option",
                              {{"option", std::string(positional[3])}});
        mode_name = positional[4];
    }
    auto mode = perspective_analysis_mode(mode_name);
    if (!mode)
        return mode.error();
    const CancellationToken cancellation;
    constexpr std::uint32_t kAnalysisRenderMaxEdge = 900U;
    RasterBuffer raster;
    QtRasterDecoder raster_decoder;
    auto decoded = raster_decoder.decode(positional[2], kAnalysisRenderMaxEdge, cancellation);
    if (decoded)
    {
        if (decoded.value().pixel_format != RasterPixelFormat::kRgb8)
            return make_error(ErrorCode::kUnsupported,
                              "Perspective analysis requires an RGB8 raster",
                              {{"reason", "unsupported_analysis_pixel_format"}});
        raster.width = decoded.value().width;
        raster.height = decoded.value().height;
        raster.source_width = decoded.value().source_width;
        raster.source_height = decoded.value().source_height;
        raster.color_profile = decoded.value().color_profile;
        raster.srgb = std::move(decoded).value().rgb;
    }
    else
    {
        if (decoded.error().code != ErrorCode::kUnsupported)
            return decoded.error();
        auto inspection = engine.inspect(positional[2], cancellation);
        if (!inspection)
            return inspection.error();
        if (inspection.value().width == 0U || inspection.value().height == 0U)
            return make_error(ErrorCode::kValidation,
                              "Perspective input dimensions are unavailable",
                              {{"reason", "invalid_dimensions"}});
        const double scale = std::min(
            1.0,
            static_cast<double>(kAnalysisRenderMaxEdge) /
                static_cast<double>(std::max(inspection.value().width, inspection.value().height)));
        const auto width = std::max<std::uint32_t>(
            16U, static_cast<std::uint32_t>(std::lround(inspection.value().width * scale)));
        const auto height = std::max<std::uint32_t>(
            16U, static_cast<std::uint32_t>(std::lround(inspection.value().height * scale)));
        DevelopParams develop;
        auto recipe = recipe_from_develop(
            {"perspective-analysis", std::string(positional[2]), std::nullopt}, develop);
        if (!recipe)
            return recipe.error();
        RenderRequest request;
        request.asset = recipe.value().asset;
        request.recipe = std::move(recipe).value();
        request.output_width = width;
        request.output_height = height;
        request.memory_budget_bytes = 1024ULL * 1024ULL * 1024ULL;
        request.worker_count = 1U;
        request.deterministic = true;
        request.cancellation = cancellation;
        request.correlation_id = "perspective-analysis";
        auto rendered = engine.render_to_image(request);
        if (!rendered)
            return rendered.error();
        raster.width = rendered.value().width;
        raster.height = rendered.value().height;
        raster.source_width = raster.width;
        raster.source_height = raster.height;
        raster.color_profile = rendered.value().color_profile;
        raster.srgb = std::move(rendered).value().rgb;
    }
    auto analysis = engine.analyze_perspective(raster, mode.value(), cancellation);
    if (!analysis)
        return analysis.error();

    const auto &params = analysis.value().params;
    JsonValue::Array lines;
    lines.reserve(analysis.value().lines.size());
    const double normalized_width = static_cast<double>(std::max(1U, raster.width - 1U));
    const double normalized_height = static_cast<double>(std::max(1U, raster.height - 1U));
    for (const auto &line : analysis.value().lines)
    {
        lines.emplace_back(JsonValue::Object{
            {"orientation", line.orientation == PerspectiveGuideOrientation::kVertical ?
                                "vertical" :
                                "horizontal"},
            {"weight", JsonValue::number(std::to_string(line.weight))},
            {"x1", JsonValue::number(std::to_string(line.x1 / normalized_width))},
            {"x2", JsonValue::number(std::to_string(line.x2 / normalized_width))},
            {"y1", JsonValue::number(std::to_string(line.y1 / normalized_height))},
            {"y2", JsonValue::number(std::to_string(line.y2 / normalized_height))},
        });
    }
    return JsonValue{JsonValue::Object{
        {"algorithm", "bounded_hough_robust_fit_v1"},
        {"analyzed_height", JsonValue::number(std::to_string(analysis.value().analyzed_height))},
        {"analyzed_width", JsonValue::number(std::to_string(analysis.value().analyzed_width))},
        {"horizontal_line_count",
         JsonValue::number(std::to_string(analysis.value().horizontal_line_count))},
        {"input", std::string(positional[2])},
        {"lines", std::move(lines)},
        {"mode", std::string(mode_name)},
        {"params",
         JsonValue::Object{
             {"constrain_crop", params.constrain_crop},
             {"horizontal_shift", JsonValue::number(std::to_string(params.horizontal_shift))},
             {"interpolation", params.interpolation},
             {"rotation_degrees", JsonValue::number(std::to_string(params.rotation_degrees))},
             {"shear", JsonValue::number(std::to_string(params.shear))},
             {"vertical_shift", JsonValue::number(std::to_string(params.vertical_shift))},
         }},
        {"residual_degrees", JsonValue::number(std::to_string(analysis.value().residual_degrees))},
        {"vertical_line_count",
         JsonValue::number(std::to_string(analysis.value().vertical_line_count))},
    }};
}

[[nodiscard]] JsonValue camera_noise_profile_json(const CameraNoiseProfile &profile,
                                                  const std::string_view path)
{
    return JsonValue::Object{
        {"fit_policy", std::string(kCameraNoiseFitPolicy)},
        {"gaussian_variance", JsonValue::number(std::to_string(profile.fit.gaussian_variance))},
        {"input_sample_count", JsonValue::number(std::to_string(profile.fit.input_sample_count))},
        {"iso", JsonValue::number(std::to_string(profile.identity.iso))},
        {"make", profile.identity.make},
        {"model", profile.identity.model},
        {"path", std::string(path)},
        {"payload_sha256", profile.payload_sha256},
        {"poisson_slope", JsonValue::number(std::to_string(profile.fit.poisson_slope))},
        {"retained_sample_count",
         JsonValue::number(std::to_string(profile.fit.retained_sample_count))},
        {"schema", std::string(kCameraNoiseProfileSchema)},
        {"source_samples_sha256", profile.source_samples_sha256},
        {"units", std::string(kCameraNoiseSignalUnits)},
        {"version", JsonValue::number(std::to_string(kCameraNoiseProfileSchemaVersion))},
        {"weighted_r_squared", JsonValue::number(std::to_string(profile.fit.weighted_r_squared))},
        {"weighted_rmse", JsonValue::number(std::to_string(profile.fit.weighted_rmse))},
    };
}

[[nodiscard]] Result<JsonValue>
run_noise_command(const std::span<const std::string_view> positional)
{
    if (positional.size() == 3U && positional[1] == "inspect")
    {
        auto text = read_utf8_text_file(positional[2], kCameraNoiseDocumentMaximumBytes);
        if (!text)
            return text.error();
        auto profile = parse_camera_noise_profile_json(text.value());
        if (!profile)
            return profile.error();
        return camera_noise_profile_json(profile.value(), positional[2]);
    }
    if (positional.size() != 5U || positional[1] != "calibrate" || positional[3] != "--output")
        return make_error(ErrorCode::kInvalidArgument,
                          "Usage: ravo noise <calibrate <samples.json> --output <profile.json>|"
                          "inspect <profile.json>>");

    auto text = read_utf8_text_file(positional[2], kCameraNoiseDocumentMaximumBytes);
    if (!text)
        return text.error();
    auto document = parse_camera_noise_calibration_json(text.value());
    if (!document)
        return document.error();
    auto fit = fit_camera_noise(document.value().samples, CancellationToken{});
    if (!fit)
        return fit.error();
    auto source_sha = camera_noise_calibration_sha256(document.value());
    if (!source_sha)
        return source_sha.error();
    auto serialized = serialize_camera_noise_profile_json(document.value().identity, fit.value(),
                                                          source_sha.value());
    if (!serialized)
        return serialized.error();
    auto published = publish_text_artifact_no_replace(positional[4], serialized.value());
    if (!published)
        return published.error();
    auto profile = parse_camera_noise_profile_json(serialized.value());
    if (!profile)
        return profile.error();
    return camera_noise_profile_json(profile.value(), positional[4]);
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

[[nodiscard]] Result<const JsonValue::Object *> studio_object(const JsonValue &value,
                                                              const std::string_view location)
{
    const auto *object = value.object_if();
    if (object == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Studio response value must be an object",
                          {{"location", std::string(location)}});
    }
    return object;
}

[[nodiscard]] Result<std::string> studio_string(const JsonValue::Object &object,
                                                const std::string_view key,
                                                const std::string_view location)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Studio response string is missing",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    return *found->second.string_if();
}

[[nodiscard]] Result<std::uint64_t> studio_uint64(const JsonValue::Object &object,
                                                  const std::string_view key,
                                                  const std::string_view location)
{
    const auto found = object.find(key);
    if (found == object.end() || found->second.number_if() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Studio response revision is missing",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    const auto &text = found->second.number_if()->text;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kValidation, "Studio response revision is invalid",
                          {{"field", std::string(key)}, {"location", std::string(location)}});
    }
    return value;
}

struct ObservedStudioState
{
    JsonValue json;
    std::string session_id;
    std::uint64_t session_revision = 0;
    std::string asset_id;
    std::uint64_t selection_revision = 0;
    std::uint64_t recipe_revision = 0;
    std::string recipe_state;
    std::string current_recipe_json;
    std::string preview_state;
    bool preview_matches_recipe = false;
    bool busy = false;
    std::string catalog_path;
    std::string error_text;
};

[[nodiscard]] Result<ObservedStudioState> observed_studio_state(JsonValue value)
{
    auto root = studio_object(value, "session");
    if (!root)
        return root.error();
    auto session_id = studio_string(*root.value(), "session_id", "session");
    auto session_revision = studio_uint64(*root.value(), "revision", "session");
    auto error_text = studio_string(*root.value(), "error", "session");
    if (!session_id)
        return session_id.error();
    if (!session_revision)
        return session_revision.error();
    if (!error_text)
        return error_text.error();
    const auto busy = root.value()->find("busy");
    if (busy == root.value()->end() || busy->second.boolean_if() == nullptr)
        return make_error(ErrorCode::kValidation, "Studio busy state is missing");

    const auto selection_value = root.value()->find("selection");
    const auto recipe_value = root.value()->find("recipe");
    const auto preview_value = root.value()->find("preview");
    const auto catalog_value = root.value()->find("catalog");
    if (selection_value == root.value()->end() || recipe_value == root.value()->end() ||
        preview_value == root.value()->end() || catalog_value == root.value()->end())
        return make_error(ErrorCode::kValidation, "Studio state sections are incomplete");
    auto selection = studio_object(selection_value->second, "selection");
    auto recipe = studio_object(recipe_value->second, "recipe");
    auto preview = studio_object(preview_value->second, "preview");
    auto catalog = studio_object(catalog_value->second, "catalog");
    if (!selection)
        return selection.error();
    if (!recipe)
        return recipe.error();
    if (!preview)
        return preview.error();
    if (!catalog)
        return catalog.error();
    auto asset_id = studio_string(*selection.value(), "primary_asset_id", "selection");
    auto selection_revision = studio_uint64(*selection.value(), "revision", "selection");
    auto recipe_revision = studio_uint64(*recipe.value(), "revision", "recipe");
    auto recipe_state = studio_string(*recipe.value(), "state", "recipe");
    auto preview_state = studio_string(*preview.value(), "state", "preview");
    auto catalog_path = studio_string(*catalog.value(), "path", "catalog");
    if (!asset_id)
        return asset_id.error();
    if (!selection_revision)
        return selection_revision.error();
    if (!recipe_revision)
        return recipe_revision.error();
    if (!recipe_state)
        return recipe_state.error();
    if (!preview_state)
        return preview_state.error();
    if (!catalog_path)
        return catalog_path.error();
    const auto current = recipe.value()->find("current");
    if (current == recipe.value()->end())
        return make_error(ErrorCode::kValidation, "Studio current recipe is missing");
    const auto matches = preview.value()->find("matches_current_recipe");
    if (matches == preview.value()->end() || matches->second.boolean_if() == nullptr)
        return make_error(ErrorCode::kValidation, "Studio preview match state is missing");
    ObservedStudioState result;
    result.json = std::move(value);
    result.session_id = std::move(session_id).value();
    result.session_revision = session_revision.value();
    result.asset_id = std::move(asset_id).value();
    result.selection_revision = selection_revision.value();
    result.recipe_revision = recipe_revision.value();
    result.recipe_state = std::move(recipe_state).value();
    result.current_recipe_json =
        current->second.is_null() ? std::string{} : serialize_json(current->second);
    result.preview_state = std::move(preview_state).value();
    result.preview_matches_recipe = *matches->second.boolean_if();
    result.busy = *busy->second.boolean_if();
    result.catalog_path = std::move(catalog_path).value();
    result.error_text = std::move(error_text).value();
    return result;
}

[[nodiscard]] Result<std::filesystem::path> canonical_workspace(const std::string_view path)
{
    std::error_code error;
    const auto canonical =
        std::filesystem::weakly_canonical(filesystem_path_from_utf8(path), error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot resolve the Studio workspace root",
                          {{"path", std::string(path)}, {"reason", error.message()}});
    return canonical;
}

[[nodiscard]] Result<bool> studio_output_exists(const std::string_view path)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(filesystem_path_from_utf8(path), error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot inspect the Studio output path",
                          {{"path", std::string(path)}, {"reason", error.message()}});
    return exists;
}

[[nodiscard]] Result<std::optional<std::filesystem::path>> default_cli_workspace()
{
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    if (error)
        return make_error(ErrorCode::kIo, "Cannot read the current working directory",
                          {{"reason", error.message()}});
    return find_ravo_workspace_root(current);
}

[[nodiscard]] Result<LiveSessionDescriptor> resolve_live_session(const StudioCliArguments &flags)
{
    if (!flags.session_id.empty())
    {
        return LocalControlClient::find_descriptor(flags.session_id);
    }

    auto discovered = LocalControlClient::discover();
    if (!discovered)
        return discovered.error();

    std::optional<std::filesystem::path> workspace;
    if (!flags.workspace_root.empty())
    {
        auto canonical = canonical_workspace(flags.workspace_root);
        if (!canonical)
            return canonical.error();
        workspace = std::move(canonical).value();
    }
    else
    {
        auto detected = default_cli_workspace();
        if (!detected)
            return detected.error();
        workspace = std::move(detected).value();
    }

    std::vector<LiveSessionDescriptor> candidates;
    for (const auto &session : discovered.value())
    {
        if (!workspace)
        {
            candidates.push_back(session);
            continue;
        }
        if (session.workspace_root.empty())
            continue;
        auto session_workspace = canonical_workspace(session.workspace_root);
        if (session_workspace && session_workspace.value() == *workspace)
            candidates.push_back(session);
    }
    if (candidates.empty())
    {
        std::map<std::string, std::string, std::less<>> context;
        if (workspace)
            context.emplace("workspace_root", filesystem_path_to_utf8(*workspace));
        return make_error(ErrorCode::kNotFound, "No matching Studio live session is running",
                          std::move(context));
    }
    if (candidates.size() != 1U)
    {
        std::string ids;
        for (const auto &candidate : candidates)
        {
            if (!ids.empty())
                ids.push_back(',');
            ids.append(candidate.session_id);
        }
        return make_error(ErrorCode::kConflict, "More than one matching Studio session is running",
                          {{"count", std::to_string(candidates.size())}, {"session_ids", ids}});
    }
    return candidates.front();
}

[[nodiscard]] Result<ObservedStudioState> read_studio_state(const LiveSessionDescriptor &session,
                                                            const int timeout_ms)
{
    auto response = LocalControlClient::request(session, "state", JsonValue::Object{},
                                                std::min(timeout_ms, 5000));
    if (!response)
        return response.error();
    return observed_studio_state(std::move(response).value());
}

[[nodiscard]] Result<ObservedStudioState>
wait_for_studio_state(const LiveSessionDescriptor &session, const std::string_view asset_id,
                      const std::optional<std::string_view> expected_recipe,
                      const bool require_saved, const bool require_matching_preview,
                      const int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        auto state = read_studio_state(session, timeout_ms);
        if (!state)
            return state.error();
        if (state.value().asset_id != asset_id)
            return make_error(ErrorCode::kConflict, "Studio selection changed while waiting",
                              {{"reason", "selection_changed"},
                               {"expected", std::string(asset_id)},
                               {"actual", state.value().asset_id}});
        if (expected_recipe && state.value().current_recipe_json != *expected_recipe)
            return state.value().error_text.empty() ?
                       make_error(
                           ErrorCode::kConflict, "Studio recipe changed while waiting",
                           {{"reason", "recipe_changed"}, {"asset_id", state.value().asset_id}}) :
                       make_error(ErrorCode::kIo, "Studio Develop mutation failed",
                                  {{"reason", "develop_failed"},
                                   {"asset_id", state.value().asset_id},
                                   {"detail", state.value().error_text}});
        if (state.value().recipe_state == "error")
            return make_error(ErrorCode::kValidation, "Studio recipe state is invalid",
                              {{"reason", "recipe_error"},
                               {"asset_id", state.value().asset_id},
                               {"detail", state.value().error_text}});
        const bool recipe_ready = !state.value().current_recipe_json.empty() &&
                                  state.value().recipe_state != "loading" &&
                                  state.value().recipe_state != "error";
        const bool saved = !require_saved || state.value().recipe_state == "saved";
        const bool preview = !require_matching_preview || (state.value().preview_state == "ready" &&
                                                           state.value().preview_matches_recipe);
        if (!state.value().busy && recipe_ready && saved && preview)
            return state;
        if (std::chrono::steady_clock::now() >= deadline)
            return make_error(
                ErrorCode::kCancelled, "Timed out waiting for Studio to settle",
                {{"asset_id", std::string(asset_id)}, {"timeout_ms", std::to_string(timeout_ms)}});
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

[[nodiscard]] Result<JsonValue>
render_studio_artifact(const EngineFacade &engine, const LiveSessionDescriptor &session,
                       const ObservedStudioState &observed, const std::string_view output,
                       const std::uint32_t max_edge, const int timeout_ms)
{
    if (observed.asset_id.empty() || observed.catalog_path.empty() ||
        observed.current_recipe_json.empty())
        return make_error(ErrorCode::kConflict, "Studio has no selected editable photo",
                          {{"reason", "no_selection"}});
    if (!ends_with_png(output))
        return make_error(ErrorCode::kInvalidArgument,
                          "Studio preview --output must be a .png path",
                          {{"path", std::string(output)}});
    auto output_exists = studio_output_exists(output);
    if (!output_exists)
        return output_exists.error();
    if (output_exists.value())
        return make_error(ErrorCode::kConflict, "Output path already exists",
                          {{"path", std::string(output)}});

    auto recipe = parse_recipe_json(observed.current_recipe_json);
    if (!recipe)
        return recipe.error();
    if (recipe.value().asset.id != observed.asset_id)
        return make_error(ErrorCode::kConflict, "Studio recipe asset does not match selection",
                          {{"reason", "wrong_asset"}});
    auto params = develop_from_recipe(recipe.value());
    if (!params)
        return params.error();
    auto service = open_catalog_session(engine, observed.catalog_path, false);
    if (!service)
        return service.error();
    PreviewRequest request;
    request.asset_id = observed.asset_id;
    request.max_edge = max_edge;
    request.prefer_embedded_preview = false;
    request.persist_preview_record = false;
    request.request_revision = observed.recipe_revision;
    request.correlation_id = "cli-live-studio-preview";
    auto cancellation = CancellationSource::with_deadline(std::chrono::steady_clock::now() +
                                                          std::chrono::milliseconds(timeout_ms));
    request.cancellation = cancellation.token();
    auto preview = service.value()->request_preview(request, params.value());
    if (!preview)
        return preview.error();
    if (!preview.value().cache_path.empty() || preview.value().rgb.empty())
        return make_error(ErrorCode::kIo,
                          "Live Studio preview did not return immutable memory pixels");

    auto current = read_studio_state(session, timeout_ms);
    if (!current)
        return current.error();
    if (current.value().asset_id != observed.asset_id ||
        current.value().selection_revision != observed.selection_revision ||
        current.value().recipe_revision != observed.recipe_revision ||
        current.value().current_recipe_json != observed.current_recipe_json)
        return make_error(ErrorCode::kConflict,
                          "Studio selection or recipe changed before preview publication",
                          {{"reason", "stale_preview"}, {"asset_id", observed.asset_id}});

    auto statistics = probe_statistics_json(preview.value());
    if (!statistics)
        return statistics.error();
    RenderedImage image;
    image.width = preview.value().width;
    image.height = preview.value().height;
    image.rgb = preview.value().rgb;
    image.color_profile = preview.value().color_profile;
    auto encoded = engine.encode_png(image);
    if (!encoded)
        return encoded.error();
    auto written = write_file_bytes_atomically(output, encoded.value());
    if (!written)
        return written.error();
    const QByteArrayView bytes(reinterpret_cast<const char *>(encoded.value().data()),
                               static_cast<qsizetype>(encoded.value().size()));
    const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    return JsonValue{JsonValue::Object{
        {"artifact",
         JsonValue::Object{
             {"bytes", JsonValue::number(std::to_string(encoded.value().size()))},
             {"color_profile", preview.value().color_profile.identifier},
             {"content_hash", digest.toStdString()},
             {"hash_algorithm", "sha256"},
             {"height", JsonValue::number(std::to_string(preview.value().height))},
             {"lifecycle", "caller_owned"},
             {"mime_type", "image/png"},
             {"path", std::string(output)},
             {"width", JsonValue::number(std::to_string(preview.value().width))},
         }},
        {"asset_id", observed.asset_id},
        {"recipe_revision", JsonValue::number(std::to_string(observed.recipe_revision))},
        {"selection_revision", JsonValue::number(std::to_string(observed.selection_revision))},
        {"session_id", observed.session_id},
        {"statistics", std::move(statistics).value()},
    }};
}

[[nodiscard]] Result<JsonValue>
run_studio_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
        return make_error(ErrorCode::kInvalidArgument,
                          "Usage: ravo studio <sessions|state|develop|preview> [options]");
    const auto subcommand = positional[1];
    auto flags = parse_studio_flags(positional);
    if (!flags)
        return flags.error();

    if (subcommand == "sessions")
    {
        if (!flags.value().session_id.empty() || !flags.value().asset_id.empty() ||
            flags.value().expected_session_revision || flags.value().expected_selection_revision ||
            !flags.value().develop_sets.empty() || !flags.value().output.empty() ||
            flags.value().max_edge)
            return make_error(ErrorCode::kInvalidArgument,
                              "studio sessions accepts only --workspace-root and --timeout-ms");
        auto sessions = LocalControlClient::discover(std::min(flags.value().timeout_ms, 5000));
        if (!sessions)
            return sessions.error();
        std::optional<std::filesystem::path> workspace;
        if (!flags.value().workspace_root.empty())
        {
            auto canonical = canonical_workspace(flags.value().workspace_root);
            if (!canonical)
                return canonical.error();
            workspace = std::move(canonical).value();
        }
        else
        {
            auto detected = default_cli_workspace();
            if (!detected)
                return detected.error();
            workspace = std::move(detected).value();
        }
        JsonValue::Array values;
        for (const auto &session : sessions.value())
        {
            auto json = live_session_descriptor_to_json(session);
            if (!json)
                return json.error();
            auto object = *json.value().object_if();
            bool matches = !workspace;
            if (workspace && !session.workspace_root.empty())
            {
                auto session_workspace = canonical_workspace(session.workspace_root);
                matches = session_workspace && session_workspace.value() == *workspace;
            }
            object.emplace("matches_workspace", matches);
            values.emplace_back(std::move(object));
        }
        return JsonValue{JsonValue::Object{
            {"sessions", std::move(values)},
            {"workspace_root", workspace ? filesystem_path_to_utf8(*workspace) : std::string{}},
        }};
    }

    auto session = resolve_live_session(flags.value());
    if (!session)
        return session.error();
    if (subcommand == "state")
    {
        if (!flags.value().asset_id.empty() || flags.value().expected_session_revision ||
            flags.value().expected_selection_revision || !flags.value().develop_sets.empty() ||
            !flags.value().output.empty() || flags.value().max_edge)
            return make_error(ErrorCode::kInvalidArgument,
                              "studio state accepts only session/workspace and timeout options");
        auto state = read_studio_state(session.value(), flags.value().timeout_ms);
        return state ? Result<JsonValue>{std::move(state).value().json} :
                       Result<JsonValue>{state.error()};
    }
    if (subcommand == "develop")
    {
        if (flags.value().develop_sets.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "studio develop requires at least one --set name=value");
        if (!flags.value().output.empty() && !ends_with_png(flags.value().output))
            return make_error(ErrorCode::kInvalidArgument,
                              "studio develop --output must be a .png path",
                              {{"path", std::string(flags.value().output)}});
        if (!flags.value().output.empty())
        {
            auto output_exists = studio_output_exists(flags.value().output);
            if (!output_exists)
                return output_exists.error();
            if (output_exists.value())
                return make_error(ErrorCode::kConflict, "Output path already exists",
                                  {{"path", std::string(flags.value().output)}});
        }
        auto initial = read_studio_state(session.value(), flags.value().timeout_ms);
        if (!initial)
            return initial.error();
        const std::string asset_id = flags.value().asset_id.empty() ?
                                         initial.value().asset_id :
                                         std::string(flags.value().asset_id);
        if (asset_id.empty())
            return make_error(ErrorCode::kConflict, "Studio has no selected photo",
                              {{"reason", "no_selection"}});
        if (initial.value().busy || initial.value().current_recipe_json.empty())
        {
            auto idle = wait_for_studio_state(session.value(), asset_id, std::nullopt, false, false,
                                              flags.value().timeout_ms);
            if (!idle)
                return idle.error();
            initial = std::move(idle);
        }
        const auto observed_selection_revision = initial.value().selection_revision;
        const std::string observed_recipe = initial.value().current_recipe_json;
        const bool strict_expectation = flags.value().expected_session_revision.has_value() ||
                                        flags.value().expected_selection_revision.has_value();
        std::optional<ObservedStudioState> mutated;
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            JsonValue::Array fields;
            for (const auto &[name, value] : flags.value().develop_sets)
                fields.push_back(JsonValue::Object{
                    {"name", name}, {"value", JsonValue::number(std::to_string(value))}});
            const auto expected_session =
                flags.value().expected_session_revision.value_or(initial.value().session_revision);
            const auto expected_selection = flags.value().expected_selection_revision.value_or(
                initial.value().selection_revision);
            auto response = LocalControlClient::request(
                session.value(), "develop",
                JsonValue::Object{
                    {"asset_id", asset_id},
                    {"expected_selection_revision",
                     JsonValue::number(std::to_string(expected_selection))},
                    {"expected_session_revision",
                     JsonValue::number(std::to_string(expected_session))},
                    {"fields", std::move(fields)},
                },
                std::min(flags.value().timeout_ms, 5000));
            if (response)
            {
                auto parsed = observed_studio_state(std::move(response).value());
                if (!parsed)
                    return parsed.error();
                mutated = std::move(parsed).value();
                break;
            }
            const auto reason = response.error().context.find("reason");
            const bool stale_session =
                reason != response.error().context.end() && reason->second == "stale_session";
            if (strict_expectation || !stale_session || attempt == 3)
                return response.error();
            auto refreshed = read_studio_state(session.value(), flags.value().timeout_ms);
            if (!refreshed)
                return refreshed.error();
            if (refreshed.value().asset_id != asset_id ||
                refreshed.value().selection_revision != observed_selection_revision ||
                refreshed.value().current_recipe_json != observed_recipe)
                return response.error();
            if (refreshed.value().busy)
            {
                auto idle = wait_for_studio_state(session.value(), asset_id, observed_recipe, false,
                                                  false, flags.value().timeout_ms);
                if (!idle)
                    return idle.error();
                refreshed = std::move(idle);
            }
            initial = std::move(refreshed);
        }
        if (!mutated)
            return make_error(ErrorCode::kInternal, "Studio Develop retry ended without a result");
        const std::string target_recipe = mutated->current_recipe_json;
        auto settled = wait_for_studio_state(session.value(), asset_id, target_recipe, true, true,
                                             flags.value().timeout_ms);
        if (!settled)
            return settled.error();
        if (!flags.value().output.empty())
        {
            auto artifact = render_studio_artifact(
                engine, session.value(), settled.value(), flags.value().output,
                flags.value().max_edge.value_or(kDefaultPreviewMaxEdge), flags.value().timeout_ms);
            if (!artifact)
            {
                auto error = artifact.error();
                error.context.insert_or_assign("mutation_applied", "true");
                error.context.insert_or_assign("asset_id", settled.value().asset_id);
                error.context.insert_or_assign("recipe_revision",
                                               std::to_string(settled.value().recipe_revision));
                return error;
            }
            return artifact;
        }
        return settled.value().json;
    }
    if (subcommand == "preview")
    {
        if (!flags.value().develop_sets.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "studio preview does not accept --set; use studio develop");
        if (flags.value().output.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "studio preview requires --output <file.png>");
        auto initial = read_studio_state(session.value(), flags.value().timeout_ms);
        if (!initial)
            return initial.error();
        const std::string asset_id = flags.value().asset_id.empty() ?
                                         initial.value().asset_id :
                                         std::string(flags.value().asset_id);
        if (asset_id.empty() || initial.value().asset_id != asset_id)
            return make_error(ErrorCode::kConflict, "Studio selected asset does not match",
                              {{"reason", "wrong_asset"},
                               {"expected", asset_id},
                               {"actual", initial.value().asset_id}});
        if (flags.value().expected_session_revision &&
            *flags.value().expected_session_revision != initial.value().session_revision)
            return make_error(ErrorCode::kConflict, "Studio session revision is stale",
                              {{"reason", "stale_session"}});
        if (flags.value().expected_selection_revision &&
            *flags.value().expected_selection_revision != initial.value().selection_revision)
            return make_error(ErrorCode::kConflict, "Studio selection revision is stale",
                              {{"reason", "stale_selection"}});
        auto ready = wait_for_studio_state(session.value(), asset_id, std::nullopt, false, false,
                                           flags.value().timeout_ms);
        if (!ready)
            return ready.error();
        return render_studio_artifact(engine, session.value(), ready.value(), flags.value().output,
                                      flags.value().max_edge.value_or(kDefaultPreviewMaxEdge),
                                      flags.value().timeout_ms);
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown Studio subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

[[nodiscard]] Result<JsonValue>
run_catalog_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo catalog <create|import|list|preview|probe|recipe|develop|fields|rate|"
            "export|export-batch|tag|metadata|refresh-metadata|history|snapshot|restore|"
            "sidecar-status|sidecar-sync|backup|backup-verify|backup-restore|backup-policy|"
            "backup-run|preview-rebuild|folders|folder-relink|sets|set-create|set-rename|"
            "set-delete|set-add|set-remove> "
            "--catalog <path>; backup-verify/backup-restore use --backup <directory>");
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
    if (!flags.value().output.empty() && subcommand != "export" && subcommand != "probe" &&
        subcommand != "backup-restore")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--output is only valid for catalog export, probe, or backup-restore",
                          {{"subcommand", std::string(subcommand)}});
    }
    if (flags.value().baseline && subcommand != "probe")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--baseline is only valid for catalog probe");
    }
    if (!flags.value().backup.empty() && subcommand != "backup" && subcommand != "backup-verify" &&
        subcommand != "backup-restore")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--backup is only valid for catalog backup, backup-verify, or "
                          "backup-restore");
    }
    const bool has_schedule_options = !flags.value().schedule_directory.empty() ||
                                      !flags.value().schedule_interval_minutes.empty() ||
                                      !flags.value().schedule_retention_count.empty() ||
                                      !flags.value().schedule_enabled.empty();
    if (has_schedule_options && subcommand != "backup-policy")
        return make_error(ErrorCode::kInvalidArgument,
                          "Backup schedule options are only valid for catalog backup-policy");
    const bool has_folder_relink_options =
        !flags.value().folder_id.empty() || !flags.value().replacement_directory.empty();
    if (has_folder_relink_options && subcommand != "folder-relink")
        return make_error(ErrorCode::kInvalidArgument,
                          "Folder relink options are only valid for catalog folder-relink");
    const bool has_set_options = !flags.value().set_id.empty() || !flags.value().set_name.empty() ||
                                 !flags.value().set_kind.empty() ||
                                 !flags.value().query_json.empty() ||
                                 flags.value().expected_revision.has_value();
    const bool set_command = subcommand == "sets" || subcommand == "set-create" ||
                             subcommand == "set-rename" || subcommand == "set-delete" ||
                             subcommand == "set-add" || subcommand == "set-remove" ||
                             subcommand == "list";
    if (has_set_options && !set_command)
        return make_error(ErrorCode::kInvalidArgument,
                          "Library set options are only valid for catalog set commands or list");
    if (!flags.value().query_json.empty() && subcommand != "set-create")
        return make_error(ErrorCode::kInvalidArgument,
                          "--query is only valid for catalog set-create");
    const bool has_import_options =
        !flags.value().import_mode.empty() || !flags.value().import_destination.empty() ||
        !flags.value().import_organization.empty() || !flags.value().import_preview.empty() ||
        !flags.value().import_filename_template.empty() ||
        !flags.value().import_second_copy.empty() || !flags.value().import_recursive;
    if (has_import_options && subcommand != "import")
        return make_error(ErrorCode::kInvalidArgument,
                          "Import options are only valid for catalog import");
    if (!flags.value().from_xmp.empty() && subcommand != "develop")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--from-xmp is only valid for catalog develop");
    }
    auto scoped = reject_scoped_export_options(flags.value(), subcommand);
    if (!scoped)
    {
        return scoped.error();
    }

    if (subcommand == "backup-verify")
    {
        if (!flags.value().catalog.empty())
        {
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog backup-verify is self-contained and does not accept --catalog or --path");
        }
        if (flags.value().backup.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog backup-verify requires --backup <directory>");
        }
        const auto sidecar_root = filesystem_path_from_utf8(flags.value().backup) /
                                  filesystem_path_from_utf8(kCatalogBackupSidecarDirectory);
        auto recovery =
            FilesystemRecoveryStore::open_existing(filesystem_path_to_utf8(sidecar_root));
        if (!recovery)
        {
            return recovery.error();
        }
        const SqliteCatalogBackupVerifier database_verifier;
        auto verified = verify_catalog_backup(database_verifier, *recovery.value(),
                                              flags.value().backup, CancellationToken{});
        if (!verified)
        {
            return verified.error();
        }
        return backup_artifact_to_json(verified.value().artifact, true);
    }

    if (subcommand == "backup-restore")
    {
        if (!flags.value().catalog.empty())
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog backup-restore is self-contained and does not accept --catalog or --path");
        if (flags.value().backup.empty() || flags.value().output.empty())
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog backup-restore requires --backup <directory> --output <absent-catalog>");
        const auto sidecar_root = filesystem_path_from_utf8(flags.value().backup) /
                                  filesystem_path_from_utf8(kCatalogBackupSidecarDirectory);
        auto recovery =
            FilesystemRecoveryStore::open_existing(filesystem_path_to_utf8(sidecar_root));
        if (!recovery)
            return recovery.error();
        const SqliteCatalogBackupVerifier verifier;
        CatalogRestoreRequest request;
        request.backup_directory = std::string(flags.value().backup);
        request.destination_catalog = std::string(flags.value().output);
        auto restored = restore_catalog_backup(verifier, verifier, *recovery.value(), request);
        if (!restored)
            return restored.error();
        return restore_result_to_json(restored.value());
    }

    if (flags.value().catalog.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog commands require --catalog or --path");
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

    if (subcommand == "sidecar-status")
    {
        JsonValue::Array states;
        std::size_t pending_count = 0U;
        if (!flags.value().asset_id.empty())
        {
            auto state = service.recovery_state(flags.value().asset_id);
            if (!state)
            {
                return state.error();
            }
            pending_count = state.value().pending() ? 1U : 0U;
            states.push_back(recovery_state_to_json(state.value()));
        }
        else
        {
            auto pending = service.pending_recovery();
            if (!pending)
            {
                return pending.error();
            }
            for (const auto &state : pending.value())
            {
                states.push_back(recovery_state_to_json(state));
            }
            pending_count = states.size();
        }
        return JsonValue{JsonValue::Object{
            {"pending", JsonValue::number(std::to_string(pending_count))},
            {"states", std::move(states)},
        }};
    }
    if (subcommand == "sidecar-sync")
    {
        const std::optional<std::string_view> asset_id =
            flags.value().asset_id.empty() ?
                std::nullopt :
                std::optional<std::string_view>{flags.value().asset_id};
        auto synchronized = service.sync_recovery(asset_id, CancellationToken{});
        if (!synchronized)
        {
            return synchronized.error();
        }
        JsonValue::Array artifacts;
        for (const auto &artifact : synchronized.value().artifacts)
        {
            artifacts.push_back(recovery_artifact_to_json(artifact));
        }
        return JsonValue{JsonValue::Object{
            {"artifacts", std::move(artifacts)},
            {"pending_after",
             JsonValue::number(std::to_string(synchronized.value().pending_after))},
            {"pending_before",
             JsonValue::number(std::to_string(synchronized.value().pending_before))},
            {"root", synchronized.value().root},
        }};
    }
    if (subcommand == "backup")
    {
        if (flags.value().backup.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog backup requires --backup <absent-directory>");
        }
        auto backup = service.create_backup(flags.value().backup, CancellationToken{});
        if (!backup)
        {
            return backup.error();
        }
        return backup_artifact_to_json(backup.value(), true);
    }
    if (subcommand == "backup-policy")
    {
        auto policy = service.backup_policy();
        if (!policy)
            return policy.error();
        if (has_schedule_options)
        {
            if (!flags.value().schedule_directory.empty())
                policy.value().destination_directory =
                    std::string(flags.value().schedule_directory);
            if (!flags.value().schedule_interval_minutes.empty())
            {
                auto interval =
                    parse_int_flag(flags.value().schedule_interval_minutes, "--interval-minutes");
                if (!interval)
                    return interval.error();
                policy.value().interval_minutes = interval.value();
            }
            if (!flags.value().schedule_retention_count.empty())
            {
                auto retention =
                    parse_int_flag(flags.value().schedule_retention_count, "--retention-count");
                if (!retention)
                    return retention.error();
                policy.value().retention_count = retention.value();
            }
            if (!flags.value().schedule_enabled.empty())
            {
                if (flags.value().schedule_enabled != "true" &&
                    flags.value().schedule_enabled != "false")
                    return make_error(ErrorCode::kInvalidArgument,
                                      "--enabled must be true or false");
                policy.value().enabled = flags.value().schedule_enabled == "true";
            }
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
            policy = service.set_backup_policy(std::move(policy).value(), now);
            if (!policy)
                return policy.error();
        }
        return backup_policy_to_json(policy.value());
    }
    if (subcommand == "backup-run")
    {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        auto scheduled = service.run_scheduled_backup(now, CancellationToken{}, true);
        if (!scheduled)
            return scheduled.error();
        return backup_schedule_to_json(scheduled.value());
    }
    if (subcommand == "preview-rebuild")
    {
        std::vector<std::string> asset_ids;
        asset_ids.reserve(flags.value().asset_ids.size());
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        auto rebuilt = service.rebuild_previews(asset_ids, CancellationToken{});
        if (!rebuilt)
            return rebuilt.error();
        return preview_rebuild_to_json(rebuilt.value());
    }
    if (subcommand == "folders")
    {
        auto folders = service.list_folders();
        if (!folders)
            return folders.error();
        JsonValue::Array items;
        items.reserve(folders.value().size());
        for (const auto &folder : folders.value())
            items.push_back(folder_to_json(folder));
        return JsonValue{JsonValue::Object{{"folders", std::move(items)}}};
    }
    if (subcommand == "folder-relink")
    {
        if (flags.value().folder_id.empty() || flags.value().replacement_directory.empty())
            return make_error(
                ErrorCode::kInvalidArgument,
                "catalog folder-relink requires --folder-id <id> --replacement <directory>");
        auto relinked = service.relink_folder(
            flags.value().folder_id, flags.value().replacement_directory, CancellationToken{});
        if (!relinked)
            return relinked.error();
        return folder_relink_to_json(relinked.value());
    }
    if (subcommand == "sets")
    {
        auto sets = service.list_library_sets();
        if (!sets)
            return sets.error();
        JsonValue::Array items;
        items.reserve(sets.value().size());
        for (const auto &set : sets.value())
        {
            auto json = library_set_to_json(set);
            if (!json)
                return json.error();
            items.push_back(std::move(json).value());
        }
        return JsonValue{JsonValue::Object{{"sets", std::move(items)}}};
    }
    if (subcommand == "set-create")
    {
        if (flags.value().set_name.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog set-create requires --name <name>");
        auto kind = parse_library_set_kind(flags.value().set_kind.empty() ?
                                               kLibrarySetKindManual :
                                               flags.value().set_kind);
        if (!kind)
            return kind.error();
        std::optional<LibraryQuery> query;
        if (!flags.value().query_json.empty())
        {
            auto parsed = parse_library_query_document(flags.value().query_json);
            if (!parsed)
                return parsed.error();
            query = std::move(parsed).value();
        }
        std::vector<std::string> asset_ids;
        if (!flags.value().asset_id.empty())
            asset_ids.emplace_back(flags.value().asset_id);
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        auto created = service.create_library_set(kind.value(), flags.value().set_name, query,
                                                  asset_ids, flags.value().expected_revision);
        if (!created)
            return created.error();
        return library_set_mutation_to_json(created.value());
    }
    if (subcommand == "set-rename")
    {
        if (flags.value().set_id.empty() || flags.value().set_name.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog set-rename requires --set-id <id> --name <name>");
        auto renamed = service.rename_library_set(flags.value().set_id, flags.value().set_name,
                                                  flags.value().expected_revision);
        if (!renamed)
            return renamed.error();
        return library_set_mutation_to_json(renamed.value());
    }
    if (subcommand == "set-delete")
    {
        if (flags.value().set_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog set-delete requires --set-id <id>");
        auto deleted =
            service.delete_library_set(flags.value().set_id, flags.value().expected_revision);
        if (!deleted)
            return deleted.error();
        return JsonValue{JsonValue::Object{
            {"revision", JsonValue::number(std::to_string(deleted.value()))},
            {"set_id", std::string(flags.value().set_id)},
        }};
    }
    if (subcommand == "set-add" || subcommand == "set-remove")
    {
        if (flags.value().set_id.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              std::string("catalog ") + std::string(subcommand) +
                                  " requires --set-id <id> --asset-id <id>");
        std::vector<std::string> asset_ids;
        if (!flags.value().asset_id.empty())
            asset_ids.emplace_back(flags.value().asset_id);
        for (const auto asset_id : flags.value().asset_ids)
            asset_ids.emplace_back(asset_id);
        if (asset_ids.empty())
            return make_error(ErrorCode::kInvalidArgument,
                              std::string("catalog ") + std::string(subcommand) +
                                  " requires --asset-id <id>");
        auto mutated =
            subcommand == "set-add" ?
                service.add_library_set_members(flags.value().set_id, asset_ids,
                                                flags.value().expected_revision) :
                service.remove_library_set_members(flags.value().set_id, asset_ids,
                                                   flags.value().expected_revision);
        if (!mutated)
            return mutated.error();
        return library_set_mutation_to_json(mutated.value());
    }
    if (subcommand == "import")
    {
        if (flags.value().inputs.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog import requires --input");
        }
        ImportRequest request;
        request.inputs.reserve(flags.value().inputs.size());
        for (const auto input : flags.value().inputs)
            request.inputs.emplace_back(input);
        const auto mode =
            flags.value().import_mode.empty() ? std::string_view{"add"} : flags.value().import_mode;
        if (mode == "add")
            request.mode = ImportTransferMode::kAdd;
        else if (mode == "copy")
            request.mode = ImportTransferMode::kCopy;
        else if (mode == "move")
            request.mode = ImportTransferMode::kMove;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown import mode",
                              {{"mode", std::string(mode)}});
        const auto organization = flags.value().import_organization.empty() ?
                                      std::string_view{"single"} :
                                      flags.value().import_organization;
        if (organization == "single")
            request.organization = ImportOrganization::kSingleFolder;
        else if (organization == "hierarchy")
            request.organization = ImportOrganization::kPreserveHierarchy;
        else if (organization == "date")
            request.organization = ImportOrganization::kCaptureDate;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown import organization",
                              {{"organization", std::string(organization)}});
        const auto preview = flags.value().import_preview.empty() ? std::string_view{"standard"} :
                                                                    flags.value().import_preview;
        if (preview == "minimal")
            request.preview = ImportPreviewPolicy::kMinimal;
        else if (preview == "standard")
            request.preview = ImportPreviewPolicy::kStandard;
        else if (preview == "one-to-one")
            request.preview = ImportPreviewPolicy::kOneToOne;
        else
            return make_error(ErrorCode::kInvalidArgument, "Unknown import preview policy",
                              {{"preview", std::string(preview)}});
        request.destination_directory = std::string(flags.value().import_destination);
        request.filename_template = std::string(flags.value().import_filename_template);
        request.second_copy_directory = std::string(flags.value().import_second_copy);
        request.source_root = request.inputs.front();
        request.recursive = flags.value().import_recursive;
        request.cancellation = CancellationToken{};
        auto imported = service.execute_import(request);
        if (!imported)
        {
            return imported.error();
        }
        JsonValue::Array items;
        for (const auto &item : imported.value().items)
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
            if (item.destination_path)
                row.emplace("destination", *item.destination_path);
            if (item.sidecar_destination_path)
                row.emplace("sidecar_destination", *item.sidecar_destination_path);
            if (item.second_copy_destination_path)
                row.emplace("second_copy_destination", *item.second_copy_destination_path);
            if (item.second_copy_sidecar_destination_path)
                row.emplace("second_copy_sidecar_destination",
                            *item.second_copy_sidecar_destination_path);
            row.emplace("copies_verified", item.copies_verified);
            if (item.source_cleanup_error)
                row.emplace("source_cleanup_error", error_object(*item.source_cleanup_error));
            items.emplace_back(std::move(row));
        }
        return JsonValue{JsonValue::Object{
            {"mode", std::string(mode)},
            {"preview", std::string(preview)},
            {"rename_template", std::string(flags.value().import_filename_template)},
            {"second_copy_destination", std::string(flags.value().import_second_copy)},
            {"imported", JsonValue::number(std::to_string(imported.value().imported))},
            {"duplicates", JsonValue::number(std::to_string(imported.value().duplicates))},
            {"unsupported", JsonValue::number(std::to_string(imported.value().unsupported))},
            {"failed", JsonValue::number(std::to_string(imported.value().failed))},
            {"source_cleanup_failed",
             JsonValue::number(std::to_string(imported.value().source_cleanup_failed))},
            {"verified_second_copies",
             JsonValue::number(std::to_string(imported.value().verified_second_copies))},
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
        if (!flags.value().set_id.empty())
            query.collection_id = std::string(flags.value().set_id);
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
        std::string crs_name;
        std::vector<CrsOmission> crs_omitted;
        if (!flags.value().from_xmp.empty())
        {
            auto xmp = read_utf8_text_file(flags.value().from_xmp);
            if (!xmp)
                return xmp.error();
            if (!is_crs_xmp_document(xmp.value()))
            {
                return make_error(ErrorCode::kUnsupported,
                                  "catalog develop --from-xmp requires Camera Raw settings XMP",
                                  {{"reason", "unsupported_xmp_dialect"}});
            }
            auto imported = import_crs_xmp({xmp.value(), loaded.value().asset});
            if (!imported)
                return imported.error();
            apply_crs_look(params.value(), imported.value().look, imported.value().mask);
            crs_name = imported.value().name;
            crs_omitted = imported.value().omitted;
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
            auto sampled =
                service.sample_white_balance(flags.value().asset_id, request, CancellationToken{});
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
        auto json_asset = asset_to_json(saved.value());
        if (flags.value().from_xmp.empty())
            return json_asset;
        JsonValue::Object object =
            json_asset.object_if() != nullptr ? *json_asset.object_if() : JsonValue::Object{};
        object.emplace("omitted", crs_omissions_json(crs_omitted));
        object.emplace("preset_name", crs_name);
        return JsonValue{std::move(object)};
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
    if (positional.front() == "perspective")
    {
        return emit(run_perspective_analysis(engine_, positional), json);
    }
    if (positional.front() == "noise")
    {
        return emit(run_noise_command(positional), json);
    }
    if (positional.front() == "lut")
    {
        if (positional.size() != 3U || positional[1] != "inspect")
            return emit(
                make_error(ErrorCode::kInvalidArgument, "Usage: ravo lut inspect <file.cube>"),
                json);
        auto inspected = engine_.inspect_lut3d(positional[2], CancellationToken{});
        if (!inspected)
            return emit(inspected.error(), json);
        const auto channels = [](const std::array<float, 3> &values)
        {
            JsonValue::Array array;
            for (const float value : values)
                array.push_back(JsonValue::number(std::to_string(value)));
            return array;
        };
        return emit(JsonValue{JsonValue::Object{
                        {"domain_max", channels(inspected.value().domain_max)},
                        {"domain_min", channels(inspected.value().domain_min)},
                        {"fingerprint", inspected.value().fingerprint},
                        {"path", inspected.value().canonical_path},
                        {"size", JsonValue::number(std::to_string(inspected.value().size))},
                        {"title", inspected.value().title},
                    }},
                    json);
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
        Recipe recipe;
        std::string dialect = "legacy";
        std::string preset_name;
        std::vector<CrsOmission> omitted;
        if (is_crs_xmp_document(xmp.value()))
        {
            auto imported = import_crs_xmp(request);
            if (!imported)
            {
                return emit(imported.error(), json);
            }
            recipe = std::move(imported.value().recipe);
            dialect = "crs";
            preset_name = imported.value().name;
            omitted = imported.value().omitted;
        }
        else
        {
            auto imported = import_legacy_xmp(request);
            if (!imported)
            {
                return emit(imported.error(), json);
            }
            recipe = std::move(imported).value();
        }
        auto serialized = serialize_recipe(recipe);
        if (!serialized)
        {
            return emit(serialized.error(), json);
        }
        auto written = write_utf8_text_file_atomically(positional[8], serialized.value());
        if (!written)
        {
            return emit(written.error(), json);
        }
        JsonValue::Object payload{
            {"asset_id", recipe.asset.id},
            {"dialect", dialect},
            {"operation_count", JsonValue::number(std::to_string(recipe.operations.size()))},
            {"output", std::string(positional[8])},
            {"schema_version", JsonValue::number(std::to_string(recipe.schema_version))}};
        if (dialect == "crs")
        {
            payload.emplace("omitted", crs_omissions_json(omitted));
            payload.emplace("preset_name", preset_name);
        }
        return emit(JsonValue{std::move(payload)}, json);
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
    if (positional.size() == 7 && positional[0] == "recipe" && positional[1] == "style-apply" &&
        positional[3] == "--target-recipe" && positional[5] == "--output")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto style = parse_recipe_style_json(text.value());
        if (!style)
            return emit(style.error(), json);
        auto style_valid = engine_.validate(style.value().recipe);
        if (!style_valid)
            return emit(style_valid.error(), json);
        auto target_text = read_utf8_text_file(positional[4]);
        if (!target_text)
            return emit(target_text.error(), json);
        auto target = parse_recipe_json(target_text.value());
        if (!target)
            return emit(target.error(), json);
        auto target_valid = engine_.validate(target.value());
        if (!target_valid)
            return emit(target_valid.error(), json);
        auto recipe = apply_recipe_style(style.value(), std::move(target).value());
        if (!recipe)
            return emit(recipe.error(), json);
        auto valid = engine_.validate(recipe.value());
        if (!valid)
            return emit(valid.error(), json);
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
            return emit(serialized.error(), json);
        auto written = write_utf8_text_file_atomically(positional[6], serialized.value());
        if (!written)
            return emit(written.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"output", std::string(positional[6])}}},
                    json);
    }
    if (positional.front() == "catalog")
    {
        return emit(run_catalog_command(engine_, positional), json);
    }
    if (positional.front() == "studio")
    {
        return emit(run_studio_command(engine_, positional), json);
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
        JsonValue::Object data{
            {"format", inspected.value().format},
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
            data.emplace("raw_sensor", inspected.value().raw_sensor);
            data.emplace("cfa_width",
                         JsonValue::number(std::to_string(inspected.value().cfa_width)));
            data.emplace("cfa_height",
                         JsonValue::number(std::to_string(inspected.value().cfa_height)));
            data.emplace("default_demosaic_mode", inspected.value().default_demosaic_mode);
            data.emplace("as_shot_white_balance", coeffs(inspected.value().as_shot_white_balance));
            data.emplace("has_camera_reference_white_balance",
                         inspected.value().has_camera_reference_white_balance);
            data.emplace("camera_reference_white_balance",
                         coeffs(inspected.value().camera_reference_white_balance));
            data.emplace("dng_opcode_list2_present", inspected.value().dng_opcode_list2_present);
            data.emplace("dng_opcode_list3_present", inspected.value().dng_opcode_list3_present);
            data.emplace("dng_gain_map_count",
                         JsonValue::number(std::to_string(inspected.value().dng_gain_map_count)));
            data.emplace("dng_has_warp_rectilinear", inspected.value().dng_has_warp_rectilinear);
            data.emplace("dng_has_fix_vignette_radial",
                         inspected.value().dng_has_fix_vignette_radial);
            const auto opcode_ids = [](const std::vector<std::uint32_t> &values)
            {
                JsonValue::Array items;
                for (const std::uint32_t value : values)
                {
                    items.push_back(JsonValue::number(std::to_string(value)));
                }
                return items;
            };
            data.emplace("dng_skipped_optional_opcode_list2",
                         opcode_ids(inspected.value().dng_skipped_optional_opcode_list2));
            data.emplace("dng_skipped_optional_opcode_list3",
                         opcode_ids(inspected.value().dng_skipped_optional_opcode_list3));
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
