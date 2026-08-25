#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_commands.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QList>
#include <QMetaObject>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "studio_qt.h"

namespace ravo
{
StudioPresenter::StudioPresenter(QObject *parent)
    : QObject(parent)
    , assets_(this)
    , folders_(this)
{
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
        status_text_ = QStringLiteral("Engine failed to start.");
    }
}

StudioPresenter::~StudioPresenter()
{
    static_cast<void>(shutdown_.cancel("window_closed"));
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
    return std::make_unique<CatalogService>(*engine_, std::move(repository).value(),
                                            std::move(raster), std::move(cache).value());
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
           query_.reject_filter != RejectFilter::kInclude || !query_.tag.empty();
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
    return asset && asset->metadata.creator ? qstring_from_utf8(*asset->metadata.creator) : QString{};
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

void StudioPresenter::applyAssets(std::vector<AssetRecord> assets, const bool restore_selection)
{
    const QString previous = selected_asset_id_;
    assets_.setAssets(std::move(assets));
    std::unordered_set<std::string> kept;
    for (const auto &id : selected_ids_)
    {
        if (assets_.indexOf(qstring_from_utf8(id)) >= 0)
        {
            kept.insert(id);
        }
    }
    selected_ids_ = std::move(kept);
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
        activate_primary(qstring_from_utf8(remaining.front()), true);
        return;
    }
    if (selected_asset_id_.isEmpty() || assets_.indexOf(selected_asset_id_) < 0)
    {
        selectAsset(assets_.assetIdAt(0));
    }
}

void StudioPresenter::applyFolders(std::vector<FolderRecord> folders)
{
    folders_.setFolders(std::move(folders));
    emit folderChanged();
}

