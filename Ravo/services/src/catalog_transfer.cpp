#include "ravo/services/exports_service.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/raster_decoder.h"
#include "ravo/domain/preview_cache.h"
#include "ravo/domain/recovery_store.h"
#include "ravo/engine/engine.h"
#include "ravo/services/preview_service.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "catalog_internal.h"
#include "export_output_sharpen.h"
#include "export_delivery_color.h"
#include "export_delivery_frame.h"
#include "export_delivery_watermark.h"
#include "catalog_service_internal.h"
#include "ravo/domain/uri.h"
#include "ravo/adapters/text_file.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

ExportService::ExportService(const std::unique_ptr<CatalogRepository> &repository,
                             const std::unique_ptr<RasterDecoder> &raster,
                             const EngineFacade *const &engine,
                             PreviewService &preview_service) noexcept
    : repository_(repository)
    , raster_(raster)
    , engine_(engine)
    , preview_service_(preview_service)
{
}

using namespace catalog_service_internal;
Result<void> ExportService::check_companion_jpegs(const std::vector<std::string> &asset_ids,
                                                  const CancellationToken &cancellation)
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (asset_ids.empty() || asset_ids.size() > kExportBatchMaxAssets)
        return make_error(ErrorCode::kInvalidArgument,
                          "Companion check requires a bounded asset set");
    for (const auto &id : asset_ids)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        auto asset = repository_->find_asset_by_id(id);
        if (!asset)
            return asset.error();
        if (!asset.value())
            return make_error(ErrorCode::kNotFound, "Asset does not exist", {{"asset_id", id}});
        auto companion = required_companion_jpeg(*asset.value());
        if (!companion)
            return companion.error();
    }
    return {};
}

