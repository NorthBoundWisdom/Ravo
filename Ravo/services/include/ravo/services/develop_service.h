#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

class CatalogService;

// Narrow DevelopService capability face over CatalogService.
class DevelopService
{
public:
    explicit DevelopService(CatalogService &catalog) noexcept;

    DevelopService(const DevelopService &) = delete;
    DevelopService &operator=(const DevelopService &) = delete;
    DevelopService(DevelopService &&) noexcept = default;
    DevelopService &operator=(DevelopService &&) noexcept = default;

    [[nodiscard]] Result<bool> asset_has_edits(std::string_view asset_id) const;
    [[nodiscard]] Result<Recipe> load_recipe(std::string_view asset_id) const;
    [[nodiscard]] Result<Recipe> load_baseline_recipe(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> save_recipe(std::string_view asset_id, const Recipe &recipe,
                                                  RecipeSaveOptions options = {});
    [[nodiscard]] Result<AssetRecord> save_develop(std::string_view asset_id,
                                                   const DevelopParams &develop,
                                                   RecipeSaveOptions options = {});
    [[nodiscard]] Result<RecipeSaveResult> save_recipe_with_history(std::string_view asset_id,
                                                                    const Recipe &recipe,
                                                                    RecipeSaveOptions options = {});
    [[nodiscard]] Result<RecipeSaveResult>
    save_develop_with_history(std::string_view asset_id, const DevelopParams &develop,
                              RecipeSaveOptions options = {});
    [[nodiscard]] Result<DevelopApplyResult>
    apply_develop_selection(const DevelopApplyRequest &request,
                            const DevelopApplyProgressCallback &progress = {});
    [[nodiscard]] Result<AssetRecord> reset_recipe(std::string_view asset_id);
    [[nodiscard]] Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> create_recipe_snapshot(std::string_view asset_id,
                                                             std::string_view label);
    [[nodiscard]] Result<AssetRecord> rename_recipe_snapshot(std::string_view asset_id,
                                                             std::int64_t history_id,
                                                             std::string_view label);
    [[nodiscard]] Result<AssetRecord> restore_recipe_history(std::string_view asset_id,
                                                             std::int64_t history_id);

private:
    CatalogService *catalog_ = nullptr;
};

} // namespace ravo
