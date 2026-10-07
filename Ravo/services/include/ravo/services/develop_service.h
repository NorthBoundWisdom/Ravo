#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/develop_types.h"
#include "ravo/engine/engine.h"

namespace ravo
{

class CatalogService;
class CatalogRepository;
class EngineFacade;
class RecoveryService;

class DevelopService
{
public:
    DevelopService(const DevelopService &) = delete;
    DevelopService &operator=(const DevelopService &) = delete;
    DevelopService(DevelopService &&) = delete;
    DevelopService &operator=(DevelopService &&) = delete;

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

    [[nodiscard]] Result<std::array<double, 4>>
    sample_white_balance(std::string_view asset_id, const WhiteBalancePickRequest &request,
                         const CancellationToken &cancellation);

private:
    friend class CatalogService;
    // Borrowed owner slots stay valid until this capability is destroyed. The
    // composition owner is immovable; reset slots make post-close calls fail.
    DevelopService(const std::unique_ptr<CatalogRepository> &repository,
                   const EngineFacade *const &engine, RecoveryService &recovery_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const EngineFacade *const &engine_;
    RecoveryService &recovery_service_;
};

} // namespace ravo
