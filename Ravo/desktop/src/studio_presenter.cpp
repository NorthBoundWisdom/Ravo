#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QStandardPaths>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] std::string utf8_from_qstring(const QString &text)
{
    const QByteArray bytes = text.toUtf8();
    return {bytes.constData(), static_cast<std::size_t>(bytes.size())};
}

[[nodiscard]] std::string preview_root_for(const std::string &database_path)
{
    return database_path + ".preview";
}

[[nodiscard]] QString pictures_directory()
{
    QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (pictures.isEmpty())
    {
        pictures = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    }
    return pictures;
}

[[nodiscard]] QUrl url_from_dialog_path(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
    {
        return {};
    }
    if (trimmed.startsWith(QStringLiteral("file:")))
    {
        return QUrl(trimmed);
    }
    return QUrl::fromLocalFile(trimmed);
}

[[nodiscard]] QString describe_import(const std::vector<ImportItemResult> &results)
{
    int imported = 0;
    int duplicate = 0;
    int unsupported = 0;
    int failed = 0;
    for (const auto &item : results)
    {
        switch (item.status)
        {
        case ImportItemStatus::kImported:
            ++imported;
            break;
        case ImportItemStatus::kDuplicate:
            ++duplicate;
            break;
        case ImportItemStatus::kUnsupported:
            ++unsupported;
            break;
        case ImportItemStatus::kFailed:
            ++failed;
            break;
        }
    }
    return QStringLiteral("Imported %1, duplicate %2, unsupported %3, failed %4")
        .arg(imported)
        .arg(duplicate)
        .arg(unsupported)
        .arg(failed);
}

[[nodiscard]] QString rating_mode_name(const RatingFilterMode mode)
{
    switch (mode)
    {
    case RatingFilterMode::kMinimum:
        return QStringLiteral("min");
    case RatingFilterMode::kExact:
        return QStringLiteral("exact");
    case RatingFilterMode::kAny:
        break;
    }
    return QStringLiteral("any");
}

[[nodiscard]] QString reject_filter_name(const RejectFilter filter)
{
    switch (filter)
    {
    case RejectFilter::kExclude:
        return QStringLiteral("exclude");
    case RejectFilter::kOnly:
        return QStringLiteral("only");
    case RejectFilter::kInclude:
        break;
    }
    return QStringLiteral("include");
}

[[nodiscard]] QString sort_field_name(const AssetSortField field)
{
    switch (field)
    {
    case AssetSortField::kDisplayName:
        return QStringLiteral("name");
    case AssetSortField::kRating:
        return QStringLiteral("rating");
    case AssetSortField::kImportTime:
        break;
    }
    return QStringLiteral("imported");
}

} // namespace

AssetListModel::AssetListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int AssetListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(assets_.size());
}

QVariant AssetListModel::data(const QModelIndex &index, const int role) const
{
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= static_cast<int>(assets_.size()))
    {
        return {};
    }
    const auto &asset = assets_[static_cast<std::size_t>(index.row())];
    switch (role)
    {
    case AssetIdRole:
        return qstring_from_utf8(asset.id);
    case DisplayNameRole:
        return qstring_from_utf8(asset_display_name(asset));
    case MediaTypeRole:
        return qstring_from_utf8(asset.media_type);
    case ImportStateRole:
        return qstring_from_utf8(asset.import_state);
    case ErrorRole:
        return asset.error_message ? qstring_from_utf8(*asset.error_message) : QString{};
    case RatingRole:
        return asset.review.rating;
    case ColorLabelRole:
        return qstring_from_utf8(color_label_name(asset.review.color_label));
    case RejectedRole:
        return asset.review.rejected;
    case ThumbnailUrlRole:
    {
        const auto found = thumbnail_urls_.find(asset.id);
        return found == thumbnail_urls_.end() ? QUrl{} : found->second;
    }
    case ThumbnailStateRole:
    {
        const auto found = thumbnail_states_.find(asset.id);
        if (found != thumbnail_states_.end())
        {
            return found->second;
        }
        return asset.import_state == kImportStateMissing ? QStringLiteral("missing") :
                                                           QStringLiteral("pending");
    }
    case WidthRole:
        return asset.width.value_or(0);
    case HeightRole:
        return asset.height.value_or(0);
    default:
        return {};
    }
}

