#include "studio_import_scan_controller.h"

namespace ravo
{

StudioImportScanController::StudioImportScanController(QObject *parent)
    : QObject(parent)
{
}

CancellationToken StudioImportScanController::begin(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
    operation_ = CancellationSource{};
    ++generation_;
    active_ = true;
    resetProgress();
    return operation_.token();
}

void StudioImportScanController::finish()
{
    active_ = false;
}

void StudioImportScanController::abandon(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
    operation_ = CancellationSource{};
    ++generation_;
    active_ = false;
    resetProgress();
}

void StudioImportScanController::bumpGeneration(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
    operation_ = CancellationSource{};
    ++generation_;
}

void StudioImportScanController::cancel(const char *reason)
{
    static_cast<void>(operation_.cancel(reason));
}

void StudioImportScanController::setProgress(int completed, int total, int duplicates)
{
    completed_ = completed;
    total_ = total;
    duplicate_count_ = duplicates;
}

void StudioImportScanController::setTotal(int total)
{
    total_ = total;
}

void StudioImportScanController::setCatalogRevision(std::optional<std::int64_t> revision)
{
    catalog_revision_ = std::move(revision);
}

void StudioImportScanController::resetProgress()
{
    completed_ = 0;
    total_ = 0;
    duplicate_count_ = 0;
    catalog_revision_.reset();
}

} // namespace ravo
