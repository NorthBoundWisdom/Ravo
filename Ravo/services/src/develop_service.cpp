#include "ravo/services/develop_service.h"

#include "ravo/services/catalog_service.h"

namespace ravo
{

DevelopService::DevelopService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<bool> DevelopService::asset_has_edits(const std::string_view asset_id) const
{
    return catalog_->asset_has_edits(asset_id);
}

Result<Recipe> DevelopService::load_recipe(const std::string_view asset_id) const
{
    return catalog_->load_recipe(asset_id);
}

Result<Recipe> DevelopService::load_baseline_recipe(const std::string_view asset_id) const
{
    return catalog_->load_baseline_recipe(asset_id);
}

Result<AssetRecord> DevelopService::save_recipe(const std::string_view asset_id,
                                                const Recipe &recipe, RecipeSaveOptions options)
{
    return catalog_->save_recipe(asset_id, recipe, std::move(options));
}

Result<AssetRecord> DevelopService::save_develop(const std::string_view asset_id,
                                                 const DevelopParams &develop,
                                                 RecipeSaveOptions options)
{
    return catalog_->save_develop(asset_id, develop, std::move(options));
}

Result<RecipeSaveResult> DevelopService::save_recipe_with_history(const std::string_view asset_id,
                                                                  const Recipe &recipe,
                                                                  RecipeSaveOptions options)
{
    return catalog_->save_recipe_with_history(asset_id, recipe, std::move(options));
}

Result<RecipeSaveResult> DevelopService::save_develop_with_history(const std::string_view asset_id,
                                                                   const DevelopParams &develop,
                                                                   RecipeSaveOptions options)
{
    return catalog_->save_develop_with_history(asset_id, develop, std::move(options));
}

Result<DevelopApplyResult>
DevelopService::apply_develop_selection(const DevelopApplyRequest &request,
                                        const DevelopApplyProgressCallback &progress)
{
    return catalog_->apply_develop_selection(request, progress);
}

Result<AssetRecord> DevelopService::reset_recipe(const std::string_view asset_id)
{
    return catalog_->reset_recipe(asset_id);
}

Result<std::vector<RecipeHistoryEntry>>
DevelopService::list_recipe_history(const std::string_view asset_id) const
{
    return catalog_->list_recipe_history(asset_id);
}

Result<AssetRecord> DevelopService::create_recipe_snapshot(const std::string_view asset_id,
                                                           const std::string_view label)
{
    return catalog_->create_recipe_snapshot(asset_id, label);
}

Result<AssetRecord> DevelopService::rename_recipe_snapshot(const std::string_view asset_id,
                                                           const std::int64_t history_id,
                                                           const std::string_view label)
{
    return catalog_->rename_recipe_snapshot(asset_id, history_id, label);
}

Result<AssetRecord> DevelopService::restore_recipe_history(const std::string_view asset_id,
                                                           const std::int64_t history_id)
{
    return catalog_->restore_recipe_history(asset_id, history_id);
}

} // namespace ravo
