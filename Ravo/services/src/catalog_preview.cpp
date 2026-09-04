#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "catalog_internal.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/offline_edit_proxy.h"

namespace ravo
{

Result<PreviewResult> CatalogService::build_import_preview(const std::string_view asset_id,
                                                           const ImportPreviewPolicy policy,
                                                           const CancellationToken &cancellation)
{
    PreviewRequest request;
    request.asset_id = std::string(asset_id);
    request.max_edge = policy == ImportPreviewPolicy::kMinimal  ? kThumbnailMaxEdge :
                       policy == ImportPreviewPolicy::kStandard ? kDefaultPreviewMaxEdge :
                                                                  0U;
    request.purpose = PreviewPurpose::kBrowse;
    request.prefer_embedded_preview = policy == ImportPreviewPolicy::kMinimal;
    request.cancellation = cancellation;
    return request_preview(request);
}
Result<PreviewResult>
CatalogService::request_preview(const PreviewRequest &request,
                                const std::optional<DevelopParams> &live_develop)
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
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
    return generate_preview(*asset.value(), request, live_develop);
}

Result<PreviewRebuildResult> CatalogService::rebuild_previews(
    const std::vector<std::string> &asset_ids, const CancellationToken &cancellation,
    const std::function<void(std::size_t, std::size_t, const PreviewRebuildItemResult *)> &progress)
{
    if (repository_ == nullptr || cache_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto listed = repository_->list_assets();
    if (!listed)
        return listed.error();
    std::set<std::string, std::less<>> known;
    for (const auto &asset : listed.value())
        known.insert(asset.id);
    std::vector<std::string> selected;
    if (asset_ids.empty())
    {
        selected.reserve(listed.value().size());
        for (const auto &asset : listed.value())
            selected.push_back(asset.id);
    }
    else
    {
        selected.reserve(asset_ids.size());
        std::set<std::string, std::less<>> unique;
        for (const auto &asset_id : asset_ids)
        {
            if (asset_id.empty() || !unique.insert(asset_id).second)
                return make_error(
                    ErrorCode::kInvalidArgument,
                    "Preview rebuild asset IDs must be non-empty and unique",
                    {{"asset_id", asset_id}, {"reason", "invalid_preview_rebuild_assets"}});
            if (!known.contains(asset_id))
                return make_error(
                    ErrorCode::kNotFound, "Preview rebuild asset does not exist",
                    {{"asset_id", asset_id}, {"reason", "preview_rebuild_asset_not_found"}});
            selected.push_back(asset_id);
        }
    }

    PreviewRebuildResult result;
    result.total = selected.size();
    result.items.reserve(selected.size());
    if (progress)
        progress(0U, result.total, nullptr);
    for (std::size_t index = 0; index < selected.size(); ++index)
    {
        active = cancellation.check();
        if (!active)
        {
            auto error = std::move(active).error();
            error.context.insert_or_assign("completed_count", std::to_string(result.completed));
            error.context.insert_or_assign("total_count", std::to_string(result.total));
            return error;
        }
        PreviewRebuildItemResult item;
        item.asset_id = selected[index];
        auto removed = cache_->remove_for_asset(item.asset_id);
        if (!removed)
        {
            item.error = removed.error();
        }
        else
        {
            PreviewRequest browse;
            browse.asset_id = item.asset_id;
            browse.max_edge = kThumbnailMaxEdge;
            browse.request_revision = static_cast<std::uint64_t>(index) * 2U + 1U;
            browse.purpose = PreviewPurpose::kBrowse;
            browse.prefer_embedded_preview = true;
            browse.cancellation = cancellation;
            browse.correlation_id = "preview-rebuild-browse";
            auto browse_result = request_preview(browse);
            if (!browse_result)
            {
                if (browse_result.error().code == ErrorCode::kCancelled)
                {
                    auto error = browse_result.error();
                    error.context.insert_or_assign("completed_count",
                                                   std::to_string(result.completed));
                    error.context.insert_or_assign("total_count", std::to_string(result.total));
                    return error;
                }
                item.error = browse_result.error();
            }
            else
            {
                item.browse_cache_path = browse_result.value().cache_path;
                PreviewRequest develop;
                develop.asset_id = item.asset_id;
                develop.max_edge = kDefaultPreviewMaxEdge;
                develop.request_revision = static_cast<std::uint64_t>(index) * 2U + 2U;
                develop.purpose = PreviewPurpose::kDevelop;
                develop.cancellation = cancellation;
                develop.correlation_id = "preview-rebuild-develop";
                auto develop_result = request_preview(develop);
                if (!develop_result)
                {
                    if (develop_result.error().code == ErrorCode::kCancelled)
                    {
                        auto error = develop_result.error();
                        error.context.insert_or_assign("completed_count",
                                                       std::to_string(result.completed));
                        error.context.insert_or_assign("total_count", std::to_string(result.total));
                        return error;
                    }
                    item.error = develop_result.error();
                }
                else
                {
                    item.develop_cache_path = develop_result.value().cache_path;
                }
            }
        }
        ++result.completed;
        if (item.error)
            ++result.failed;
        else
            ++result.succeeded;
        result.items.push_back(std::move(item));
        if (progress)
            progress(result.completed, result.total, &result.items.back());
    }
    return result;
}

Result<PreviewResult> CatalogService::persist_embedded_browse_preview(
    const AssetRecord &asset, const EmbeddedPreview &embedded, const std::uint32_t max_edge,
    const CancellationToken &cancellation)
{
    if (engine_ == nullptr || raster_ == nullptr || cache_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    AssetRecord catalog_asset = asset;
    if (embedded.width > 0 && embedded.height > 0 && catalog_asset.width && catalog_asset.height &&
        (*catalog_asset.width >= *catalog_asset.height) != (embedded.width >= embedded.height))
    {
        const auto previous_width = catalog_asset.width;
        catalog_asset.width = catalog_asset.height;
        catalog_asset.height = previous_width;
        static_cast<void>(repository_->update_asset(catalog_asset));
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(catalog_asset.width.value_or(0), catalog_asset.height.value_or(0), max_edge,
                        width, height);
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    const auto cache_key = make_preview_cache_key(catalog_asset.id, width, height, fingerprint,
                                                  kEmbeddedBrowsePreviewDigest);
    PreviewResult result;
    result.asset_id = catalog_asset.id;
    result.cache_key = cache_key;
    auto existing = cache_->existing_png(cache_key);
    if (!existing)
    {
        return existing.error();
    }
    if (existing.value())
    {
        result.cache_path = *existing.value();
        result.width = width;
        result.height = height;
        PreviewRecord record;
        record.asset_id = catalog_asset.id;
        record.cache_key = cache_key;
        record.width = width;
        record.height = height;
        record.state = std::string(kPreviewStateReady);
        record.cache_relpath = cache_->relative_png_path(cache_key);
        record.last_success_unix_ms = now_unix_ms();
        static_cast<void>(repository_->upsert_preview(record));
        return result;
    }

    auto decoded =
        raster_->decode_memory(embedded.bytes, max_edge, cancellation, embedded.rotate_quarters);
    if (!decoded)
    {
        return decoded.error();
    }
    RasterBuffer source;
    source.width = decoded.value().width;
    source.height = decoded.value().height;
    source.source_width = decoded.value().source_width;
    source.source_height = decoded.value().source_height;
    source.srgb = std::move(decoded.value().rgb);
    source.color_profile = std::move(decoded.value().color_profile);
    if (source.color_profile.kind == ColorProfileKind::kMissing)
    {
        source.color_profile = embedded.color_profile;
    }
    DevelopParams develop;
    auto recipe = recipe_from_develop(
        {catalog_asset.id, catalog_asset.normalized_uri, catalog_asset.content_fingerprint},
        develop);
    if (!recipe)
    {
        return recipe.error();
    }
    RenderRequest render;
    render.asset = recipe.value().asset;
    render.recipe = recipe.value();
    render.cancellation = cancellation;
    render.correlation_id = catalog_asset.id;
    auto rendered = engine_->render_to_image(render, &source);
    if (!rendered)
    {
        return rendered.error();
    }
    auto encoded = engine_->encode_preview_png(rendered.value());
    if (!encoded)
    {
        return encoded.error();
    }
    if (testing_before_preview_cache_publication_)
        testing_before_preview_cache_publication_();
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto committed = cache_->commit_png_bytes(cache_key, encoded.value());
    if (!committed)
    {
        return committed.error();
    }
    result.cache_path = committed.value();
    result.width = rendered.value().width;
    result.height = rendered.value().height;
    PreviewRecord record;
    record.asset_id = catalog_asset.id;
    record.cache_key = cache_key;
    record.width = result.width;
    record.height = result.height;
    record.state = std::string(kPreviewStateReady);
    record.cache_relpath = cache_->relative_png_path(cache_key);
    record.last_success_unix_ms = now_unix_ms();
    const auto stored = repository_->upsert_preview(record);
    if (!stored)
    {
        return stored.error();
    }
    return result;
}

Result<PreviewResult> CatalogService::persist_companion_jpeg_browse_preview(
    const AssetRecord &asset, const std::string_view jpeg_path, const std::uint32_t max_edge,
    const CancellationToken &cancellation)
{
    if (engine_ == nullptr || raster_ == nullptr || cache_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(asset.width.value_or(0), asset.height.value_or(0), max_edge, width, height);
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    const auto cache_key = make_preview_cache_key(asset.id, width, height, fingerprint,
                                                  kCompanionJpegBrowsePreviewDigest);
    PreviewResult result;
    result.asset_id = asset.id;
    result.cache_key = cache_key;
    auto existing = cache_->existing_png(cache_key);
    if (!existing)
    {
        return existing.error();
    }
    if (existing.value())
    {
        result.cache_path = *existing.value();
        result.width = width;
        result.height = height;
        PreviewRecord record;
        record.asset_id = asset.id;
        record.cache_key = cache_key;
        record.width = width;
        record.height = height;
        record.state = std::string(kPreviewStateReady);
        record.cache_relpath = cache_->relative_png_path(cache_key);
        record.last_success_unix_ms = now_unix_ms();
        static_cast<void>(repository_->upsert_preview(record));
        return result;
    }

    auto decoded = raster_->decode(jpeg_path, max_edge, cancellation);
    if (!decoded)
    {
        return decoded.error();
    }
    RasterBuffer source;
    source.width = decoded.value().width;
    source.height = decoded.value().height;
    source.source_width = decoded.value().source_width;
    source.source_height = decoded.value().source_height;
    source.srgb = std::move(decoded.value().rgb);
    source.color_profile = std::move(decoded.value().color_profile);
    if (source.color_profile.kind == ColorProfileKind::kMissing)
    {
        source.color_profile.kind = ColorProfileKind::kBuiltin;
        source.color_profile.model = ColorModel::kRgb;
        source.color_profile.identifier = "srgb";
    }
    DevelopParams develop;
    auto recipe =
        recipe_from_develop({asset.id, asset.normalized_uri, asset.content_fingerprint}, develop);
    if (!recipe)
    {
        return recipe.error();
    }
    RenderRequest render;
    render.asset = recipe.value().asset;
    render.recipe = recipe.value();
    render.cancellation = cancellation;
    render.correlation_id = asset.id;
    auto rendered = engine_->render_to_image(render, &source);
    if (!rendered)
    {
        return rendered.error();
    }
    auto encoded = engine_->encode_preview_png(rendered.value());
    if (!encoded)
    {
        return encoded.error();
    }
    if (testing_before_preview_cache_publication_)
        testing_before_preview_cache_publication_();
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto committed = cache_->commit_png_bytes(cache_key, encoded.value());
    if (!committed)
    {
        return committed.error();
    }
    result.cache_path = committed.value();
    result.width = rendered.value().width;
    result.height = rendered.value().height;
    PreviewRecord record;
    record.asset_id = asset.id;
    record.cache_key = cache_key;
    record.width = result.width;
    record.height = result.height;
    record.state = std::string(kPreviewStateReady);
    record.cache_relpath = cache_->relative_png_path(cache_key);
    record.last_success_unix_ms = now_unix_ms();
    const auto stored = repository_->upsert_preview(record);
    if (!stored)
    {
        return stored.error();
    }
    return result;
}

Result<PreviewResult>
CatalogService::generate_preview(const AssetRecord &asset, const PreviewRequest &request,
                                 const std::optional<DevelopParams> &live_develop)
{
    if (engine_ == nullptr || raster_ == nullptr || cache_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }

    auto location = normalize_local_input(asset.normalized_uri);
    if (!location)
    {
        LOG_ERROR(ravo::logger(), "preview normalize failed asset={} uri={} error={}", asset.id,
                  asset.normalized_uri, location.error().message);
        return location.error();
    }
    std::error_code exists_error;
    const bool original_exists =
        std::filesystem::is_regular_file(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            exists_error) &&
        !exists_error;
    AssetRecord working = asset;
    if (!original_exists)
    {
        if (working.import_state != kImportStateMissing)
        {
            LOG_INFO(ravo::logger(), "original file missing asset={} path={}", asset.id,
                     location.value().path);
            working.import_state = std::string(kImportStateMissing);
            working.error_code = std::string(error_code_name(ErrorCode::kNotFound));
            working.error_message = "Original file is missing";
            static_cast<void>(repository_->update_asset(working));
        }
    }
    else if (working.import_state == kImportStateMissing)
    {
        working.import_state = std::string(kImportStateImported);
        working.error_code.reset();
        working.error_message.reset();
        static_cast<void>(repository_->update_asset(working));
    }

    std::string render_path = location.value().path;
    bool using_offline_proxy = false;
    std::string offline_proxy_cache_tag;
    std::string preview_media_state =
        original_exists ?
            std::string(offline_edit_media_state_name(OfflineEditMediaState::kOriginal)) :
            std::string(offline_edit_media_state_name(OfflineEditMediaState::kMissing));
    if (!original_exists)
    {
        auto offline = verify_offline_edit_proxy(asset.id);
        if (offline && offline.value().usable_for_develop && offline.value().manifest.has_value())
        {
            render_path = offline.value().manifest->proxy_path;
            using_offline_proxy = true;
            offline_proxy_cache_tag = "offline_proxy:" + offline.value().manifest->proxy_sha256;
            preview_media_state =
                std::string(offline_edit_media_state_name(OfflineEditMediaState::kProxy));
            // Proxy is a bounded sRGB TIFF stand-in; never re-enter RAW demosaic.
            working.media_type = std::string(kMediaTypeTiff);
            if (offline.value().manifest->width > 0 && offline.value().manifest->height > 0)
            {
                working.width = offline.value().manifest->width;
                working.height = offline.value().manifest->height;
            }
        }
    }

    const auto source_width = working.width.value_or(asset.width.value_or(0));
    const auto source_height = working.height.value_or(asset.height.value_or(0));
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(source_width, source_height, request.max_edge, width, height);
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    const bool embedded_browse =
        request.purpose == PreviewPurpose::kBrowse && request.prefer_embedded_preview &&
        request.persist_preview_record && is_raw_media_type(working.media_type) &&
        !live_develop.has_value() && !request.ignore_edits && !request.ignore_crop &&
        !request.ignore_straighten && !working.has_edits;
    if (embedded_browse && original_exists)
    {
        auto companion = adjacent_jpeg(location.value().path);
        if (!companion)
        {
            return companion.error();
        }
        if (companion.value())
        {
            auto persisted = persist_companion_jpeg_browse_preview(
                working, *companion.value(), request.max_edge, request.cancellation);
            if (persisted)
            {
                persisted.value().request_revision = request.request_revision;
                persisted.value().original_missing = false;
                return persisted;
            }
            LOG_INFO(ravo::logger(),
                     "companion JPEG browse preview persist failed asset={} error={}", asset.id,
                     persisted.error().message);
        }
        const auto cache_key = make_preview_cache_key(asset.id, width, height, fingerprint,
                                                      kEmbeddedBrowsePreviewDigest);
        auto existing = cache_->existing_png(cache_key);
        if (!existing)
        {
            return existing.error();
        }
        if (existing.value())
        {
            PreviewResult result;
            result.asset_id = asset.id;
            result.request_revision = request.request_revision;
            result.cache_key = cache_key;
            result.cache_path = *existing.value();
            result.width = width;
            result.height = height;
            PreviewRecord record;
            record.asset_id = asset.id;
            record.cache_key = cache_key;
            record.width = width;
            record.height = height;
            record.state = std::string(kPreviewStateReady);
            record.cache_relpath = cache_->relative_png_path(cache_key);
            record.last_success_unix_ms = now_unix_ms();
            static_cast<void>(repository_->upsert_preview(record));
            return result;
        }
        auto extracted = engine_->extract_embedded_preview(location.value().path, request.max_edge,
                                                           request.cancellation);
        if (extracted)
        {
            auto persisted = persist_embedded_browse_preview(
                working, extracted.value(), request.max_edge, request.cancellation);
            if (persisted)
            {
                persisted.value().request_revision = request.request_revision;
                persisted.value().original_missing = false;
                return persisted;
            }
            LOG_INFO(ravo::logger(), "embedded browse preview persist failed asset={} error={}",
                     asset.id, persisted.error().message);
        }
        else
        {
            LOG_INFO(ravo::logger(), "embedded browse preview unavailable asset={} error={}",
                     asset.id, extracted.error().message);
        }
    }
    auto baseline_recipe = baseline_recipe_for(working, location.value().path);
    if (!baseline_recipe)
    {
        return baseline_recipe.error();
    }
    Recipe edit_recipe = std::move(baseline_recipe).value();
    const auto recipe_digest = [&](const Recipe &recipe) -> Result<std::string>
    {
        auto serialized = serialize_recipe(recipe);
        if (!serialized)
        {
            return serialized.error();
        }
        auto color_fingerprint = engine_->input_color_cache_fingerprint(recipe);
        if (!color_fingerprint)
        {
            return color_fingerprint.error();
        }
        auto output_fingerprint = engine_->output_color_cache_fingerprint(recipe);
        if (!output_fingerprint)
        {
            return output_fingerprint.error();
        }
        auto lut_fingerprint = engine_->lut3d_cache_fingerprint(recipe, request.cancellation);
        if (!lut_fingerprint)
        {
            return lut_fingerprint.error();
        }
        return fnv1a64_hex(serialized.value() + "|" + color_fingerprint.value() + "|" +
                           output_fingerprint.value() + "|" + lut_fingerprint.value());
    };
    std::string edit_digest = "identity";
    if (!edit_recipe.operations.empty())
    {
        auto digest = recipe_digest(edit_recipe);
        if (!digest)
        {
            return digest.error();
        }
        edit_digest = std::move(digest).value();
    }
    const std::string baseline_digest = edit_digest;
    if (!request.ignore_edits)
    {
        if (live_develop.has_value())
        {
            auto built = recipe_from_develop(edit_recipe.asset, *live_develop);
            if (!built)
            {
                return built.error();
            }
            auto valid = engine_->validate(built.value());
            if (!valid)
            {
                return valid.error();
            }
            edit_recipe = std::move(built).value();
            auto digest = recipe_digest(edit_recipe);
            if (!digest)
            {
                return digest.error();
            }
            edit_digest = matches_develop_baseline(working, *live_develop) ?
                              baseline_digest :
                              std::move(digest).value();
        }
        else
        {
            auto stored = repository_->load_recipe_json(asset.id);
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
                auto digest = recipe_digest(edit_recipe);
                if (!digest)
                {
                    return digest.error();
                }
                edit_digest = std::move(digest).value();
            }
        }
        if (request.ignore_crop)
        {
            strip_crop_operations(edit_recipe);
            if (edit_digest != "identity")
            {
                edit_digest += "_nocrop";
            }
        }
        if (request.ignore_straighten)
        {
            strip_straighten_operations(edit_recipe);
            if (edit_digest != "identity")
            {
                edit_digest += "_nostraighten";
            }
        }
    }
    if (request.roi.has_value())
    {
        if (!original_exists)
        {
            return make_error(
                ErrorCode::kNotFound,
                "Original file is missing; offline-edit proxy cannot serve ROI inspect",
                {{"path", location.value().path},
                 {"asset_id", asset.id},
                 {"reason",
                  using_offline_proxy ? "offline_proxy_roi_unsupported" : "original_missing"}});
        }
        return generate_roi_preview(asset, request, edit_recipe, render_path);
    }
    const bool interactive = !request.persist_preview_record || request.overlay_mask_id.has_value();
    std::string cache_digest = interactive ? "interactive" : edit_digest;
    if (using_offline_proxy)
    {
        // Cache keys must remain filesystem-safe; fold the proxy tag into hex.
        cache_digest += "_opx_" + fnv1a64_hex(offline_proxy_cache_tag);
    }
    const auto cache_key =
        make_preview_cache_key(asset.id, width, height, fingerprint, cache_digest);

    PreviewResult result;
    result.asset_id = asset.id;
    result.request_revision = request.request_revision;
    result.cache_key = cache_key;
    result.original_missing = !original_exists;
    result.media_state = preview_media_state;

    if (!interactive)
    {
        auto existing = cache_->existing_png(cache_key);
        if (!existing)
        {
            return existing.error();
        }
        if (existing.value())
        {
            result.cache_path = *existing.value();
            result.width = width;
            result.height = height;
            PreviewRecord record;
            record.asset_id = asset.id;
            record.cache_key = cache_key;
            record.width = width;
            record.height = height;
            record.state = std::string(kPreviewStateReady);
            record.cache_relpath = cache_->relative_png_path(cache_key);
            record.last_success_unix_ms = now_unix_ms();
            static_cast<void>(repository_->upsert_preview(record));
            return result;
        }
    }

    if (!original_exists && !using_offline_proxy)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", location.value().path},
                           {"asset_id", asset.id},
                           {"reason", "original_missing"}});
    }

    if (using_offline_proxy)
    {
        disable_raw_preprocess(edit_recipe);
    }

    const auto render_started = std::chrono::steady_clock::now();
    RenderedImage rendered;
    const PreviewLane lane = request.purpose == PreviewPurpose::kBrowse ?
                                 PreviewLane::kBackgroundBrowse :
                                 PreviewLane::kForegroundDevelop;
    if (is_raw_media_type(working.media_type))
    {
        auto linear = cached_linear_working(working, render_path, edit_recipe, width, height,
                                            request.max_edge, request.cancellation, lane);
        if (!linear)
        {
            return linear.error();
        }
        Recipe rgb_recipe = edit_recipe;
        disable_raw_preprocess(rgb_recipe);
        auto applied =
            interactive && lane == PreviewLane::kForegroundDevelop &&
                    request.max_edge <= kInteractivePreviewMaxEdge ?
                engine_->render_interactive_linear_working(
                    linear.value()->buffer, rgb_recipe, linear.value()->interactive_render_cache,
                    request.cancellation, request.overlay_mask_id, request.need_cpu_pixels) :
                engine_->render_linear_working(linear.value()->buffer, rgb_recipe,
                                               request.cancellation, request.overlay_mask_id);
        if (!applied)
        {
            return applied.error();
        }
        rendered = std::move(applied).value();
    }
    else
    {
        auto linear = cached_linear_working(working, render_path, edit_recipe, width, height,
                                            request.max_edge, request.cancellation, lane);
        if (!linear)
        {
            return linear.error();
        }
        auto applied =
            interactive && lane == PreviewLane::kForegroundDevelop &&
                    request.max_edge <= kInteractivePreviewMaxEdge ?
                engine_->render_interactive_linear_working(
                    linear.value()->buffer, edit_recipe, linear.value()->interactive_render_cache,
                    request.cancellation, request.overlay_mask_id, request.need_cpu_pixels) :
                engine_->render_linear_working(linear.value()->buffer, edit_recipe,
                                               request.cancellation, request.overlay_mask_id);
        if (!applied)
        {
            return applied.error();
        }
        rendered = std::move(applied).value();
    }
    const auto render_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - render_started)
                               .count();
    LOG_INFO(ravo::logger(), "preview render asset={} {}x{} interactive={} gpu={} {}ms", asset.id,
             rendered.width, rendered.height, interactive,
             rendered.gpu_backend.empty() ? "cpu" : rendered.gpu_backend, render_ms);

    result.width = rendered.width;
    result.height = rendered.height;
    result.gpu_backend = rendered.gpu_backend;
    result.gpu_display_generation = rendered.gpu_display_generation;
    if (rendered.gpu_display_generation != 0U)
    {
        const auto frame = engine_->gpu_display_frame(EngineFacade::GpuDisplayKind::kPreview);
        if (frame.generation == rendered.gpu_display_generation)
        {
            result.gpu_display_width = frame.width;
            result.gpu_display_height = frame.height;
            result.gpu_display_native_surface = frame.native_surface;
        }
    }
    if (interactive)
    {
        result.rgb = std::move(rendered.rgb);
        result.color_profile = std::move(rendered.color_profile);
        result.mask_alpha = std::move(rendered.mask_alpha);
        return result;
    }

    auto encoded = engine_->encode_preview_png(rendered);
    if (!encoded)
    {
        return encoded.error();
    }
    if (testing_before_preview_cache_publication_)
        testing_before_preview_cache_publication_();
    cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto committed = cache_->commit_png_bytes(cache_key, encoded.value());
    if (!committed)
    {
        return committed.error();
    }
    result.cache_path = committed.value();

    PreviewRecord record;
    record.asset_id = asset.id;
    record.cache_key = cache_key;
    record.width = result.width;
    record.height = result.height;
    record.state = std::string(kPreviewStateReady);
    record.cache_relpath = cache_->relative_png_path(cache_key);
    record.last_success_unix_ms = now_unix_ms();
    const auto stored = repository_->upsert_preview(record);
    if (!stored)
    {
        return stored.error();
    }
    return result;
}

