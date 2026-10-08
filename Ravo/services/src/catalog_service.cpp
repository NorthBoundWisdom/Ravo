#include "ravo/services/catalog_service.h"
#include "ravo/services/library_service.h"
#include "ravo/services/develop_service.h"
#include "ravo/services/metadata_service.h"
#include "ravo/services/import_service.h"
#include "ravo/services/ingest_service.h"
#include "ravo/services/recovery_service.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "catalog_service_test_support.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/local_adjustment.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
using namespace catalog_service_internal;
void testing::CatalogServiceTestControl::set_merge_checkpoint(
    CatalogService &service, std::function<Result<void>(std::string_view)> callback)
{
    service.testing_merge_checkpoint_ = std::move(callback);
}

bool testing::CatalogServiceTestControl::has_decoded_raw(const CatalogService &service)
{
    return service.preview_capability_->decoded_raw_.has_value() ||
           service.preview_capability_->browse_decoded_raw_.has_value();
}

void testing::CatalogServiceTestControl::set_before_import_publication(
    CatalogService &service, std::function<void()> callback)
{
    service.testing_before_import_publication_ = std::move(callback);
}

void testing::CatalogServiceTestControl::set_before_preview_cache_publication(
    CatalogService &service, std::function<void()> callback)
{
    service.testing_before_preview_cache_publication_ = std::move(callback);
}

void testing::CatalogServiceTestControl::set_import_checkpoint(
    CatalogService &service,
    std::function<Result<void>(std::string_view checkpoint, std::string_view path)> callback)
{
    service.testing_import_checkpoint_ = std::move(callback);
}

void testing::CatalogServiceTestControl::set_backup_checkpoint(
    CatalogService &service,
    std::function<Result<void>(std::string_view checkpoint, std::string_view path)> callback)
{
    service.testing_backup_checkpoint_ = std::move(callback);
}

void testing::CatalogServiceTestControl::set_before_offline_proxy_publish(
    CatalogService &service,
    std::function<Result<void>(std::string_view, std::string_view)> callback)
{
    service.testing_before_offline_proxy_publish_ = std::move(callback);
}

std::array<std::optional<std::uint32_t>, 2>
testing::CatalogServiceTestControl::linear_working_max_edges(const CatalogService &service)
{
    std::array<std::optional<std::uint32_t>, 2> result;
    for (std::size_t index = 0; index < service.preview_capability_->linear_working_.size();
         ++index)
    {
        if (service.preview_capability_->linear_working_[index])
        {
            result[index] = service.preview_capability_->linear_working_[index]->max_edge;
        }
    }
    return result;
}

std::optional<std::uint32_t>
testing::CatalogServiceTestControl::browse_linear_working_max_edge(const CatalogService &service)
{
    if (!service.preview_capability_->browse_linear_working_)
    {
        return std::nullopt;
    }
    return service.preview_capability_->browse_linear_working_->max_edge;
}

std::optional<std::uint64_t>
testing::CatalogServiceTestControl::roi_linear_working_generation(const CatalogService &service)
{
    if (!service.preview_capability_->roi_linear_working_)
    {
        return std::nullopt;
    }
    return service.preview_capability_->roi_linear_working_->generation;
}

CatalogService::CatalogService(const EngineFacade &engine,
                               std::unique_ptr<CatalogRepository> repository,
                               std::unique_ptr<RasterDecoder> raster,
                               std::shared_ptr<PreviewCache> cache,
                               std::unique_ptr<RecoveryStore> recovery,
                               std::shared_ptr<std::mutex> recovery_publication_mutex)
    : engine_(&engine)
    , repository_(std::move(repository))
    , raster_(std::move(raster))
    , cache_(std::move(cache))
    , recovery_(std::move(recovery))
    , recovery_publication_mutex_(recovery_publication_mutex ?
                                      std::move(recovery_publication_mutex) :
                                      std::make_shared<std::mutex>())
{
    preview_capability_.reset(new PreviewService(engine_, repository_, raster_, cache_,
                                                 testing_before_preview_cache_publication_));
    recovery_capability_.reset(new RecoveryService(
        repository_, recovery_, recovery_publication_mutex_, testing_backup_checkpoint_));
    library_capability_.reset(
        new LibraryService(repository_, cache_, recovery_, *recovery_capability_));
    develop_capability_.reset(new DevelopService(repository_, engine_, *recovery_capability_));
    metadata_capability_.reset(new MetadataService(repository_, engine_, *recovery_capability_));
    import_capability_.reset(new ImportService(
        repository_, raster_, engine_, cache_, *preview_capability_, *recovery_capability_,
        testing_before_import_publication_, testing_import_checkpoint_));
    ingest_capability_.reset(new IngestService(repository_, *import_capability_));
    ai_capability_.reset(new AiService(repository_, *develop_capability_, *library_capability_,
                                       *metadata_capability_, *recovery_capability_));
    conversion_capability_.reset(new ConversionService(repository_, *develop_capability_,
                                                       *import_capability_, *library_capability_,
                                                       *metadata_capability_));
    cull_capability_.reset(new CullService(repository_, *import_capability_, *library_capability_,
                                           *recovery_capability_));
    exports_capability_.reset(
        new ExportService(repository_, raster_, engine_, *preview_capability_));
    merge_capability_.reset(new PhotoMergeService(repository_, raster_, engine_,
                                                  testing_merge_checkpoint_, *develop_capability_,
                                                  *recovery_capability_));
    xmp_capability_.reset(new XmpInterchangeService(repository_, *develop_capability_,
                                                    *library_capability_, *metadata_capability_));
    external_editor_capability_.reset(
        new ExternalEditorService(repository_, raster_, engine_, *exports_capability_,
                                  *import_capability_, *library_capability_));
    offline_capability_.reset(new OfflineEditService(repository_, raster_, engine_,
                                                     testing_before_offline_proxy_publish_,
                                                     *exports_capability_, *develop_capability_));
}

