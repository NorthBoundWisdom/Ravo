#include "ravo/desktop/studio_presenter.h"

#include "ravo/desktop/export_option_conversion.h"

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

CatalogListing load_catalog_listing(CatalogService *service, const LibraryQuery &query)
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
    fill_thumbnail_maps(*service, listing);
    return listing;
}

} // namespace

StudioPresenter::StudioPresenter(QObject *parent)
    : QObject(parent)
    , assets_(this)
    , folders_(this)
    , import_candidates_(this)
{
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
        if (!asset || asset->import_state == kImportStateMissing)
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

QString StudioPresenter::ratingFilterMode() const
{
    return rating_mode_name(query_.rating_mode);
}

int StudioPresenter::ratingFilterValue() const noexcept
{
    return query_.rating_value;
}

QStringList StudioPresenter::colorFilters() const
{
    QStringList labels;
    labels.reserve(static_cast<qsizetype>(query_.color_labels.size()));
    for (const auto label : query_.color_labels)
    {
        labels.push_back(qstring_from_utf8(color_label_name(label)));
    }
    return labels;
}

QString StudioPresenter::rejectFilter() const
{
    return reject_filter_name(query_.reject_filter);
}

QString StudioPresenter::filterText() const
{
    return qstring_from_utf8(query_.text);
}

QString StudioPresenter::mediaFilter() const
{
    if (query_.media_types.empty())
        return QStringLiteral("any");
    const std::string_view type = query_.media_types.front();
    return type == kMediaTypeRaw  ? QStringLiteral("raw") :
           type == kMediaTypeJpeg ? QStringLiteral("jpeg") :
           type == kMediaTypePng  ? QStringLiteral("png") :
           type == kMediaTypeTiff ? QStringLiteral("tiff") :
                                    qstring_from_utf8(type);
}

QString StudioPresenter::editFilter() const
{
    switch (query_.edit_filter)
    {
    case EditFilter::kEdited:
        return QStringLiteral("edited");
    case EditFilter::kUnedited:
        return QStringLiteral("unedited");
    case EditFilter::kAny:
        return QStringLiteral("any");
    }
    return QStringLiteral("any");
}

QString StudioPresenter::sortField() const
{
    return sort_field_name(query_.sort_field);
}

QString StudioPresenter::sortDirection() const
{
    return query_.sort_direction == SortDirection::kAscending ? QStringLiteral("asc") :
                                                                QStringLiteral("desc");
}

int StudioPresenter::visibleCount() const
{
    return libraryTotal();
}

bool StudioPresenter::filtersActive() const noexcept
{
    return query_.rating_mode != RatingFilterMode::kAny || !query_.color_labels.empty() ||
           query_.reject_filter != RejectFilter::kInclude || !query_.tag.empty() ||
           !query_.text.empty() || !query_.media_types.empty() ||
           query_.edit_filter != EditFilter::kAny || !query_.camera.empty() || query_.iso.minimum ||
           query_.iso.maximum || query_.aperture.minimum || query_.aperture.maximum ||
           query_.focal_length_mm.minimum || query_.focal_length_mm.maximum ||
           query_.shutter_s.minimum || query_.shutter_s.maximum || query_.aspect_ratio.minimum ||
           query_.aspect_ratio.maximum ||
           (!last_import_selected_ &&
            (query_.imported_after_unix_ms || query_.imported_before_unix_ms)) ||
           query_.captured_after_unix_s || query_.captured_before_unix_s;
}

bool StudioPresenter::selectedHasEdits() const noexcept
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->has_edits;
}

QString StudioPresenter::selectedTags() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    QStringList tags;
    for (const auto &tag : asset->tags)
    {
        tags.push_back(qstring_from_utf8(tag));
    }
    return tags.join(QStringLiteral(", "));
}

QString StudioPresenter::selectedTitle() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.title ? qstring_from_utf8(*asset->metadata.title) : QString{};
}

QString StudioPresenter::selectedDescription() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.description ? qstring_from_utf8(*asset->metadata.description) :
                                                  QString{};
}

QString StudioPresenter::selectedCreator() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.creator ? qstring_from_utf8(*asset->metadata.creator) :
                                              QString{};
}

QString StudioPresenter::selectedCopyright() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset && asset->metadata.copyright ? qstring_from_utf8(*asset->metadata.copyright) :
                                                QString{};
}

