#include "ravo/services/catalog_service.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "catalog_internal.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
CatalogService::CatalogService(const EngineFacade &engine,
                               std::unique_ptr<CatalogRepository> repository,
                               std::unique_ptr<RasterDecoder> raster,
                               std::unique_ptr<PreviewCache> cache)
    : engine_(&engine)
    , repository_(std::move(repository))
    , raster_(std::move(raster))
    , cache_(std::move(cache))
{
}

CatalogService::~CatalogService()
{
    static_cast<void>(close());
}

Result<void> CatalogService::close()
{
    if (repository_ == nullptr)
    {
        return {};
    }
    const auto closed = repository_->close();
    repository_.reset();
    raster_.reset();
    cache_.reset();
    engine_ = nullptr;
    decoded_preview_source_.reset();
    decoded_raw_.reset();
    linear_working_.reset();
    return closed;
}

Result<CatalogSnapshot> CatalogService::snapshot() const
{
    if (repository_ == nullptr || cache_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
    {
        return snapshot.error();
    }
    snapshot.value().cache_root = cache_->root();
    return snapshot;
}

Result<std::vector<AssetRecord>> CatalogService::list_assets() const
{
    return list_assets(LibraryQuery{});
}

Result<std::vector<AssetRecord>> CatalogService::list_assets(const LibraryQuery &query) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto listed = repository_->list_assets();
    if (!listed)
    {
        return listed.error();
    }
    return filter_and_sort_assets(std::move(listed).value(), query);
}

Result<std::vector<PreviewRecord>> CatalogService::list_previews() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_previews();
}

Result<std::vector<FolderRecord>> CatalogService::list_folders() const
{
    auto listed = list_assets();
    if (!listed)
    {
        return listed.error();
    }
    return library_folders(listed.value());
}

Result<AssetRecord> CatalogService::set_rating(const std::string_view asset_id, const int rating)
{
    auto valid = validate_rating(rating);
    if (!valid)
    {
        return valid.error();
    }
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.rating = rating;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_color_label(const std::string_view asset_id,
                                                    const ColorLabel label)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.color_label = label;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_rejected(const std::string_view asset_id,
                                                 const bool rejected)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.rejected = rejected;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    return *asset.value();
}

Result<void> CatalogService::remove_from_catalog(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto removed = repository_->remove_asset(asset_id);
    if (!removed)
    {
        return removed.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    if (cache_ != nullptr)
    {
        const auto removed_cache = cache_->remove_for_asset(asset_id);
        if (!removed_cache)
        {
            return removed_cache.error();
        }
    }
    return {};
}

Result<void> CatalogService::remove_original_and_catalog(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    const auto path = std::filesystem::path(
        std::u8string(location.value().path.begin(), location.value().path.end()));
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error)
    {
        return make_error(ErrorCode::kIo, "Unable to inspect original file",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", exists_error.message()}});
    }
    if (!exists)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", location.value().path}, {"asset_id", std::string(asset_id)}});
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(path, remove_error) || remove_error)
    {
        return make_error(ErrorCode::kIo, "Unable to delete original file",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", remove_error.message()}});
    }
    return remove_from_catalog(asset_id);
}

Result<bool> CatalogService::asset_has_edits(const std::string_view asset_id) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return asset.value()->has_edits;
}

Result<Recipe> CatalogService::load_baseline_recipe(const std::string_view asset_id) const
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    return baseline_recipe_for(*asset.value(), location.value().path);
}

Result<Recipe> CatalogService::load_recipe(const std::string_view asset_id) const
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto baseline = load_baseline_recipe(asset_id);
    if (!baseline)
    {
        return baseline.error();
    }
    auto stored = repository_->load_recipe_json(asset_id);
    if (!stored)
    {
        return stored.error();
    }
    if (!stored.value())
    {
        return baseline;
    }
    auto parsed = parse_recipe_json(*stored.value());
    if (!parsed)
    {
        return parsed.error();
    }
    parsed.value().asset = baseline.value().asset;
    auto valid = engine_->validate(parsed.value());
    if (!valid)
    {
        return valid.error();
    }
    return parsed;
}

