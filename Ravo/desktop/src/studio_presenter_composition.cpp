#include "ravo/desktop/studio_presenter.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_import_destination_preview_controller.h"
#include "studio_import_scan_controller.h"
#include "studio_import_workspace.h"
#include "studio_import_worker.h"
#include "studio_library_resume.h"

#include "ravo/desktop/export_option_conversion.h"
#include "ravo/desktop/filesystem_browser_model.h"

#include <algorithm>
#include <climits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_file_manager.h"
#include "studio_qt.h"

namespace ravo
{
namespace
{
inline constexpr int kCatalogRevisionPollMs = 1000;
}

StudioPresenter::StudioPresenter(QObject *parent)
    : QObject(parent)
    , assets_(this)
    , folders_(this)
    , library_sets_(this)
    , library_(this)
    , inspect_(this)
{
    import_workspace_ = std::make_unique<StudioImportWorkspace>(this);
    StudioDevelopPresenter::Host develop_host;
    develop_host.develop_service = [this] { return service_ ? &service_->develop() : nullptr; };
    develop_host.preview_service = [this] { return service_ ? &service_->preview() : nullptr; };
    develop_host.recovery_service = [this] { return service_ ? &service_->recovery() : nullptr; };
    develop_host.preview_loading = [this](bool loading) { preview_loading_ = loading; };
    develop_host.observed_revision = [this](std::int64_t revision)
    { observed_catalog_revision_ = revision; };
    develop_host.begin_catalog_operation = [this] { catalog_operation_ = CancellationSource{}; };
    develop_host.catalog_operation_token = [this] { return catalog_operation_.token(); };
    develop_host.catalog_progress = [this](QString stage, int completed, int total, bool active)
    { setCatalogOperation(std::move(stage), completed, total, active); };
    develop_host.clear_comparison_images = [this]
    {
        {
            const QMutexLocker lock(&preview_image_mutex_);
            comparison_before_image_ = QImage();
        }
        comparison_before_base_image_ = QImage();
        comparison_before_output_profile_ = {};
        comparison_before_url_.clear();
    };
    develop_host.adopt_preview_as_comparison = [this]
    {
        QImage before;
        {
            const QMutexLocker lock(&preview_image_mutex_);
            before = preview_image_;
            comparison_before_image_ = before;
        }
        if (before.isNull())
            return false;
        comparison_before_url_ =
            QUrl(QStringLiteral("image://studioPreview/before?r=%1").arg(live_preview_revision_));
        return true;
    };
    develop_host.restore_preview_base = [this]
    {
        const QMutexLocker lock(&preview_image_mutex_);
        preview_image_ = preview_base_image_;
    };
    develop_host.refresh_roi = [this] { refresh_inspect_roi(); };
    develop_host.reload_library = [this] { reloadVisibleAssets(); };
    develop_host.request_selection_preview = [this] { requestPreviewForSelection(); };
    develop_host.kick_thumbnails = [this] { kickThumbnailDemand(); };
    develop_host.cancel_thumbnails = [this](std::string reason)
    { static_cast<void>(thumbnail_work_.cancel(std::move(reason))); };
    develop_host.zoom_mode = [this](QString mode) { setZoomMode(mode); };
    develop_host.publish_preview =
        [this](const PreviewResult &result, std::uint64_t revision, bool preserve)
    { show_preview_result(result, revision, preserve); };
    develop_host.publish_before = [this](const PreviewResult &result, std::uint64_t revision)
    { show_comparison_before_result(result, revision); };
    develop_host.selected_assets = [this] { return selected_asset_ids(); };
    develop_host.selected_media_type = [this] { return selectedMediaType(); };
    develop_presenter_.reset(new StudioDevelopPresenter(
        {selected_asset_id_, catalog_path_, browse_mode_, busy_, catalog_operation_active_,
         import_work_active_, observed_catalog_revision_, live_preview_revision_, preview_loading_,
         preview_image_, preview_base_image_, preview_image_mutex_, comparison_before_url_, assets_,
         inspect_, engine_, executor_},
        std::move(develop_host), this));
    connect(develop_presenter_.get(), &StudioDevelopPresenter::errorOccurred, this,
            &StudioPresenter::setError);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::statusOccurred, this,
            &StudioPresenter::setStatus);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::selectionChanged, this,
            &StudioPresenter::selectionChanged);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::previewChanged, this,
            &StudioPresenter::previewChanged);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::interactivePreviewPublished, this,
            &StudioPresenter::interactivePreviewPublished);
    connect(this, &StudioPresenter::previewChanged, develop_presenter_.get(),
            &StudioDevelopPresenter::frameProjectionChanged);
    connect(this, &StudioPresenter::previewChanged, &inspect_,
            &StudioInspectPresenter::frameChanged);
    connect(this, &StudioPresenter::zoomChanged, this, &StudioPresenter::inspectContextChanged);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::editChanged, this,
            &StudioPresenter::inspectContextChanged);
    export_presenter_.reset(new StudioExportPresenter(
        catalog_path_, selected_asset_id_, busy_, library_query_generation_, executor_,
        shutdown_.token(), [this] { return service_ ? &service_->exports() : nullptr; },
        [this] { return selected_asset_ids(); }, this));
    connect(export_presenter_.get(), &StudioExportPresenter::busyRequested, this,
            &StudioPresenter::setBusy);
    connect(export_presenter_.get(), &StudioExportPresenter::errorOccurred, this,
            &StudioPresenter::setError);
    connect(export_presenter_.get(), &StudioExportPresenter::statusOccurred, this,
            &StudioPresenter::setStatus);
    connect(&library_, &StudioLibraryPresenter::reloadRequested, this,
            [this]
            {
                emit filterChanged();
                reloadVisibleAssets();
            });
    connect(&library_, &StudioLibraryPresenter::errorOccurred, this, &StudioPresenter::setError);
    connect(&inspect_, &StudioInspectPresenter::errorOccurred, this, &StudioPresenter::setError);
    connect(this, &StudioPresenter::selectionChanged, &inspect_,
            [this] { inspect_.observeSelection(selected_asset_id_); });
    initializeLibraryResume();
    connect(this, &StudioPresenter::previewChanged, develop_presenter_.get(),
            &StudioDevelopPresenter::sync_local_edit_scope, Qt::QueuedConnection);
    const auto bind_browser = [this](FilesystemBrowserModel *browser)
    {
        QObject::connect(browser, &FilesystemBrowserModel::directoryListingRequested, this,
                         [this, browser](const QString &path, quint64 generation)
                         { requestFilesystemListing(browser, path, generation); });
    };
    bind_browser(&import_workspace_->source_folders);
    bind_browser(&import_workspace_->destination_folders);
    connect(this, &StudioPresenter::importPageChanged, this,
            &StudioPresenter::refreshImportDestinationPreview);
    connect(&import_workspace_->candidates, &ImportCandidateListModel::selectionChanged, this,
            &StudioPresenter::importPageChanged);
    import_workspace_->scan = std::make_unique<StudioImportScanController>(
        StudioImportScanController::Host{
            this,
            &import_workspace_->worker->executor(),
            [this] { return import_workspace_->worker->service(); },
            [this] { return import_page_open_; },
            [this] { return import_work_active_; },
            [this] { return &import_workspace_->candidates; },
            [this] { return import_workspace_ ? import_workspace_->draft.source_root : QString{}; },
            [this](const QString &root)
            { return import_source_recursion(root, QDir::homePath(), import_recursive_); },
            [this] { emit importPageChanged(); },
            [this](QString message) { setError(std::move(message)); },
            [this]
            {
                if (import_workspace_->thumbnails)
                {
                    import_workspace_->thumbnails->cancel("import_source_changed");
                    import_workspace_->thumbnails->resetOperation();
                    import_workspace_->thumbnails->resetSourceSession();
                }
            },
            [this] { import_preflight_active_ = false; },
            [this]
            {
                if (import_workspace_->thumbnails)
                    import_workspace_->thumbnails->startBackgroundPass();
            },
        },
        this);
    import_workspace_->thumbnails = std::make_unique<StudioImportThumbnailController>(
        StudioImportThumbnailController::Host{
            &import_workspace_->candidates,
            this,
            [this] { return import_page_open_; },
            [this] { return import_work_active_; },
            [this] { return import_preflight_active_; },
            [this] { return import_workspace_->scan ? import_workspace_->scan->generation() : 0U; },
            [this](QString message) { setError(std::move(message)); },
        },
        this);
    import_workspace_->destination_preview =
        std::make_unique<StudioImportDestinationPreviewController>(
            StudioImportDestinationPreviewController::Host{
                this,
                &import_workspace_->worker->executor(),
                [this]() -> CatalogService * { return import_workspace_->worker->service(); },
                [this] { return import_page_open_; },
                [this]
                {
                    return import_page_open_ && import_workspace_->scan &&
                           !import_workspace_->scan->active() && !import_work_active_ &&
                           !import_preflight_active_ &&
                           import_workspace_->scan->catalogRevision() &&
                           import_workspace_->draft.mode != QLatin1String("add") &&
                           !import_workspace_->draft.destination.isEmpty() &&
                           import_workspace_->draft.destination_error.isEmpty() &&
                           import_workspace_->candidates.selectedCount() > 0;
                },
                [this]
                {
                    // Lightweight invalidation key only — owning path snapshots are built
                    // after the destination-preview debounce timer fires (build_request).
                    return QJsonDocument(
                               QJsonObject{
                                   {QStringLiteral("catalog"), catalog_path_},
                                   {QStringLiteral("revision"),
                                    QString::number(*import_workspace_->scan->catalogRevision())},
                                   {QStringLiteral("source"), import_workspace_->draft.source_root},
                                   {QStringLiteral("destination"),
                                    import_workspace_->draft.destination},
                                   {QStringLiteral("second"),
                                    import_workspace_->draft.second_copy_destination},
                                   {QStringLiteral("mode"), import_workspace_->draft.mode},
                                   {QStringLiteral("organization"),
                                    import_workspace_->draft.organization},
                                   {QStringLiteral("name"),
                                    import_workspace_->draft.filename_pattern},
                                   {QStringLiteral("generation"),
                                    QString::number(import_workspace_->candidates.generation())},
                                   {QStringLiteral("selectionRevision"),
                                    QString::number(
                                        import_workspace_->candidates.selectionRevision())}})
                        .toJson(QJsonDocument::Compact);
                },
                [this] { return plannedImportRequest(); },
            },
            this);
    connect(import_workspace_->destination_preview.get(),
            &StudioImportDestinationPreviewController::changed, this,
            [this]
            {
                import_workspace_->destination_folders.setPreviewFolders(
                    import_workspace_->destination_preview->treeFolders(), importDestination());
                emit importDestinationPreviewChanged();
            });
    catalog_revision_timer_ = new QTimer(this);
    catalog_revision_timer_->setInterval(kCatalogRevisionPollMs);
    catalog_revision_timer_->setTimerType(Qt::CoarseTimer);
    QObject::connect(catalog_revision_timer_, &QTimer::timeout, this,
                     &StudioPresenter::pollCatalogRevision);
    backup_schedule_timer_ = new QTimer(this);
    backup_schedule_timer_->setInterval(60'000);
    backup_schedule_timer_->setTimerType(Qt::VeryCoarseTimer);
    QObject::connect(backup_schedule_timer_, &QTimer::timeout, this,
                     &StudioPresenter::checkScheduledBackup);
    const auto created = executor_.submit(
        [this]() -> Result<void>
        {
            auto engine = EngineFacade::create_phase1();
            if (!engine)
            {
                return engine.error();
            }
            engine_ = std::move(engine).value();
            return {};
        });
    if (!created)
    {
        error_text_ = qstring_from_utf8(created.error().message);
        status_text_ = QCoreApplication::translate("StudioPresenter", "Engine failed to start.");
    }
}

} // namespace ravo