QString StudioPresenter::selectedCaptureSummary() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    QStringList parts;
    if (asset->capture.camera_make)
    {
        parts.push_back(qstring_from_utf8(*asset->capture.camera_make));
    }
    if (asset->capture.camera_model)
    {
        parts.push_back(qstring_from_utf8(*asset->capture.camera_model));
    }
    if (asset->capture.iso)
    {
        parts.push_back(QStringLiteral("ISO %1").arg(*asset->capture.iso, 0, 'f', 0));
    }
    if (asset->capture.aperture)
    {
        parts.push_back(QStringLiteral("f/%1").arg(*asset->capture.aperture, 0, 'f', 1));
    }
    if (asset->capture.focal_length_mm)
    {
        parts.push_back(QStringLiteral("%1 mm").arg(*asset->capture.focal_length_mm, 0, 'f', 0));
    }
    return parts.join(QStringLiteral(" · "));
}

QString StudioPresenter::tagFilter() const
{
    return qstring_from_utf8(query_.tag);
}

QUrl StudioPresenter::selectedThumbnailUrl() const
{
    const int row = assets_.indexOf(selected_asset_id_);
    if (row < 0)
    {
        return {};
    }
    return assets_.data(assets_.index(row, 0), AssetListModel::ThumbnailUrlRole).toUrl();
}

QString StudioPresenter::selectedFolderUri() const
{
    return qstring_from_utf8(query_.folder_uri);
}

bool StudioPresenter::lastImportAvailable() const noexcept
{
    return last_import_count_ > 0U && last_import_after_unix_ms_ && last_import_before_unix_ms_;
}

bool StudioPresenter::lastImportSelected() const noexcept
{
    return last_import_selected_;
}

int StudioPresenter::lastImportCount() const noexcept
{
    return static_cast<int>(
        std::min<std::size_t>(last_import_count_, static_cast<std::size_t>(INT_MAX)));
}

QString StudioPresenter::selectedDisplayName() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset_display_name(*asset)) : QString{};
}

QString StudioPresenter::selectedFolderPath() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
    {
        return {};
    }
    const QUrl file = QUrl(qstring_from_utf8(asset->normalized_uri));
    return QFileInfo(file.toLocalFile()).absolutePath();
}

QString StudioPresenter::selectedMediaType() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->media_type) : QString{};
}

QString StudioPresenter::selectedDimensions() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->width || !asset->height)
    {
        return {};
    }
    return QStringLiteral("%1 × %2").arg(*asset->width).arg(*asset->height);
}

QString StudioPresenter::selectedFileSize() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || asset->size_bytes == 0)
    {
        return {};
    }
    const auto bytes = static_cast<double>(asset->size_bytes);
    if (bytes < 1024.0)
    {
        return QStringLiteral("%1 B").arg(asset->size_bytes);
    }
    if (bytes < 1024.0 * 1024.0)
    {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QString StudioPresenter::selectedUri() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    return asset ? qstring_from_utf8(asset->normalized_uri) : QString{};
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
    assets_.setAssets(std::move(assets), std::move(thumbnail_urls), std::move(thumbnail_states),
                      total);
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
    if (query_.folder_uri == next && !leaving_last_import)
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
    query_.imported_after_unix_ms = last_import_after_unix_ms_;
    query_.imported_before_unix_ms = last_import_before_unix_ms_;
    last_import_selected_ = true;
    emit folderChanged();
    reloadVisibleAssets();
}

