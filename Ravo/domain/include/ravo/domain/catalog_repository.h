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
    [[nodiscard]] virtual Result<void> insert_asset(const AssetRecord &asset) = 0;
    [[nodiscard]] virtual Result<void> update_asset(const AssetRecord &asset) = 0;
    [[nodiscard]] virtual Result<void> update_review(std::string_view asset_id,
                                                     const ReviewState &review) = 0;
    [[nodiscard]] virtual Result<std::optional<std::string>>
    load_recipe_json(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<void> save_recipe_json(std::string_view asset_id,
                                                        std::int64_t recipe_schema_version,
                                                        std::string_view recipe_json) = 0;
    [[nodiscard]] virtual Result<void> clear_recipe(std::string_view asset_id) = 0;
    [[nodiscard]] virtual Result<std::optional<PreviewRecord>>
    find_preview(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<void> upsert_preview(const PreviewRecord &preview) = 0;
    [[nodiscard]] virtual Result<std::int64_t> bump_revision() = 0;
    virtual Result<void> close() = 0;
};

} // namespace ravo
