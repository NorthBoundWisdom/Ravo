#include "ravo/services/catalog_service.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
using namespace catalog_service_internal;
Result<ImportItemResult> CatalogService::import_one(const std::string_view path,
                                                    const CancellationToken &cancellation,
                                                    const ImportPreviewPolicy preview_policy,
                                                    const bool defer_preview)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return failed_item(std::string(path), cancelled.error());
    }
    if (repository_ == nullptr || raster_ == nullptr || engine_ == nullptr || cache_ == nullptr)
    {
        return failed_item(std::string(path),
                           make_error(ErrorCode::kIo, "Catalog session is closed"));
    }

    LOG_INFO(ravo::logger(), "import one path={}", path);
    auto location = normalize_local_input(path);
    if (!location)
    {
        LOG_ERROR(ravo::logger(), "import path normalize failed path={} error={}", path,
                  location.error().message);
        return failed_item(std::string(path), location.error());
    }
    LOG_DEBUG(ravo::logger(), "import normalized path={} uri={}", location.value().path,
              location.value().uri);

    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            exists_error) ||
        exists_error)
    {
        return failed_item(location.value().path,
                           make_error(ErrorCode::kNotFound, "Import input does not exist",
                                      {{"path", location.value().path}}));
    }

    auto existing = repository_->find_asset_by_uri(location.value().uri);
    if (!existing)
    {
        return failed_item(location.value().path, existing.error());
    }
    if (existing.value())
    {
        ImportItemResult duplicate;
        duplicate.status = ImportItemStatus::kDuplicate;
        duplicate.input_path = location.value().path;
        duplicate.asset = *existing.value();
        return duplicate;
    }

    auto identity = read_file_identity(location.value().path);
    if (!identity)
    {
        return failed_item(location.value().path, identity.error());
    }

    AssetRecord asset;
    asset.id = generate_asset_id();
    asset.normalized_uri = location.value().uri;
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    asset.content_fingerprint = make_content_fingerprint(identity.value());
    asset.created_unix_ms = now_unix_ms();
    asset.import_state = std::string(kImportStateImported);

    const std::filesystem::path file_path(
        std::u8string(location.value().path.begin(), location.value().path.end()));
    std::optional<EmbeddedPreview> embedded_preview;
    std::optional<DecodedRaster> validated_raster;
    const auto apply_inspection = [&](const InspectionResult &inspected) -> Result<void>
    {
        if (!inspected.is_raw)
        {
            return make_error(ErrorCode::kUnsupported, "Input is not a supported RAW file",
                              {{"path", location.value().path}});
        }
        asset.media_type = std::string(kMediaTypeRaw);
        asset.width = inspected.width;
        asset.height = inspected.height;
        if (!inspected.make.empty())
        {
            asset.capture.camera_make = inspected.make;
        }
        if (!inspected.model.empty())
        {
            asset.capture.camera_model = inspected.model;
        }
        asset.capture.iso = inspected.iso;
        asset.capture.aperture = inspected.aperture;
        asset.capture.focal_length_mm = inspected.focal_length_mm;
        asset.capture.shutter_s = inspected.shutter_s;
        asset.capture.captured_unix_s = inspected.captured_unix_s;
        return {};
    };
    const auto map_raw_probe_error = [&](const TaskError &error) -> ImportItemResult
    {
        if (error.code == ErrorCode::kUnsupported || error.code == ErrorCode::kValidation)
        {
            return unsupported_item(location.value().path, error);
        }
        return failed_item(location.value().path, error);
    };

    if (is_raw_extension(file_path))
    {
        auto probed = engine_->inspect_with_embedded_preview(location.value().path,
                                                             kThumbnailMaxEdge, cancellation);
        if (!probed)
        {
            return map_raw_probe_error(probed.error());
        }
        auto applied = apply_inspection(probed.value().inspection);
        if (!applied)
        {
            return unsupported_item(location.value().path, applied.error());
        }
        embedded_preview = std::move(probed.value().embedded_preview);
    }
    else
    {
        auto raster = raster_->probe(location.value().path);
        if (raster)
        {
            asset.media_type = raster.value().media_type;
            asset.width = raster.value().width;
            asset.height = raster.value().height;
            if (is_common_raster_media(asset.media_type))
            {
                auto decoded =
                    raster_->decode(location.value().path, kThumbnailMaxEdge, cancellation);
                if (!decoded)
                {
                    if (decoded.error().code == ErrorCode::kUnsupported)
                    {
                        return unsupported_item(location.value().path, decoded.error());
                    }
                    return failed_item(location.value().path, decoded.error());
                }
                validated_raster = std::move(decoded).value();
            }
        }
        else if (should_try_raw_after_raster(raster.error()))
        {
            auto probed = engine_->inspect_with_embedded_preview(location.value().path,
                                                                 kThumbnailMaxEdge, cancellation);
            if (!probed)
            {
                return map_raw_probe_error(probed.error());
            }
            auto applied = apply_inspection(probed.value().inspection);
            if (!applied)
            {
                return unsupported_item(location.value().path, applied.error());
            }
            embedded_preview = std::move(probed.value().embedded_preview);
        }
        else if (raster.error().code == ErrorCode::kUnsupported)
        {
            return unsupported_item(location.value().path, raster.error());
        }
        else
        {
            return failed_item(location.value().path, raster.error());
        }
    }

    if (is_raw_media_type(asset.media_type) && !embedded_preview)
    {
        auto decoded = engine_->decode_raw_frame(location.value().path, cancellation);
        if (!decoded)
        {
            return map_raw_probe_error(decoded.error());
        }
    }

    std::optional<std::string> jpeg_companion;
    if (is_raw_media_type(asset.media_type))
    {
        auto companion = adjacent_jpeg(location.value().path);
        if (!companion)
        {
            return failed_item(location.value().path, companion.error());
        }
        jpeg_companion = std::move(companion).value();
    }

    if (media_type_has_embedded_capture(asset.media_type))
    {
        auto extracted =
            engine_->read_embedded_capture_metadata(location.value().path, cancellation);
        if (!extracted)
        {
            return failed_item(location.value().path, extracted.error());
        }
        merge_engine_capture(asset.capture, extracted.value());
        auto valid_capture = validate_capture_metadata(asset.capture);
        if (!valid_capture)
        {
            return failed_item(location.value().path, valid_capture.error());
        }
    }

    if (testing_before_import_publication_)
    {
        auto callback = std::move(testing_before_import_publication_);
        callback();
    }
    auto ready_to_publish = cancellation.check();
    if (!ready_to_publish)
    {
        return failed_item(location.value().path, ready_to_publish.error());
    }
    const auto published = repository_->commit_imported_asset(asset);
    if (!published)
    {
        return failed_item(location.value().path, published.error());
    }

    if (validated_raster)
    {
        RasterBuffer raster;
        raster.width = validated_raster->width;
        raster.height = validated_raster->height;
        raster.source_width = validated_raster->source_width;
        raster.source_height = validated_raster->source_height;
        raster.srgb = std::move(validated_raster->rgb);
        raster.color_profile = std::move(validated_raster->color_profile);
        browse_decoded_preview_source_ =
            DecodedPreviewSource{asset.id, asset.content_fingerprint.value_or("none"),
                                 kThumbnailMaxEdge, std::move(raster)};
    }

    Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Preview was not generated");
    const auto persist_browse_thumbnail = [&]() -> Result<PreviewResult>
    {
        PreviewRequest browse;
        browse.max_edge = kThumbnailMaxEdge;
        browse.purpose = PreviewPurpose::kBrowse;
        browse.prefer_embedded_preview = true;
        browse.cancellation = cancellation;
        Result<PreviewResult> result = make_error(ErrorCode::kIo, "Preview was not generated");
        if (jpeg_companion)
            result = persist_companion_jpeg_browse_preview(asset, *jpeg_companion, kThumbnailMaxEdge,
                                                          cancellation);
        if (!result && embedded_preview)
            result = persist_embedded_browse_preview(asset, *embedded_preview, kThumbnailMaxEdge,
                                                     cancellation);
        if (!result)
            result = generate_preview(asset, browse, {});
        return result;
    };
    if (!defer_preview)
    {
        if (preview_policy == ImportPreviewPolicy::kMinimal)
            preview = persist_browse_thumbnail();
        else
        {
            const std::uint32_t preview_edge =
                preview_policy == ImportPreviewPolicy::kStandard ? kDefaultPreviewMaxEdge : 0U;
            PreviewRequest imported_preview;
            imported_preview.max_edge = preview_edge;
            imported_preview.purpose = PreviewPurpose::kBrowse;
            imported_preview.prefer_embedded_preview = false;
            imported_preview.cancellation = cancellation;
            preview = generate_preview(asset, imported_preview, {});
        }
        if (!preview)
        {
            LOG_ERROR(ravo::logger(), "preview failed asset={} path={} error={}", asset.id,
                      location.value().path, preview.error().message);
            PreviewRecord failed;
            failed.asset_id = asset.id;
            failed.state = std::string(kPreviewStateFailed);
            failed.cache_key =
                make_preview_cache_key(asset.id, asset.width.value_or(0), asset.height.value_or(0),
                                       asset.content_fingerprint.value_or("none"));
            static_cast<void>(repository_->upsert_preview(failed));
        }
        else
        {
            LOG_INFO(ravo::logger(), "preview ready asset={} cache={}", asset.id,
                     preview.value().cache_path);
        }
    }
    else
    {
        preview = persist_browse_thumbnail();
        if (preview)
            LOG_INFO(ravo::logger(), "browse preview ready asset={} cache={}", asset.id,
                     preview.value().cache_path);
        else
            LOG_ERROR(ravo::logger(), "browse preview failed asset={} path={} error={}", asset.id,
                      location.value().path, preview.error().message);
    }

    ImportItemResult result;
    result.status = ImportItemStatus::kImported;
    result.input_path = location.value().path;
    result.asset = asset;
    if (preview)
    {
        result.preview_cache_path = preview.value().cache_path;
    }
    result.preview_pending = defer_preview;
    auto recovered = synchronize_committed_change(asset.id, cancellation);
    if (!recovered)
    {
        result.error = recovered.error();
    }
    return result;
}