Result<AssetRecord> CatalogService::save_recipe(const std::string_view asset_id,
                                                const Recipe &recipe)
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    Recipe stored = recipe;
    stored.asset = {asset.value()->id, location.value().path, asset.value()->content_fingerprint};
    auto valid = engine_->validate(stored);
    if (!valid)
    {
        return valid.error();
    }
    auto params = develop_from_recipe(stored);
    if (!params)
    {
        return params.error();
    }
    std::optional<std::string> recipe_json;
    if (matches_develop_baseline(*asset.value(), params.value()))
    {
        recipe_json.reset();
    }
    else
    {
        auto json = serialize_recipe(stored);
        if (!json)
        {
            return json.error();
        }
        recipe_json = std::move(json).value();
    }
    const std::optional<std::string_view> recipe_json_view =
        recipe_json ? std::optional<std::string_view>{*recipe_json} : std::nullopt;
    // Keep an owned, non-null empty string for the explicit baseline history entry. A default
    // string_view has a null data pointer, which Qt Sql correctly binds as SQL NULL.
    const std::string history_json = recipe_json.value_or(std::string{});
    const auto committed =
        repository_->commit_recipe(asset_id, stored.schema_version, recipe_json_view, history_json);
    if (!committed)
    {
        return committed.error();
    }
    asset.value()->has_edits = recipe_json.has_value();
    return *asset.value();
}

Result<AssetRecord> CatalogService::save_develop(const std::string_view asset_id,
                                                 const DevelopParams &params)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    DevelopParams stored = params;
    if (stored.lens_mode == kLensModeLookup)
    {
        if (stored.lens_make.empty() && asset.value()->capture.camera_make)
        {
            stored.lens_make = *asset.value()->capture.camera_make;
        }
        if (stored.lens_model.empty() && asset.value()->capture.camera_model)
        {
            stored.lens_model = *asset.value()->capture.camera_model;
        }
        if (stored.lens_focal_mm <= 0.0 && asset.value()->capture.focal_length_mm)
        {
            stored.lens_focal_mm = *asset.value()->capture.focal_length_mm;
        }
    }
    auto recipe = recipe_from_develop(
        {asset.value()->id, location.value().path, asset.value()->content_fingerprint}, stored);
    if (!recipe)
    {
        return recipe.error();
    }
    return save_recipe(asset_id, recipe.value());
}

Result<AssetRecord> CatalogService::reset_recipe(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return save_develop(asset_id, baseline_develop_for(*asset.value()));
}

