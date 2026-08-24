#include "ravo/desktop/studio_presenter.h"

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

[[nodiscard]] QString display_name_for(const AssetRecord &asset)
{
    const QUrl uri(qstring_from_utf8(asset.normalized_uri));
    const QString local = uri.toLocalFile();
    if (!local.isEmpty())
    {
        return QFileInfo(local).fileName();
    }
    return qstring_from_utf8(asset.id);
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
        return display_name_for(asset);
    case MediaTypeRole:
        return qstring_from_utf8(asset.media_type);
    case ImportStateRole:
        return qstring_from_utf8(asset.import_state);
    case ErrorRole:
        return asset.error_message ? qstring_from_utf8(*asset.error_message) : QString{};
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
            {ErrorRole, "errorText"}};
}

void AssetListModel::setAssets(std::vector<AssetRecord> assets)
{
    beginResetModel();
    assets_ = std::move(assets);
    endResetModel();
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

QUrl StudioPresenter::previewUrl() const
{
    return preview_url_;
}

bool StudioPresenter::previewLoading() const noexcept
{
    return preview_loading_;
}

QString StudioPresenter::viewMode() const
{
    return view_mode_;
}

AssetListModel *StudioPresenter::assets() noexcept
{
    return &assets_;
}

void StudioPresenter::setViewMode(const QString &mode)
{
    const QString normalized = mode == QStringLiteral("actual") ? QStringLiteral("actual") :
                                                                  QStringLiteral("fit");
    if (view_mode_ == normalized)
    {
        return;
    }
    view_mode_ = normalized;
    emit viewModeChanged();
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

void StudioPresenter::applyAssets(std::vector<AssetRecord> assets)
{
    const bool select_first = selected_asset_id_.isEmpty() && !assets.empty();
    assets_.setAssets(std::move(assets));
    if (select_first)
    {
        selectAsset(assets_.data(assets_.index(0, 0), AssetListModel::AssetIdRole).toString());
    }
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
            listed = service_->list_assets();
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
                applyAssets(std::move(listed).value());
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
            listed = service_->list_assets();
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
                applyAssets(std::move(listed).value());
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
    executor_.post([this, paths = std::move(paths)]() {
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
            listed = service_->list_assets();
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
                applyAssets(std::move(listed).value());
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
            [this, revision, preview = std::move(preview)]() mutable {
                if (revision != preview_revision_)
                {
                    return;
                }
                preview_loading_ = false;
                if (!preview)
                {
                    preview_url_.clear();
                    setError(qstring_from_utf8(preview.error().message));
                    emit previewChanged();
                    return;
                }
                preview_url_ = QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path));
                emit previewChanged();
            },
            Qt::QueuedConnection);
    });
}

} // namespace ravo