Result<RenderedExportImage> CatalogService::render_for_export(const AssetRecord &asset,
                                                              const std::string_view path,
                                                              const Recipe &recipe,
                                                              const ExportOptions &options,
                                                              const CancellationToken &cancellation,
                                                              const RenderSampleKind sample_kind)
{
    RenderRequest render;
    render.asset = {asset.id, std::string(path), asset.content_fingerprint};
    render.recipe = recipe;
    render.cancellation = cancellation;
    render.correlation_id = asset.id;
    const std::uint32_t source_width = asset.width.value_or(0);
    const std::uint32_t source_height = asset.height.value_or(0);
    std::uint32_t width = source_width;
    std::uint32_t height = source_height;
    fit_export_output_size(source_width, source_height, options.max_edge, options.max_width,
                           options.max_height, width, height);
    std::uint32_t decode_edge = 0;
    if (width != source_width || height != source_height)
    {
        render.output_width = width;
        render.output_height = height;
        decode_edge = std::max(width, height);
    }
    if (is_raster_media_type(asset.media_type))
    {
        auto source = decode_preview_source(asset, path, decode_edge, cancellation,
                                            PreviewLane::kForegroundDevelop);
        if (!source)
        {
            return source.error();
        }
        return engine_->render_to_export_image(render, sample_kind, &source.value());
    }
    if (is_raw_media_type(asset.media_type))
    {
        return engine_->render_to_export_image(render, sample_kind, nullptr);
    }
    return make_error(ErrorCode::kUnsupported, "Asset media type cannot be exported",
                      {{"media_type", asset.media_type}, {"asset_id", asset.id}});
}

