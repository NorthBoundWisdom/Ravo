#include "ravo/desktop/studio_presenter.h"

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
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
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

struct CatalogListing
{
    Result<std::vector<AssetRecord>> assets =
        make_error(ErrorCode::kIo, "Catalog session is closed");
    Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
    Result<std::vector<LibrarySetRecord>> library_sets = std::vector<LibrarySetRecord>{};
    Result<LibraryCaptureFacets> capture_facets =
        make_error(ErrorCode::kIo, "Catalog session is closed");
    Result<LibraryLocationFacets> location_facets =
        make_error(ErrorCode::kIo, "Catalog session is closed");
    std::unordered_map<std::string, QUrl> thumbnail_urls;
    std::unordered_map<std::string, QString> thumbnail_states;
    std::int64_t revision = -1;
    std::size_t total = 0U;
    bool has_more = false;
};

void fill_thumbnail_maps(CatalogService &service, CatalogListing &listing)
{
    if (!listing.assets)
    {
        return;
    }
    auto snapshot = service.snapshot();
    std::vector<std::string> asset_ids;
    asset_ids.reserve(listing.assets.value().size());
    for (const auto &asset : listing.assets.value())
        asset_ids.push_back(asset.id);
    auto previews = service.list_previews_for_assets(asset_ids);
    if (!snapshot || !previews)
    {
        return;
    }
    std::unordered_map<std::string, PreviewRecord> by_id;
    by_id.reserve(previews.value().size());
    for (auto &preview : previews.value())
    {
        by_id.emplace(preview.asset_id, std::move(preview));
    }
    const QDir cache_dir(qstring_from_utf8(snapshot.value().cache_root));
    for (const auto &asset : listing.assets.value())
    {
        const auto found = by_id.find(asset.id);
        if (found == by_id.end() || found->second.contract_version != kPreviewContractVersion ||
            found->second.state != kPreviewStateReady || !found->second.cache_relpath)
        {
            continue;
        }
        const QString path = cache_dir.filePath(qstring_from_utf8(*found->second.cache_relpath));
        if (!QFileInfo::exists(path))
        {
            continue;
        }
        listing.thumbnail_urls.emplace(asset.id, QUrl::fromLocalFile(path));
        listing.thumbnail_states.emplace(asset.id, QStringLiteral("ready"));
    }
}

[[nodiscard]] QString catalog_error_text(const TaskError &error)
{
    QString text = qstring_from_utf8(error.message);
    const auto qt_error = error.context.find("qt_error");
    if (qt_error != error.context.end() && !qt_error->second.empty())
    {
        text += QStringLiteral(": ");
        text += qstring_from_utf8(qt_error->second);
    }
    return text;
}

CatalogListing load_catalog_listing(CatalogService *service, const LibraryQuery &query,
                                    const bool collapse_stacks)
{
    CatalogListing listing;
    if (service == nullptr)
    {
        return listing;
    }
    auto snapshot = service->snapshot();
    if (snapshot)
    {
        listing.revision = snapshot.value().revision;
    }
    LibraryPageRequest page_request;
    page_request.query = query;
    page_request.collapse_stacks = collapse_stacks;
    auto page = service->list_assets_page(page_request);
    if (page)
    {
        listing.total = page.value().total;
        listing.has_more = page.value().has_more;
        listing.assets = std::move(page).value().assets;
    }
    else
    {
        listing.assets = page.error();
    }
    listing.folders = service->list_folders();
    listing.library_sets = service->list_library_sets();
    listing.capture_facets = service->list_capture_facets(query);
    listing.location_facets = service->list_location_facets(query);
    fill_thumbnail_maps(*service, listing);
    return listing;
}

} // namespace