Result<std::vector<ExportResult>> ExportService::export_assets(
    const ExportBatchRequest &request,
    const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress)
{
    if (engine_ == nullptr || raster_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (request.asset_ids.empty() || request.asset_ids.size() > kExportBatchMaxAssets)
    {
        return make_error(ErrorCode::kInvalidArgument, "Export batch size is invalid",
                          {{"asset_count", std::to_string(request.asset_ids.size())},
                           {"max_assets", std::to_string(kExportBatchMaxAssets)},
                           {"reason", "invalid_export_batch_size"}});
    }
    if (request.output_directory.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export batch requires an output directory");
    }
    auto valid_options = validate_export_options(request.options);
    if (!valid_options)
        return valid_options.error();

    auto normalized_root = normalize_local_input(request.output_directory);
    if (!normalized_root)
        return normalized_root.error();
    const auto root_path = utf8_path(normalized_root.value().path);
    std::error_code root_error;
    const bool root_is_directory = std::filesystem::is_directory(root_path, root_error);
    if (root_error)
    {
        return make_error(
            ErrorCode::kIo, "Unable to inspect export output directory",
            {{"path", normalized_root.value().path}, {"detail", root_error.message()}});
    }
    if (!root_is_directory)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Export output directory does not exist or is not a directory",
                          {{"path", normalized_root.value().path},
                           {"reason", "invalid_export_output_directory"}});
    }

    struct PlannedExport
    {
        std::string asset_id;
        std::string output_path;
    };
    std::vector<PlannedExport> planned;
    planned.reserve(request.asset_ids.size());
    std::set<std::string, std::less<>> unique_assets;
    std::set<std::string, std::less<>> unique_outputs;
    for (std::size_t index = 0; index < request.asset_ids.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return annotate_batch_export_error(cancelled.error(), 0, request.asset_ids.size(),
                                               index, request.asset_ids[index], {});
        }
        const auto &asset_id = request.asset_ids[index];
        if (asset_id.empty() || !unique_assets.emplace(asset_id).second)
        {
            return make_error(ErrorCode::kValidation,
                              "Export batch asset IDs must be nonempty and unique",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"reason", "duplicate_export_asset_id"}});
        }
        auto asset = repository_->find_asset_by_id(asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Asset does not exist",
                {{"asset_id", asset_id}, {"batch_index", std::to_string(index + 1U)}});
        }
        if (is_video_media_type(asset.value()->media_type) &&
            request.options.format != ExportFormat::kOriginalCopy)
            return make_error(
                ErrorCode::kUnsupported, "Video supports original-byte export only",
                {{"asset_id", asset_id}, {"reason", "video_photo_operation_unsupported"}});
        auto source = normalize_local_input(asset.value()->normalized_uri);
        if (request.options.format == ExportFormat::kCompanionJpeg)
        {
            auto companion = required_companion_jpeg(*asset.value());
            if (!companion)
                return annotate_batch_export_error(companion.error(), 0, request.asset_ids.size(),
                                                   index, asset_id, {});
        }
        if (!source)
            return source.error();
        const auto source_path = utf8_path(source.value().path);
        std::error_code source_error;
        const bool source_is_file = std::filesystem::is_regular_file(source_path, source_error);
        if (source_error)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect export source",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"path", source.value().path},
                               {"detail", source_error.message()}});
        }
        if (!source_is_file)
        {
            return make_error(ErrorCode::kNotFound, "Original file is missing",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"path", source.value().path}});
        }
        const auto stem = utf8_string(source_path.stem().u8string());
        const auto extension = request.options.format == ExportFormat::kOriginalCopy ?
                                   utf8_string(source_path.extension().u8string()) :
                                   std::string(export_format_extension(request.options.format));
        auto filename = expand_export_filename_template(request.filename_template, stem, asset_id,
                                                        index + 1U, extension);
        if (!filename)
        {
            auto error = filename.error();
            error.context.insert_or_assign("asset_id", asset_id);
            error.context.insert_or_assign("batch_index", std::to_string(index + 1U));
            return error;
        }
        const auto output_path = root_path / utf8_path(filename.value());
        const auto output = utf8_string(output_path.generic_u8string());
        if (!unique_outputs.emplace(output).second)
        {
            return make_error(ErrorCode::kConflict,
                              "Export filename template resolves multiple assets to one output",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"output", output},
                               {"reason", "duplicate_export_output"}});
        }
        std::error_code target_error;
        const auto target_status = std::filesystem::symlink_status(output_path, target_error);
        if (target_error && target_error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect export output path",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"output", output},
                               {"detail", target_error.message()}});
        }
        if (!target_error && std::filesystem::exists(target_status))
        {
            return make_error(ErrorCode::kConflict, "Export output already exists",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"completed_count", "0"},
                               {"output", output},
                               {"partial_batch", "false"},
                               {"reason", "export_batch_preflight_conflict"},
                               {"total_count", std::to_string(request.asset_ids.size())}});
        }
        planned.push_back({asset_id, output});
    }

    std::vector<ExportResult> results;
    results.reserve(planned.size());
    for (std::size_t index = 0; index < planned.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return annotate_batch_export_error(cancelled.error(), results.size(), planned.size(),
                                               index, planned[index].asset_id,
                                               planned[index].output_path);
        }
        ExportRequest item;
        static_cast<ExportOptions &>(item) = request.options;
        item.asset_id = planned[index].asset_id;
        item.output_path = planned[index].output_path;
        item.cancellation = request.cancellation;
        item.correlation_id = request.correlation_id.empty() ?
                                  planned[index].asset_id :
                                  request.correlation_id + ":" + std::to_string(index + 1U);
        auto exported = export_asset(item);
        if (!exported)
        {
            return annotate_batch_export_error(exported.error(), results.size(), planned.size(),
                                               index, planned[index].asset_id,
                                               planned[index].output_path);
        }
        results.push_back(std::move(exported).value());
        if (progress)
            progress(index + 1U, planned.size(), &results.back());
    }
    return results;
}

