#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "ravo/foundation/executor.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
// Desktop composition owner. Service/Engine/SQLite are accessed only on executor().
// The UI cancels its operation tokens before draining this owner on replacement/close.
class StudioImportWorker
{
public:
    ~StudioImportWorker();
    [[nodiscard]] Result<void> open(const std::string &catalog, std::shared_ptr<PreviewCache> cache,
                                    std::shared_ptr<std::mutex> recovery_publication_mutex);
    [[nodiscard]] SerialExecutor &executor() noexcept
    {
        return executor_;
    }
    [[nodiscard]] CatalogService *service() noexcept;
    void shutdown();

private:
    SerialExecutor executor_;
    std::unique_ptr<EngineFacade> engine_;
    std::unique_ptr<CatalogService> service_;
    bool stopped_ = false;
};
} // namespace ravo
