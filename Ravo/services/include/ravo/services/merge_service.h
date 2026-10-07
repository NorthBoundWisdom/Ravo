#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/services/photo_merge.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class DevelopService;
class RecoveryService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class PhotoMergeService
{
public:
    PhotoMergeService(const PhotoMergeService &) = delete;
    PhotoMergeService &operator=(const PhotoMergeService &) = delete;
    PhotoMergeService(PhotoMergeService &&) = delete;
    PhotoMergeService &operator=(PhotoMergeService &&) = delete;

    [[nodiscard]] Result<PhotoMergeResult> merge_selected_photos(const PhotoMergeRequest &request);

private:
    friend class CatalogService;
    PhotoMergeService(const std::unique_ptr<CatalogRepository> &repository,
                      const std::unique_ptr<RasterDecoder> &raster,
                      const EngineFacade *const &engine,
                      const std::function<Result<void>(std::string_view)> &merge_checkpoint,
                      DevelopService &develop_service, RecoveryService &recovery_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RasterDecoder> &raster_;
    const EngineFacade *const &engine_;
    const std::function<Result<void>(std::string_view)> &testing_merge_checkpoint_;
    DevelopService &develop_service_;
    RecoveryService &recovery_service_;
};

} // namespace ravo
