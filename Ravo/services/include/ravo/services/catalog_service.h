#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/preview_cache.h"
#include "ravo/domain/raster_decoder.h"
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

class CatalogService
{
public:
    CatalogService(const EngineFacade &engine, std::unique_ptr<CatalogRepository> repository,
                   std::unique_ptr<RasterDecoder> raster, std::unique_ptr<PreviewCache> cache);

    CatalogService(const CatalogService &) = delete;
    CatalogService &operator=(const CatalogService &) = delete;
    CatalogService(CatalogService &&) noexcept = default;
    CatalogService &operator=(CatalogService &&) noexcept = default;
    ~CatalogService();

    [[nodiscard]] Result<CatalogSnapshot> snapshot() const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets() const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets(const LibraryQuery &query) const;
    [[nodiscard]] Result<std::vector<FolderRecord>> list_folders() const;
    [[nodiscard]] Result<AssetRecord> set_rating(std::string_view asset_id, int rating);
    [[nodiscard]] Result<AssetRecord> set_color_label(std::string_view asset_id, ColorLabel label);
    [[nodiscard]] Result<AssetRecord> set_rejected(std::string_view asset_id, bool rejected);
    [[nodiscard]] Result<void> remove_from_catalog(std::string_view asset_id);
    [[nodiscard]] Result<Recipe> load_recipe(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> save_recipe(std::string_view asset_id, const Recipe &recipe);
    [[nodiscard]] Result<AssetRecord> save_develop(std::string_view asset_id,
                                                   const DevelopParams &params);
    [[nodiscard]] Result<AssetRecord> reset_recipe(std::string_view asset_id);
    [[nodiscard]] Result<ImportItemResult> import_one(std::string_view path,
                                                      const CancellationToken &cancellation);
    [[nodiscard]] Result<std::vector<ImportItemResult>>
    import_inputs(const std::vector<std::string> &paths, const CancellationToken &cancellation,
                  const std::function<void(std::size_t, std::size_t)> &progress = {});
    [[nodiscard]] Result<PreviewResult> request_preview(const PreviewRequest &request);
    Result<void> close();

private:
    [[nodiscard]] Result<PreviewResult> generate_preview(const AssetRecord &asset,
                                                         std::uint32_t max_edge,
                                                         const CancellationToken &cancellation,
                                                         std::uint64_t request_revision,
                                                         bool ignore_edits = false,
                                                         bool ignore_crop = false);

    const EngineFacade *engine_ = nullptr;
    std::unique_ptr<CatalogRepository> repository_;
    std::unique_ptr<RasterDecoder> raster_;
    std::unique_ptr<PreviewCache> cache_;
};

} // namespace ravo