PreviewService &CatalogService::preview() noexcept
{
    return *preview_capability_;
}

const PreviewService &CatalogService::preview() const noexcept
{
    return *preview_capability_;
}

LibraryService &CatalogService::library() noexcept
{
    return *library_capability_;
}

const LibraryService &CatalogService::library() const noexcept
{
    return *library_capability_;
}

DevelopService &CatalogService::develop() noexcept
{
    return *develop_capability_;
}

const DevelopService &CatalogService::develop() const noexcept
{
    return *develop_capability_;
}

MetadataService &CatalogService::metadata() noexcept
{
    return *metadata_capability_;
}

const MetadataService &CatalogService::metadata() const noexcept
{
    return *metadata_capability_;
}

ImportService &CatalogService::import() noexcept
{
    return *import_capability_;
}

const ImportService &CatalogService::import() const noexcept
{
    return *import_capability_;
}

IngestService &CatalogService::ingest() noexcept
{
    return *ingest_capability_;
}

const IngestService &CatalogService::ingest() const noexcept
{
    return *ingest_capability_;
}

RecoveryService &CatalogService::recovery() noexcept
{
    return *recovery_capability_;
}

const RecoveryService &CatalogService::recovery() const noexcept
{
    return *recovery_capability_;
}

AiService &CatalogService::ai() noexcept
{
    return *ai_capability_;
}

const AiService &CatalogService::ai() const noexcept
{
    return *ai_capability_;
}

ConversionService &CatalogService::conversion() noexcept
{
    return *conversion_capability_;
}

const ConversionService &CatalogService::conversion() const noexcept
{
    return *conversion_capability_;
}

CullService &CatalogService::cull() noexcept
{
    return *cull_capability_;
}

const CullService &CatalogService::cull() const noexcept
{
    return *cull_capability_;
}

ExportService &CatalogService::exports() noexcept
{
    return *exports_capability_;
}

const ExportService &CatalogService::exports() const noexcept
{
    return *exports_capability_;
}

PhotoMergeService &CatalogService::merge() noexcept
{
    return *merge_capability_;
}

const PhotoMergeService &CatalogService::merge() const noexcept
{
    return *merge_capability_;
}

XmpInterchangeService &CatalogService::xmp() noexcept
{
    return *xmp_capability_;
}

const XmpInterchangeService &CatalogService::xmp() const noexcept
{
    return *xmp_capability_;
}

ExternalEditorService &CatalogService::external_editor() noexcept
{
    return *external_editor_capability_;
}

const ExternalEditorService &CatalogService::external_editor() const noexcept
{
    return *external_editor_capability_;
}

OfflineEditService &CatalogService::offline() noexcept
{
    return *offline_capability_;
}

const OfflineEditService &CatalogService::offline() const noexcept
{
    return *offline_capability_;
}

CatalogService::~CatalogService()
{
    static_cast<void>(close());
}

Result<void> CatalogService::close()
{
    if (repository_ == nullptr)
    {
        return {};
    }
    std::optional<TaskError> recovery_error;
    if (recovery_ != nullptr)
    {
        auto synchronized = this->recovery().sync_recovery(std::nullopt);
        if (!synchronized)
        {
            recovery_error = synchronized.error();
        }
    }
    const auto closed = repository_->close();
    repository_.reset();
    raster_.reset();
    cache_.reset();
    recovery_.reset();
    engine_ = nullptr;
    preview_capability_->clear_working_cache();
    import_capability_->destination_preview_candidates_.clear();
    if (recovery_error)
    {
        if (!closed)
        {
            recovery_error->context.insert_or_assign("catalog_close_failed", "true");
            recovery_error->context.insert_or_assign("catalog_close_error", closed.error().message);
        }
        return *recovery_error;
    }
    return closed;
}

} // namespace ravo