QHash<int, QByteArray> AssetListModel::roleNames() const
{
    return {{AssetIdRole, "assetId"},
            {DisplayNameRole, "displayName"},
            {MediaTypeRole, "mediaType"},
            {ImportStateRole, "importState"},
            {ErrorRole, "errorText"},
            {RatingRole, "rating"},
            {ColorLabelRole, "colorLabel"},
            {RejectedRole, "rejected"},
            {ThumbnailUrlRole, "thumbnailUrl"},
            {ThumbnailStateRole, "thumbnailState"},
            {WidthRole, "pixelWidth"},
            {HeightRole, "pixelHeight"}};
}

void AssetListModel::setAssets(std::vector<AssetRecord> assets)
{
    beginResetModel();
    assets_ = std::move(assets);
    std::unordered_map<std::string, QUrl> kept_urls;
    std::unordered_map<std::string, QString> kept_states;
    for (const auto &asset : assets_)
    {
        if (const auto found = thumbnail_urls_.find(asset.id); found != thumbnail_urls_.end())
        {
            kept_urls.emplace(found->first, found->second);
        }
        if (const auto found = thumbnail_states_.find(asset.id); found != thumbnail_states_.end())
        {
            kept_states.emplace(found->first, found->second);
        }
    }
    thumbnail_urls_ = std::move(kept_urls);
    thumbnail_states_ = std::move(kept_states);
    endResetModel();
}

void AssetListModel::setThumbnail(const std::string &asset_id, const QUrl &url, const QString &state)
{
    thumbnail_urls_[asset_id] = url;
    thumbnail_states_[asset_id] = state;
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
    {
        return;
    }
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ThumbnailUrlRole, ThumbnailStateRole});
}

void AssetListModel::updateAsset(const AssetRecord &asset)
{
    const auto row = indexOf(qstring_from_utf8(asset.id));
    if (row < 0)
    {
        return;
    }
    assets_[static_cast<std::size_t>(row)] = asset;
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index);
}

void AssetListModel::markOriginalMissing(const std::string &asset_id)
{
    const auto row = indexOf(qstring_from_utf8(asset_id));
    if (row < 0)
    {
        return;
    }
    auto &asset = assets_[static_cast<std::size_t>(row)];
    asset.import_state = std::string(kImportStateMissing);
    thumbnail_states_[asset_id] = QStringLiteral("missing");
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, {ImportStateRole, ThumbnailStateRole});
}

int AssetListModel::indexOf(const QString &asset_id) const
{
    const auto id = utf8_from_qstring(asset_id);
    for (int row = 0; row < static_cast<int>(assets_.size()); ++row)
    {
        if (assets_[static_cast<std::size_t>(row)].id == id)
        {
            return row;
        }
    }
    return -1;
}

std::optional<AssetRecord> AssetListModel::assetById(const QString &asset_id) const
{
    const auto row = indexOf(asset_id);
    if (row < 0)
    {
        return std::nullopt;
    }
    return assets_[static_cast<std::size_t>(row)];
}

QString AssetListModel::assetIdAt(const int row) const
{
    if (row < 0 || row >= static_cast<int>(assets_.size()))
    {
        return {};
    }
    return qstring_from_utf8(assets_[static_cast<std::size_t>(row)].id);
}

