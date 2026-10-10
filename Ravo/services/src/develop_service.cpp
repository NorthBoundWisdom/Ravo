#include "ravo/services/develop_service.h"

#include <filesystem>
#include <utility>
#include <set>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/uri.h"
#include "ravo/services/recovery_service.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/local_adjustment.h"

namespace ravo
{
using namespace catalog_service_internal;

DevelopService::DevelopService(const std::unique_ptr<CatalogRepository> &repository,
                               const EngineFacade *const &engine,
                               RecoveryService &recovery_service) noexcept
    : repository_(repository)
    , engine_(engine)
    , recovery_service_(recovery_service)
{
}

Result<bool> DevelopService::asset_has_edits(const std::string_view asset_id) const
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

Result<Recipe> DevelopService::load_baseline_recipe(const std::string_view asset_id) const
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
    if (is_video_media_type(asset.value()->media_type))
        return make_error(ErrorCode::kUnsupported, "Video does not support photo Develop",
                          {{"reason", "video_photo_operation_unsupported"}});
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    return baseline_recipe_for(*asset.value(), location.value().path);
}

Result<Recipe> DevelopService::load_recipe(const std::string_view asset_id) const
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
    auto merged = merge_missing_raw_baseline_operations(baseline.value(), parsed.value());
    if (!merged)
    {
        return merged.error();
    }
    auto valid = engine_->validate(parsed.value());
    if (!valid)
    {
        return valid.error();
    }
    return parsed;
}

Result<AssetRecord> DevelopService::save_recipe(const std::string_view asset_id,
                                                const Recipe &recipe,
                                                const RecipeSaveOptions options)
{
    auto saved = save_recipe_with_history(asset_id, recipe, options);
    if (!saved)
    {
        return saved.error();
    }
    return std::move(saved).value().asset;
}

Result<RecipeSaveResult> DevelopService::save_recipe_with_history(const std::string_view asset_id,
                                                                  const Recipe &recipe,
                                                                  const RecipeSaveOptions options)
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    std::optional<std::int64_t> expected_generation;
    if (options.expected_base)
    {
        if (!options.expected_source)
            return make_error(ErrorCode::kInvalidArgument, "Photo edit has no observed source",
                              {{"reason", "missing_recipe_edit_source"}});
        auto before = repository_->recovery_state(asset_id);
        if (!before)
            return before.error();
        auto current = load_recipe(asset_id);
        if (!current)
            return current.error();
        auto params = develop_from_recipe(current.value());
        if (!params)
            return params.error();
        if (auto promoted = promote_legacy_local_adjustments(params.value()); !promoted)
            return promoted.error();
        auto expected_params = *options.expected_base;
        if (auto promoted = promote_legacy_local_adjustments(expected_params); !promoted)
            return promoted.error();
        auto expected_path = normalize_local_input(options.expected_source->input_uri);
        auto current_path = normalize_local_input(current.value().asset.input_uri);
        if (!expected_path)
            return expected_path.error();
        if (!current_path)
            return current_path.error();
        auto expected_recipe = recipe_from_develop(current.value().asset, expected_params);
        auto current_recipe = recipe_from_develop(current.value().asset, params.value());
        if (!expected_recipe)
            return expected_recipe.error();
        if (!current_recipe)
            return current_recipe.error();
        auto expected_json = serialize_recipe(expected_recipe.value());
        auto current_json = serialize_recipe(current_recipe.value());
        if (!expected_json)
            return expected_json.error();
        if (!current_json)
            return current_json.error();
        if (options.expected_source->id != asset_id ||
            expected_path.value().uri != current_path.value().uri ||
            options.expected_source->content_hash != current.value().asset.content_hash ||
            expected_json.value() != current_json.value())
            return make_error(
                ErrorCode::kConflict, "Photo changed since its recipe was loaded",
                {{"reason", "stale_recipe_state"}, {"asset_id", std::string(asset_id)}});
        auto after = repository_->recovery_state(asset_id);
        if (!after)
            return after.error();
        if (before.value().generation != after.value().generation)
            return make_error(
                ErrorCode::kConflict, "Photo changed while checking its recipe",
                {{"reason", "stale_recipe_state"}, {"asset_id", std::string(asset_id)}});
        expected_generation = after.value().generation;
    }
    if (options.expected_revision)
    {
        auto current = repository_->snapshot();
        if (!current)
            return current.error();
        if (*options.expected_revision != current.value().revision)
        {
            return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                              {{"reason", "stale_catalog_revision"},
                               {"expected_revision", std::to_string(*options.expected_revision)},
                               {"revision", std::to_string(current.value().revision)}});
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
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    if (is_video_media_type(asset.value()->media_type))
        return make_error(ErrorCode::kUnsupported, "Video does not support photo Develop",
                          {{"reason", "video_photo_operation_unsupported"}});
    Recipe stored = recipe;
    stored.asset = {asset.value()->id, location.value().path, asset.value()->content_fingerprint};
    auto valid = engine_->validate(stored);
    if (!valid)
    {
        return valid.error();
    }
    auto lut_fingerprint = engine_->lut3d_cache_fingerprint(stored);
    if (!lut_fingerprint)
    {
        return lut_fingerprint.error();
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
    const auto committed = repository_->commit_recipe(
        asset_id, stored.schema_version, recipe_json_view, history_json, options.history_write,
        options.discard_history_after_seq, options.coalesce_history_id,
        RecipeCommitPrecondition{options.expected_revision, expected_generation,
                                 options.expected_history_head});
    if (!committed)
    {
        return committed.error();
    }
    asset.value()->has_edits = recipe_json.has_value();
    if (!options.defer_recovery_publication)
    {
        auto recovered = recovery_service_.synchronize_committed_change(asset_id);
        if (!recovered)
        {
            return recovered.error();
        }
    }
    return RecipeSaveResult{*asset.value(), committed.value().revision,
                            committed.value().history_id, committed.value().history_head};
}

Result<AssetRecord> DevelopService::save_develop(const std::string_view asset_id,
                                                 const DevelopParams &params,
                                                 const RecipeSaveOptions options)
{
    auto saved = save_develop_with_history(asset_id, params, options);
    if (!saved)
    {
        return saved.error();
    }
    return std::move(saved).value().asset;
}

Result<RecipeSaveResult> DevelopService::save_develop_with_history(const std::string_view asset_id,
                                                                   const DevelopParams &params,
                                                                   const RecipeSaveOptions options)
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
    return save_recipe_with_history(asset_id, recipe.value(), options);
}

