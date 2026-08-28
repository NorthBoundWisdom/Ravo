#include "ravo/services/catalog_service.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

#include "catalog_internal.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
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
    auto encoded = engine_->encode_png(rendered.value());
    if (!encoded)
    {
        return encoded.error();
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

    const auto source_width = asset.width.value_or(0);
    const auto source_height = asset.height.value_or(0);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(source_width, source_height, request.max_edge, width, height);
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    const bool embedded_browse =
        request.prefer_embedded_preview && request.persist_preview_record &&
        is_raw_media_type(working.media_type) && !live_develop.has_value() &&
        !request.ignore_edits && !request.ignore_crop && !request.ignore_straighten &&
        !working.has_edits;
    if (embedded_browse && original_exists)
    {
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
        return fnv1a64_hex(serialized.value() + "|" + color_fingerprint.value() + "|" +
                           output_fingerprint.value());
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
    const bool interactive =
        !request.persist_preview_record || request.overlay_mask_id.has_value();
    const auto cache_key = make_preview_cache_key(asset.id, width, height, fingerprint,
                                                  interactive ? "interactive" : edit_digest);

    PreviewResult result;
    result.asset_id = asset.id;
    result.request_revision = request.request_revision;
    result.cache_key = cache_key;
    result.original_missing = !original_exists;

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

    if (!original_exists)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", location.value().path}, {"asset_id", asset.id}});
    }

    const auto render_started = std::chrono::steady_clock::now();
    RenderedImage rendered;
    if (is_raw_media_type(working.media_type))
    {
        auto linear = cached_linear_working(working, location.value().path, edit_recipe, width,
                                            height, request.max_edge, request.cancellation);
        if (!linear)
        {
            return linear.error();
        }
        Recipe rgb_recipe = edit_recipe;
        disable_raw_preprocess(rgb_recipe);
        auto applied = engine_->render_linear_working(*linear.value(), rgb_recipe,
                                                      request.cancellation, request.overlay_mask_id);
        if (!applied)
        {
            return applied.error();
        }
        rendered = std::move(applied).value();
    }
    else
    {
        auto linear = cached_linear_working(working, location.value().path, edit_recipe, width,
                                            height, request.max_edge, request.cancellation);
        if (!linear)
        {
            return linear.error();
        }
        auto applied = engine_->render_linear_working(*linear.value(), edit_recipe,
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
    LOG_INFO(ravo::logger(), "preview render asset={} {}x{} interactive={} {}ms", asset.id,
             rendered.width, rendered.height, interactive, render_ms);

    result.width = rendered.width;
    result.height = rendered.height;
    if (interactive)
    {
        result.rgb = std::move(rendered.rgb);
        result.color_profile = std::move(rendered.color_profile);
        result.mask_alpha = std::move(rendered.mask_alpha);
        return result;
    }

    auto encoded = engine_->encode_png(rendered);
    if (!encoded)
    {
        return encoded.error();
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
                                                              const std::uint32_t max_edge,
                                                              const CancellationToken &cancellation,
                                                              const RenderSampleKind sample_kind)
{
    RenderRequest render;
    render.asset = {asset.id, std::string(path), asset.content_fingerprint};
    render.recipe = recipe;
    render.cancellation = cancellation;
    render.correlation_id = asset.id;
    if (max_edge > 0)
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        fit_within_max_edge(asset.width.value_or(0), asset.height.value_or(0), max_edge, width,
                            height);
        render.output_width = width;
        render.output_height = height;
    }
    if (is_raster_media_type(asset.media_type))
    {
        auto source = decode_preview_source(asset, path, max_edge, cancellation);
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
                                                            const CancellationToken &cancellation)
{
    if (engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    if (decoded_raw_.has_value() && decoded_raw_->asset_id == asset.id &&
        decoded_raw_->fingerprint == fingerprint && decoded_raw_->path == path)
    {
        return &decoded_raw_->raw;
    }
    auto decoded = engine_->decode_raw_frame(path, cancellation);
    if (!decoded)
    {
        return decoded.error();
    }
    decoded_raw_ = CachedRawFrame{std::string(asset.id), fingerprint, std::string(path),
                                  std::move(decoded).value()};
    linear_working_.reset();
    return &decoded_raw_->raw;
}

Result<const LinearWorkingBuffer *>
CatalogService::cached_linear_working(const AssetRecord &asset, const std::string_view path,
                                      const Recipe &recipe, const std::uint32_t width,
                                      const std::uint32_t height, const std::uint32_t max_edge,
                                      const CancellationToken &cancellation)
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
    if (linear_working_.has_value() && linear_working_->asset_id == asset.id &&
        linear_working_->fingerprint == fingerprint && linear_working_->max_edge == max_edge &&
        linear_working_->preprocess_key == preprocess_key &&
        linear_working_->buffer.width == width && linear_working_->buffer.height == height)
    {
        return &linear_working_->buffer;
    }

    LinearWorkingBuffer buffer;
    if (is_raw_media_type(asset.media_type))
    {
        auto raw = cached_raw_frame(asset, path, cancellation);
        if (!raw)
        {
            return raw.error();
        }
        auto working =
            engine_->linear_working_from_raw(*raw.value(), recipe, width, height, cancellation);
        if (!working)
        {
            return working.error();
        }
        buffer = std::move(working).value();
    }
    else
    {
        auto source = decode_preview_source(asset, path, max_edge, cancellation);
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

    linear_working_ = CachedLinearWorking{std::string(asset.id), fingerprint, max_edge,
                                          preprocess_key, std::move(buffer)};
    return &linear_working_->buffer;
}

Result<RasterBuffer> CatalogService::decode_preview_source(const AssetRecord &asset,
                                                           const std::string_view path,
                                                           const std::uint32_t max_edge,
                                                           const CancellationToken &cancellation)
{
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    if (decoded_preview_source_.has_value() && decoded_preview_source_->asset_id == asset.id &&
        decoded_preview_source_->fingerprint == fingerprint &&
        decoded_preview_source_->max_edge == max_edge)
    {
        return decoded_preview_source_->raster;
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
            RenderRequest render;
            render.asset = {asset.id, std::string(path), asset.content_fingerprint};
            render.recipe = identity_recipe_for(asset, std::string(path));
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

    decoded_preview_source_ =
        DecodedPreviewSource{std::string(asset.id), fingerprint, max_edge, std::move(raster)};
    return decoded_preview_source_->raster;
}

} // namespace ravo