void StudioPresenter::selectFolder(const QString &folder_uri)
{
    const auto next = utf8_from_qstring(folder_uri);
    if (query_.folder_uri == next)
    {
        return;
    }
    query_.folder_uri = next;
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
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                listed = service_->list_assets(query);
                folders = service_->list_folders();
            }
            QMetaObject::invokeMethod(
                this,
                [this, listed = std::move(listed), folders = std::move(folders)]() mutable
                {
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
                    applyAssets(std::move(listed).value(), true);
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
        setError(QStringLiteral("Catalog path is not a local file."));
        return;
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Creating library…"));
    const auto path = utf8_from_qstring(local);
    executor_.post(
        [this, path]()
        {
            QString failure;
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            auto built = make_catalog_service(path, true);
            if (!built)
            {
                failure = qstring_from_utf8(built.error().message);
            }
            else
            {
                listed = built.value()->list_assets(query_);
                if (!listed)
                {
                    failure = qstring_from_utf8(listed.error().message);
                }
                else
                {
                    folders = built.value()->list_folders();
                    if (!folders)
                    {
                        failure = qstring_from_utf8(folders.error().message);
                    }
                    else
                    {
                        service_ = std::move(built).value();
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, failure = std::move(failure), listed = std::move(listed),
                 folders = std::move(folders)]() mutable
                {
                    setBusy(false);
                    if (!failure.isEmpty())
                    {
                        setError(failure);
                        setStatus(QStringLiteral("Create failed."));
                        return;
                    }
                    catalog_path_ = qstring_from_utf8(path);
                    thumbnail_requests_.clear();
                    emit catalogChanged();
                    setError({});
                    setStatus(QStringLiteral("Library created. Import photos or a folder."));
                    applyFolders(std::move(folders).value());
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
    executor_.post(
        [this, path]()
        {
            QString failure;
            Result<std::vector<AssetRecord>> listed =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<FolderRecord>> folders =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            auto built = make_catalog_service(path, false);
            if (!built)
            {
                failure = qstring_from_utf8(built.error().message);
            }
            else
            {
                listed = built.value()->list_assets(query_);
                if (!listed)
                {
                    failure = qstring_from_utf8(listed.error().message);
                }
                else
                {
                    folders = built.value()->list_folders();
                    if (!folders)
                    {
                        failure = qstring_from_utf8(folders.error().message);
                    }
                    else
                    {
                        service_ = std::move(built).value();
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, path, failure = std::move(failure), listed = std::move(listed),
                 folders = std::move(folders)]() mutable
                {
                    setBusy(false);
                    if (!failure.isEmpty())
                    {
                        setError(failure);
                        setStatus(QStringLiteral("Open failed."));
                        return;
                    }
                    catalog_path_ = qstring_from_utf8(path);
                    selected_asset_id_.clear();
                    clear_displayed_preview();
                    thumbnail_requests_.clear();
                    emit catalogChanged();
                    emit selectionChanged();
                    emit previewChanged();
                    setError({});
                    setStatus(QStringLiteral("Library opened."));
                    applyFolders(std::move(folders).value());
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

void StudioPresenter::exportSelectedToPath(const QString &path, const QString &filter)
{
    if (busy_ || catalog_path_.isEmpty() || selected_asset_id_.isEmpty())
    {
        return;
    }
    QString output = path.trimmed();
    if (output.startsWith(QStringLiteral("file:")))
    {
        output = QUrl(output).toLocalFile();
    }
    if (output.isEmpty())
    {
        setError(QStringLiteral("Export path must not be empty."));
        return;
    }
    auto format = export_format_from_ui(output, filter);
    if (!format)
    {
        setError(qstring_from_utf8(format.error().message));
        return;
    }
    if (QFileInfo(output).suffix().isEmpty() && format.value() != ExportFormat::kOriginalCopy)
    {
        output += QString::fromUtf8(
            export_format_extension(format.value()).data(),
            static_cast<qsizetype>(export_format_extension(format.value()).size()));
    }
    setBusy(true);
    setError({});
    setStatus(QStringLiteral("Exporting…"));
    executor_.post(
        [this, asset_id = utf8_from_qstring(selected_asset_id_),
         output_path = utf8_from_qstring(output), export_format = format.value()]()
        {
            Result<ExportResult> exported = make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                ExportRequest request;
                request.asset_id = asset_id;
                request.output_path = output_path;
                request.format = export_format;
                request.cancellation = shutdown_.token();
                exported = service_->export_asset(request);
            }
            QMetaObject::invokeMethod(
                this,
                [this, exported = std::move(exported)]() mutable
                {
                    setBusy(false);
                    if (!exported)
                    {
                        setError(qstring_from_utf8(exported.error().message));
                        setStatus(QStringLiteral("Export failed."));
                        return;
                    }
                    setStatus(QStringLiteral("Exported %1 (%2×%3)")
                                  .arg(QFileInfo(qstring_from_utf8(exported.value().output_path))
                                           .fileName())
                                  .arg(exported.value().width)
                                  .arg(exported.value().height));
                },
                Qt::QueuedConnection);
        });
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
    executor_.post(
        [this, paths = std::move(paths), query = current_query()]()
        {
            Result<std::vector<ImportItemResult>> imported =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                imported = service_->import_inputs(
                    paths, shutdown_.token(),
                    [this](const std::size_t completed, const std::size_t total)
                    {
                        const auto completed_count = static_cast<int>(completed);
                        const auto total_count = static_cast<int>(total);
                        QMetaObject::invokeMethod(
                            this,
                            [this, completed_count, total_count]()
                            {
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
            Result<std::vector<FolderRecord>> folders = std::vector<FolderRecord>{};
            if (service_ != nullptr)
            {
                listed = service_->list_assets(query);
                folders = service_->list_folders();
            }
            QMetaObject::invokeMethod(
                this,
                [this, results = std::move(results), listed = std::move(listed),
                 folders = std::move(folders), first_error = std::move(first_error)]() mutable
                {
                    setBusy(false);
                    if (!listed)
                    {
                        setError(qstring_from_utf8(listed.error().message));
                        setStatus(QStringLiteral("Import failed."));
                        return;
                    }
                    if (!folders)
                    {
                        setError(qstring_from_utf8(folders.error().message));
                        setStatus(QStringLiteral("Import failed."));
                        return;
                    }
                    setError(first_error);
                    setStatus(describe_import(results));
                    applyFolders(std::move(folders).value());
                    applyAssets(std::move(listed).value(), true);
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
    preview_loading_ = !asset_id.isEmpty();
    before_after_ = false;
    crop_tool_active_ = false;
    pending_preview_.reset();
    load_develop_for_selection();
    publish_selection();
    emit previewChanged();
    emit editChanged();
    requestPreviewForSelection();
}

std::vector<std::string> StudioPresenter::selected_asset_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(selected_ids_.size());
    for (int row = 0; row < assets_.rowCount(); ++row)
    {
        const auto id = utf8_from_qstring(assets_.assetIdAt(row));
        if (selected_ids_.contains(id))
        {
            ids.push_back(id);
        }
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
        [field, text](CatalogService &service, const std::string_view asset_id) -> Result<AssetRecord>
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
                metadata.title = text.empty() ? std::optional<std::string>{} : std::optional<std::string>{text};
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

void StudioPresenter::createSnapshot(const QString &label)
{
    const auto text = utf8_from_qstring(label);
    mutate_selected_review([text](CatalogService &service, const std::string_view asset_id)
                           { return service.create_recipe_snapshot(asset_id, text); });
    load_develop_for_selection();
}

void StudioPresenter::restoreHistory(const int history_id)
{
    mutate_selected_review(
        [history_id](CatalogService &service, const std::string_view asset_id)
        { return service.restore_recipe_history(asset_id, history_id); });
    load_develop_for_selection();
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
    query_.tag.clear();
    emit filterChanged();
    reloadVisibleAssets();
}

void StudioPresenter::executeCommand(const QString &id, const QVariant &argument)
{
    using namespace command_id;
    static const QStringList kWindowCommands{
        QLatin1String(kLibraryCreate),      QLatin1String(kLibraryOpen),
        QLatin1String(kLibraryImportFiles), QLatin1String(kLibraryImportFolder),
        QLatin1String(kLibraryExport),      QLatin1String(kWindowSettings),
        QLatin1String(kWindowClose),        QLatin1String(kWindowQuit),
        QLatin1String(kWindowAbout),
    };
    if (kWindowCommands.contains(id))
    {
        emit uiCommandRequested(id);
        return;
    }

    if (id == QLatin1String(kLibraryExportWrite))
    {
        const auto fields = argument.toMap();
        exportSelectedToPath(fields.value(QStringLiteral("path"), argument.toString()).toString(),
                             fields.value(QStringLiteral("filter")).toString());
    }
    else if (id == QLatin1String(kPhotoSelect))
    {
        QString asset_id = argument.toString();
        QString mode = QStringLiteral("single");
        const auto fields = argument.toMap();
        if (!fields.isEmpty())
        {
            asset_id = fields.value(QStringLiteral("id")).toString();
            mode = fields.value(QStringLiteral("mode"), QStringLiteral("single")).toString();
        }
        if (mode == QStringLiteral("range"))
        {
            selectAssetRange(asset_id);
        }
        else if (mode == QStringLiteral("toggle"))
        {
            toggleAssetSelected(asset_id);
        }
        else
        {
            selectAsset(asset_id);
        }
    }
    else if (id == QLatin1String(kPhotoRate))
    {
        setRating(argument.toInt());
    }
    else if (id == QLatin1String(kPhotoColor))
    {
        setColorLabel(argument.toString());
    }
    else if (id == QLatin1String(kPhotoReject))
    {
        toggleRejected();
    }
    else if (id == QLatin1String(kPhotoRemove))
    {
        remove_selected_from_catalog();
    }
    else if (id == QLatin1String(kPhotoRemoveFromDisk))
    {
        remove_selected_from_disk();
    }
    else if (id == QLatin1String(kPhotoPrevious))
    {
        if (argument.toString() == QStringLiteral("range") && !selected_asset_id_.isEmpty())
        {
            const auto row = assets_.indexOf(selected_asset_id_);
            if (row > 0)
            {
                selectAssetRange(assets_.assetIdAt(row - 1));
            }
        }
        else
        {
            selectPrevious();
        }
    }
    else if (id == QLatin1String(kPhotoNext))
    {
        if (argument.toString() == QStringLiteral("range") && !selected_asset_id_.isEmpty())
        {
            const auto row = assets_.indexOf(selected_asset_id_);
            if (row >= 0 && row + 1 < assets_.rowCount())
            {
                selectAssetRange(assets_.assetIdAt(row + 1));
            }
        }
        else
        {
            selectNext();
        }
    }
    else if (id == QLatin1String(kViewGrid))
    {
        returnToGrid();
    }
    else if (id == QLatin1String(kViewLoupe))
    {
        openLoupe();
    }
    else if (id == QLatin1String(kViewDevelop))
    {
        openDevelop();
    }
    else if (id == QLatin1String(kViewFit))
    {
        setZoomMode(QStringLiteral("fit"));
    }
    else if (id == QLatin1String(kViewFill))
    {
        setZoomMode(QStringLiteral("fill"));
    }
    else if (id == QLatin1String(kViewActual))
    {
        setZoomMode(QStringLiteral("actual"));
    }
    else if (id == QLatin1String(kEditUndo))
    {
        undoEdit();
    }
    else if (id == QLatin1String(kEditRedo))
    {
        redoEdit();
    }
    else if (id == QLatin1String(kEditResetAll))
    {
        resetAllEdits();
    }
    else if (id == QLatin1String(kEditResetSection))
    {
        resetSection(argument.toString());
    }
    else if (id == QLatin1String(kEditResetControl))
    {
        resetControl(argument.toString());
    }
    else if (id == QLatin1String(kEditSetNumber))
    {
        const auto fields = argument.toMap();
        const auto name = fields.value(QStringLiteral("name")).toString();
        const auto value = fields.value(QStringLiteral("value")).toDouble();
        if (fields.value(QStringLiteral("live")).toBool())
        {
            previewDevelopNumber(name, value);
        }
        else
        {
            setDevelopNumber(name, value);
        }
    }
    else if (id == QLatin1String(kEditSetToneCurve))
    {
        const auto fields = argument.toMap();
        const auto points = fields.value(QStringLiteral("points")).toList();
        if (fields.value(QStringLiteral("live")).toBool())
        {
            previewToneCurve(points);
        }
        else
        {
            setToneCurve(points);
        }
    }
    else if (id == QLatin1String(kEditSetCrop))
    {
        const auto fields = argument.toMap();
        const auto x = fields.value(QStringLiteral("x")).toDouble();
        const auto y = fields.value(QStringLiteral("y")).toDouble();
        const auto width = fields.value(QStringLiteral("width")).toDouble();
        const auto height = fields.value(QStringLiteral("height")).toDouble();
        if (fields.value(QStringLiteral("live")).toBool())
        {
            previewCropRect(x, y, width, height);
        }
        else
        {
            setCropRect(x, y, width, height);
        }
    }
    else if (id == QLatin1String(kEditSetCropAspect))
    {
        setCropAspect(argument.toString());
    }
    else if (id == QLatin1String(kEditRotateLeft))
    {
        rotateLeft();
    }
    else if (id == QLatin1String(kEditRotateRight))
    {
        rotateRight();
    }
    else if (id == QLatin1String(kEditFlipHorizontal))
    {
        flipHorizontal();
    }
    else if (id == QLatin1String(kEditFlipVertical))
    {
        flipVertical();
    }
    else if (id == QLatin1String(kEditCropTool))
    {
        setCropToolActive(argument.isValid() ? argument.toBool() : !crop_tool_active_);
    }
    else if (id == QLatin1String(kEditBeforeAfter))
    {
        toggleBeforeAfter();
    }
    else if (id == QLatin1String(kPhotoSetTags))
    {
        setAssetTags(argument.toString());
    }
    else if (id == QLatin1String(kPhotoSetMetadata))
    {
        const auto fields = argument.toMap();
        setMetadataField(fields.value(QStringLiteral("name")).toString(),
                         fields.value(QStringLiteral("value")).toString());
    }
    else if (id == QLatin1String(kPhotoCreateSnapshot))
    {
        createSnapshot(argument.toString());
    }
    else if (id == QLatin1String(kPhotoRestoreHistory))
    {
        restoreHistory(argument.toInt());
    }
    else if (id == QLatin1String(kLibrarySetTagFilter))
    {
        setTagFilter(argument.toString());
    }
    else
    {
        setError(QStringLiteral("Unknown command: %1").arg(id));
    }
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
                    applyAssets(std::move(listed).value(), false);
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
                            QStringLiteral("Removed from catalog. Original file was not deleted.") :
                            QStringLiteral(
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
                    applyAssets(std::move(listed).value(), false);
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
                    setStatus(count == 1 ?
                                  QStringLiteral("Deleted original file and catalog record.") :
                                  QStringLiteral("Deleted %1 original files and catalog records.")
                                      .arg(count));
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