Result<std::vector<std::string>>
CatalogService::enumerate_import_inputs(const std::vector<std::string> &paths,
                                        const CancellationToken &cancellation,
                                        const bool recursive) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return collect_import_paths(paths, cancellation, recursive);
}

Result<std::vector<ImportItemResult>> CatalogService::import_inputs(
    const std::vector<std::string> &paths, const CancellationToken &cancellation,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    auto files = collect_import_paths(paths, cancellation);
    if (!files)
    {
        return files.error();
    }
    std::vector<ImportItemResult> results;
    results.reserve(files.value().size());
    if (files.value().empty())
    {
        if (progress)
        {
            progress(0, 0, nullptr);
        }
        return results;
    }
    const auto started = std::chrono::steady_clock::now();
    if (progress)
    {
        progress(0, files.value().size(), nullptr);
    }
    int imported_count = 0;
    int duplicate_count = 0;
    int unsupported_count = 0;
    int failed_count = 0;
    for (std::size_t index = 0; index < files.value().size(); ++index)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            ImportItemResult stopped;
            stopped.status = ImportItemStatus::kFailed;
            stopped.input_path = files.value()[index];
            stopped.error = cancelled.error();
            results.push_back(std::move(stopped));
            ++failed_count;
            break;
        }
        auto item = import_one(files.value()[index], cancellation);
        if (!item)
        {
            results.push_back(failed_item(files.value()[index], item.error()));
            ++failed_count;
        }
        else
        {
            switch (item.value().status)
            {
            case ImportItemStatus::kImported:
                ++imported_count;
                break;
            case ImportItemStatus::kDuplicate:
                ++duplicate_count;
                break;
            case ImportItemStatus::kUnsupported:
                ++unsupported_count;
                break;
            case ImportItemStatus::kFailed:
                ++failed_count;
                break;
            }
            results.push_back(std::move(item).value());
        }
        if (progress)
        {
            progress(index + 1U, files.value().size(), &results.back());
        }
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    LOG_INFO(ravo::logger(),
             "import batch files={} imported={} duplicate={} unsupported={} failed={} {}ms",
             files.value().size(), imported_count, duplicate_count, unsupported_count, failed_count,
             elapsed_ms);
    return results;
}

