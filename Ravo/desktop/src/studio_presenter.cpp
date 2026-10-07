#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_import_workspace.h"
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
    std::size_t offset = 0U;
};

void fill_thumbnail_maps(CatalogService &service, CatalogListing &listing)
{
    if (!listing.assets)
    {
        return;
    }
    auto snapshot = service.library().snapshot();
    std::vector<std::string> asset_ids;
    asset_ids.reserve(listing.assets.value().size());
    for (const auto &asset : listing.assets.value())
        asset_ids.push_back(asset.id);
    auto previews = service.library().list_previews_for_assets(asset_ids);
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
                                    const bool collapse_stacks,
                                    const std::optional<std::string> &anchor = std::nullopt)
{
    CatalogListing listing;
    if (service == nullptr)
    {
        return listing;
    }
    auto snapshot = service->library().snapshot();
    if (snapshot)
    {
        listing.revision = snapshot.value().revision;
    }
    LibraryPageRequest page_request;
    page_request.query = query;
    page_request.collapse_stacks = collapse_stacks;
    page_request.around_asset_id = anchor;
    auto page = service->library().list_assets_page(page_request);
    // An obsolete bookmark is an explicit supported outcome. Real query/SQL
    // failures must still abort opening; only an absent anchor uses page zero.
    if (!page && page.error().code == ErrorCode::kNotFound &&
        page.error().context.contains("reason") &&
        page.error().context.at("reason") == "library_anchor_missing")
    {
        page_request.around_asset_id.reset();
        page = service->library().list_assets_page(page_request);
    }
    if (page)
    {
        listing.total = page.value().total;
        listing.has_more = page.value().has_more;
        listing.offset = page.value().offset;
        listing.assets = std::move(page).value().assets;
    }
    else
    {
        listing.assets = page.error();
    }
    listing.folders = service->library().list_folders();
    listing.library_sets = service->library().list_library_sets();
    listing.capture_facets = service->metadata().list_capture_facets(query);
    listing.location_facets = service->metadata().list_location_facets(query);
    fill_thumbnail_maps(*service, listing);
    return listing;
}

} // namespace