Result<ExportResult> ExportService::export_asset(const ExportRequest &request)
{
    if (engine_ == nullptr || raster_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export requires an asset ID");
    }
    if (request.output_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export requires an output path");
    }
    {
        auto options = validate_export_options(static_cast<const ExportOptions &>(request));
        if (!options)
        {
            return options.error();
        }
    }
    auto output = normalize_local_input(request.output_path);
    if (!output)
    {
        return output.error();
    }
    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", request.asset_id}});
    }
    ExportMetadataSnapshot export_metadata;
    if (is_video_media_type(asset.value()->media_type) &&
        request.format != ExportFormat::kOriginalCopy)
        return make_error(
            ErrorCode::kUnsupported, "Video supports original-byte export only",
            {{"asset_id", request.asset_id}, {"reason", "video_photo_operation_unsupported"}});
    if (request.format != ExportFormat::kOriginalCopy &&
        request.format != ExportFormat::kCompanionJpeg)
    {
        if (request.metadata_mode == ExportMetadataMode::kNone)
        {
            export_metadata.embed_metadata = false;
        }
        else
        {
            if (request.format == ExportFormat::kTiff)
            {
                export_metadata.destination_document_name = output.value().path;
            }
            export_metadata.writable = asset.value()->metadata;
            export_metadata.capture = asset.value()->capture;
            if (request.metadata_mode == ExportMetadataMode::kNoLocation)
            {
                export_metadata.capture.location.reset();
                export_metadata.writable.country.reset();
                export_metadata.writable.province_state.reset();
                export_metadata.writable.city.reset();
                export_metadata.writable.sublocation.reset();
            }
            auto tags = canonicalize_export_tags(asset.value()->tags, request.cancellation);
            if (!tags)
            {
                return tags.error();
            }
            export_metadata.tags = std::move(tags).value();
        }
        auto valid_metadata =
            request.format == ExportFormat::kTiff ?
                validate_tiff_export_metadata(export_metadata, request.cancellation) :
                validate_export_metadata(export_metadata, request.cancellation);
        if (!valid_metadata)
        {
            return valid_metadata.error();
        }
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }

    // COR-01: refuse export when the on-disk original does not match catalog identity.
    // Missing originals are handled below for rendered formats; original-copy also
    // requires a present matching file.
    {
        std::error_code exists_error;
        const bool original_exists =
            std::filesystem::is_regular_file(utf8_path(location.value().path), exists_error) &&
            !exists_error;
        if (original_exists)
        {
            auto identity = read_file_identity(location.value().path);
            if (!identity)
                return identity.error();
            const bool size_mtime_match =
                identity.value().size_bytes == asset.value()->size_bytes &&
                identity.value().mtime_unix_ms == asset.value()->mtime_unix_ms;
            const bool fingerprint_match =
                !asset.value()->content_fingerprint ||
                make_content_fingerprint(identity.value()) == *asset.value()->content_fingerprint;
            if (!size_mtime_match || !fingerprint_match)
            {
                return make_error(ErrorCode::kConflict,
                                  "Original no longer matches catalog identity; export refused",
                                  {{"asset_id", request.asset_id},
                                   {"path", location.value().path},
                                   {"reason", "source_identity_mismatch"}});
            }
        }
    }

    ExportResult result;
    result.asset_id = request.asset_id;
    result.output_path = output.value().path;
    result.format = request.format;
    if (request.format == ExportFormat::kCompanionJpeg)
    {
        auto companion = required_companion_jpeg(*asset.value());
        if (!companion)
            return companion.error();
        auto metadata = raster_->probe(companion.value());
        if (!metadata)
            return metadata.error();
        auto copied =
            copy_file_atomically(companion.value(), output.value().path, request.cancellation);
        if (!copied)
            return copied.error();
        result.bytes_written = copied.value();
        result.width = metadata.value().width;
        result.height = metadata.value().height;
        return result;
    }
    if (request.format == ExportFormat::kOriginalCopy)
    {
        auto copied =
            copy_file_atomically(location.value().path, output.value().path, request.cancellation);
        if (!copied)
        {
            return copied.error();
        }
        result.width = asset.value()->width.value_or(0);
        result.height = asset.value()->height.value_or(0);
        result.bytes_written = copied.value();
        LOG_INFO(ravo::logger(), "export original asset={} output={} bytes={}", request.asset_id,
                 output.value().path, result.bytes_written);
        return result;
    }

    std::error_code exists_error;
    const bool original_exists =
        std::filesystem::is_regular_file(utf8_path(location.value().path), exists_error) &&
        !exists_error;
    if (!original_exists)
    {
        bool proxy_present = false;
        auto snapshot = repository_->snapshot();
        if (snapshot)
        {
            const auto proxy_root = snapshot.value().database_path + ".ravo/offline-edit-proxies/" +
                                    request.asset_id + "/proxy.tif";
            std::error_code proxy_error;
            proxy_present = std::filesystem::is_regular_file(utf8_path(proxy_root), proxy_error) &&
                            !proxy_error;
        }
        return make_error(
            ErrorCode::kNotFound, "Original file is missing; export refuses offline-edit proxy",
            {{"asset_id", request.asset_id},
             {"path", location.value().path},
             {"reason", proxy_present ? "proxy_export_forbidden" : "original_missing"}});
    }

    auto baseline_recipe = baseline_recipe_for(*asset.value(), location.value().path);
    if (!baseline_recipe)
    {
        return baseline_recipe.error();
    }
    Recipe edit_recipe = std::move(baseline_recipe).value();
    auto stored = repository_->load_recipe_json(request.asset_id);
    if (!stored)
    {
        return stored.error();
    }
    if (stored.value())
    {
        auto parsed = parse_recipe_json(*stored.value());
        if (!parsed)
        {
            return parsed.error();
        }
        parsed.value().asset = edit_recipe.asset;
        auto merged = merge_missing_raw_baseline_operations(edit_recipe, parsed.value());
        if (!merged)
        {
            return merged.error();
        }
        auto valid = engine_->validate(parsed.value());
        if (!valid)
        {
            return valid.error();
        }
        edit_recipe = std::move(parsed).value();
    }
    if (export_options_request_output_color(request))
    {
        auto overridden = apply_export_color_override(std::move(edit_recipe), request.output_color);
        if (!overridden)
            return overridden.error();
        edit_recipe = std::move(overridden).value();
    }
    const RenderSampleKind sample_kind = [&request]()
    {
        if (request.format == ExportFormat::kPng &&
            request.png_options.bit_depth == PngBitDepth::k16)
        {
            return RenderSampleKind::kRgb16;
        }
        if (request.format == ExportFormat::kTiff)
        {
            switch (request.tiff_options.sample_type)
            {
            case TiffSampleType::kUint16:
                return RenderSampleKind::kRgb16;
            case TiffSampleType::kFloat16:
            case TiffSampleType::kFloat32:
                return RenderSampleKind::kRgbFloat;
            case TiffSampleType::kUint8:
                break;
            }
        }
        return RenderSampleKind::kRgb8;
    }();
    auto rendered = preview_service_.render_for_export(
        *asset.value(), location.value().path, edit_recipe,
        static_cast<const ExportOptions &>(request), request.cancellation, sample_kind);
    if (!rendered)
    {
        return rendered.error();
    }
    if (export_options_request_output_sharpen(request))
    {
        auto sharpened = apply_export_output_sharpen(std::move(rendered).value(),
                                                     request.output_sharpen, request.cancellation);
        if (!sharpened)
            return sharpened.error();
        rendered = std::move(sharpened);
    }
    if (export_options_request_frame(request))
    {
        auto framed = apply_export_delivery_frame(std::move(rendered).value(), request.frame,
                                                  request.cancellation);
        if (!framed)
            return framed.error();
        rendered = std::move(framed);
    }
    if (export_options_request_watermark(request))
    {
        AssetDescriptor descriptor{asset.value()->id, location.value().path,
                                   asset.value()->content_fingerprint};
        auto watermarked = apply_export_delivery_watermark(
            std::move(rendered).value(), request.watermark, descriptor, request.cancellation);
        if (!watermarked)
            return watermarked.error();
        rendered = std::move(watermarked);
    }
    ExportPixelBuffer pixels;
    pixels.width = rendered.value().width;
    pixels.height = rendered.value().height;
    pixels.color_profile = std::move(rendered.value().color_profile);
    pixels.samples = std::move(rendered.value().samples);
    auto encoded =
        raster_->encode(pixels, request.format, request.jpeg_options, request.cancellation,
                        request.png_options, request.tiff_options, export_metadata);
    if (!encoded)
    {
        return encoded.error();
    }
    if (request.jpeg_max_bytes != 0U && encoded.value().size() > request.jpeg_max_bytes)
    {
        // Render once. Search a bounded quality bracket over complete encoded
        // files, including metadata/ICC. Only a verified fitting buffer is published.
        auto options = request.jpeg_options;
        options.quality = kJpegQualityMin;
        auto best = raster_->encode(pixels, request.format, options, request.cancellation,
                                    request.png_options, request.tiff_options, export_metadata);
        if (!best)
            return best.error();
        if (best.value().size() > request.jpeg_max_bytes)
            return make_error(ErrorCode::kValidation,
                              "JPEG cannot fit the requested file size limit",
                              {{"reason", "jpeg_size_limit_unreachable"},
                               {"max_bytes", std::to_string(request.jpeg_max_bytes)},
                               {"minimum_quality_bytes", std::to_string(best.value().size())}});
        int low = kJpegQualityMin + 1;
        int high = request.jpeg_options.quality - 1;
        while (low <= high)
        {
            if (auto active = request.cancellation.check(); !active)
                return active.error();
            options.quality = low + (high - low) / 2;
            auto candidate =
                raster_->encode(pixels, request.format, options, request.cancellation,
                                request.png_options, request.tiff_options, export_metadata);
            if (!candidate)
                return candidate.error();
            if (candidate.value().size() <= request.jpeg_max_bytes)
            {
                best = std::move(candidate);
                low = options.quality + 1;
            }
            else
                high = options.quality - 1;
        }
        encoded = std::move(best);
    }
    auto written =
        write_bytes_atomically(output.value().path, encoded.value(), request.cancellation);
    if (!written)
    {
        return written.error();
    }
    result.width = pixels.width;
    result.height = pixels.height;
    result.bytes_written = encoded.value().size();
    LOG_INFO(ravo::logger(), "export asset={} format={} output={} {}x{} bytes={}", request.asset_id,
             export_format_name(request.format), output.value().path, result.width, result.height,
             result.bytes_written);
    return result;
}

} // namespace ravo
