#include "ravo/services/import_service.h"

#include <chrono>
#include <filesystem>
#include <set>
#include <utility>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/preview_cache.h"
#include "ravo/domain/raster_decoder.h"
#include "ravo/domain/uri.h"
#include "ravo/adapters/text_file.h"
#include "ravo/foundation/log.h"
#include "ravo/services/preview_service.h"
#include "ravo/services/recovery_service.h"

namespace ravo
{
using namespace catalog_service_internal;

ImportService::ImportService(
    const std::unique_ptr<CatalogRepository> &repository,
    const std::unique_ptr<RasterDecoder> &raster, const EngineFacade *const &engine,
    const std::shared_ptr<PreviewCache> &cache, PreviewService &preview, RecoveryService &recovery,
    std::function<void()> &before_publication,
    const std::function<Result<void>(std::string_view, std::string_view)> &checkpoint) noexcept
    : repository_(repository)
    , raster_(raster)
    , engine_(engine)
    , cache_(cache)
    , preview_service_(preview)
    , recovery_service_(recovery)
    , testing_before_import_publication_(before_publication)
    , testing_import_checkpoint_(checkpoint)
{
}

Result<CatalogSnapshot> ImportService::library_snapshot() const
{
    if (!repository_ || !cache_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    snapshot.value().cache_root = cache_->root();
    return snapshot;
}

Result<ImportItemResult>
ImportService::import_one(const std::string_view path, const CancellationToken &cancellation,
                          const ImportPreviewPolicy preview_policy, const bool defer_preview,
                          const bool skip_existing, const std::string_view expected_sha256)
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
    auto digest = sha256_file_hex(location.value().path, cancellation);
    if (!digest)
        return failed_item(location.value().path, digest.error());
    if (!expected_sha256.empty() && expected_sha256 != digest.value())
        return failed_item(location.value().path,
                           make_error(ErrorCode::kConflict,
                                      "Source content changed after import selection",
                                      {{"reason", "import_content_source_changed"}}));
    auto after_hash = read_file_identity(location.value().path);
    if (!after_hash)
        return failed_item(location.value().path, after_hash.error());
    if (after_hash.value().size_bytes != asset.size_bytes ||
        after_hash.value().mtime_unix_ms != asset.mtime_unix_ms)
        return failed_item(location.value().path,
                           make_error(ErrorCode::kConflict,
                                      "Source changed before import publication",
                                      {{"reason", "import_content_source_changed"}}));
    const auto published = repository_->commit_imported_asset(asset, digest.value(), skip_existing);
    if (!published)
    {
        const auto reason = published.error().context.find("reason");
        if (skip_existing && reason != published.error().context.end() &&
            reason->second == "import_duplicate_content")
        {
            auto matched = repository_->find_asset_by_id(published.error().context.at("asset_id"));
            if (!matched)
                return matched.error();
            ImportItemResult duplicate;
            duplicate.status = ImportItemStatus::kDuplicate;
            duplicate.input_path = location.value().path;
            duplicate.asset = std::move(matched).value();
            return duplicate;
        }
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
        preview_service_.seed_browse_source(asset, std::move(raster));
    }

    auto preview_generation = repository_->recovery_state(asset.id);
    if (!preview_generation)
        return preview_generation.error();
    Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Preview was not generated");
    std::optional<TaskError> preview_publication_error;
    const auto persist_browse_thumbnail = [&]() -> Result<PreviewResult>
    {
        auto generation = repository_->recovery_state(asset.id);
        if (!generation)
            return generation.error();
        PreviewRequest browse;
        browse.asset_id = asset.id;
        browse.max_edge = kThumbnailMaxEdge;
        browse.purpose = PreviewPurpose::kBrowse;
        browse.prefer_embedded_preview = true;
        browse.cancellation = cancellation;
        Result<PreviewResult> result = make_error(ErrorCode::kIo, "Preview was not generated");
        if (jpeg_companion)
            result = preview_service_.persist_companion_jpeg_browse_preview(
                asset, *jpeg_companion, kThumbnailMaxEdge, cancellation,
                generation.value().generation);
        if (!result && (result.error().code == ErrorCode::kConflict ||
                        result.error().code == ErrorCode::kCancelled))
            return result.error();
        if (!result && embedded_preview)
            result = preview_service_.persist_embedded_browse_preview(
                asset, *embedded_preview, kThumbnailMaxEdge, cancellation,
                generation.value().generation);
        if (!result && (result.error().code == ErrorCode::kConflict ||
                        result.error().code == ErrorCode::kCancelled))
            return result.error();
        if (!result)
            result = preview_service_.request_preview(browse);
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
            imported_preview.asset_id = asset.id;
            imported_preview.max_edge = preview_edge;
            imported_preview.purpose = PreviewPurpose::kBrowse;
            imported_preview.prefer_embedded_preview = false;
            imported_preview.cancellation = cancellation;
            preview = preview_service_.request_preview(imported_preview);
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
            if (preview.error().code != ErrorCode::kConflict &&
                preview.error().code != ErrorCode::kCancelled)
            {
                auto recorded =
                    repository_->upsert_preview(failed, preview_generation.value().generation);
                if (!recorded && recorded.error().code != ErrorCode::kConflict)
                    preview_publication_error = recorded.error();
            }
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
    result.error = std::move(preview_publication_error);
    if (preview)
    {
        result.preview_cache_path = preview.value().cache_path;
    }
    result.preview_pending = defer_preview;
    auto recovered = recovery_service_.synchronize_committed_change(asset.id, cancellation);
    if (!recovered)
    {
        if (result.error)
            recovered.error().context.emplace("preview_publication_error", result.error->message);
        result.error = recovered.error();
    }
    return result;
}

Result<std::vector<std::string>>
ImportService::enumerate_import_inputs(const std::vector<std::string> &paths,
                                       const CancellationToken &cancellation,
                                       const bool recursive) const
{
    if (repository_ == nullptr || cache_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto catalog = repository_->snapshot();
    if (!catalog)
        return catalog.error();
    return collect_import_paths(paths, cancellation, recursive,
                                {cache_->root(), catalog.value().database_path + ".ravo"});
}

Result<std::vector<ImportItemResult>> ImportService::import_inputs(
    const std::vector<std::string> &paths, const CancellationToken &cancellation,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    auto files = enumerate_import_inputs(paths, cancellation);
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
            case ImportItemStatus::kSkipped:
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

} // namespace ravo