StudioPresenter::StudioPresenter(QObject *parent)
    : QObject(parent)
    , assets_(this)
    , folders_(this)
    , library_sets_(this)
    , import_candidates_(this)
    , import_source_folders_(this)
    , import_destination_folders_(this)
{
    const auto bind_browser = [this](FilesystemBrowserModel *browser)
    {
        QObject::connect(browser, &FilesystemBrowserModel::directoryListingRequested, this,
                         [this, browser](const QString &path, quint64 generation)
                         { requestFilesystemListing(browser, path, generation); });
    };
    bind_browser(&import_source_folders_);
    bind_browser(&import_destination_folders_);
    connect(&import_candidates_, &ImportCandidateListModel::selectionChanged, this,
            &StudioPresenter::importPageChanged);
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

StudioPresenter::~StudioPresenter()
{
    release_gpu_preview_presented_surface();
    release_gpu_roi_presented_surface();
    if (catalog_revision_timer_ != nullptr)
    {
        catalog_revision_timer_->stop();
    }
    if (backup_schedule_timer_ != nullptr)
    {
        backup_schedule_timer_->stop();
    }
    static_cast<void>(shutdown_.cancel("window_closed"));
    static_cast<void>(thumbnail_work_.cancel("window_closed"));
    static_cast<void>(catalog_operation_.cancel("window_closed"));
    static_cast<void>(import_operation_.cancel("window_closed"));
    static_cast<void>(import_preview_operation_.cancel("window_closed"));
    develop_preview_owner_.cancel("window_closed");
    cancel_preview_analysis("window_closed");
    perspective_analysis_owner_.cancel("window_closed");
    preview_analysis_executor_.request_stop();
    preview_analysis_executor_.wait();
    executor_.submit(
        [this]()
        {
            service_.reset();
            engine_.reset();
        });
    executor_.request_stop();
    executor_.wait();
}

AssetListModel *StudioPresenter::assets() noexcept
{
    return &assets_;
}

FolderListModel *StudioPresenter::folders() noexcept
{
    return &folders_;
}

LibrarySetListModel *StudioPresenter::librarySets() noexcept
{
    return &library_sets_;
}

Result<std::unique_ptr<CatalogService>>
StudioPresenter::make_catalog_service(const std::string &path, const bool create)
{
    if (!engine_)
    {
        return make_error(ErrorCode::kInternal, "Engine is not available");
    }
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
    auto service =
        std::make_unique<CatalogService>(*engine_, std::move(repository).value(), std::move(raster),
                                         std::move(cache).value(), std::move(recovery).value());
    auto resumed = service->sync_recovery(std::nullopt);
    if (!resumed)
    {
        return resumed.error();
    }
    return service;
}

bool StudioPresenter::catalogOpen() const noexcept
{
    return !catalog_path_.isEmpty();
}

QString StudioPresenter::catalogPath() const
{
    return catalog_path_;
}

QUrl StudioPresenter::defaultCatalogFolder() const
{
    return QUrl::fromLocalFile(pictures_directory());
}

QUrl StudioPresenter::defaultCatalogFile() const
{
    return QUrl::fromLocalFile(
        QDir(pictures_directory()).filePath(QStringLiteral("Ravo Library.sqlite")));
}

QString StudioPresenter::startupCatalogPath() const
{
    return startup_catalog_path_;
}

bool StudioPresenter::importWorkActive() const noexcept
{
    return import_work_active_;
}

int StudioPresenter::importWorkCompleted() const noexcept
{
    return import_work_completed_;
}

int StudioPresenter::importWorkTotal() const noexcept
{
    return import_work_total_;
}

bool StudioPresenter::previewWorkActive() const noexcept
{
    return preview_work_active_;
}

int StudioPresenter::previewWorkCompleted() const noexcept
{
    return preview_work_completed_;
}

int StudioPresenter::previewWorkTotal() const noexcept
{
    return preview_work_total_;
}

bool StudioPresenter::catalogOperationActive() const noexcept
{
    return catalog_operation_active_;
}

QString StudioPresenter::catalogOperationStage() const
{
    return catalog_operation_stage_;
}

int StudioPresenter::catalogOperationCompleted() const noexcept
{
    return catalog_operation_completed_;
}

int StudioPresenter::catalogOperationTotal() const noexcept
{
    return catalog_operation_total_;
}

int StudioPresenter::recoveryPendingCount() const noexcept
{
    return recovery_pending_count_;
}

int StudioPresenter::libraryTotal() const noexcept
{
    return static_cast<int>(
        std::min<std::size_t>(library_total_, static_cast<std::size_t>(INT_MAX)));
}

bool StudioPresenter::libraryHasMore() const noexcept
{
    return library_has_more_;
}

void StudioPresenter::setStartupCatalogPath(const QString &path)
{
    const QFileInfo info(path);
    startup_catalog_path_ = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
}

bool StudioPresenter::defaultCatalogExists() const
{
    const QFileInfo info(defaultCatalogFile().toLocalFile());
    return info.exists() && info.isFile();
}

bool StudioPresenter::busy() const noexcept
{
    return busy_;
}

QString StudioPresenter::statusText() const
{
    return status_text_;
}

QString StudioPresenter::errorText() const
{
    return error_text_;
}

QString StudioPresenter::selectedAssetId() const
{
    return selected_asset_id_;
}

int StudioPresenter::selectedIndex() const
{
    return assets_.indexOf(selected_asset_id_);
}

int StudioPresenter::selectedCount() const noexcept
{
    return static_cast<int>(selected_ids_.size());
}

bool StudioPresenter::isAssetSelected(const QString &asset_id) const
{
    return selected_ids_.contains(utf8_from_qstring(asset_id));
}

int StudioPresenter::selectedRating() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? asset->review.rating : 0;
}

QString StudioPresenter::selectedColorLabel() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(color_label_name(asset->review.color_label)) :
                   QStringLiteral("none");
}

bool StudioPresenter::selectedRejected() const noexcept
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->review.rejected;
}

bool StudioPresenter::selectedPicked() const noexcept
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->review.picked;
}

QString StudioPresenter::selectedImportState() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->import_state) : QString{};
}

bool StudioPresenter::canDeleteFromDisk() const
{
    if (selected_ids_.empty())
    {
        return false;
    }
    for (const auto &id : selected_ids_)
    {
        const auto asset = assets_.assetById(qstring_from_utf8(id));
        if (!asset || asset->import_state == kImportStateMissing ||
            asset->version_ordinal != kAssetVersionOrdinalPrimary)
        {
            return false;
        }
    }
    return true;
}

QString StudioPresenter::browseMode() const
{
    return browse_mode_;
}

bool StudioPresenter::collapseStacks() const noexcept
{
    return collapse_stacks_;
}

int StudioPresenter::surveySlotCount() const noexcept
{
    return static_cast<int>(survey_slot_ids_.size());
}

QVariantList StudioPresenter::surveySlots() const
{
    QVariantList items;
    items.reserve(static_cast<qsizetype>(survey_slot_ids_.size()));
    for (const auto &id : survey_slot_ids_)
    {
        QVariantMap slot;
        slot.insert(QStringLiteral("assetId"), qstring_from_utf8(id));
        const auto url = survey_preview_urls_.find(id);
        slot.insert(QStringLiteral("url"),
                    url == survey_preview_urls_.end() ? QUrl{} : url->second);
        slot.insert(QStringLiteral("loading"), url == survey_preview_urls_.end());
        items.push_back(slot);
    }
    return items;
}

QString StudioPresenter::zoomMode() const
{
    return zoom_mode_;
}

double StudioPresenter::zoomFactor() const noexcept
{
    return zoom_factor_;
}

int StudioPresenter::thumbnailSize() const noexcept
{
    return thumbnail_size_;
}

LibraryQuery StudioPresenter::current_query() const
{
    return query_;
}

void StudioPresenter::setBusy(const bool busy)
{
    if (busy_ == busy)
    {
        return;
    }
    busy_ = busy;
    emit busyChanged();
}

void StudioPresenter::setStatus(QString text)
{
    if (status_text_ == text)
    {
        return;
    }
    status_text_ = std::move(text);
    emit statusChanged();
}

