#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_import_workspace.h"
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
    , inspect_(
          StudioInspectPresenter::Context{selected_asset_id_, catalog_path_, engine_, executor_},
          StudioInspectPresenter::Host{
              [this] { return service_ ? &service_->preview() : nullptr; },
              [this] { return develop_presenter_->state().develop_; },
              [this] { return develop_presenter_->state().mask_overlay_visible_; },
              [this] { static_cast<void>(develop_presenter_->clear_comparison()); }},
          this)
{
    import_workspace_ = std::make_unique<StudioImportWorkspace>(
        StudioImportWorkspace::Context{catalog_path_, busy_},
        StudioImportWorkspace::Host{
            [this] { return current_query(); }, [this](const std::vector<std::string> &paths)
            { beginImportGalleryPlaceholders(paths); },
            [this](const ImportItemResult &item, int row) { publishImportItem(item, row); },
            [this](StudioImportWorkspace::BatchCompletion batch)
            { finishImportPresentation(std::move(batch)); },
            [this](const LibraryQuery &query)
            {
                last_import_selected_ = false;
                last_import_count_ = 0;
                last_import_after_unix_ms_.reset();
                last_import_before_unix_ms_.reset();
                library_.replaceQuery(query);
            },
            [this] { reloadVisibleAssets(); }, [this] { setBrowseMode(QStringLiteral("grid")); }},
        this);
    connect(import_workspace_.get(), &StudioImportWorkspace::errorOccurred, this,
            &StudioPresenter::setError);
    connect(import_workspace_.get(), &StudioImportWorkspace::statusOccurred, this,
            &StudioPresenter::setStatus);
    StudioDevelopPresenter::Host develop_host;
    develop_host.develop_service = [this] { return service_ ? &service_->develop() : nullptr; };
    develop_host.preview_service = [this] { return service_ ? &service_->preview() : nullptr; };
    develop_host.recovery_service = [this] { return service_ ? &service_->recovery() : nullptr; };
    develop_host.preview_loading = [this](bool loading) { inspect_.setPreviewLoading(loading); };
    develop_host.observed_revision = [this](std::int64_t revision)
    { observed_catalog_revision_ = revision; };
    develop_host.begin_catalog_operation = [this] { catalog_operation_ = CancellationSource{}; };
    develop_host.catalog_operation_token = [this] { return catalog_operation_.token(); };
    develop_host.catalog_progress = [this](QString stage, int completed, int total, bool active)
    { setCatalogOperation(std::move(stage), completed, total, active); };
    develop_host.clear_comparison_images = [this] { inspect_.clearComparisonImages(); };
    develop_host.adopt_preview_as_comparison = [this]
    { return inspect_.adoptPreviewAsComparison(); };
    develop_host.restore_preview_base = [this] { inspect_.restorePreviewBase(); };
    develop_host.refresh_roi = [this] { inspect_.refresh_inspect_roi(); };
    develop_host.reload_library = [this] { reloadVisibleAssets(); };
    develop_host.request_selection_preview = [this] { requestPreviewForSelection(); };
    develop_host.kick_thumbnails = [this] { kickThumbnailDemand(); };
    develop_host.cancel_thumbnails = [this](std::string reason)
    { static_cast<void>(thumbnail_work_.cancel(std::move(reason))); };
    develop_host.zoom_mode = [this](QString mode) { inspect_.setZoomMode(mode); };
    develop_host.publish_preview =
        [this](const PreviewResult &result, std::uint64_t revision, bool preserve)
    { return inspect_.show_preview_result(result, revision, preserve); };
    develop_host.publish_before = [this](const PreviewResult &result, std::uint64_t revision)
    { inspect_.show_comparison_before_result(result, revision); };
    develop_host.selected_assets = [this] { return selected_asset_ids(); };
    develop_host.selected_media_type = [this] { return selectedMediaType(); };
    develop_presenter_.reset(new StudioDevelopPresenter(
        {selected_asset_id_, catalog_path_, browse_mode_, busy_, catalog_operation_active_,
         import_workspace_->workActiveState(), observed_catalog_revision_, assets_, inspect_,
         engine_, executor_},
        std::move(develop_host), this));
    connect(develop_presenter_.get(), &StudioDevelopPresenter::errorOccurred, this,
            &StudioPresenter::setError);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::statusOccurred, this,
            &StudioPresenter::setStatus);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::selectionChanged, this,
            &StudioPresenter::selectionChanged);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::previewChanged, &inspect_,
            &StudioInspectPresenter::notifyPreviewChanged);
    connect(&inspect_, &StudioInspectPresenter::previewChanged, develop_presenter_.get(),
            &StudioDevelopPresenter::frameProjectionChanged);
    connect(develop_presenter_.get(), &StudioDevelopPresenter::editChanged, &inspect_,
            &StudioInspectPresenter::inspectContextChanged);
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
    connect(&inspect_, &StudioInspectPresenter::previewChanged, develop_presenter_.get(),
            &StudioDevelopPresenter::sync_local_edit_scope, Qt::QueuedConnection);
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