void StudioPresenter::reloadVisibleAssets()
{
    if (catalog_path_.isEmpty())
    {
        return;
    }
    executor_.post(
        [this, query = current_query()]()
        {
            auto listing = load_catalog_listing(service_.get(), query);
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
                    applyFolders(std::move(listing.folders).value());
                    applyAssets(
                        std::move(listing.assets).value(), true, std::move(listing.thumbnail_urls),
                        std::move(listing.thumbnail_states), listing.total, listing.has_more);
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
    library_page_in_flight_ = true;
    pending_library_page_offset_.reset();
    emit libraryWorkChanged();
    executor_.post(
        [this, offset, generation, query, cursor = std::move(cursor), known_total, sequential]
        {
            Result<LibraryPage> page = make_error(ErrorCode::kIo, "Catalog session is closed");
            CatalogListing listing;
            if (service_ != nullptr)
            {
                LibraryPageRequest request;
                request.query = query;
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
    executor_.post(
        [this, query, selected, observed]()
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
                    listing = load_catalog_listing(service_.get(), query);
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
    if (busy_)
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
    setStatus(QCoreApplication::translate("StudioPresenter", "Creating library…"));
    const auto path = utf8_from_qstring(local);
    LibraryQuery initial_query = current_query();
    if (last_import_selected_)
    {
        initial_query.imported_after_unix_ms.reset();
        initial_query.imported_before_unix_ms.reset();
    }
    executor_.post(
        [this, path, initial_query]()
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
                listing = load_catalog_listing(built.value().get(), initial_query);
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
                    emit catalogChanged();
                    setError({});
                    setStatus(QCoreApplication::translate(
                        "StudioPresenter", "Library created. Import photos or a folder."));
                    applyFolders(std::move(listing.folders).value());
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
    if (busy_)
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
    setStatus(QCoreApplication::translate("StudioPresenter", "Opening library…"));
    const auto path = utf8_from_qstring(local);
    LibraryQuery initial_query = current_query();
    if (last_import_selected_)
    {
        initial_query.imported_after_unix_ms.reset();
        initial_query.imported_before_unix_ms.reset();
    }
    executor_.post(
        [this, path, initial_query]()
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
                listing = load_catalog_listing(built.value().get(), initial_query);
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
                    emit catalogChanged();
                    emit selectionChanged();
                    emit previewChanged();
                    emit thumbnailsChanged();
                    setError({});
                    setStatus(QCoreApplication::translate("StudioPresenter", "Library opened."));
                    applyFolders(std::move(listing.folders).value());
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

QVariantList StudioPresenter::exportFormatChoices() const
{
    return studio_export_format_choices();
}

QVariantList StudioPresenter::jpegSubsamplingChoices() const
{
    return studio_jpeg_subsampling_choices();
}

QVariantList StudioPresenter::pngBitDepthChoices() const
{
    return studio_png_bit_depth_choices();
}

QVariantList StudioPresenter::tiffSampleTypeChoices() const
{
    return studio_tiff_sample_type_choices();
}

QVariantList StudioPresenter::tiffCompressionChoices() const
{
    return studio_tiff_compression_choices();
}

QVariantList StudioPresenter::exportMetadataModeChoices() const
{
    return studio_export_metadata_mode_choices();
}

QVariantMap StudioPresenter::exportDefaultOptions() const
{
    return studio_export_default_options();
}

QVariantMap StudioPresenter::exportOptionBounds() const
{
    return studio_export_option_bounds();
}

void StudioPresenter::exportSelectedToPath(const QString &path, const QString &format,
                                           const QVariantMap &options)
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
    {
        return;
    }
    auto request =
        make_studio_export_request(utf8_from_qstring(selected_asset_id_), path, format, options);
    if (!request)
    {
        setError(QCoreApplication::translate("StudioExport", request.error().message.c_str()));
        return;
    }
    ExportRequest snapshot = std::move(request).value();
    snapshot.cancellation = shutdown_.token();
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Exporting…"));
    executor_.post(
        [this, snapshot]()
        {
            Result<ExportResult> exported = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                exported = service_->export_asset(snapshot);
            }
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported)]() mutable
                {
                    setBusy(false);
                    if (!exported)
                    {
                        setError(qstring_from_utf8(exported.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Export failed."));
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter", "Exported %1 (%2×%3)")
                                  .arg(QFileInfo(qstring_from_utf8(exported.value().output_path))
                                           .fileName())
                                  .arg(exported.value().width)
                                  .arg(exported.value().height));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::exportSelectedToDirectory(const QString &directory,
                                                const QString &filename_template,
                                                const QString &format, const QVariantMap &options)
{
    const auto asset_ids = selected_asset_ids();
    if (busy_ || catalog_path_.isEmpty() || asset_ids.empty())
        return;
    auto export_options = make_studio_export_options(format, options);
    if (!export_options)
    {
        setError(
            QCoreApplication::translate("StudioExport", export_options.error().message.c_str()));
        return;
    }
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = utf8_from_qstring(directory);
    request.filename_template = utf8_from_qstring(filename_template);
    request.options = std::move(export_options).value();
    request.cancellation = shutdown_.token();
    setBusy(true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Exporting selected photos…"));
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            Result<std::vector<ExportResult>> exported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                exported = service_->export_assets(request);
            const QString destination = qstring_from_utf8(request.output_directory);
            const auto total = request.asset_ids.size();
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported), destination, total]() mutable
                {
                    setBusy(false);
                    if (!exported)
                    {
                        setError(qstring_from_utf8(exported.error().message));
                        const auto completed = exported.error().context.find("completed_count");
                        if (completed != exported.error().context.end())
                        {
                            setStatus(QCoreApplication::translate(
                                          "StudioPresenter",
                                          "Export stopped after %1 of %2 selected photos.")
                                          .arg(qstring_from_utf8(completed->second))
                                          .arg(total));
                        }
                        else
                        {
                            setStatus(QCoreApplication::translate("StudioPresenter",
                                                                  "Batch export failed."));
                        }
                        return;
                    }
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Exported %1 selected photos to %2")
                                  .arg(exported.value().size())
                                  .arg(destination));
                },
                Qt::QueuedConnection);
        });
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
    executor_.post(
        [this, path, cancellation]
        {
            Result<ImportItemResult> imported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                imported = service_->import_one(path, cancellation);
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
                    import_results_.push_back(item);
                    ++import_next_index_;
                    setImportWork(static_cast<int>(import_next_index_),
                                  static_cast<int>(pending_import_paths_.size()), true);
                    setStatus(QCoreApplication::translate("StudioPresenter", "Importing %1 / %2…")
                                  .arg(import_next_index_)
                                  .arg(pending_import_paths_.size()));
                    startNextImportItem();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::finishImportBatch()
{
    if (!import_work_active_)
        return;
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
         cancelled, completed, total]() mutable
        {
            auto listing = load_catalog_listing(service_.get(), query);
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

void StudioPresenter::publish_selection()
{
    assets_.setSelectedIds(selected_ids_);
    emit selectionChanged();
}

void StudioPresenter::activate_primary(const QString &asset_id, const bool reload_preview)
{
    const bool same = selected_asset_id_ == asset_id;
    selected_asset_id_ = asset_id;
    if (!reload_preview && same && !preview_url_.isEmpty())
    {
        publish_selection();
        return;
    }
    clear_displayed_preview();
    if (const auto asset = assets_.assetById(asset_id); asset && asset->width && asset->height)
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        fit_within_max_edge(*asset->width, *asset->height, kDefaultPreviewMaxEdge, width, height);
        preview_viewport_width_ = static_cast<int>(width);
        preview_viewport_height_ = static_cast<int>(height);
    }
    preview_loading_ = !asset_id.isEmpty();
    before_after_ = false;
    crop_tool_active_ = false;
    static_cast<void>(develop_preview_owner_.supersede("selection_changed"));
    static_cast<void>(perspective_analysis_owner_.supersede("selection_changed"));
    pending_preview_.reset();
    load_develop_for_selection();
    publish_selection();
    emit previewChanged();
    emit thumbnailsChanged();
    emit editChanged();
    requestPreviewForSelection();
}

std::vector<std::string> StudioPresenter::selected_asset_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(selected_ids_.size());
    for (const auto &asset : assets_.records())
    {
        if (selected_ids_.contains(asset.id))
            ids.push_back(asset.id);
    }
    return ids;
}

void StudioPresenter::selectAsset(const QString &asset_id)
{
    if (selected_asset_id_ == asset_id && selected_ids_.size() == 1U && !preview_url_.isEmpty())
    {
        return;
    }
    selected_ids_.clear();
    if (!asset_id.isEmpty())
    {
        selected_ids_.insert(utf8_from_qstring(asset_id));
    }
    selection_anchor_id_ = asset_id;
    activate_primary(asset_id, true);
}

void StudioPresenter::selectAssetRange(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        return;
    }
    const int clicked = assets_.indexOf(asset_id);
    if (clicked < 0)
    {
        return;
    }
    int anchor = assets_.indexOf(selection_anchor_id_);
    if (anchor < 0)
    {
        anchor = assets_.indexOf(selected_asset_id_);
    }
    if (anchor < 0)
    {
        selectAsset(asset_id);
        return;
    }
    const int begin = std::min(anchor, clicked);
    const int end = std::max(anchor, clicked);
    selected_ids_.clear();
    for (int row = begin; row <= end; ++row)
    {
        selected_ids_.insert(utf8_from_qstring(assets_.assetIdAt(row)));
    }
    activate_primary(asset_id, true);
}

void StudioPresenter::toggleAssetSelected(const QString &asset_id)
{
    if (asset_id.isEmpty())
    {
        return;
    }
    const auto id = utf8_from_qstring(asset_id);
    if (selected_ids_.contains(id))
    {
        selected_ids_.erase(id);
        selection_anchor_id_ = asset_id;
        if (selected_asset_id_ == asset_id)
        {
            const auto remaining = selected_asset_ids();
            activate_primary(remaining.empty() ? QString{} : qstring_from_utf8(remaining.back()),
                             true);
            return;
        }
        publish_selection();
        return;
    }
    selected_ids_.insert(id);
    selection_anchor_id_ = asset_id;
    activate_primary(asset_id, true);
}

void StudioPresenter::selectNext()
{
    const auto row = assets_.indexOf(selected_asset_id_);
    if (row < 0 || row + 1 >= assets_.rowCount())
    {
        return;
    }
    selectAsset(assets_.assetIdAt(row + 1));
}

void StudioPresenter::selectPrevious()
{
    const auto row = assets_.indexOf(selected_asset_id_);
    if (row <= 0)
    {
        return;
    }
    selectAsset(assets_.assetIdAt(row - 1));
}

void StudioPresenter::setBrowseMode(const QString &mode)
{
    QString normalized = QStringLiteral("grid");
    if (mode == QStringLiteral("loupe"))
    {
        normalized = QStringLiteral("loupe");
    }
    else if (mode == QStringLiteral("develop"))
    {
        normalized = QStringLiteral("develop");
    }
    if (browse_mode_ == normalized)
    {
        return;
    }
    const bool comparison_changed = normalized != QLatin1String("develop") && clear_comparison();
    const QString previous = browse_mode_;
    if (previous == QLatin1String("develop") && normalized != QLatin1String("develop"))
    {
        break_history_coalescing();
    }
    browse_mode_ = normalized;
    emit browseModeChanged();
    if (comparison_changed)
    {
        emit editChanged();
        emit previewChanged();
    }
    if (normalized != QLatin1String("grid") &&
        (previous == QLatin1String("grid") || normalized == QLatin1String("develop")) &&
        !selected_asset_id_.isEmpty())
    {
        requestPreviewForSelection();
    }
}

void StudioPresenter::openLoupe()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    setBrowseMode(QStringLiteral("loupe"));
}

void StudioPresenter::openDevelop()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    setBrowseMode(QStringLiteral("develop"));
}

void StudioPresenter::returnToGrid()
{
    setBrowseMode(QStringLiteral("grid"));
}

void StudioPresenter::setZoomMode(const QString &mode)
{
    QString normalized = QStringLiteral("fit");
    double factor = zoom_factor_;
    if (mode == QStringLiteral("fill"))
    {
        normalized = QStringLiteral("fill");
    }
    else if (mode == QStringLiteral("actual") || mode == QStringLiteral("100"))
    {
        normalized = QStringLiteral("actual");
        factor = 1.0;
    }
    else if (mode == QStringLiteral("custom"))
    {
        normalized = QStringLiteral("custom");
    }
    if (zoom_mode_ == normalized && zoom_factor_ == factor)
    {
        return;
    }
    zoom_mode_ = normalized;
    zoom_factor_ = factor;
    if (zoom_mode_ != QStringLiteral("actual"))
    {
        last_non_actual_zoom_mode_ = zoom_mode_;
        last_non_actual_zoom_factor_ = zoom_factor_;
    }
    emit zoomChanged();
}

void StudioPresenter::setZoomFactor(const double factor)
{
    const double clamped = std::clamp(factor, 0.1, 8.0);
    if (zoom_mode_ == QStringLiteral("custom") && zoom_factor_ == clamped)
    {
        return;
    }
    zoom_mode_ = QStringLiteral("custom");
    zoom_factor_ = clamped;
    last_non_actual_zoom_mode_ = zoom_mode_;
    last_non_actual_zoom_factor_ = zoom_factor_;
    emit zoomChanged();
}

void StudioPresenter::adjustZoom(const int wheel_delta)
{
    const double step = wheel_delta > 0 ? 1.1 : 1.0 / 1.1;
    const double current = zoom_mode_ == QStringLiteral("actual") ? 1.0 : zoom_factor_;
    setZoomFactor(current * step);
}

void StudioPresenter::toggleActualSize()
{
    if (zoom_mode_ == QStringLiteral("actual"))
    {
        if (last_non_actual_zoom_mode_ == QStringLiteral("custom"))
        {
            setZoomFactor(last_non_actual_zoom_factor_);
            return;
        }
        setZoomMode(last_non_actual_zoom_mode_);
        return;
    }
    setZoomMode(QStringLiteral("actual"));
}

void StudioPresenter::setThumbnailSize(const int size)
{
    const int clamped = std::clamp(size, 96, 320);
    if (thumbnail_size_ == clamped)
    {
        return;
    }
    thumbnail_size_ = clamped;
    emit thumbnailSizeChanged();
}

void StudioPresenter::mutate_selected_review(
    const std::function<Result<AssetRecord>(CatalogService &, std::string_view)> &action)
{
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    executor_.post(
        [this, action, ids]()
        {
            std::vector<AssetRecord> updated;
            TaskError error = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool ok = false;
            if (service_ != nullptr)
            {
                ok = true;
                for (const auto &asset_id : ids)
                {
                    auto result = action(*service_, asset_id);
                    if (!result)
                    {
                        error = result.error();
                        ok = false;
                        break;
                    }
                    updated.push_back(std::move(result).value());
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, ok, error = std::move(error), updated = std::move(updated)]() mutable
                {
                    if (!ok)
                    {
                        setError(qstring_from_utf8(error.message));
                        return;
                    }
                    for (const auto &asset : updated)
                    {
                        assets_.updateAsset(asset);
                    }
                    emit selectionChanged();
                    if (filtersActive())
                    {
                        reloadVisibleAssets();
                    }
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setRating(const int rating)
{
    mutate_selected_review([rating](CatalogService &service, const std::string_view asset_id)
                           { return service.set_rating(asset_id, rating); });
}

void StudioPresenter::setColorLabel(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    const auto color = parsed.value();
    mutate_selected_review([color](CatalogService &service, const std::string_view asset_id)
                           { return service.set_color_label(asset_id, color); });
}

void StudioPresenter::toggleRejected()
{
    const bool next = !selectedRejected();
    mutate_selected_review([next](CatalogService &service, const std::string_view asset_id)
                           { return service.set_rejected(asset_id, next); });
}

void StudioPresenter::setAssetTags(const QString &text)
{
    auto parsed = parse_tag_list(utf8_from_qstring(text));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    const auto tags = parsed.value();
    mutate_selected_review([tags](CatalogService &service, const std::string_view asset_id)
                           { return service.set_tags(asset_id, tags); });
}

void StudioPresenter::setMetadataField(const QString &name, const QString &value)
{
    const auto field = utf8_from_qstring(name);
    const auto text = utf8_from_qstring(value);
    mutate_selected_review(
        [field, text](CatalogService &service,
                      const std::string_view asset_id) -> Result<AssetRecord>
        {
            auto listed = service.list_assets();
            if (!listed)
            {
                return listed.error();
            }
            WritableMetadata metadata;
            for (const auto &asset : listed.value())
            {
                if (asset.id == asset_id)
                {
                    metadata = asset.metadata;
                    break;
                }
            }
            if (field == "title")
            {
                metadata.title =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else if (field == "description")
            {
                metadata.description =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else if (field == "creator")
            {
                metadata.creator =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else if (field == "copyright")
            {
                metadata.copyright =
                    text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
            }
            else
            {
                return make_error(ErrorCode::kInvalidArgument, "Writable metadata field is unknown",
                                  {{"field", field}});
            }
            return service.set_writable_metadata(asset_id, metadata);
        });
}

void StudioPresenter::refreshSelectedMetadata()
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
        return;
    const std::string asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<AssetRecord> refreshed = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
                refreshed = service_->refresh_capture_metadata(asset_id, shutdown_.token());
            QMetaObject::invokeMethod(
                this,
                [this, refreshed = std::move(refreshed)]() mutable
                {
                    if (!refreshed)
                    {
                        if (refreshed.error().code != ErrorCode::kCancelled)
                            setError(qstring_from_utf8(refreshed.error().message));
                        return;
                    }
                    assets_.updateAsset(refreshed.value());
                    emit selectionChanged();
                    setStatus(QCoreApplication::translate("StudioPresenter",
                                                          "Capture metadata refreshed."));
                    reloadVisibleAssets();
                },
                Qt::QueuedConnection);
        });
}

[[nodiscard]] QString next_snapshot_label(const std::vector<RecipeHistoryEntry> &entries)
{
    const QString format = QCoreApplication::translate("DevelopHistoryPanel", "Snapshot %1");
    QString pattern = QRegularExpression::escape(format);
    pattern.replace(QLatin1String("%1"), QStringLiteral("(\\d+)"));
    const QRegularExpression re(QStringLiteral("^") + pattern + QStringLiteral("$"));
    int next = 1;
    for (const auto &entry : entries)
    {
        if (entry.kind != kRecipeHistoryKindSnapshot || !entry.label)
        {
            continue;
        }
        const auto match = re.match(qstring_from_utf8(*entry.label));
        if (match.hasMatch())
        {
            next = std::max(next, match.captured(1).toInt() + 1);
        }
    }
    return format.arg(next);
}

void StudioPresenter::createSnapshot(const QString &label)
{
    QString trimmed = label.trimmed();
    const QString generic = QCoreApplication::translate("DevelopHistoryPanel", "Snapshot");
    if (trimmed.isEmpty() || trimmed.compare(generic, Qt::CaseInsensitive) == 0)
    {
        trimmed = next_snapshot_label(recipe_history_entries_);
    }
    const auto text = utf8_from_qstring(trimmed);
    mutate_selected_review([text](CatalogService &service, const std::string_view asset_id)
                           { return service.create_recipe_snapshot(asset_id, text); });
    load_develop_for_selection();
}

void StudioPresenter::renameSnapshot(const int history_id, const QString &label)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const auto text = utf8_from_qstring(label);
    executor_.post(
        [this, asset_id, history_id, text]()
        {
            Result<AssetRecord> renamed = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                renamed = service_->rename_recipe_snapshot(asset_id, history_id, text);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, renamed = std::move(renamed)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    if (!renamed)
                    {
                        setError(qstring_from_utf8(renamed.error().message));
                        return;
                    }
                    reload_recipe_history();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::restoreHistory(const int history_id)
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    DevelopParams params;
    std::int64_t seq = 0;
    if (history_id == 0)
    {
        params = baseline_develop();
    }
    else
    {
        const RecipeHistoryEntry *found = nullptr;
        for (const auto &entry : recipe_history_entries_)
        {
            if (entry.id == history_id)
            {
                found = &entry;
                break;
            }
        }
        if (found == nullptr)
        {
            setError(QCoreApplication::translate("DevelopHistoryPanel",
                                                 "Recipe history entry does not exist."));
            return;
        }
        params = develop_from_history_entry(*found);
        seq = found->seq;
    }
    active_history_id_ = history_id;
    active_history_seq_ = seq;
    if (!mutate_develop(std::move(params), StudioPresenter::DevelopEdit::Restore))
    {
        emit editChanged();
    }
}

void StudioPresenter::setTagFilter(const QString &tag)
{
    auto parsed = tag.trimmed().isEmpty() ? Result<std::string>{std::string{}} :
                                            normalize_tag_name(utf8_from_qstring(tag));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    if (query_.tag == parsed.value())
    {
        return;
    }
    query_.tag = parsed.value();
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setRatingFilter(const QString &mode, const int value)
{
    RatingFilterMode next_mode = RatingFilterMode::kAny;
    if (mode == QStringLiteral("min"))
    {
        next_mode = RatingFilterMode::kMinimum;
    }
    else if (mode == QStringLiteral("exact"))
    {
        next_mode = RatingFilterMode::kExact;
    }
    if (query_.rating_mode == next_mode && query_.rating_value == value)
    {
        return;
    }
    query_.rating_mode = next_mode;
    query_.rating_value = value;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::toggleColorFilter(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    auto &labels = query_.color_labels;
    const auto found = std::find(labels.begin(), labels.end(), parsed.value());
    if (found == labels.end())
    {
        labels.push_back(parsed.value());
    }
    else
    {
        labels.erase(found);
    }
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setRejectFilter(const QString &mode)
{
    RejectFilter next = RejectFilter::kInclude;
    if (mode == QStringLiteral("exclude"))
    {
        next = RejectFilter::kExclude;
    }
    else if (mode == QStringLiteral("only"))
    {
        next = RejectFilter::kOnly;
    }
    if (query_.reject_filter == next)
    {
        return;
    }
    query_.reject_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setFilterText(const QString &text)
{
    LibraryQuery next = query_;
    next.text = utf8_from_qstring(text.trimmed());
    auto valid = validate_library_query(next);
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setMediaFilter(const QString &mode)
{
    LibraryQuery next = query_;
    next.media_types.clear();
    if (mode == QLatin1String("raw"))
        next.media_types.emplace_back(kMediaTypeRaw);
    else if (mode == QLatin1String("jpeg"))
        next.media_types.emplace_back(kMediaTypeJpeg);
    else if (mode == QLatin1String("png"))
        next.media_types.emplace_back(kMediaTypePng);
    else if (mode == QLatin1String("tiff"))
        next.media_types.emplace_back(kMediaTypeTiff);
    else if (mode != QLatin1String("any"))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Unknown media filter mode."));
        return;
    }
    if (next == query_)
        return;
    query_ = std::move(next);
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setEditFilter(const QString &mode)
{
    EditFilter next = EditFilter::kAny;
    if (mode == QLatin1String("edited"))
        next = EditFilter::kEdited;
    else if (mode == QLatin1String("unedited"))
        next = EditFilter::kUnedited;
    else if (mode != QLatin1String("any"))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Unknown edit filter mode."));
        return;
    }
    if (query_.edit_filter == next)
        return;
    query_.edit_filter = next;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::setSort(const QString &field, const QString &direction)
{
    AssetSortField next_field = AssetSortField::kImportTime;
    if (field == QStringLiteral("name"))
    {
        next_field = AssetSortField::kDisplayName;
    }
    else if (field == QStringLiteral("rating"))
    {
        next_field = AssetSortField::kRating;
    }
    else if (field == QStringLiteral("captured"))
    {
        next_field = AssetSortField::kCaptureTime;
    }
    else if (field == QStringLiteral("size"))
    {
        next_field = AssetSortField::kFileSize;
    }
    const auto next_direction =
        direction == QStringLiteral("asc") ? SortDirection::kAscending : SortDirection::kDescending;
    if (query_.sort_field == next_field && query_.sort_direction == next_direction)
    {
        return;
    }
    query_.sort_field = next_field;
    query_.sort_direction = next_direction;
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::clearFilters()
{
    if (!filtersActive())
    {
        return;
    }
    query_.rating_mode = RatingFilterMode::kAny;
    query_.rating_value = 0;
    query_.color_labels.clear();
    query_.reject_filter = RejectFilter::kInclude;
    query_.tag.clear();
    query_.text.clear();
    query_.media_types.clear();
    query_.edit_filter = EditFilter::kAny;
    query_.camera.clear();
    query_.iso = {};
    query_.aperture = {};
    query_.focal_length_mm = {};
    query_.shutter_s = {};
    query_.aspect_ratio = {};
    query_.imported_after_unix_ms.reset();
    query_.imported_before_unix_ms.reset();
    query_.captured_after_unix_s.reset();
    query_.captured_before_unix_s.reset();
    if (last_import_selected_)
    {
        query_.imported_after_unix_ms = last_import_after_unix_ms_;
        query_.imported_before_unix_ms = last_import_before_unix_ms_;
    }
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::remove_selected_from_catalog()
{
    if (selected_ids_.empty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const int keep_index = std::max(0, selectedIndex());
    const auto count = ids.size();
    executor_.post(
        [this, ids, keep_index, count]()
        {
            Result<void> removed = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                removed = Result<void>{};
                for (const auto &asset_id : ids)
                {
                    removed = service_->remove_from_catalog(asset_id);
                    if (!removed)
                    {
                        break;
                    }
                }
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), keep_index, count]() mutable
                {
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    const auto total = listed.value().size();
                    applyAssets(std::move(listed).value(), false, {}, {}, total, false);
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
                    }
                    else
                    {
                        const int row = std::min(keep_index, assets_.rowCount() - 1);
                        selectAsset(assets_.assetIdAt(row));
                    }
                    setStatus(
                        count == 1 ?
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Removed from catalog. Original file was not deleted.") :
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Removed %1 photos from catalog. Original files were not deleted.")
                                .arg(count));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::remove_selected_from_disk()
{
    if (!canDeleteFromDisk() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto ids = selected_asset_ids();
    const int keep_index = std::max(0, selectedIndex());
    const auto count = ids.size();
    executor_.post(
        [this, ids, keep_index, count]()
        {
            Result<void> removed = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                removed = Result<void>{};
                for (const auto &asset_id : ids)
                {
                    removed = service_->remove_original_and_catalog(asset_id);
                    if (!removed)
                    {
                        break;
                    }
                }
                if (removed)
                {
                    listed = service_->list_assets(current_query());
                    folders = service_->list_folders();
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, removed = std::move(removed), listed = std::move(listed),
                 folders = std::move(folders), keep_index, count]() mutable
                {
                    if (!removed)
                    {
                        setError(qstring_from_utf8(removed.error().message));
                        return;
                    }
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        return;
                    }
                    applyFolders(std::move(folders).value());
                    const auto total = listed.value().size();
                    applyAssets(std::move(listed).value(), false, {}, {}, total, false);
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
                    }
                    else
                    {
                        const int row = std::min(keep_index, assets_.rowCount() - 1);
                        selectAsset(assets_.assetIdAt(row));
                    }
                    setStatus(
                        count == 1 ?
                            QCoreApplication::translate(
                                "StudioPresenter", "Deleted original file and catalog record.") :
                            QCoreApplication::translate(
                                "StudioPresenter", "Deleted %1 original files and catalog records.")
                                .arg(count));
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
