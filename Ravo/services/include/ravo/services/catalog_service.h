#pragma once

#include <functional>
#include <memory>
#include <optional>
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
    [[nodiscard]] Result<void> remove_original_and_catalog(std::string_view asset_id);
    [[nodiscard]] Result<bool> asset_has_edits(std::string_view asset_id) const;
    [[nodiscard]] Result<Recipe> load_recipe(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> save_recipe(std::string_view asset_id, const Recipe &recipe);
    [[nodiscard]] Result<AssetRecord> save_develop(std::string_view asset_id,
                                                   const DevelopParams &params);
    [[nodiscard]] Result<AssetRecord> reset_recipe(std::string_view asset_id);
    [[nodiscard]] Result<AssetRecord> set_tags(std::string_view asset_id,
                                               const std::vector<std::string> &tags);
    [[nodiscard]] Result<AssetRecord> set_writable_metadata(std::string_view asset_id,
                                                            const WritableMetadata &metadata);
    [[nodiscard]] Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> create_recipe_snapshot(std::string_view asset_id,
                                                             std::string_view label);
    [[nodiscard]] Result<AssetRecord> restore_recipe_history(std::string_view asset_id,
                                                             std::int64_t history_id);
    [[nodiscard]] Result<ImportItemResult> import_one(std::string_view path,
                                                      const CancellationToken &cancellation);
    [[nodiscard]] Result<std::vector<ImportItemResult>>
    import_inputs(const std::vector<std::string> &paths, const CancellationToken &cancellation,
                  const std::function<void(std::size_t, std::size_t)> &progress = {});
    [[nodiscard]] Result<PreviewResult>
    request_preview(const PreviewRequest &request,
                    const std::optional<DevelopParams> &live_develop = {});
    [[nodiscard]] Result<ExportResult> export_asset(const ExportRequest &request);
    Result<void> close();

private:
    struct DecodedPreviewSource
    {
        std::string asset_id;
        std::string fingerprint;
        std::uint32_t max_edge = 0;
        RasterBuffer raster;
    };

    [[nodiscard]] Result<PreviewResult>
    generate_preview(const AssetRecord &asset, const PreviewRequest &request,
                     const std::optional<DevelopParams> &live_develop);
    [[nodiscard]] Result<RasterBuffer> decode_preview_source(const AssetRecord &asset,
                                                             std::string_view path,
                                                             std::uint32_t max_edge,
                                                             const CancellationToken &cancellation);
    [[nodiscard]] Result<RenderedImage>
    render_for_export(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                      std::uint32_t max_edge, const CancellationToken &cancellation);

    const EngineFacade *engine_ = nullptr;
    std::unique_ptr<CatalogRepository> repository_;
    std::unique_ptr<RasterDecoder> raster_;
    std::unique_ptr<PreviewCache> cache_;
    std::optional<DecodedPreviewSource> decoded_preview_source_;
};

} // namespace ravo
