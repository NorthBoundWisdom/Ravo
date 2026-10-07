#include "ravo/desktop/studio_import_workspace.h"
#include "studio_import_worker.h"
#include "ravo/desktop/studio_presenter.h"
#include "studio_qt.h"

#include <cassert>
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"

namespace ravo
{
Result<StudioPresenter::PreparedCatalogSession>
StudioPresenter::make_catalog_service(const std::string &path, const bool create)
{
    if (!engine_)
    {
        return make_error(ErrorCode::kInternal, "Engine is not available");
    }
    // Cancellation is issued on the UI thread before switching catalogs. Drain
    // old import/cache publications before indexing a replacement cache owner.
    import_workspace_->importWorker().executor().submit([] {});
    auto repository =
        create ? SqliteCatalogRepository::create(path) : SqliteCatalogRepository::open(path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(preview_root_for(path));
    if (!cache)
    {
        return cache.error();
    }
    auto raster = std::make_unique<QtRasterDecoder>();
    auto recovery = FilesystemRecoveryStore::create_for_catalog(path);
    if (!recovery)
    {
        return recovery.error();
    }
    std::shared_ptr<PreviewCache> shared_cache = std::move(cache).value();
    auto publication_mutex = std::make_shared<std::mutex>();
    auto service = std::make_unique<CatalogService>(*engine_, std::move(repository).value(),
                                                    std::move(raster), shared_cache,
                                                    std::move(recovery).value(), publication_mutex);
    auto resumed = service->recovery().sync_recovery(std::nullopt);
    if (!resumed)
    {
        return resumed.error();
    }
    return PreparedCatalogSession{std::move(service), std::move(shared_cache),
                                  std::move(publication_mutex)};
}

StudioImportWorker::~StudioImportWorker()
{
    shutdown();
}

Result<void> StudioImportWorker::open(const std::string &catalog,
                                      std::shared_ptr<PreviewCache> cache,
                                      std::shared_ptr<std::mutex> recovery_publication_mutex)
{
    return executor_.submit(
        [this, &catalog, &cache, &recovery_publication_mutex]() -> Result<void>
        {
            auto created = EngineFacade::create_phase1();
            if (!created)
                return created.error();
            auto engine = std::make_unique<EngineFacade>(std::move(created).value());
            auto repository = SqliteCatalogRepository::open(catalog);
            if (!repository)
                return repository.error();
            auto recovery = FilesystemRecoveryStore::create_for_catalog(catalog);
            if (!recovery)
                return recovery.error();
            auto service = std::make_unique<CatalogService>(
                *engine, std::move(repository).value(), std::make_unique<QtRasterDecoder>(), cache,
                std::move(recovery).value(), recovery_publication_mutex);
            service_.reset();
            engine_ = std::move(engine);
            service_ = std::move(service);
            return {};
        });
}

CatalogService *StudioImportWorker::service() noexcept
{
    assert(executor_.is_worker_thread());
    return service_.get();
}

void StudioImportWorker::shutdown()
{
    if (stopped_)
        return;
    stopped_ = true;
    executor_.submit(
        [this]
        {
            service_.reset();
            engine_.reset();
        });
    executor_.request_stop();
    executor_.wait();
}
} // namespace ravo
