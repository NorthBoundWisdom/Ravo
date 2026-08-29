#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ravo/domain/catalog_repository.h"

namespace ravo
{

namespace testing
{
class SqliteCatalogTestControl;
}

class SqliteCatalogRepository final : public CatalogRepository
{
public:
    static Result<std::unique_ptr<SqliteCatalogRepository>> create(std::string_view database_path);
    static Result<std::unique_ptr<SqliteCatalogRepository>> open(std::string_view database_path);

    SqliteCatalogRepository(const SqliteCatalogRepository &) = delete;
    SqliteCatalogRepository &operator=(const SqliteCatalogRepository &) = delete;
    ~SqliteCatalogRepository() override;

    [[nodiscard]] Result<CatalogSnapshot> snapshot() const override;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets() const override;
    [[nodiscard]] Result<std::optional<AssetRecord>>
    find_asset_by_id(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::optional<AssetRecord>>
    find_asset_by_uri(std::string_view normalized_uri) const override;
    [[nodiscard]] Result<void> commit_imported_asset(const AssetRecord &asset) override;
    [[nodiscard]] Result<void> commit_refreshed_asset(const AssetRecord &asset) override;
    [[nodiscard]] Result<void> update_asset(const AssetRecord &asset) override;
    [[nodiscard]] Result<void> update_review(std::string_view asset_id,
                                             const ReviewState &review) override;
    [[nodiscard]] Result<void> remove_asset(std::string_view asset_id) override;
    [[nodiscard]] Result<std::optional<std::string>>
    load_recipe_json(std::string_view asset_id) const override;
    [[nodiscard]] Result<void> save_recipe_json(std::string_view asset_id,
                                                std::int64_t recipe_schema_version,
                                                std::string_view recipe_json) override;
    [[nodiscard]] Result<void> clear_recipe(std::string_view asset_id) override;
    [[nodiscard]] Result<std::int64_t>
    commit_recipe(std::string_view asset_id, std::int64_t recipe_schema_version,
                  std::optional<std::string_view> recipe_json, std::string_view history_json,
                  RecipeHistoryWrite history_write,
                  std::optional<std::int64_t> discard_history_after_seq) override;
    [[nodiscard]] Result<void> replace_asset_tags(std::string_view asset_id,
                                                  const std::vector<std::string> &tags) override;
    [[nodiscard]] Result<void> upsert_writable_metadata(std::string_view asset_id,
                                                        const WritableMetadata &metadata) override;
    [[nodiscard]] Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::optional<RecipeHistoryEntry>>
    find_recipe_history(std::int64_t history_id) const override;
    [[nodiscard]] Result<RecipeHistoryEntry>
    append_recipe_history(std::string_view asset_id, std::string_view kind,
                          std::optional<std::string_view> label,
                          std::string_view recipe_json) override;
    [[nodiscard]] Result<void> update_recipe_history_label(std::int64_t history_id,
                                                           std::string_view label) override;
    [[nodiscard]] Result<std::optional<PreviewRecord>>
    find_preview(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::vector<PreviewRecord>> list_previews() const override;
    [[nodiscard]] Result<void> upsert_preview(const PreviewRecord &preview) override;
    [[nodiscard]] Result<std::int64_t> bump_revision() override;
    Result<void> close() override;

private:
    struct Impl;

    explicit SqliteCatalogRepository(std::unique_ptr<Impl> impl);
    static Result<std::unique_ptr<Impl>> open_database(std::string_view database_path, bool create);
    [[nodiscard]] Result<void> insert_asset(const AssetRecord &asset);
    [[nodiscard]] Result<void> upsert_capture_metadata(std::string_view asset_id,
                                                       const CaptureMetadata &capture);

    std::unique_ptr<Impl> impl_;

    friend class testing::SqliteCatalogTestControl;
};

} // namespace ravo