StudioPresenter::StudioPresenter(QObject *parent)
    : QObject(parent),
      assets_(this)
{
    const auto created = executor_.submit([this]() -> Result<void> {
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
        status_text_ = QStringLiteral("Engine failed to start.");
    }
}

StudioPresenter::~StudioPresenter()
{
    static_cast<void>(shutdown_.cancel("window_closed"));
    executor_.submit([this]() {
        service_.reset();
        engine_.reset();
    });
    executor_.request_stop();
    executor_.wait();
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

QUrl StudioPresenter::previewUrl() const
{
    return preview_url_;
}

bool StudioPresenter::previewLoading() const noexcept
{
    return preview_loading_;
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
    return assets_.rowCount();
}

bool StudioPresenter::filtersActive() const noexcept
{
    return query_.rating_mode != RatingFilterMode::kAny || !query_.color_labels.empty() ||
           query_.reject_filter != RejectFilter::kInclude;
}

AssetListModel *StudioPresenter::assets() noexcept
{
    return &assets_;
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

void StudioPresenter::applyAssets(std::vector<AssetRecord> assets, const bool restore_selection)
{
    const QString previous = selected_asset_id_;
    assets_.setAssets(std::move(assets));
    emit filterChanged();
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
        return;
    }
    if (assets_.rowCount() == 0)
    {
        selected_asset_id_.clear();
        preview_url_.clear();
        preview_loading_ = false;
        emit selectionChanged();
        emit previewChanged();
        return;
    }
    if (selected_asset_id_.isEmpty() || assets_.indexOf(selected_asset_id_) < 0)
    {
        selectAsset(assets_.assetIdAt(0));
    }
}

void StudioPresenter::reloadVisibleAssets()
{
    if (catalog_path_.isEmpty())
    {
        return;
    }
    executor_.post([this, query = current_query()]() {
        Result<std::vector<AssetRecord>> listed =
            make_error(ErrorCode::kIo, "Catalog session is closed");
        if (service_ != nullptr)
        {
            listed = service_->list_assets(query);
        }
        QMetaObject::invokeMethod(
            this,
            [this, listed = std::move(listed)]() mutable {
                if (!listed)
                {
                    setError(qstring_from_utf8(listed.error().message));
                    return;
                }
                applyAssets(std::move(listed).value(), true);
            },
            Qt::QueuedConnection);
    });
}

Result<void> StudioPresenter::open_on_worker(const std::string &path, const bool create)
{
    if (!engine_)
    {
        return make_error(ErrorCode::kInternal, "Engine is not available");
    }
    service_.reset();
    auto repository = create ? SqliteCatalogRepository::create(path) :
                               SqliteCatalogRepository::open(path);
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
    service_ = std::make_unique<CatalogService>(*engine_, std::move(repository).value(),
                                                std::move(raster), std::move(cache).value());
    return {};
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
        setError(QStringLiteral("Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Creating library…"));
    const auto path = utf8_from_qstring(local);
    executor_.post([this, path]() {
        const auto opened = open_on_worker(path, true);
        Result<std::vector<AssetRecord>> listed = std::vector<AssetRecord>{};
        if (opened && service_ != nullptr)
        {
            listed = service_->list_assets(query_);
        }
        QMetaObject::invokeMethod(
            this,
            [this, path, opened, listed = std::move(listed)]() mutable {
                setBusy(false);
                if (!opened)
                {
                    setError(qstring_from_utf8(opened.error().message));
                    setStatus(QStringLiteral("Create failed."));
                    return;
                }
                if (!listed)
                {
                    setError(qstring_from_utf8(listed.error().message));
                    setStatus(QStringLiteral("Create failed."));
                    return;
                }
                catalog_path_ = qstring_from_utf8(path);
                emit catalogChanged();
                setError({});
                setStatus(QStringLiteral("Library created. Import photos or a folder."));
                applyAssets(std::move(listed).value(), true);
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
        setError(QStringLiteral("Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Opening library…"));
    const auto path = utf8_from_qstring(local);
    executor_.post([this, path]() {
        const auto opened = open_on_worker(path, false);
        Result<std::vector<AssetRecord>> listed = std::vector<AssetRecord>{};
        if (opened && service_ != nullptr)
        {
            listed = service_->list_assets(query_);
        }
        QMetaObject::invokeMethod(
            this,
            [this, path, opened, listed = std::move(listed)]() mutable {
                setBusy(false);
                if (!opened)
                {
                    setError(qstring_from_utf8(opened.error().message));
                    setStatus(QStringLiteral("Open failed."));
                    return;
                }
                if (!listed)
                {
                    setError(qstring_from_utf8(listed.error().message));
                    setStatus(QStringLiteral("Open failed."));
                    return;
                }
                catalog_path_ = qstring_from_utf8(path);
                selected_asset_id_.clear();
                preview_url_.clear();
                emit catalogChanged();
                emit selectionChanged();
                emit previewChanged();
                setError({});
                setStatus(QStringLiteral("Library opened."));
                applyAssets(std::move(listed).value(), true);
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
    if (busy_ || catalog_path_.isEmpty())
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
        setError(QStringLiteral("No local files selected."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Importing…"));
    executor_.post([this, paths = std::move(paths), query = current_query()]() {
        Result<std::vector<ImportItemResult>> imported =
            make_error(ErrorCode::kIo, "Catalog session is closed");
        if (service_ != nullptr)
        {
            imported = service_->import_inputs(
                paths, shutdown_.token(), [this](const std::size_t completed, const std::size_t total) {
                    const auto completed_count = static_cast<int>(completed);
                    const auto total_count = static_cast<int>(total);
                    QMetaObject::invokeMethod(
                        this,
                        [this, completed_count, total_count]() {
                            setStatus(QStringLiteral("Importing %1 / %2…")
                                          .arg(completed_count)
                                          .arg(total_count));
                        },
                        Qt::QueuedConnection);
                });
        }
        std::vector<ImportItemResult> results;
        QString first_error;
        if (!imported)
        {
            first_error = qstring_from_utf8(imported.error().message);
        }
        else
        {
            results = std::move(imported).value();
            for (const auto &item : results)
            {
                if (first_error.isEmpty() && item.error)
                {
                    first_error = qstring_from_utf8(item.error->message);
                }
            }
        }
        Result<std::vector<AssetRecord>> listed = std::vector<AssetRecord>{};
        if (service_ != nullptr)
        {
            listed = service_->list_assets(query);
        }
        QMetaObject::invokeMethod(
            this,
            [this, results = std::move(results), listed = std::move(listed),
             first_error = std::move(first_error)]() mutable {
                setBusy(false);
                if (!listed)
                {
                    setError(qstring_from_utf8(listed.error().message));
                    setStatus(QStringLiteral("Import failed."));
                    return;
                }
                setError(first_error);
                setStatus(describe_import(results));
                applyAssets(std::move(listed).value(), true);
            },
            Qt::QueuedConnection);
    });
}

void StudioPresenter::selectAsset(const QString &asset_id)
{
    if (selected_asset_id_ == asset_id && !preview_url_.isEmpty())
    {
        return;
    }
    selected_asset_id_ = asset_id;
    preview_url_.clear();
    preview_loading_ = !asset_id.isEmpty();
    emit selectionChanged();
    emit previewChanged();
    requestPreviewForSelection();
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
    const QString normalized =
        mode == QStringLiteral("loupe") ? QStringLiteral("loupe") : QStringLiteral("grid");
    if (browse_mode_ == normalized)
    {
        return;
    }
    browse_mode_ = normalized;
    emit browseModeChanged();
}

void StudioPresenter::openLoupe()
{
    if (selected_asset_id_.isEmpty())
    {
        return;
    }
    setBrowseMode(QStringLiteral("loupe"));
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
    emit zoomChanged();
}

void StudioPresenter::adjustZoom(const int wheel_delta)
{
    const double step = wheel_delta > 0 ? 1.1 : 1.0 / 1.1;
    const double current = zoom_mode_ == QStringLiteral("actual") ? 1.0 : zoom_factor_;
    setZoomFactor(current * step);
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
    const std::function<Result<AssetRecord>(CatalogService &)> &action)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    executor_.post([this, action]() {
        Result<AssetRecord> updated = make_error(ErrorCode::kIo, "Catalog session is closed");
        if (service_ != nullptr)
        {
            updated = action(*service_);
        }
        QMetaObject::invokeMethod(
            this,
            [this, updated = std::move(updated)]() mutable {
                if (!updated)
                {
                    setError(qstring_from_utf8(updated.error().message));
                    return;
                }
                assets_.updateAsset(updated.value());
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
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    mutate_selected_review(
        [asset_id, rating](CatalogService &service) { return service.set_rating(asset_id, rating); });
}

void StudioPresenter::setColorLabel(const QString &label)
{
    auto parsed = parse_color_label(utf8_from_qstring(label));
    if (!parsed)
    {
        setError(qstring_from_utf8(parsed.error().message));
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const auto color = parsed.value();
    mutate_selected_review([asset_id, color](CatalogService &service) {
        return service.set_color_label(asset_id, color);
    });
}

void StudioPresenter::toggleRejected()
{
    const bool next = !selectedRejected();
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    mutate_selected_review([asset_id, next](CatalogService &service) {
        return service.set_rejected(asset_id, next);
    });
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
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::ensureThumbnail(const QString &asset_id)
{
    if (asset_id.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    const auto existing = assets_.assetById(asset_id);
    if (!existing)
    {
        return;
    }
    const auto revision = ++thumbnail_revision_;
    const auto id = utf8_from_qstring(asset_id);
    executor_.post([this, id, revision]() {
        Result<PreviewResult> preview =
            make_error(ErrorCode::kIo, "Catalog session is closed");
        if (service_ != nullptr)
        {
            PreviewRequest request;
            request.asset_id = id;
            request.max_edge = kThumbnailMaxEdge;
            request.request_revision = revision;
            request.cancellation = shutdown_.token();
            preview = service_->request_preview(request);
        }
        QMetaObject::invokeMethod(
            this,
            [this, id, preview = std::move(preview)]() mutable {
                if (preview)
                {
                    assets_.setThumbnail(
                        id, QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path)),
                        preview.value().original_missing ? QStringLiteral("missing") :
                                                           QStringLiteral("ready"));
                    if (preview.value().original_missing)
                    {
                        assets_.markOriginalMissing(id);
                    }
                    return;
                }
                if (preview.error().code == ErrorCode::kNotFound)
                {
                    assets_.markOriginalMissing(id);
                    assets_.setThumbnail(id, {}, QStringLiteral("missing"));
                    return;
                }
                assets_.setThumbnail(id, {}, QStringLiteral("failed"));
            },
            Qt::QueuedConnection);
    });
}

void StudioPresenter::requestPreviewForSelection()
{
    if (selected_asset_id_.isEmpty())
    {
        preview_loading_ = false;
        emit previewChanged();
        return;
    }
    const auto revision = ++preview_revision_;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post([this, asset_id, revision]() {
        Result<PreviewResult> preview =
            make_error(ErrorCode::kIo, "Catalog session is closed");
        if (service_ != nullptr)
        {
            PreviewRequest request;
            request.asset_id = asset_id;
            request.max_edge = kDefaultPreviewMaxEdge;
            request.request_revision = revision;
            request.cancellation = shutdown_.token();
            preview = service_->request_preview(request);
        }
        QMetaObject::invokeMethod(
            this,
            [this, revision, asset_id, preview = std::move(preview)]() mutable {
                if (revision != preview_revision_)
                {
                    return;
                }
                preview_loading_ = false;
                if (!preview)
                {
                    preview_url_.clear();
                    if (preview.error().code == ErrorCode::kNotFound)
                    {
                        assets_.markOriginalMissing(asset_id);
                        emit selectionChanged();
                        emit previewChanged();
                        return;
                    }
                    setError(qstring_from_utf8(preview.error().message));
                    emit previewChanged();
                    return;
                }
                if (preview.value().original_missing)
                {
                    assets_.markOriginalMissing(asset_id);
                    emit selectionChanged();
                }
                preview_url_ = QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path));
                emit previewChanged();
            },
            Qt::QueuedConnection);
    });
}

} // namespace ravo
