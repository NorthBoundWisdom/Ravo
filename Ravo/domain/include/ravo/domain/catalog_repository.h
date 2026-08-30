#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogRepository
{
public:
    virtual ~CatalogRepository() = default;

    [[nodiscard]] virtual Result<CatalogSnapshot> snapshot() const = 0;
    [[nodiscard]] virtual Result<std::vector<AssetRecord>> list_assets() const = 0;
    [[nodiscard]] virtual Result<std::optional<AssetRecord>>
    find_asset_by_id(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::optional<AssetRecord>>
    find_asset_by_uri(std::string_view normalized_uri) const = 0;
    // One transaction: asset row, optional capture row, revision bump.
    // Failure leaves no newly visible asset.
    [[nodiscard]] virtual Result<void> commit_imported_asset(const AssetRecord &asset) = 0;
    // One transaction: refresh the existing asset/capture rows and bump revision.
    // Failure preserves the previous asset, capture values, and revision.
    [[nodiscard]] virtual Result<void> commit_refreshed_asset(const AssetRecord &asset) = 0;
    [[nodiscard]] virtual Result<void> update_asset(const AssetRecord &asset) = 0;
    [[nodiscard]] virtual Result<void> update_review(std::string_view asset_id,
                                                     const ReviewState &review) = 0;
    // One transaction: delete the asset cascade and bump catalog revision.
    // Failure leaves the asset and prior revision visible.
    [[nodiscard]] virtual Result<void> remove_asset(std::string_view asset_id) = 0;
    [[nodiscard]] virtual Result<std::optional<std::string>>
    load_recipe_json(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<void> save_recipe_json(std::string_view asset_id,
                                                        std::int64_t recipe_schema_version,
                                                        std::string_view recipe_json) = 0;
    [[nodiscard]] virtual Result<void> clear_recipe(std::string_view asset_id) = 0;
    // Publishes the current recipe, optional newer-history deletion, the automatic history
    // entry, and the catalog revision as one transaction. A missing recipe_json clears the
    // current recipe while history_json keeps the explicit baseline state. discard_history_after_seq
    // deletes this asset's history rows with seq greater than that value before the optional
    // append. coalesce_history_id replaces that ordinary row only while it remains the latest
    // row for this asset; otherwise the new state appends without overwriting intervening work.
    // The repository must leave the previous recipe and history visible when any part of the
    // transaction fails.
    [[nodiscard]] virtual Result<RecipeCommitResult>
    commit_recipe(std::string_view asset_id, std::int64_t recipe_schema_version,
                  std::optional<std::string_view> recipe_json, std::string_view history_json,
                  RecipeHistoryWrite history_write,
                  std::optional<std::int64_t> discard_history_after_seq,
                  std::optional<std::int64_t> coalesce_history_id) = 0;
    [[nodiscard]] virtual Result<void> replace_asset_tags(std::string_view asset_id,
                                                          const std::vector<std::string> &tags) = 0;
    [[nodiscard]] virtual Result<void>
    upsert_writable_metadata(std::string_view asset_id, const WritableMetadata &metadata) = 0;
    [[nodiscard]] virtual Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::optional<RecipeHistoryEntry>>
    find_recipe_history(std::int64_t history_id) const = 0;
    [[nodiscard]] virtual Result<RecipeHistoryEntry>
    append_recipe_history(std::string_view asset_id, std::string_view kind,
                          std::optional<std::string_view> label, std::string_view recipe_json) = 0;
    [[nodiscard]] virtual Result<void>
    update_recipe_history_label(std::int64_t history_id, std::string_view label) = 0;
    [[nodiscard]] virtual Result<std::optional<PreviewRecord>>
    find_preview(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::vector<PreviewRecord>> list_previews() const = 0;
    [[nodiscard]] virtual Result<void> upsert_preview(const PreviewRecord &preview) = 0;
    [[nodiscard]] virtual Result<std::int64_t> bump_revision() = 0;
    virtual Result<void> close() = 0;
};

} // namespace ravo