void StudioPresenter::setError(QString text)
{
    if (error_text_ == text)
    {
        return;
    }
    error_text_ = std::move(text);
    emit errorChanged();
}

void StudioPresenter::applyAssets(std::vector<AssetRecord> assets, const bool restore_selection,
                                  std::unordered_map<std::string, QUrl> thumbnail_urls,
                                  std::unordered_map<std::string, QString> thumbnail_states,
                                  const std::size_t total, const bool has_more)
{
    ++library_query_generation_;
    const QString previous = selected_asset_id_;
    const auto incoming_thumbs = thumbnail_urls;
    const auto incoming_states = thumbnail_states;
    assets_.setAssets(std::move(assets), std::move(thumbnail_urls), std::move(thumbnail_states),
                      total);
    for (auto it = thumbnail_base_paths_.begin(); it != thumbnail_base_paths_.end();)
    {
        if (!assets_.assetById(qstring_from_utf8(it->first)))
        {
            thumbnail_base_profiles_.erase(it->first);
            it = thumbnail_base_paths_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (const auto &[id, url] : incoming_thumbs)
    {
        if (!url.isLocalFile() || !assets_.assetById(qstring_from_utf8(id)))
            continue;
        QString state = QStringLiteral("ready");
        const auto state_it = incoming_states.find(id);
        if (state_it != incoming_states.end() && !state_it->second.isEmpty())
            state = state_it->second;
        remember_thumbnail_base(id, url.toLocalFile(), ColorProfileState{}, state);
    }
    library_total_ = total == 0U && assets_.rowCount() > 0 ?
                         static_cast<std::size_t>(assets_.rowCount()) :
                         total;
    library_has_more_ = has_more;
    library_page_in_flight_ = false;
    library_next_offset_ = static_cast<std::size_t>(assets_.loadedCount());
    emit thumbnailsChanged();
    resetThumbnailDemand();
    assets_.setSelectedIds(selected_ids_);
    emit filterChanged();
    emit selectionChanged();
    if (!restore_selection)
    {
        return;
    }
    if (!previous.isEmpty() && assets_.indexOf(previous) >= 0)
    {
        if (selected_asset_id_ != previous)
        {
            selectAsset(previous);
        }
        else
        {
            publish_selection();
        }
        return;
    }
    if (assets_.rowCount() == 0)
    {
        selected_asset_id_.clear();
        selection_anchor_id_.clear();
        selected_ids_.clear();
        assets_.setSelectedIds({});
        clear_displayed_preview();
        preview_loading_ = false;
        emit selectionChanged();
        emit previewChanged();
        return;
    }
    if (!selected_ids_.empty())
    {
        const auto remaining = selected_asset_ids();
        if (!remaining.empty())
            activate_primary(qstring_from_utf8(remaining.front()), true);
        else
            publish_selection();
        return;
    }
    if (selected_asset_id_.isEmpty() || assets_.indexOf(selected_asset_id_) < 0)
    {
        selectAsset(assets_.assetIdAt(0));
    }
}

void StudioPresenter::setImportWork(const int completed, const int total, const bool active)
{
    const int clamped_total = std::max(0, total);
    const int clamped_completed = std::clamp(completed, 0, std::max(clamped_total, completed));
    if (import_work_active_ == active && import_work_completed_ == clamped_completed &&
        import_work_total_ == clamped_total)
    {
        return;
    }
    import_work_active_ = active;
    import_work_completed_ = clamped_completed;
    import_work_total_ = clamped_total;
    emit libraryWorkChanged();
}

void StudioPresenter::setCatalogOperation(QString stage, const int completed, const int total,
                                          const bool active)
{
    const int clamped_total = std::max(0, total);
    const int clamped_completed = std::clamp(completed, 0, std::max(clamped_total, completed));
    if (catalog_operation_active_ == active && catalog_operation_stage_ == stage &&
        catalog_operation_completed_ == clamped_completed &&
        catalog_operation_total_ == clamped_total)
        return;
    catalog_operation_active_ = active;
    catalog_operation_stage_ = std::move(stage);
    catalog_operation_completed_ = clamped_completed;
    catalog_operation_total_ = clamped_total;
    emit libraryWorkChanged();
}

void StudioPresenter::resetThumbnailDemand()
{
    pending_thumbnail_ids_.clear();
    if (thumbnail_request_in_flight_)
    {
        static_cast<void>(thumbnail_work_.cancel("thumbnail_demand_reset"));
        preview_work_active_ = true;
        preview_work_completed_ = 0;
        preview_work_total_ = 1;
    }
    else
    {
        preview_work_active_ = false;
        preview_work_completed_ = 0;
        preview_work_total_ = 0;
    }
    emit libraryWorkChanged();
}

void StudioPresenter::kickThumbnailDemand()
{
    if (develop_job_in_flight_ || pending_save_.has_value() || pending_preview_.has_value())
    {
        return;
    }
    if (thumbnail_request_in_flight_)
    {
        return;
    }
    if (thumbnail_work_.token().is_cancellation_requested())
    {
        thumbnail_work_ = CancellationSource{};
    }
    while (!pending_thumbnail_ids_.empty())
    {
        std::string id = std::move(pending_thumbnail_ids_.front());
        pending_thumbnail_ids_.pop_front();
        if (!assets_.assetById(qstring_from_utf8(id)) ||
            assets_.thumbnailState(id) == QLatin1String("ready") ||
            assets_.thumbnailState(id) == QLatin1String("missing") ||
            assets_.thumbnailState(id) == QLatin1String("failed"))
        {
            preview_work_completed_ = std::min(preview_work_total_, preview_work_completed_ + 1);
            continue;
        }
        thumbnail_request_in_flight_ = true;
        startThumbnailRequest(std::move(id));
        return;
    }
    if (preview_work_active_)
    {
        preview_work_active_ = false;
        preview_work_completed_ = preview_work_total_;
        emit libraryWorkChanged();
    }
}

void StudioPresenter::finishThumbnailRequest(const bool success)
{
    static_cast<void>(success);
    thumbnail_request_in_flight_ = false;
    if (preview_work_active_)
    {
        preview_work_completed_ = std::min(preview_work_total_, preview_work_completed_ + 1);
        if (pending_thumbnail_ids_.empty())
        {
            preview_work_active_ = preview_work_completed_ < preview_work_total_;
        }
        emit libraryWorkChanged();
    }
    kickThumbnailDemand();
}

void StudioPresenter::applyFolders(std::vector<FolderRecord> folders)
{
    folders_.setFolders(std::move(folders));
    emit folderChanged();
}

void StudioPresenter::applyLibrarySets(std::vector<LibrarySetRecord> sets)
{
    library_sets_.setSets(std::move(sets), query_.collection_id);
    emit folderChanged();
}

void StudioPresenter::clearLastImportQuery()
{
    query_.imported_after_unix_ms.reset();
    query_.imported_before_unix_ms.reset();
    last_import_selected_ = false;
}

void StudioPresenter::selectFolder(const QString &folder_uri)
{
    const auto next = utf8_from_qstring(folder_uri);
    const bool leaving_last_import = last_import_selected_;
    if (leaving_last_import)
        clearLastImportQuery();
    const bool leaving_set = !query_.collection_id.empty();
    query_.collection_id.clear();
    if (query_.folder_uri == next && !leaving_last_import && !leaving_set)
    {
        return;
    }
    query_.folder_uri = next;
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::selectLastImport()
{
    if (!lastImportAvailable() || last_import_selected_)
        return;
    query_.folder_uri.clear();
    query_.collection_id.clear();
    query_.imported_after_unix_ms = last_import_after_unix_ms_;
    query_.imported_before_unix_ms = last_import_before_unix_ms_;
    last_import_selected_ = true;
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::selectLibrarySet(const QString &set_id)
{
    const auto next = utf8_from_qstring(set_id);
    if (last_import_selected_)
        clearLastImportQuery();
    if (query_.collection_id == next && query_.folder_uri.empty())
        return;
    query_.folder_uri.clear();
    query_.collection_id = next;
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::createManualLibrarySet(const QString &name)
{
    if (catalog_path_.isEmpty())
        return;
    const auto ids = selected_asset_ids();
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, name = utf8_from_qstring(name), ids, revision]()
        {
            Result<LibrarySetMutation> created =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                created = service_->create_library_set(LibrarySetKind::kManual, name, std::nullopt,
                                                       ids, revision);
            }
            QMetaObject::invokeMethod(
                this,
                [this, created = std::move(created)]() mutable
                {
                    if (!created)
                    {
                        setError(qstring_from_utf8(created.error().message));
                        return;
                    }
                    observed_catalog_revision_ = created.value().revision;
                    query_.folder_uri.clear();
                    if (last_import_selected_)
                        clearLastImportQuery();
                    query_.collection_id = created.value().set.id;
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Collection created."));
                    emit folderChanged();
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createSmartLibrarySet(const QString &name)
{
    if (catalog_path_.isEmpty())
        return;
    LibraryQuery stored = query_;
    stored.collection_id.clear();
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, name = utf8_from_qstring(name), stored, revision]()
        {
            Result<LibrarySetMutation> created =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                created = service_->create_library_set(LibrarySetKind::kSmart, name, stored, {},
                                                       revision);
            }
            QMetaObject::invokeMethod(
                this,
                [this, created = std::move(created)]() mutable
                {
                    if (!created)
                    {
                        setError(qstring_from_utf8(created.error().message));
                        return;
                    }
                    observed_catalog_revision_ = created.value().revision;
                    query_.folder_uri.clear();
                    if (last_import_selected_)
                        clearLastImportQuery();
                    query_.collection_id = created.value().set.id;
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Smart collection created."));
                    emit folderChanged();
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::renameLibrarySet(const QString &set_id, const QString &name)
{
    if (catalog_path_.isEmpty())
        return;
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, set_id = utf8_from_qstring(set_id), name = utf8_from_qstring(name), revision]()
        {
            Result<LibrarySetMutation> renamed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                renamed = service_->rename_library_set(set_id, name, revision);
            QMetaObject::invokeMethod(
                this,
                [this, renamed = std::move(renamed)]() mutable
                {
                    if (!renamed)
                    {
                        setError(qstring_from_utf8(renamed.error().message));
                        return;
                    }
                    observed_catalog_revision_ = renamed.value().revision;
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Collection renamed."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::deleteLibrarySet(const QString &set_id)
{
    if (catalog_path_.isEmpty())
        return;
    const auto id = utf8_from_qstring(set_id);
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, id, revision]()
        {
            Result<std::int64_t> deleted = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                deleted = service_->delete_library_set(id, revision);
            QMetaObject::invokeMethod(
                this,
                [this, id, deleted = std::move(deleted)]() mutable
                {
                    if (!deleted)
                    {
                        setError(qstring_from_utf8(deleted.error().message));
                        return;
                    }
                    observed_catalog_revision_ = deleted.value();
                    if (query_.collection_id == id)
                        query_.collection_id.clear();
                    setStatus(
                        QCoreApplication::translate("StudioPresenter", "Collection deleted."));
                    emit folderChanged();
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::addSelectionToLibrarySet(const QString &set_id)
{
    if (catalog_path_.isEmpty() || selected_ids_.empty())
        return;
    const auto ids = selected_asset_ids();
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, set_id = utf8_from_qstring(set_id), ids, revision]()
        {
            Result<LibrarySetMutation> mutated =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                mutated = service_->add_library_set_members(set_id, ids, revision);
            QMetaObject::invokeMethod(
                this,
                [this, mutated = std::move(mutated)]() mutable
                {
                    if (!mutated)
                    {
                        setError(qstring_from_utf8(mutated.error().message));
                        return;
                    }
                    observed_catalog_revision_ = mutated.value().revision;
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Added photos to collection."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::removeSelectionFromLibrarySet(const QString &set_id)
{
    if (catalog_path_.isEmpty() || selected_ids_.empty())
        return;
    const auto ids = selected_asset_ids();
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, set_id = utf8_from_qstring(set_id), ids, revision]()
        {
            Result<LibrarySetMutation> mutated =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                mutated = service_->remove_library_set_members(set_id, ids, revision);
            QMetaObject::invokeMethod(
                this,
                [this, mutated = std::move(mutated)]() mutable
                {
                    if (!mutated)
                    {
                        setError(qstring_from_utf8(mutated.error().message));
                        return;
                    }
                    observed_catalog_revision_ = mutated.value().revision;
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Removed photos from collection."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::reloadVisibleAssets()
{
    if (catalog_path_.isEmpty())
    {
        return;
    }
    executor_.post(
        [this, query = current_query(), collapse = collapse_stacks_]()
        {
            auto listing = load_catalog_listing(service_.get(), query, collapse);
            QMetaObject::invokeMethod(
                this,
                [this, listing = std::move(listing)]() mutable
                {
                    if (import_work_active_)
                        return;
                    if (!listing.assets)
                    {
                        setError(qstring_from_utf8(listing.assets.error().message));
                        return;
                    }
                    if (!listing.folders)
                    {
                        setError(qstring_from_utf8(listing.folders.error().message));
                        return;
                    }
                    if (!listing.library_sets)
                    {
                        setError(qstring_from_utf8(listing.library_sets.error().message));
                        return;
                    }
                    applyFolders(std::move(listing.folders).value());
                    applyLibrarySets(std::move(listing.library_sets).value());
                    if (listing.capture_facets && listing.location_facets)
                        applyFacets(std::move(listing.capture_facets).value(),
                                    std::move(listing.location_facets).value());
                    auto assets = std::move(listing.assets).value();
                    if (cull_suggestion_filter_ != QStringLiteral("none"))
                    {
                        std::vector<AssetRecord> filtered;
                        filtered.reserve(assets.size());
                        for (auto &asset : assets)
                        {
                            if (cull_suggestion_asset_ids_.count(asset.id) > 0U)
                            {
                                filtered.push_back(std::move(asset));
                            }
                        }
                        listing.total = filtered.size();
                        listing.has_more = false;
                        assets = std::move(filtered);
                    }
                    applyAssets(std::move(assets), true, std::move(listing.thumbnail_urls),
                                std::move(listing.thumbnail_states), listing.total,
                                listing.has_more);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::loadNextLibraryPage()
{
    if (catalog_path_.isEmpty() || busy_ || library_page_in_flight_ || !library_has_more_)
        return;
    const auto offset = library_next_offset_;
    const auto previous =
        offset > 0U ? assets_.assetIdAt(static_cast<int>(offset - 1U)) : QString{};
    if (offset > 0U && previous.isEmpty())
        return;
    requestLibraryPage(offset,
                       previous.isEmpty() ? std::nullopt :
                                            std::optional<std::string>{utf8_from_qstring(previous)},
                       true);
}

void StudioPresenter::ensureLibraryRow(const int row)
{
    if (row < 0 || row >= libraryTotal() || assets_.rowLoaded(row) || catalog_path_.isEmpty() ||
        busy_)
        return;
    const auto offset =
        static_cast<std::size_t>(row) / kLibraryPageDefaultSize * kLibraryPageDefaultSize;
    requestLibraryPage(offset, std::nullopt, false);
}

void StudioPresenter::requestLibraryPage(const std::size_t offset,
                                         std::optional<std::string> cursor, const bool sequential)
{
    if (library_page_in_flight_)
    {
        pending_library_page_offset_ = offset;
        return;
    }
    const auto generation = library_query_generation_;
    const auto query = current_query();
    const auto known_total = library_total_;
    const auto collapse = collapse_stacks_;
    library_page_in_flight_ = true;
    pending_library_page_offset_.reset();
    emit libraryWorkChanged();
    executor_.post(
        [this, offset, generation, query, cursor = std::move(cursor), known_total, sequential,
         collapse]
        {
            Result<LibraryPage> page = make_error(ErrorCode::kIo, "Catalog session is closed");
            CatalogListing listing;
            if (service_ != nullptr)
            {
                LibraryPageRequest request;
                request.query = query;
                request.collapse_stacks = collapse;
                request.offset = offset;
                request.after_asset_id = cursor;
                request.known_total = known_total;
                page = service_->list_assets_page(request);
                if (page)
                {
                    listing.total = page.value().total;
                    listing.has_more = page.value().has_more;
                    listing.assets = page.value().assets;
                    fill_thumbnail_maps(*service_, listing);
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, generation, sequential, page = std::move(page),
                 listing = std::move(listing)]() mutable
                {
                    if (generation != library_query_generation_)
                        return;
                    library_page_in_flight_ = false;
                    emit libraryWorkChanged();
                    if (!page)
                    {
                        setError(qstring_from_utf8(page.error().message));
                        return;
                    }
                    assets_.setPage(page.value().offset, std::move(listing.assets).value(),
                                    std::move(listing.thumbnail_urls),
                                    std::move(listing.thumbnail_states), page.value().total);
                    library_total_ = page.value().total;
                    if (sequential)
                    {
                        library_next_offset_ = page.value().offset + page.value().assets.size();
                        library_has_more_ = page.value().has_more;
                    }
                    emit thumbnailsChanged();
                    emit filterChanged();
                    if (pending_library_page_offset_)
                    {
                        const auto pending = *pending_library_page_offset_;
                        pending_library_page_offset_.reset();
                        if (!assets_.rowLoaded(static_cast<int>(pending)))
                            requestLibraryPage(pending, std::nullopt, false);
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::start_catalog_revision_watch(const std::int64_t revision)
{
    observed_catalog_revision_ = revision;
    catalog_poll_in_flight_ = false;
    if (catalog_revision_timer_ != nullptr)
    {
        catalog_revision_timer_->start();
    }
    if (backup_schedule_timer_ != nullptr)
        backup_schedule_timer_->start();
    QTimer::singleShot(0, this, &StudioPresenter::checkScheduledBackup);
}

void StudioPresenter::pollCatalogRevision()
{
    if (catalog_path_.isEmpty() || busy_ || import_work_active_ || catalog_poll_in_flight_ ||
        develop_job_in_flight_ || pending_save_ || pending_preview_)
    {
        return;
    }
    catalog_poll_in_flight_ = true;
    const auto query = current_query();
    const auto selected = utf8_from_qstring(selected_asset_id_);
    const auto observed = observed_catalog_revision_;
    const auto collapse = collapse_stacks_;
    executor_.post(
        [this, query, selected, observed, collapse]()
        {
            Result<CatalogSnapshot> snapshot =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            CatalogListing listing;
            Result<Recipe> recipe = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            bool changed = false;
            if (service_ != nullptr)
            {
                snapshot = service_->snapshot();
                if (snapshot && snapshot.value().revision != observed)
                {
                    changed = true;
                    listing = load_catalog_listing(service_.get(), query, collapse);
                    if (!selected.empty())
                    {
                        recipe = service_->load_recipe(selected);
                        history = service_->list_recipe_history(selected);
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, snapshot = std::move(snapshot), listing = std::move(listing),
                 recipe = std::move(recipe), history = std::move(history), selected,
                 changed]() mutable
                {
                    catalog_poll_in_flight_ = false;
                    if (catalog_path_.isEmpty() || busy_ || import_work_active_ ||
                        develop_job_in_flight_ || pending_save_ || pending_preview_)
                    {
                        return;
                    }
                    if (!snapshot)
                    {
                        setError(catalog_error_text(snapshot.error()));
                        return;
                    }
                    if (!changed || snapshot.value().revision == observed_catalog_revision_)
                    {
                        return;
                    }
                    if (!listing.assets)
                    {
                        setError(catalog_error_text(listing.assets.error()));
                        return;
                    }
                    if (!listing.folders)
                    {
                        setError(catalog_error_text(listing.folders.error()));
                        return;
                    }
                    const QString previous_selection = selected_asset_id_;
                    applyFolders(std::move(listing.folders).value());
                    if (listing.capture_facets && listing.location_facets)
                        applyFacets(std::move(listing.capture_facets).value(),
                                    std::move(listing.location_facets).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
                    observed_catalog_revision_ = snapshot.value().revision;
                    if (selected.empty() || selected_asset_id_ != previous_selection ||
                        utf8_from_qstring(selected_asset_id_) != selected)
                    {
                        setStatus(QCoreApplication::translate(
                            "StudioPresenter", "Library updated from another client."));
                        return;
                    }
                    break_history_coalescing();
                    if (history)
                    {
                        apply_recipe_history(history.value());
                    }
                    else
                    {
                        recipe_history_.clear();
                        recipe_history_entries_.clear();
                    }
                    if (!recipe)
                    {
                        setError(catalog_error_text(recipe.error()));
                        sync_active_history();
                        emit editChanged();
                        return;
                    }
                    auto params = develop_from_recipe(recipe.value());
                    if (!params)
                    {
                        setError(catalog_error_text(params.error()));
                        sync_active_history();
                        emit editChanged();
                        return;
                    }
                    const bool same_recipe = params.value() == develop_ &&
                                             params.value() == saved_develop_ && !crop_tool_active_;
                    if (!same_recipe)
                    {
                        undo_stack_.clear();
                        redo_stack_.clear();
                        before_after_ = false;
                        crop_tool_active_ = false;
                        crop_guide_ready_ = false;
                        develop_ = params.value();
                        saved_develop_ = develop_;
                        develop_loaded_ = true;
                        develop_load_error_.clear();
                        static_cast<void>(develop_preview_owner_.supersede("catalog_revision"));
                        pending_preview_.reset();
                        requestPreviewForSelection();
                    }
                    else
                    {
                        saved_develop_ = params.value();
                        develop_loaded_ = true;
                        develop_load_error_.clear();
                    }
                    sync_active_history();
                    emit editChanged();
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Library updated from another client."));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createCatalog(const QUrl &file_url)
{
    if (busy_ || import_work_active_)
    {
        return;
    }
    const QString local = file_url.toLocalFile();
    if (local.isEmpty())
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    closeImportPage();
    setStatus(QCoreApplication::translate("StudioPresenter", "Creating library…"));
    const auto path = utf8_from_qstring(local);
    LibraryQuery initial_query = current_query();
    if (last_import_selected_)
    {
        initial_query.imported_after_unix_ms.reset();
        initial_query.imported_before_unix_ms.reset();
    }
    executor_.post(
        [this, path, initial_query, collapse = collapse_stacks_]()
        {
            QString failure;
            CatalogListing listing;
            auto built = make_catalog_service(path, true);
            if (!built)
            {
                failure = catalog_error_text(built.error());
            }
            else
            {
                listing = load_catalog_listing(built.value().get(), initial_query, collapse);
                if (!listing.assets)
                {
                    failure = catalog_error_text(listing.assets.error());
                }
                else if (!listing.folders)
                {
                    failure = catalog_error_text(listing.folders.error());
                }
                else
                {
                    service_ = std::move(built).value();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, initial_query, failure = std::move(failure),
                 listing = std::move(listing)]() mutable
                {
                    setBusy(false);
                    if (!failure.isEmpty())
                    {
                        setError(failure);
                        setStatus(QCoreApplication::translate("StudioPresenter", "Create failed."));
                        return;
                    }
                    query_ = initial_query;
                    last_import_after_unix_ms_.reset();
                    last_import_before_unix_ms_.reset();
                    last_import_count_ = 0U;
                    last_import_selected_ = false;
                    catalog_path_ = qstring_from_utf8(path);
                    thumbnail_requests_.clear();
                    clear_thumbnail_presentation_cache();
                    emit catalogChanged();
                    setError({});
                    setStatus(QCoreApplication::translate(
                        "StudioPresenter", "Library created. Import photos or a folder."));
                    applyFolders(std::move(listing.folders).value());
                    if (listing.capture_facets && listing.location_facets)
                        applyFacets(std::move(listing.capture_facets).value(),
                                    std::move(listing.location_facets).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
                    start_catalog_revision_watch(listing.revision);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::openCatalog(const QUrl &file_url)
{
    if (busy_ || import_work_active_)
    {
        return;
    }
    const QString local = file_url.toLocalFile();
    if (local.isEmpty())
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    closeImportPage();
    setStatus(QCoreApplication::translate("StudioPresenter", "Opening library…"));
    const auto path = utf8_from_qstring(local);
    LibraryQuery initial_query = current_query();
    if (last_import_selected_)
    {
        initial_query.imported_after_unix_ms.reset();
        initial_query.imported_before_unix_ms.reset();
    }
    executor_.post(
        [this, path, initial_query, collapse = collapse_stacks_]()
        {
            QString failure;
            CatalogListing listing;
            auto built = make_catalog_service(path, false);
            if (!built)
            {
                LOG_ERROR(logger(), "catalog open failed path={} message={} action={} qt_error={}",
                          path, built.error().message,
                          built.error().context.contains("action") ?
                              built.error().context.at("action") :
                              "",
                          built.error().context.contains("qt_error") ?
                              built.error().context.at("qt_error") :
                              "");
                failure = catalog_error_text(built.error());
            }
            else
            {
                listing = load_catalog_listing(built.value().get(), initial_query, collapse);
                if (!listing.assets)
                {
                    LOG_ERROR(logger(),
                              "catalog list_assets failed path={} message={} action={} qt_error={}",
                              path, listing.assets.error().message,
                              listing.assets.error().context.contains("action") ?
                                  listing.assets.error().context.at("action") :
                                  "",
                              listing.assets.error().context.contains("qt_error") ?
                                  listing.assets.error().context.at("qt_error") :
                                  "");
                    failure = catalog_error_text(listing.assets.error());
                }
                else if (!listing.folders)
                {
                    failure = catalog_error_text(listing.folders.error());
                }
                else
                {
                    service_ = std::move(built).value();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, initial_query, failure = std::move(failure),
                 listing = std::move(listing)]() mutable
                {
                    setBusy(false);
                    if (!failure.isEmpty())
                    {
                        setError(failure);
                        setStatus(QCoreApplication::translate("StudioPresenter", "Open failed."));
                        return;
                    }
                    query_ = initial_query;
                    last_import_after_unix_ms_.reset();
                    last_import_before_unix_ms_.reset();
                    last_import_count_ = 0U;
                    last_import_selected_ = false;
                    catalog_path_ = qstring_from_utf8(path);
                    reload_presets();
                    selected_asset_id_.clear();
                    clear_displayed_preview();
                    thumbnail_requests_.clear();
                    clear_thumbnail_presentation_cache();
                    emit catalogChanged();
                    emit selectionChanged();
                    emit previewChanged();
                    emit thumbnailsChanged();
                    setError({});
                    setStatus(QCoreApplication::translate("StudioPresenter", "Library opened."));
                    applyFolders(std::move(listing.folders).value());
                    if (listing.capture_facets && listing.location_facets)
                        applyFacets(std::move(listing.capture_facets).value(),
                                    std::move(listing.location_facets).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
                    start_catalog_revision_watch(listing.revision);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::importFolder(const QUrl &folder_url)
{
    importFiles(QList<QUrl>{folder_url});
}

void StudioPresenter::createCatalogFromPath(const QString &path)
{
    createCatalog(url_from_dialog_path(path));
}

void StudioPresenter::openCatalogFromPath(const QString &path)
{
    openCatalog(url_from_dialog_path(path));
}

void StudioPresenter::importFilePaths(const QStringList &paths)
{
    QList<QUrl> urls;
    urls.reserve(paths.size());
    for (const auto &path : paths)
    {
        const QUrl url = url_from_dialog_path(path);
        if (url.isValid() && !url.isEmpty())
        {
            urls.push_back(url);
        }
    }
    importFiles(urls);
}

void StudioPresenter::importFolderFromPath(const QString &path)
{
    importFolder(url_from_dialog_path(path));
}

void StudioPresenter::importFiles(const QList<QUrl> &files)
{
    if (busy_ || import_work_active_ || catalog_path_.isEmpty())
    {
        return;
    }
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(files.size()));
    for (const auto &file : files)
    {
        const QString local = file.toLocalFile();
        if (!local.isEmpty())
        {
            paths.push_back(utf8_from_qstring(local));
        }
    }
    if (paths.empty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "No local files selected."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Scanning folder…"));
    closeImportPage();
    import_skip_existing_ = false;
    pending_import_content_hashes_.clear();
    pending_import_destination_.clear();
    import_preference_error_.clear();
    setImportWork(0, 0, true);
    import_operation_ = CancellationSource{};
    const auto cancellation = import_operation_.token();
    import_query_snapshot_ = current_query();
    executor_.post(
        [this, paths = std::move(paths), cancellation]
        {
            Result<std::vector<std::string>> enumerated =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                enumerated = service_->enumerate_import_inputs(paths, cancellation);
            QMetaObject::invokeMethod(
                this,
                [this, enumerated = std::move(enumerated)]() mutable
                {
                    if (!enumerated)
                    {
                        setImportWork(0, 0, false);
                        setError(qstring_from_utf8(enumerated.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        return;
                    }
                    pending_import_paths_ = std::move(enumerated).value();
                    import_results_.clear();
                    import_results_.reserve(pending_import_paths_.size());
                    import_next_index_ = 0U;
                    setImportWork(0, static_cast<int>(pending_import_paths_.size()), true);
                    if (pending_import_paths_.empty())
                    {
                        finishImportBatch();
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter", "Importing 0 / %1…")
                                  .arg(pending_import_paths_.size()));
                    startNextImportItem();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::startNextImportItem()
{
    if (!import_work_active_)
        return;
    if (import_operation_.token().is_cancellation_requested() ||
        import_next_index_ >= pending_import_paths_.size())
    {
        finishImportBatch();
        return;
    }
    const auto path = pending_import_paths_[import_next_index_];
    const auto cancellation = import_operation_.token();
    const auto policy =
        import_defer_previews_ ? pending_import_preview_policy_ : ImportPreviewPolicy::kMinimal;
    const bool defer = import_defer_previews_;
    const bool skip_existing = import_skip_existing_;
    const auto hash = pending_import_content_hashes_.find(path);
    const std::string expected_hash =
        hash == pending_import_content_hashes_.end() ? std::string{} : hash->second;
    executor_.post(
        [this, path, cancellation, policy, defer, skip_existing, expected_hash]
        {
            Result<ImportItemResult> imported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                imported = service_->import_one(path, cancellation, policy, defer, skip_existing,
                                                expected_hash);
            QMetaObject::invokeMethod(
                this,
                [this, path, imported = std::move(imported)]() mutable
                {
                    ImportItemResult item;
                    if (imported)
                    {
                        item = std::move(imported).value();
                    }
                    else
                    {
                        item.status = ImportItemStatus::kFailed;
                        item.input_path = path;
                        item.error = imported.error();
                    }
                    const auto row = static_cast<int>(import_next_index_);
                    import_results_.push_back(item);
                    ++import_next_index_;
                    setImportWork(static_cast<int>(import_next_index_),
                                  static_cast<int>(pending_import_paths_.size()), true);
                    setStatus(QCoreApplication::translate("StudioPresenter", "Importing %1 / %2…")
                                  .arg(import_next_index_)
                                  .arg(pending_import_paths_.size()));
                    publishImportItem(item, row);
                    startNextImportItem();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::finishImportBatch()
{
    if (!import_work_active_)
        return;
    import_gallery_placeholders_ = false;
    import_skip_existing_ = false;
    pending_import_content_hashes_.clear();
    import_defer_previews_ = false;
    const bool cancelled = import_operation_.token().is_cancellation_requested();
    const auto completed = import_results_.size();
    const auto total = pending_import_paths_.size();
    auto results = std::move(import_results_);
    pending_import_paths_.clear();
    import_next_index_ = 0U;
    LibraryQuery query = import_query_snapshot_;
    std::optional<std::int64_t> imported_after;
    std::optional<std::int64_t> imported_before;
    std::size_t imported_count = 0U;
    for (const auto &item : results)
    {
        if (item.status != ImportItemStatus::kImported || !item.asset)
            continue;
        const auto created = item.asset->created_unix_ms;
        imported_after = imported_after ? std::min(*imported_after, created) : created;
        imported_before = imported_before ? std::max(*imported_before, created) : created;
        ++imported_count;
    }
    if (imported_count > 0U)
    {
        query.folder_uri.clear();
        query.imported_after_unix_ms = imported_after;
        query.imported_before_unix_ms = imported_before;
    }
    executor_.post(
        [this, results = std::move(results), query, imported_after, imported_before, imported_count,
         cancelled, completed, total, collapse = collapse_stacks_]() mutable
        {
            auto listing = load_catalog_listing(service_.get(), query, collapse);
            QMetaObject::invokeMethod(
                this,
                [this, results = std::move(results), listing = std::move(listing), cancelled, query,
                 imported_after, imported_before, imported_count, completed, total]() mutable
                {
                    setImportWork(static_cast<int>(completed), static_cast<int>(total), false);
                    if (!listing.assets)
                    {
                        setError(qstring_from_utf8(listing.assets.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        return;
                    }
                    if (!listing.folders)
                    {
                        setError(qstring_from_utf8(listing.folders.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        return;
                    }
                    QString first_error;
                    for (const auto &item : results)
                        if (first_error.isEmpty() && item.error)
                            first_error = qstring_from_utf8(item.error->message);
                    if (!import_preference_error_.isEmpty())
                        first_error += (first_error.isEmpty() ? QString{} : QStringLiteral("\n")) +
                                       import_preference_error_;
                    setError(first_error);
                    setStatus(cancelled ?
                                  QCoreApplication::translate(
                                      "StudioPresenter", "Import cancelled after %1 of %2 photos.")
                                      .arg(completed)
                                      .arg(total) :
                                  describe_import(results));
                    if (listing.revision >= 0)
                        observed_catalog_revision_ = listing.revision;
                    if (imported_count > 0U)
                    {
                        query_ = query;
                        last_import_after_unix_ms_ = imported_after;
                        last_import_before_unix_ms_ = imported_before;
                        last_import_count_ = imported_count;
                        last_import_selected_ = true;
                        selected_asset_id_.clear();
                        selection_anchor_id_.clear();
                        selected_ids_.clear();
                        assets_.setSelectedIds({});
                    }
                    applyFolders(std::move(listing.folders).value());
                    if (listing.capture_facets && listing.location_facets)
                        applyFacets(std::move(listing.capture_facets).value(),
                                    std::move(listing.location_facets).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
                    if (!pending_import_preview_ids_.empty())
                    {
                        import_preview_work_active_ = true;
                        import_preview_work_completed_ = 0;
                        import_preview_work_total_ =
                            static_cast<int>(pending_import_preview_ids_.size());
                        emit libraryWorkChanged();
                        startNextImportPreview();
                    }
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