Result<AssetRecord> DevelopService::reset_recipe(const std::string_view asset_id)
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

Result<std::vector<RecipeHistoryEntry>>
DevelopService::list_recipe_history(const std::string_view asset_id) const
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

Result<AssetRecord> DevelopService::create_recipe_snapshot(const std::string_view asset_id,
                                                           const std::string_view label)
{
    return create_recipe_snapshot_impl(asset_id, label, nullptr);
}

Result<AssetRecord> DevelopService::create_recipe_snapshot(const std::string_view asset_id,
                                                           const Recipe &recipe,
                                                           const std::string_view label)
{
    return create_recipe_snapshot_impl(asset_id, label, &recipe);
}

Result<AssetRecord> DevelopService::create_recipe_snapshot_impl(const std::string_view asset_id,
                                                                const std::string_view label,
                                                                const Recipe *recipe)
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
    std::string recipe_json;
    if (recipe != nullptr)
    {
        if (engine_ == nullptr)
            return make_error(ErrorCode::kIo, "Engine is unavailable");
        auto location = normalize_local_input(recipe->asset.input_uri);
        if (!location)
            return location.error();
        if (recipe->asset.id != asset_id ||
            recipe->asset.content_hash != asset.value()->content_fingerprint ||
            location.value().uri != asset.value()->normalized_uri)
            return make_error(ErrorCode::kConflict, "Snapshot recipe belongs to a different source",
                              {{"reason", "snapshot_asset_mismatch"}});
        auto valid = engine_->validate(*recipe);
        if (!valid)
            return valid.error();
        auto json = serialize_recipe(*recipe);
        if (!json)
            return json.error();
        recipe_json = std::move(json).value();
    }
    else
    {
        auto json = repository_->load_recipe_json(asset_id);
        if (!json)
            return json.error();
        recipe_json = json.value().value_or(std::string{});
    }
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
    auto recovered = recovery_service_.synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> DevelopService::rename_recipe_snapshot(const std::string_view asset_id,
                                                           const std::int64_t history_id,
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
    auto entry = repository_->find_recipe_history(history_id);
    if (!entry)
    {
        return entry.error();
    }
    if (!entry.value() || entry.value()->asset_id != asset_id)
    {
        return make_error(ErrorCode::kNotFound, "Recipe snapshot does not exist",
                          {{"history_id", std::to_string(history_id)}});
    }
    if (entry.value()->kind != kRecipeHistoryKindSnapshot)
    {
        return make_error(ErrorCode::kValidation, "Only snapshots can be renamed",
                          {{"kind", entry.value()->kind}});
    }
    auto updated = repository_->update_recipe_history_label(history_id, trimmed.value());
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    auto recovered = recovery_service_.synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> DevelopService::restore_recipe_history(const std::string_view asset_id,
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

Result<std::array<double, 4>>
DevelopService::sample_white_balance(const std::string_view asset_id,
                                     const WhiteBalancePickRequest &request,
                                     const CancellationToken &cancellation)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
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
    if (!is_raw_media_type(asset.value()->media_type))
    {
        return make_error(ErrorCode::kUnsupported,
                          "White-balance pick requires a Bayer RAW original",
                          {{"media_type", asset.value()->media_type}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    auto decoded = engine_->decode_raw_frame(location.value().path, cancellation);
    if (!decoded)
    {
        return decoded.error();
    }
    return engine_->sample_white_balance(decoded.value(), request);
}

} // namespace ravo