Result<AssetRecord> CatalogService::set_tags(const std::string_view asset_id,
                                             const std::vector<std::string> &tags)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    std::vector<std::string> normalized;
    normalized.reserve(tags.size());
    for (const auto &tag : tags)
    {
        auto parsed = normalize_tag_name(tag);
        if (!parsed)
        {
            return parsed.error();
        }
        if (std::find(normalized.begin(), normalized.end(), parsed.value()) == normalized.end())
        {
            normalized.push_back(std::move(parsed).value());
        }
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto saved = repository_->replace_asset_tags(asset_id, normalized);
    if (!saved)
    {
        return saved.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->tags = std::move(normalized);
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_writable_metadata(const std::string_view asset_id,
                                                          const WritableMetadata &metadata)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    const auto check_field = [](const std::string_view name,
                                const std::optional<std::string> &value) -> Result<void>
    {
        if (!value)
        {
            return {};
        }
        return validate_metadata_field(name, *value);
    };
    auto title = check_field("title", metadata.title);
    if (!title)
    {
        return title.error();
    }
    auto description = check_field("description", metadata.description);
    if (!description)
    {
        return description.error();
    }
    auto creator = check_field("creator", metadata.creator);
    if (!creator)
    {
        return creator.error();
    }
    auto copyright = check_field("copyright", metadata.copyright);
    if (!copyright)
    {
        return copyright.error();
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto saved = repository_->upsert_writable_metadata(asset_id, metadata);
    if (!saved)
    {
        return saved.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->metadata = metadata;
    return *asset.value();
}

Result<std::vector<RecipeHistoryEntry>>
CatalogService::list_recipe_history(const std::string_view asset_id) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return repository_->list_recipe_history(asset_id);
}

Result<AssetRecord> CatalogService::create_recipe_snapshot(const std::string_view asset_id,
                                                           const std::string_view label)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto trimmed = normalize_tag_name(label);
    if (!trimmed)
    {
        return trimmed.error();
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto json = repository_->load_recipe_json(asset_id);
    if (!json)
    {
        return json.error();
    }
    const std::string recipe_json = json.value().value_or(std::string{});
    auto recorded = repository_->append_recipe_history(
        asset_id, kRecipeHistoryKindSnapshot, std::string_view{trimmed.value()}, recipe_json);
    if (!recorded)
    {
        return recorded.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::restore_recipe_history(const std::string_view asset_id,
                                                           const std::int64_t history_id)
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto entry = repository_->find_recipe_history(history_id);
    if (!entry)
    {
        return entry.error();
    }
    if (!entry.value() || entry.value()->asset_id != asset_id)
    {
        return make_error(ErrorCode::kNotFound, "Recipe history entry does not exist",
                          {{"history_id", std::to_string(history_id)}});
    }
    if (entry.value()->recipe_json.empty())
    {
        return reset_recipe(asset_id);
    }
    auto parsed = parse_recipe_json(entry.value()->recipe_json);
    if (!parsed)
    {
        return parsed.error();
    }
    return save_recipe(asset_id, parsed.value());
}

Result<ImportItemResult> CatalogService::import_one(const std::string_view path,
                                                    const CancellationToken &cancellation)
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
            if (asset.media_type == kMediaTypeJpeg)
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
        else if (raster.error().code == ErrorCode::kUnsupported)
        {
            const auto format = raster.error().context.find("format");
            if (format != raster.error().context.end() && format->second == "jpeg")
            {
                return unsupported_item(location.value().path, raster.error());
            }
            const auto reason = raster.error().context.find("reason");
            if (format != raster.error().context.end() && format->second == "qoi" &&
                reason != raster.error().context.end() && reason->second == "unsupported_qoi_input")
            {
                return unsupported_item(location.value().path, raster.error());
            }
            if (format != raster.error().context.end() && format->second == "rgbe" &&
                reason != raster.error().context.end() &&
                reason->second == "unsupported_rgbe_input")
            {
                return unsupported_item(location.value().path, raster.error());
            }
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
            return failed_item(location.value().path, raster.error());
        }
    }

    const auto inserted = repository_->insert_asset(asset);
    if (!inserted)
    {
        return failed_item(location.value().path, inserted.error());
    }
    if (asset.capture.camera_make || asset.capture.camera_model || asset.capture.iso ||
        asset.capture.aperture || asset.capture.focal_length_mm || asset.capture.shutter_s ||
        asset.capture.captured_unix_s)
    {
        const auto captured = repository_->upsert_capture_metadata(asset.id, asset.capture);
        if (!captured)
        {
            return failed_item(location.value().path, captured.error());
        }
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return failed_item(location.value().path, revision.error());
    }

    if (validated_raster)
    {
        RasterBuffer raster;
        raster.width = validated_raster->width;
        raster.height = validated_raster->height;
        raster.srgb = std::move(validated_raster->rgb);
        raster.color_profile = std::move(validated_raster->color_profile);
        decoded_preview_source_ =
            DecodedPreviewSource{asset.id, asset.content_fingerprint.value_or("none"),
                                 kThumbnailMaxEdge, std::move(raster)};
    }

    PreviewRequest imported_preview;
    imported_preview.max_edge = kThumbnailMaxEdge;
    imported_preview.prefer_embedded_preview = is_raw_media_type(asset.media_type);
    imported_preview.cancellation = cancellation;
    Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Preview was not generated");
    if (embedded_preview)
    {
        preview = persist_embedded_browse_preview(asset, *embedded_preview, kThumbnailMaxEdge,
                                                  cancellation);
    }
    if (!preview)
    {
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

    ImportItemResult result;
    result.status = ImportItemStatus::kImported;
    result.input_path = location.value().path;
    result.asset = asset;
    if (preview)
    {
        result.preview_cache_path = preview.value().cache_path;
    }
    return result;
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
    auto rendered = render_for_export(*asset.value(), location.value().path, edit_recipe,
                                      request.max_edge, request.cancellation);
    if (!rendered)
    {
        return rendered.error();
    }
    auto encoded = raster_->encode(rendered.value().width, rendered.value().height,
                                   rendered.value().rgb, rendered.value().color_profile,
                                   request.format, request.jpeg_options, request.cancellation);
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
    result.width = rendered.value().width;
    result.height = rendered.value().height;
    result.bytes_written = encoded.value().size();
    LOG_INFO(ravo::logger(), "export asset={} format={} output={} {}x{} bytes={}", request.asset_id,
             export_format_name(request.format), output.value().path, result.width, result.height,
             result.bytes_written);
    return result;
}

} // namespace ravo