StudioPresenter::~StudioPresenter()
{
    persistLibraryPosition();
    static_cast<void>(thumbnail_presentation_cancel_.cancel("window_closed"));
    static_cast<void>(library_reload_cancel_.cancel("window_closed"));
    pending_thumbnail_presentations_.clear();
    thumbnail_presentation_executor_.request_stop();
    thumbnail_presentation_executor_.wait();
    // Cancel borrowed foreground ROI work before draining that executor.
    inspect_.shutdown();
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
    if (import_workspace_)
        import_workspace_->shutdown();
    develop_presenter_->shutdown();
    // Catalog construction on the foreground owner may still be opening the
    // import session. Finish that handoff before stopping its destination.
    executor_.submit([] {});
    import_workspace_->shutdownWorker();
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

int StudioPresenter::thumbnailSize() const noexcept
{
    return thumbnail_size_;
}

LibraryQuery StudioPresenter::current_query() const
{
    return library_.query();
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
                                  const std::size_t total, const bool has_more,
                                  const std::size_t offset)
{
    // Clear old listing demand before modelReset can synchronously admit the
    // replacement delegates' requests. Clearing it afterwards strands them.
    resetThumbnailDemand();
    static_cast<void>(thumbnail_presentation_cancel_.cancel("library_listing_replaced"));
    thumbnail_presentation_cancel_ = CancellationSource{};
    pending_thumbnail_presentations_.clear();
    thumbnail_presentation_revisions_.clear();
    thumbnail_repair_attempts_.clear();
    ++library_query_generation_;
    const QString previous = selected_asset_id_;
    const auto incoming_thumbs = thumbnail_urls;
    const auto incoming_states = thumbnail_states;
    if (offset == 0U)
        assets_.setAssets(std::move(assets), std::move(thumbnail_urls), std::move(thumbnail_states),
                          total);
    else
    {
        assets_.setAssets({}, {}, {}, total);
        assets_.setPage(offset, std::move(assets), std::move(thumbnail_urls),
                        std::move(thumbnail_states), total);
    }
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
    library_next_offset_ = offset + static_cast<std::size_t>(assets_.loadedCount());
    emit thumbnailsChanged();
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
        inspect_.clear_displayed_preview();
        inspect_.setPreviewLoading(false);
        emit selectionChanged();
        inspect_.notifyPreviewChanged();
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
    if (develop_presenter_->state().develop_job_in_flight_ ||
        develop_presenter_->state().pending_save_.has_value() ||
        develop_presenter_->state().pending_preview_.has_value())
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
    library_sets_.setSets(std::move(sets), library_.query().collection_id);
    emit folderChanged();
}

void StudioPresenter::clearLastImportQuery()
{
    library_.setImportScope({}, {});
    last_import_selected_ = false;
}

void StudioPresenter::selectFolder(const QString &folder_uri)
{
    const auto next = utf8_from_qstring(folder_uri);
    const bool leaving_last_import = last_import_selected_;
    if (leaving_last_import)
        clearLastImportQuery();
    const bool leaving_set = !library_.query().collection_id.empty();
    library_.setCollectionScope({});
    if (library_.query().folder_uri == next && !leaving_last_import && !leaving_set)
    {
        return;
    }
    library_.setFolderScope(next);
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::selectLastImport()
{
    if (!lastImportAvailable() || last_import_selected_)
        return;
    library_.setFolderScope({});
    library_.setCollectionScope({});
    library_.setImportScope(last_import_after_unix_ms_, last_import_before_unix_ms_);
    last_import_selected_ = true;
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::selectLibrarySet(const QString &set_id)
{
    const auto next = utf8_from_qstring(set_id);
    if (last_import_selected_)
        clearLastImportQuery();
    if (library_.query().collection_id == next && library_.query().folder_uri.empty())
        return;
    library_.setFolderScope({});
    library_.setCollectionScope(next);
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
                created = service_->library().create_library_set(LibrarySetKind::kManual, name,
                                                                 std::nullopt, ids, revision);
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
                    library_.setFolderScope({});
                    if (last_import_selected_)
                        clearLastImportQuery();
                    library_.setCollectionScope(created.value().set.id);
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
    LibraryQuery stored = library_.query();
    stored.collection_id.clear();
    const auto revision = observed_catalog_revision_;
    executor_.post(
        [this, name = utf8_from_qstring(name), stored, revision]()
        {
            Result<LibrarySetMutation> created =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                created = service_->library().create_library_set(LibrarySetKind::kSmart, name,
                                                                 stored, {}, revision);
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
                    library_.setFolderScope({});
                    if (last_import_selected_)
                        clearLastImportQuery();
                    library_.setCollectionScope(created.value().set.id);
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
                renamed = service_->library().rename_library_set(set_id, name, revision);
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
                deleted = service_->library().delete_library_set(id, revision);
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
                    if (library_.query().collection_id == id)
                        library_.setCollectionScope({});
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
                mutated = service_->library().add_library_set_members(set_id, ids, revision);
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
                mutated = service_->library().remove_library_set_members(set_id, ids, revision);
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
    pending_library_selection_.reset();
    if (catalog_path_.isEmpty())
    {
        return;
    }
    resetThumbnailDemand();
    static_cast<void>(library_reload_cancel_.cancel("library_query_replaced"));
    library_reload_cancel_ = CancellationSource{};
    const auto cancellation = library_reload_cancel_.token();
    const auto generation = ++library_query_generation_;
    executor_.post(
        [this, query = current_query(), collapse = collapse_stacks_, cancellation, generation]()
        {
            if (cancellation.is_cancellation_requested())
                return;
            auto listing = load_catalog_listing(service_.get(), query, collapse);
            QMetaObject::invokeMethod(
                this,
                [this, listing = std::move(listing), cancellation, generation]() mutable
                {
                    if (import_workspace_->importWorkActive() ||
                        cancellation.is_cancellation_requested() ||
                        generation != library_query_generation_)
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
                        library_.apply(std::move(listing.capture_facets).value(),
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
        },
        TaskPriority::kForeground);
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
                page = service_->library().list_assets_page(request);
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
                        pending_library_selection_.reset();
                        setError(qstring_from_utf8(page.error().message));
                        return;
                    }
                    const auto incoming_thumbs = listing.thumbnail_urls;
                    assets_.setPage(page.value().offset, std::move(listing.assets).value(),
                                    std::move(listing.thumbnail_urls),
                                    std::move(listing.thumbnail_states), page.value().total);
                    for (const auto &[id, url] : incoming_thumbs)
                    {
                        if (url.isLocalFile() && assets_.assetById(qstring_from_utf8(id)))
                            remember_thumbnail_base(id, url.toLocalFile(), ColorProfileState{},
                                                    QStringLiteral("ready"));
                    }
                    library_total_ = page.value().total;
                    if (sequential)
                    {
                        library_next_offset_ = page.value().offset + page.value().assets.size();
                        library_has_more_ = page.value().has_more;
                    }
                    emit thumbnailsChanged();
                    emit filterChanged();
                    completePendingLibrarySelection();
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
    if (catalog_path_.isEmpty() || busy_ || import_workspace_->importWorkActive() ||
        catalog_poll_in_flight_ || develop_presenter_->state().develop_job_in_flight_ ||
        develop_presenter_->state().pending_save_ || develop_presenter_->state().pending_preview_)
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
                snapshot = service_->library().snapshot();
                if (snapshot && snapshot.value().revision != observed)
                {
                    changed = true;
                    listing = load_catalog_listing(service_.get(), query, collapse);
                    if (!selected.empty())
                    {
                        recipe = service_->develop().load_recipe(selected);
                        history = service_->develop().list_recipe_history(selected);
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
                    if (catalog_path_.isEmpty() || busy_ || import_workspace_->importWorkActive() ||
                        develop_presenter_->state().develop_job_in_flight_ ||
                        develop_presenter_->state().pending_save_ ||
                        develop_presenter_->state().pending_preview_)
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
                        library_.apply(std::move(listing.capture_facets).value(),
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
                    develop_presenter_->break_history_coalescing();
                    develop_presenter_->acceptLoadedHistory(history);
                    if (!recipe)
                    {
                        setError(catalog_error_text(recipe.error()));
                        develop_presenter_->sync_active_history();
                        develop_presenter_->refreshContextProjection();
                        return;
                    }
                    auto params = develop_from_recipe(recipe.value());
                    if (!params)
                    {
                        setError(catalog_error_text(params.error()));
                        develop_presenter_->sync_active_history();
                        develop_presenter_->refreshContextProjection();
                        return;
                    }
                    develop_presenter_->acceptExternalRecipe(recipe.value(),
                                                             std::move(params).value());
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Library updated from another client."));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createCatalog(const QUrl &file_url)
{
    if (busy_ || import_workspace_->importWorkActive())
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
    persistLibraryPosition();
    setBusy(true);
    setError({});
    import_workspace_->closeImportPage();
    setStatus(QCoreApplication::translate("StudioPresenter", "Creating library…"));
    import_workspace_->cancelImportPreviews();
    const auto path = utf8_from_qstring(local);
    const LibraryQuery initial_query;
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
                listing =
                    load_catalog_listing(built.value().service.get(), initial_query, collapse);
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
                    auto ready = import_workspace_->importWorker().open(
                        path, built.value().cache, built.value().recovery_publication_mutex);
                    if (!ready)
                        failure = catalog_error_text(ready.error());
                    else
                        service_ = std::move(built).value().service;
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
                    library_.replaceQuery(initial_query);
                    last_import_after_unix_ms_.reset();
                    last_import_before_unix_ms_.reset();
                    last_import_count_ = 0U;
                    last_import_selected_ = false;
                    catalog_path_ = qstring_from_utf8(path);
                    import_workspace_->catalogReplaced();
                    inspect_.clearDecodedImages();
                    thumbnail_requests_.clear();
                    clear_thumbnail_presentation_cache();
                    emit catalogChanged();
                    setError({});
                    setStatus(QCoreApplication::translate(
                        "StudioPresenter", "Library created. Import photos or a folder."));
                    applyFolders(std::move(listing.folders).value());
                    if (listing.capture_facets && listing.location_facets)
                        library_.apply(std::move(listing.capture_facets).value(),
                                       std::move(listing.location_facets).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
                    setBrowseMode(QStringLiteral("grid"));
                    start_catalog_revision_watch(listing.revision);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::openCatalog(const QUrl &file_url)
{
    if (busy_ || import_workspace_->importWorkActive())
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
    import_workspace_->closeImportPage();
    setStatus(QCoreApplication::translate("StudioPresenter", "Opening library…"));
    import_workspace_->cancelImportPreviews();
    const auto path = utf8_from_qstring(local);
    LibraryQuery initial_query = current_query();
    if (last_import_selected_)
    {
        initial_query.imported_after_unix_ms.reset();
        initial_query.imported_before_unix_ms.reset();
    }
    auto restored_query = beginLibraryResume(local, std::move(initial_query));
    if (!restored_query)
    {
        setBusy(false);
        setError(qstring_from_utf8(restored_query.error().message));
        return;
    }
    initial_query = std::move(restored_query).value();
    executor_.post(
        [this, path, initial_query, collapse = resumeCollapseStacks(), anchor = resumeAssetId()]()
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
                listing = load_catalog_listing(built.value().service.get(), initial_query, collapse,
                                               anchor);
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
                    auto ready = import_workspace_->importWorker().open(
                        path, built.value().cache, built.value().recovery_publication_mutex);
                    if (!ready)
                        failure = catalog_error_text(ready.error());
                    else
                        service_ = std::move(built).value().service;
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
                        finishLibraryResume(false);
                        setError(failure);
                        setStatus(QCoreApplication::translate("StudioPresenter", "Open failed."));
                        return;
                    }
                    library_.replaceQuery(initial_query);
                    collapse_stacks_ = resumeCollapseStacks();
                    last_import_after_unix_ms_.reset();
                    last_import_before_unix_ms_.reset();
                    last_import_count_ = 0U;
                    last_import_selected_ = false;
                    catalog_path_ = qstring_from_utf8(path);
                    import_workspace_->catalogReplaced();
                    inspect_.clearDecodedImages();
                    develop_presenter_->reload_presets();
                    selected_asset_id_.clear();
                    selected_ids_.clear();
                    selection_anchor_id_.clear();
                    inspect_.clear_displayed_preview();
                    thumbnail_requests_.clear();
                    clear_thumbnail_presentation_cache();
                    emit catalogChanged();
                    emit selectionChanged();
                    inspect_.notifyPreviewChanged();
                    emit thumbnailsChanged();
                    setError({});
                    setStatus(QCoreApplication::translate("StudioPresenter", "Library opened."));
                    applyFolders(std::move(listing.folders).value());
                    if (listing.capture_facets && listing.location_facets)
                        library_.apply(std::move(listing.capture_facets).value(),
                                       std::move(listing.location_facets).value());
                    applyAssets(std::move(listing.assets).value(), false,
                                std::move(listing.thumbnail_urls),
                                std::move(listing.thumbnail_states), listing.total,
                                listing.has_more, listing.offset);
                    finishLibraryResume(true);
                    start_catalog_revision_watch(listing.revision);
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::createCatalogFromPath(const QString &path)
{
    createCatalog(url_from_dialog_path(path));
}

void StudioPresenter::openCatalogFromPath(const QString &path)
{
    openCatalog(url_from_dialog_path(path));
}

void StudioPresenter::finishImportPresentation(StudioImportWorkspace::BatchCompletion batch)
{
    const auto generation = batch.generation;
    auto results = std::move(batch.results);
    const auto query = batch.query;
    const auto imported_after = batch.imported_after;
    const auto imported_before = batch.imported_before;
    const auto imported_count = batch.imported_count;
    const auto cancelled = batch.cancelled;
    const auto completed = batch.completed;
    const auto total = batch.total;
    const auto preference_error = batch.preference_error;
    executor_.post(
        [this, results = std::move(results), query, imported_after, imported_before, imported_count,
         cancelled, completed, total, generation, preference_error,
         collapse = collapse_stacks_]() mutable
        {
            auto listing = load_catalog_listing(service_.get(), query, collapse);
            QMetaObject::invokeMethod(
                this,
                [this, results = std::move(results), listing = std::move(listing), cancelled, query,
                 imported_after, imported_before, imported_count, completed, total, generation,
                 preference_error]() mutable
                {
                    if (!import_workspace_->finishPublication(generation, completed, total))
                        return;
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
                    if (!preference_error.isEmpty())
                        first_error += (first_error.isEmpty() ? QString{} : QStringLiteral("\n")) +
                                       preference_error;
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
                        library_.replaceQuery(query);
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
                        library_.apply(std::move(listing.capture_facets).value(),
                                       std::move(listing.location_facets).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
                    import_workspace_->startDeferredPreviews();
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