Result<std::vector<ExportResult>> CatalogService::export_assets(
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
    Result<void> valid_options;
    switch (request.options.format)
    {
    case ExportFormat::kJpeg:
        valid_options = validate_jpeg_export_options(request.options.jpeg_options);
        break;
    case ExportFormat::kPng:
        valid_options = validate_png_export_options(request.options.png_options);
        break;
    case ExportFormat::kTiff:
        valid_options = validate_tiff_export_options(request.options.tiff_options);
        break;
    case ExportFormat::kOriginalCopy:
        if (request.options.metadata_mode != ExportMetadataMode::kFull)
        {
            return make_error(ErrorCode::kValidation,
                              "Metadata privacy mode does not apply to original copy",
                              {{"format", "original"}, {"reason", "metadata_mode_not_applicable"}});
        }
        break;
    }
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
        auto source = normalize_local_input(asset.value()->normalized_uri);
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

Result<ExportResult> CatalogService::export_asset(const ExportRequest &request)
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
    if (request.format == ExportFormat::kJpeg)
    {
        auto options = validate_jpeg_export_options(request.jpeg_options);
        if (!options)
        {
            return options.error();
        }
    }
    if (request.format == ExportFormat::kPng)
    {
        auto options = validate_png_export_options(request.png_options);
        if (!options)
        {
            return options.error();
        }
    }
    if (request.format == ExportFormat::kTiff)
    {
        auto options = validate_tiff_export_options(request.tiff_options);
        if (!options)
        {
            return options.error();
        }
    }
    if (request.format == ExportFormat::kOriginalCopy &&
        request.metadata_mode != ExportMetadataMode::kFull)
    {
        return make_error(ErrorCode::kValidation,
                          "Metadata privacy mode does not apply to original copy",
                          {{"format", "original"}, {"reason", "metadata_mode_not_applicable"}});
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
    if (request.format != ExportFormat::kOriginalCopy)
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

    ExportResult result;
    result.asset_id = request.asset_id;
    result.output_path = output.value().path;
    result.format = request.format;
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
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"asset_id", request.asset_id}, {"path", location.value().path}});
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
        auto valid = engine_->validate(parsed.value());
        if (!valid)
        {
            return valid.error();
        }
        edit_recipe = std::move(parsed).value();
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
    auto rendered = render_for_export(*asset.value(), location.value().path, edit_recipe,
                                      request.max_edge, request.cancellation, sample_kind);
    if (!rendered)
    {
        return rendered.error();
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