Result<const DecodedRaw *> CatalogService::cached_raw_frame(const AssetRecord &asset,
                                                            const std::string_view path,
                                                            const CancellationToken &cancellation,
                                                            const PreviewLane lane)
{
    if (engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    auto &cache_entry = lane == PreviewLane::kBackgroundBrowse ? browse_decoded_raw_ : decoded_raw_;
    if (cache_entry.has_value() && cache_entry->asset_id == asset.id &&
        cache_entry->fingerprint == fingerprint && cache_entry->path == path)
    {
        return &cache_entry->raw;
    }
    auto decoded_frame = engine_->decode_raw_frame(path, cancellation);
    if (!decoded_frame)
    {
        return decoded_frame.error();
    }
    cache_entry = CachedRawFrame{std::string(asset.id), fingerprint, std::string(path),
                                 std::move(decoded_frame).value()};
    if (lane == PreviewLane::kBackgroundBrowse)
    {
        browse_linear_working_.reset();
    }
    else
    {
        for (auto &working : linear_working_)
        {
            working.reset();
        }
        roi_linear_working_.reset();
    }
    return &cache_entry->raw;
}

Result<CatalogService::CachedLinearWorking *>
CatalogService::cached_linear_working(const AssetRecord &asset, const std::string_view path,
                                      const Recipe &recipe, const std::uint32_t width,
                                      const std::uint32_t height, const std::uint32_t max_edge,
                                      const CancellationToken &cancellation, const PreviewLane lane)
{
    if (engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    auto color_fingerprint = engine_->input_color_cache_fingerprint(recipe);
    if (!color_fingerprint)
    {
        return color_fingerprint.error();
    }
    const std::string preprocess_key =
        (is_raw_media_type(asset.media_type) ? raw_preprocess_key(recipe) :
                                               input_color_preprocess_key(recipe)) +
        ":" + color_fingerprint.value();
    const auto matches = [&](const CachedLinearWorking &working)
    {
        return working.asset_id == asset.id && working.fingerprint == fingerprint &&
               working.max_edge == max_edge && working.preprocess_key == preprocess_key &&
               working.buffer.width == width && working.buffer.height == height;
    };
    const auto same_generation = [&](const CachedLinearWorking &working)
    {
        return working.asset_id == asset.id && working.fingerprint == fingerprint &&
               working.preprocess_key == preprocess_key;
    };
    const auto slot_for = [](const std::uint32_t edge) -> std::size_t
    { return edge <= kInteractivePreviewMaxEdge ? 0U : 1U; };
    const auto store_foreground = [&](CachedLinearWorking cached) -> CachedLinearWorking *
    {
        const std::size_t slot = slot_for(cached.max_edge);
        linear_working_[slot] = std::move(cached);
        return &*linear_working_[slot];
    };
    if (lane == PreviewLane::kBackgroundBrowse)
    {
        if (browse_linear_working_.has_value() && matches(*browse_linear_working_))
        {
            return &*browse_linear_working_;
        }
    }
    else
    {
        for (auto &working : linear_working_)
        {
            if (working.has_value() && matches(*working))
            {
                return &*working;
            }
        }
        if (max_edge <= kInteractivePreviewMaxEdge)
        {
            CachedLinearWorking *source = nullptr;
            for (auto &working : linear_working_)
            {
                if (!working.has_value() || !same_generation(*working) ||
                    working->buffer.width < width || working->buffer.height < height)
                {
                    continue;
                }
                if (source == nullptr ||
                    static_cast<std::uint64_t>(working->buffer.width) * working->buffer.height >
                        static_cast<std::uint64_t>(source->buffer.width) * source->buffer.height)
                {
                    source = &*working;
                }
            }
            if (source != nullptr)
            {
                if (source->buffer.width == width && source->buffer.height == height)
                {
                    return store_foreground(CachedLinearWorking{
                        .asset_id = std::string(asset.id),
                        .fingerprint = fingerprint,
                        .max_edge = max_edge,
                        .preprocess_key = preprocess_key,
                        .buffer = source->buffer,
                        .interactive_render_cache = {},
                    });
                }
                auto scaled = engine_->scale_linear_working(source->buffer, width, height,
                                                            asset.width.value_or(0),
                                                            asset.height.value_or(0), cancellation);
                if (!scaled)
                {
                    return scaled.error();
                }
                return store_foreground(CachedLinearWorking{
                    .asset_id = std::string(asset.id),
                    .fingerprint = fingerprint,
                    .max_edge = max_edge,
                    .preprocess_key = preprocess_key,
                    .buffer = std::move(scaled).value(),
                    .interactive_render_cache = {},
                });
            }
        }
    }

    std::uint32_t build_width = width;
    std::uint32_t build_height = height;
    std::uint32_t build_edge = max_edge;
    if (lane == PreviewLane::kForegroundDevelop && max_edge == kInteractivePreviewMaxEdge)
    {
        std::uint32_t settled_width = 0U;
        std::uint32_t settled_height = 0U;
        fit_within_max_edge(asset.width.value_or(0), asset.height.value_or(0),
                            kDefaultPreviewMaxEdge, settled_width, settled_height);
        if (settled_width >= width && settled_height >= height)
        {
            build_width = settled_width;
            build_height = settled_height;
            build_edge = kDefaultPreviewMaxEdge;
        }
    }

    LinearWorkingBuffer buffer;
    if (is_raw_media_type(asset.media_type))
    {
        auto raw = cached_raw_frame(asset, path, cancellation, lane);
        if (!raw)
        {
            return raw.error();
        }
        auto working = engine_->linear_working_from_raw(*raw.value(), recipe, build_width,
                                                        build_height, cancellation);
        if (!working)
        {
            return working.error();
        }
        buffer = std::move(working).value();
    }
    else
    {
        auto source = decode_preview_source(asset, path, build_edge, cancellation, lane);
        if (!source)
        {
            return source.error();
        }
        auto working = engine_->linear_working_from_raster(source.value(), recipe, cancellation);
        if (!working)
        {
            return working.error();
        }
        buffer = std::move(working).value();
    }

    CachedLinearWorking built{
        .asset_id = std::string(asset.id),
        .fingerprint = fingerprint,
        .max_edge = build_edge,
        .preprocess_key = preprocess_key,
        .buffer = std::move(buffer),
        .interactive_render_cache = {},
    };
    if (lane == PreviewLane::kBackgroundBrowse)
    {
        browse_linear_working_ = std::move(built);
        return &*browse_linear_working_;
    }
    auto *stored = store_foreground(std::move(built));
    if (build_edge == max_edge && stored->buffer.width == width && stored->buffer.height == height)
    {
        return stored;
    }
    if (stored->buffer.width == width && stored->buffer.height == height)
    {
        return store_foreground(CachedLinearWorking{
            .asset_id = std::string(asset.id),
            .fingerprint = fingerprint,
            .max_edge = max_edge,
            .preprocess_key = preprocess_key,
            .buffer = stored->buffer,
            .interactive_render_cache = {},
        });
    }
    auto scaled =
        engine_->scale_linear_working(stored->buffer, width, height, asset.width.value_or(0),
                                      asset.height.value_or(0), cancellation);
    if (!scaled)
    {
        return scaled.error();
    }
    return store_foreground(CachedLinearWorking{
        .asset_id = std::string(asset.id),
        .fingerprint = fingerprint,
        .max_edge = max_edge,
        .preprocess_key = preprocess_key,
        .buffer = std::move(scaled).value(),
        .interactive_render_cache = {},
    });
}

Result<RasterBuffer> CatalogService::decode_preview_source(const AssetRecord &asset,
                                                           const std::string_view path,
                                                           const std::uint32_t max_edge,
                                                           const CancellationToken &cancellation,
                                                           const PreviewLane lane)
{
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    auto &cache_entry = lane == PreviewLane::kBackgroundBrowse ? browse_decoded_preview_source_ :
                                                                 decoded_preview_source_;
    const auto tag_untagged_raster_browse = [&](RasterBuffer raster) -> RasterBuffer
    {
        if (lane == PreviewLane::kBackgroundBrowse &&
            raster.color_profile.kind == ColorProfileKind::kMissing)
        {
            raster.color_profile.kind = ColorProfileKind::kBuiltin;
            raster.color_profile.model = ColorModel::kRgb;
            raster.color_profile.identifier = "srgb";
        }
        return raster;
    };
    if (cache_entry.has_value() && cache_entry->asset_id == asset.id &&
        cache_entry->fingerprint == fingerprint && cache_entry->max_edge == max_edge)
    {
        return tag_untagged_raster_browse(cache_entry->raster);
    }

    RasterBuffer raster;
    if (is_raster_media_type(asset.media_type))
    {
        auto decoded = raster_->decode(path, max_edge, cancellation);
        if (!decoded)
        {
            return decoded.error();
        }
        raster.width = decoded.value().width;
        raster.height = decoded.value().height;
        raster.source_width = decoded.value().source_width;
        raster.source_height = decoded.value().source_height;
        raster.srgb = std::move(decoded.value().rgb);
        raster.color_profile = std::move(decoded.value().color_profile);
    }
    else if (is_raw_media_type(asset.media_type))
    {
        if (lane == PreviewLane::kBackgroundBrowse)
        {
            auto companion = adjacent_jpeg(path);
            if (!companion)
            {
                return companion.error();
            }
            if (companion.value())
            {
                auto decoded = raster_->decode(*companion.value(), max_edge, cancellation);
                if (decoded)
                {
                    raster.width = decoded.value().width;
                    raster.height = decoded.value().height;
                    raster.source_width = decoded.value().source_width;
                    raster.source_height = decoded.value().source_height;
                    raster.srgb = std::move(decoded.value().rgb);
                    raster.color_profile = std::move(decoded.value().color_profile);
                    raster = tag_untagged_raster_browse(std::move(raster));
                    cache_entry = DecodedPreviewSource{std::string(asset.id), fingerprint, max_edge,
                                                       std::move(raster)};
                    return cache_entry->raster;
                }
                LOG_INFO(ravo::logger(), "companion JPEG browse decode failed asset={} error={}",
                         asset.id, decoded.error().message);
            }
        }
        auto embedded = engine_->extract_embedded_preview(path, max_edge, cancellation);
        if (embedded)
        {
            auto decoded = raster_->decode_memory(embedded.value().bytes, max_edge, cancellation,
                                                  embedded.value().rotate_quarters);
            if (!decoded)
            {
                return decoded.error();
            }
            raster.width = decoded.value().width;
            raster.height = decoded.value().height;
            raster.source_width = decoded.value().source_width;
            raster.source_height = decoded.value().source_height;
            raster.srgb = std::move(decoded.value().rgb);
            raster.color_profile = std::move(decoded.value().color_profile);
            if (raster.color_profile.kind == ColorProfileKind::kMissing)
            {
                raster.color_profile = embedded.value().color_profile;
            }
        }
        else
        {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            fit_within_max_edge(asset.width.value_or(0), asset.height.value_or(0), max_edge, width,
                                height);
            auto recipe = baseline_recipe_for(asset, std::string(path));
            if (!recipe)
            {
                return recipe.error();
            }
            RenderRequest render;
            render.asset = {asset.id, std::string(path), asset.content_fingerprint};
            render.recipe = std::move(recipe).value();
            render.output_width = width;
            render.output_height = height;
            render.cancellation = cancellation;
            render.correlation_id = asset.id;
            auto rendered = engine_->render_to_image(render, nullptr);
            if (!rendered)
            {
                return rendered.error();
            }
            raster.width = rendered.value().width;
            raster.height = rendered.value().height;
            raster.srgb = std::move(rendered.value().rgb);
            raster.color_profile = std::move(rendered.value().color_profile);
        }
    }
    else
    {
        return make_error(ErrorCode::kUnsupported, "Asset media type cannot be previewed",
                          {{"media_type", asset.media_type}, {"asset_id", asset.id}});
    }

    raster = tag_untagged_raster_browse(std::move(raster));
    cache_entry =
        DecodedPreviewSource{std::string(asset.id), fingerprint, max_edge, std::move(raster)};
    return cache_entry->raster;
}

} // namespace ravo
