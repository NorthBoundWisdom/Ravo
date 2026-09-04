#include "ravo/cli/application.h"
#include "application_internal.h"

#include <fstream>
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

namespace ravo::cli_internal
{
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

[[nodiscard]] Result<CatalogCliArguments>
parse_catalog_flags(const std::span<const std::string_view> positional)
{
    CatalogCliArguments result;
    const bool batch_export = positional.size() > 1U && positional[1] == "export-batch";
    const bool multi_asset =
        batch_export || (positional.size() > 1U &&
                         (positional[1] == "preview-rebuild" || positional[1] == "set-create" ||
                          positional[1] == "set-add" || positional[1] == "set-remove" ||
                          positional[1] == "stack" || positional[1] == "develop-apply"));
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
        if (option == "--delivery-watermark")
        {
            result.delivery_watermark = true;
            continue;
        }
        if (option == "--delivery-frame")
        {
            result.delivery_frame = true;
            continue;
        }
        if (option == "--delivery-color")
        {
            result.delivery_color = true;
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
        if (option == "--stack-expanded")
        {
            if (result.stack_expanded)
                return make_error(ErrorCode::kInvalidArgument,
                                  "--stack-expanded can only be specified once");
            result.stack_expanded = true;
            continue;
        }
        if (option == "--recursive")
        {
            if (result.keyword_recursive)
                return make_error(ErrorCode::kInvalidArgument,
                                  "--recursive can only be specified once");
            result.keyword_recursive = true;
            continue;
        }

        if (option == "--user-initiated")
        {
            if (result.user_initiated)
                return make_error(ErrorCode::kInvalidArgument,
                                  "--user-initiated can only be specified once");
            result.user_initiated = true;
            continue;
        }
        if (option == "--auto-stack")
        {
            if (result.editor_auto_stack)
                return make_error(ErrorCode::kInvalidArgument,
                                  "--auto-stack can only be specified once");
            result.editor_auto_stack = true;
            continue;
        }
        if (option == "--invoke-os-open")
        {
            if (result.editor_invoke_os_open)
                return make_error(ErrorCode::kInvalidArgument,
                                  "--invoke-os-open can only be specified once");
            result.editor_invoke_os_open = true;
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
        else if (option == "--xmp")
        {
            if (!result.xmp_path.empty())
                return make_error(ErrorCode::kInvalidArgument, "--xmp was specified twice");
            result.xmp_path = value;
        }
        else if (option == "--resolve")
        {
            if (!result.xmp_resolve.empty())
                return make_error(ErrorCode::kInvalidArgument, "--resolve was specified twice");
            result.xmp_resolve = value;
        }
        else if (option == "--editor")
        {
            if (!result.editor_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "--editor was specified twice");
            result.editor_id = value;
        }
        else if (option == "--editor-version")
        {
            if (!result.editor_version.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--editor-version was specified twice");
            result.editor_version = value;
        }
        else if (option == "--foreign-source")
        {
            if (!result.foreign_source.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--foreign-source was specified twice");
            result.foreign_source = value;
        }
        else if (option == "--source-kind")
        {
            if (!result.foreign_source_kind.empty())
                return make_error(ErrorCode::kInvalidArgument, "--source-kind was specified twice");
            result.foreign_source_kind = value;
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
        else if (option == "--max-width")
        {
            auto dimension = parse_dimension(value, option);
            if (!dimension)
                return dimension.error();
            result.max_width = dimension.value();
        }
        else if (option == "--max-height")
        {
            auto dimension = parse_dimension(value, option);
            if (!dimension)
                return dimension.error();
            result.max_height = dimension.value();
        }
        else if (option == "--sharpen-amount")
        {
            result.output_sharpen = true;
            result.sharpen_amount = value;
        }
        else if (option == "--sharpen-radius")
        {
            result.output_sharpen = true;
            result.sharpen_radius = value;
        }
        else if (option == "--sharpen-threshold")
        {
            result.output_sharpen = true;
            result.sharpen_threshold = value;
        }
        else if (option == "--delivery-watermark-text")
        {
            result.delivery_watermark = true;
            result.delivery_watermark_text = value;
        }
        else if (option == "--delivery-watermark-opacity")
        {
            result.delivery_watermark = true;
            result.delivery_watermark_opacity = value;
        }
        else if (option == "--delivery-watermark-scale")
        {
            result.delivery_watermark = true;
            result.delivery_watermark_scale = value;
        }
        else if (option == "--delivery-watermark-alignment")
        {
            result.delivery_watermark = true;
            result.delivery_watermark_alignment = value;
        }
        else if (option == "--delivery-frame-size")
        {
            result.delivery_frame = true;
            result.delivery_frame_size = value;
        }
        else if (option == "--delivery-output-profile")
        {
            result.delivery_color = true;
            result.delivery_output_profile = value;
        }
        else if (option == "--delivery-rendering-intent")
        {
            result.delivery_color = true;
            result.delivery_rendering_intent = value;
        }
        else if (option == "--export-preset")
        {
            if (!result.export_preset.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--export-preset was specified twice");
            result.export_preset = value;
        }
        else if (option == "--export-job")
        {
            if (!result.export_job.empty())
                return make_error(ErrorCode::kInvalidArgument, "--export-job was specified twice");
            result.export_job = value;
        }
        else if (option == "--job-id")
        {
            if (!result.job_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "--job-id was specified twice");
            result.job_id = value;
        }
        else if (option == "--roi")
        {
            if (result.roi.has_value())
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "--roi was specified more than once");
            }
            PreviewNormRect roi;
            const char *begin = value.data();
            const char *const end = value.data() + value.size();
            double *fields[4] = {&roi.x, &roi.y, &roi.width, &roi.height};
            for (std::size_t field = 0; field < 4U; ++field)
            {
                auto parsed = std::from_chars(begin, end, *fields[field]);
                if (parsed.ec != std::errc{} || parsed.ptr == begin)
                {
                    return make_error(
                        ErrorCode::kInvalidArgument, "--roi requires x,y,width,height in 0-1",
                        {{"value", std::string(value)}, {"reason", "invalid_preview_roi"}});
                }
                begin = parsed.ptr;
                if (field + 1U < 4U)
                {
                    if (begin == end || *begin != ',')
                    {
                        return make_error(
                            ErrorCode::kInvalidArgument, "--roi requires x,y,width,height in 0-1",
                            {{"value", std::string(value)}, {"reason", "invalid_preview_roi"}});
                    }
                    ++begin;
                }
            }
            if (begin != end)
            {
                return make_error(
                    ErrorCode::kInvalidArgument, "--roi requires x,y,width,height in 0-1",
                    {{"value", std::string(value)}, {"reason", "invalid_preview_roi"}});
            }
            result.roi = roi;
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
        else if (option == "--camera")
        {
            result.camera = value;
        }
        else if (option == "--camera-make")
        {
            result.camera_make = value;
        }
        else if (option == "--camera-model")
        {
            result.camera_model = value;
        }
        else if (option == "--lens-make")
        {
            result.lens_make = value;
        }
        else if (option == "--lens-model")
        {
            result.lens_model = value;
        }
        else if (option == "--focal-length-mm")
        {
            result.focal_length_mm = value;
        }
        else if (option == "--captured-local-date")
        {
            result.captured_local_date = value;
        }
        else if (option == "--captured-after")
        {
            result.captured_after_unix_s = value;
        }
        else if (option == "--captured-before")
        {
            result.captured_before_unix_s = value;
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
        else if (option == "--country")
        {
            result.country = value;
        }
        else if (option == "--province-state")
        {
            result.province_state = value;
        }
        else if (option == "--city")
        {
            result.city = value;
        }
        else if (option == "--sublocation")
        {
            result.sublocation = value;
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
        else if (option == "--folder-uri")
        {
            if (!result.folder_uri.empty())
                return make_error(ErrorCode::kInvalidArgument, "Folder URI was specified twice");
            result.folder_uri = value;
        }
        else if (option == "--replacement")
        {
            if (!result.replacement_directory.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Replacement folder was specified twice");
            result.replacement_directory = value;
        }
        else if (option == "--keyword-id")
        {
            result.keyword_id = value;
        }
        else if (option == "--keyword-name")
        {
            result.keyword_name = value;
        }
        else if (option == "--parent-id")
        {
            result.parent_id = value;
        }
        else if (option == "--set-id")
        {
            if (!result.set_id.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Library set ID was specified twice");
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
        else if (option == "--stack-id")
        {
            if (!result.stack_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "Stack ID was specified twice");
            result.stack_id = value;
        }
        else if (option == "--pick-id")
        {
            if (!result.pick_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "Stack pick ID was specified twice");
            result.pick_id = value;
        }
        else if (option == "--from-asset")
        {
            if (!result.from_asset.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Source asset ID was specified twice");
            result.from_asset = value;
        }
        else if (option == "--fields")
        {
            if (!result.fields.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "Develop field list was specified twice");
            result.fields = value;
        }
        else if (option == "--proposal-id")
        {
            if (!result.proposal_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "--proposal-id was specified twice");
            result.proposal_id = value;
        }
        else if (option == "--provider")
        {
            if (!result.provider_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "--provider was specified twice");
            result.provider_id = value;
        }
        else if (option == "--model")
        {
            if (!result.model_id.empty())
                return make_error(ErrorCode::kInvalidArgument, "--model was specified twice");
            result.model_id = value;
        }
        else if (option == "--proposal-kind")
        {
            if (!result.proposal_kind.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--proposal-kind was specified twice");
            result.proposal_kind = value;
        }
        else if (option == "--reference-asset")
        {
            if (!result.reference_asset.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--reference-asset was specified twice");
            result.reference_asset = value;
        }
        else if (option == "--destination-assets")
        {
            result.destination_assets.push_back(value);
        }
        else if (option == "--semantic-label")
        {
            if (!result.semantic_label.empty())
                return make_error(ErrorCode::kInvalidArgument,
                                  "--semantic-label was specified twice");
            result.semantic_label = value;
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
    if (flags.max_width)
        request.max_width = *flags.max_width;
    if (flags.max_height)
        request.max_height = *flags.max_height;
    if (flags.output_sharpen || !flags.sharpen_amount.empty() || !flags.sharpen_radius.empty() ||
        !flags.sharpen_threshold.empty())
    {
        request.output_sharpen.enabled = true;
        auto parse_double = [](const std::string_view text,
                               const std::string_view option) -> Result<double>
        {
            if (text.empty())
                return make_error(ErrorCode::kInvalidArgument, "Missing export sharpen value",
                                  {{"option", std::string(option)}});
            char *end = nullptr;
            const std::string owned(text);
            const double value = std::strtod(owned.c_str(), &end);
            if (end != owned.c_str() + owned.size() || !std::isfinite(value))
            {
                return make_error(ErrorCode::kInvalidArgument, "Invalid export sharpen value",
                                  {{"option", std::string(option)}, {"value", owned}});
            }
            return value;
        };
        if (!flags.sharpen_amount.empty())
        {
            auto amount = parse_double(flags.sharpen_amount, "--sharpen-amount");
            if (!amount)
                return amount.error();
            request.output_sharpen.amount = amount.value();
        }
        if (!flags.sharpen_radius.empty())
        {
            auto radius = parse_double(flags.sharpen_radius, "--sharpen-radius");
            if (!radius)
                return radius.error();
            request.output_sharpen.radius = radius.value();
        }
        if (!flags.sharpen_threshold.empty())
        {
            auto threshold = parse_double(flags.sharpen_threshold, "--sharpen-threshold");
            if (!threshold)
                return threshold.error();
            request.output_sharpen.threshold = threshold.value();
        }
    }
    if (flags.delivery_watermark || !flags.delivery_watermark_text.empty() ||
        !flags.delivery_watermark_opacity.empty() || !flags.delivery_watermark_scale.empty() ||
        !flags.delivery_watermark_alignment.empty())
    {
        request.watermark.enabled = true;
        if (!flags.delivery_watermark_text.empty())
            request.watermark.text = std::string(flags.delivery_watermark_text);
        auto parse_wm_double = [](const std::string_view text,
                                  const std::string_view option) -> Result<double>
        {
            if (text.empty())
                return make_error(ErrorCode::kInvalidArgument, "Missing delivery watermark value",
                                  {{"option", std::string(option)}});
            char *end = nullptr;
            const std::string owned(text);
            const double value = std::strtod(owned.c_str(), &end);
            if (end != owned.c_str() + owned.size() || !std::isfinite(value))
            {
                return make_error(ErrorCode::kInvalidArgument, "Invalid delivery watermark value",
                                  {{"option", std::string(option)}, {"value", owned}});
            }
            return value;
        };
        if (!flags.delivery_watermark_opacity.empty())
        {
            auto opacity =
                parse_wm_double(flags.delivery_watermark_opacity, "--delivery-watermark-opacity");
            if (!opacity)
                return opacity.error();
            request.watermark.opacity = opacity.value();
        }
        if (!flags.delivery_watermark_scale.empty())
        {
            auto scale =
                parse_wm_double(flags.delivery_watermark_scale, "--delivery-watermark-scale");
            if (!scale)
                return scale.error();
            request.watermark.scale_percent = scale.value();
        }
        if (!flags.delivery_watermark_alignment.empty())
        {
            auto alignment =
                validate_export_watermark_alignment(flags.delivery_watermark_alignment);
            if (!alignment)
                return alignment.error();
            request.watermark.alignment = std::string(flags.delivery_watermark_alignment);
        }
    }

    if (flags.delivery_frame || !flags.delivery_frame_size.empty())
    {
        request.frame.enabled = true;
        if (!flags.delivery_frame_size.empty())
        {
            char *end = nullptr;
            const std::string owned(flags.delivery_frame_size);
            const double value = std::strtod(owned.c_str(), &end);
            if (end != owned.c_str() + owned.size() || !std::isfinite(value))
            {
                return make_error(ErrorCode::kInvalidArgument, "Invalid delivery frame size",
                                  {{"option", "--delivery-frame-size"}, {"value", owned}});
            }
            request.frame.size = value;
        }
    }
    if (flags.delivery_color || !flags.delivery_output_profile.empty() ||
        !flags.delivery_rendering_intent.empty())
    {
        request.output_color.enabled = true;
        if (!flags.delivery_output_profile.empty())
            request.output_color.output_profile = std::string(flags.delivery_output_profile);
        if (!flags.delivery_rendering_intent.empty())
            request.output_color.rendering_intent = std::string(flags.delivery_rendering_intent);
    }
    if (!flags.export_preset.empty())
    {
        auto text_body = read_utf8_text_file(flags.export_preset, kExportPresetFileMaxBytes);
        if (!text_body)
            return text_body.error();
        auto preset = parse_export_preset_json(text_body.value());
        if (!preset)
            return preset.error();
        auto applied = apply_export_preset(preset.value());
        if (!applied)
            return applied.error();
        // Explicit CLI flags override preset snapshot fields that were set.
        ExportOptions merged = std::move(applied).value();
        if (!flags.format.empty())
            merged.format = request.format;
        if (!flags.metadata_mode.empty())
            merged.metadata_mode = request.metadata_mode;
        if (!flags.quality.empty() || !flags.jpeg_subsampling.empty())
            merged.jpeg_options = request.jpeg_options;
        if (!flags.png_bit_depth.empty() || !flags.png_compression.empty())
            merged.png_options = request.png_options;
        if (has_explicit_tiff_options(flags))
            merged.tiff_options = request.tiff_options;
        if (flags.max_edge)
            merged.max_edge = request.max_edge;
        if (flags.max_width)
            merged.max_width = request.max_width;
        if (flags.max_height)
            merged.max_height = request.max_height;
        if (flags.output_sharpen || !flags.sharpen_amount.empty() ||
            !flags.sharpen_radius.empty() || !flags.sharpen_threshold.empty())
            merged.output_sharpen = request.output_sharpen;
        if (flags.delivery_watermark || !flags.delivery_watermark_text.empty() ||
            !flags.delivery_watermark_opacity.empty() || !flags.delivery_watermark_scale.empty() ||
            !flags.delivery_watermark_alignment.empty())
            merged.watermark = request.watermark;
        if (flags.delivery_frame || !flags.delivery_frame_size.empty())
            merged.frame = request.frame;
        if (flags.delivery_color || !flags.delivery_output_profile.empty() ||
            !flags.delivery_rendering_intent.empty())
            merged.output_color = request.output_color;
        request = ExportRequest{};
        static_cast<ExportOptions &>(request) = std::move(merged);
    }
    auto valid = validate_cli_export_options(request, flags);
    if (!valid)
        return valid.error();
    auto domain_valid = validate_export_options(static_cast<const ExportOptions &>(request));
    if (!domain_valid)
        return domain_valid.error();
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
    if (flags.delivery_watermark || !flags.delivery_watermark_text.empty() ||
        !flags.delivery_watermark_opacity.empty() || !flags.delivery_watermark_scale.empty() ||
        !flags.delivery_watermark_alignment.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Delivery watermark options require catalog export",
                          {{"reason", "delivery_watermark_requires_export"},
                           {"subcommand", std::string(subcommand)}});
    }
    if (flags.delivery_frame || !flags.delivery_frame_size.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Delivery frame options require catalog export",
                          {{"reason", "delivery_frame_requires_export"},
                           {"subcommand", std::string(subcommand)}});
    }
    if (flags.delivery_color || !flags.delivery_output_profile.empty() ||
        !flags.delivery_rendering_intent.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Delivery colour options require catalog export",
                          {{"reason", "delivery_color_requires_export"},
                           {"subcommand", std::string(subcommand)}});
    }
    return {};
}

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

} // namespace ravo::cli_internal
