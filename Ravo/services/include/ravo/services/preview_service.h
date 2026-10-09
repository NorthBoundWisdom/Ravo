#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class PreviewCache;
namespace testing
{
class CatalogServiceTestControl;
}

// One owner-thread render session. Foreground, settled, browse and ROI slots
// remain independently bounded; this object never commits a Recipe.
class PreviewService
{
public:
    PreviewService(const PreviewService &) = delete;
    PreviewService &operator=(const PreviewService &) = delete;
    PreviewService(PreviewService &&) = delete;
    PreviewService &operator=(PreviewService &&) = delete;
    [[nodiscard]] Result<PreviewResult> build_import_preview(std::string_view asset_id,
                                                             ImportPreviewPolicy policy,
                                                             const CancellationToken &cancellation);
    [[nodiscard]] Result<PreviewResult>
    request_preview(const PreviewRequest &request,
                    const std::optional<DevelopParams> &live_develop = {});
    [[nodiscard]] Result<PreviewRebuildResult> rebuild_previews(
        const std::vector<std::string> &asset_ids, const CancellationToken &cancellation,
        const std::function<void(std::size_t, std::size_t, const PreviewRebuildItemResult *)>
            &progress = {});
    [[nodiscard]] Result<RenderedExportImage>
    render_for_export(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                      const ExportOptions &options, const CancellationToken &cancellation,
                      RenderSampleKind sample_kind);

    void clear_working_cache() noexcept;

private:
    friend class CatalogService;
    friend class ImportService;
    friend class testing::CatalogServiceTestControl;
    PreviewService(const EngineFacade *const &engine,
                   const std::unique_ptr<CatalogRepository> &repository,
                   const std::unique_ptr<RasterDecoder> &raster,
                   const std::unique_ptr<VideoDecoder> &video,
                   const std::shared_ptr<PreviewCache> &cache,
                   std::function<void()> &before_cache_publication) noexcept;
    void seed_browse_source(const AssetRecord &asset, RasterBuffer raster);
    [[nodiscard]] Result<PreviewResult> generate_video_preview(const AssetRecord &asset,
                                                               const PreviewRequest &request,
                                                               std::int64_t generation);

    enum class PreviewLane
    {
        kForegroundDevelop,
        kBackgroundBrowse,
    };

    struct DecodedPreviewSource
    {
        std::string asset_id;
        std::string fingerprint;
        std::uint32_t max_edge = 0;
        RasterBuffer raster;
    };

    struct CachedRawFrame
    {
        std::string asset_id;
        std::string fingerprint;
        std::string path;
        DecodedRaw raw;
    };

    struct CachedLinearWorking
    {
        std::string asset_id;
        std::string fingerprint;
        std::uint32_t max_edge = 0;
        std::string preprocess_key;
        LinearWorkingBuffer buffer;
        InteractivePreviewRenderCache interactive_render_cache;
    };

    struct CachedRoiLinearWorking
    {
        std::string asset_id;
        std::string fingerprint;
        std::string preprocess_key;
        std::uint32_t origin_x = 0;
        std::uint32_t origin_y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        LinearWorkingBuffer buffer;
        InteractivePreviewRenderCache interactive_render_cache;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] Result<PreviewResult>
    generate_preview(const AssetRecord &asset, const PreviewRequest &request,
                     const std::optional<DevelopParams> &live_develop,
                     std::int64_t expected_generation);
    [[nodiscard]] Result<void>
    require_preview_generation(std::string_view asset_id,
                               std::optional<std::int64_t> generation) const;
    [[nodiscard]] Result<PreviewResult> generate_roi_preview(const AssetRecord &asset,
                                                             const PreviewRequest &request,
                                                             const Recipe &recipe,
                                                             std::string_view path);
    [[nodiscard]] Result<PreviewResult>
    persist_embedded_browse_preview(const AssetRecord &asset, const EmbeddedPreview &embedded,
                                    std::uint32_t max_edge, const CancellationToken &cancellation,
                                    std::optional<std::int64_t> expected_generation = {});
    [[nodiscard]] Result<PreviewResult>
    persist_companion_jpeg_browse_preview(const AssetRecord &asset, std::string_view jpeg_path,
                                          std::uint32_t max_edge,
                                          const CancellationToken &cancellation,
                                          std::optional<std::int64_t> expected_generation = {});
    [[nodiscard]] Result<RasterBuffer>
    decode_preview_source(const AssetRecord &asset, std::string_view path, std::uint32_t max_edge,
                          const CancellationToken &cancellation, PreviewLane lane);
    [[nodiscard]] Result<const DecodedRaw *> cached_raw_frame(const AssetRecord &asset,
                                                              std::string_view path,
                                                              const CancellationToken &cancellation,
                                                              PreviewLane lane);
    [[nodiscard]] Result<CachedLinearWorking *>
    cached_linear_working(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                          std::uint32_t width, std::uint32_t height, std::uint32_t max_edge,
                          const CancellationToken &cancellation, PreviewLane lane);
    [[nodiscard]] Result<CachedRoiLinearWorking *>
    cached_roi_linear_working(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                              std::uint32_t origin_x, std::uint32_t origin_y, std::uint32_t width,
                              std::uint32_t height, const CancellationToken &cancellation);

    const EngineFacade *const &engine_;
    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RasterDecoder> &raster_;
    const std::unique_ptr<VideoDecoder> &video_;
    const std::shared_ptr<PreviewCache> &cache_;
    std::function<void()> &testing_before_preview_cache_publication_;
    // Foreground Develop and background Gallery work have independent bounded
    // decode/working ownership. A thumbnail must never evict the selected
    // photo's interactive or settled scene-linear buffers.
    std::optional<DecodedPreviewSource> decoded_preview_source_;
    std::optional<CachedRawFrame> decoded_raw_;
    // Preview interaction alternates between the live frame and the
    // 1600px settled frame. Keep one bounded slot for each size class so the
    // live request cannot evict the already-prepared settled working buffer.
    // Interactive-class requests may box-filter the settled slot instead of a
    // second CFA demosaic; settled and larger sizes stay native. Viewport 1:1
    // owns one additional CFA-window slot so RGB sliders do not remosaic.
    std::array<std::optional<CachedLinearWorking>, 2> linear_working_;
    std::optional<CachedRoiLinearWorking> roi_linear_working_;
    std::optional<DecodedPreviewSource> browse_decoded_preview_source_;
    std::optional<CachedRawFrame> browse_decoded_raw_;
    std::optional<CachedLinearWorking> browse_linear_working_;
};

} // namespace ravo
