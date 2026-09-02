#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QUrl>

#include "studio_qt.h"

namespace ravo
{
namespace
{

inline constexpr int kMaximumPendingImportThumbnails = 64;

[[nodiscard]] ImportCandidate placeholder_candidate(const std::string &path,
                                                    const std::string &source_root)
{
    ImportCandidate candidate;
    candidate.source_path = path;
    const QString qpath = qstring_from_utf8(path);
    candidate.display_name = utf8_from_qstring(QFileInfo(qpath).fileName());
    if (!source_root.empty())
    {
        const QString relative = QDir(qstring_from_utf8(source_root)).relativeFilePath(qpath);
        if (!relative.isEmpty() && !relative.startsWith(QLatin1String("..")))
            candidate.relative_path = utf8_from_qstring(QDir::fromNativeSeparators(relative));
    }
    if (candidate.relative_path.empty())
        candidate.relative_path = candidate.display_name;
    return candidate;
}

[[nodiscard]] QImage import_thumbnail_image(const RasterBuffer &raster)
{
    if (raster.width == 0 || raster.height == 0 ||
        raster.srgb.size() < static_cast<std::size_t>(raster.width) * raster.height * 3U)
        return {};
    QImage image(static_cast<int>(raster.width), static_cast<int>(raster.height),
                 QImage::Format_RGB888);
    const auto row_bytes = static_cast<std::size_t>(raster.width) * 3U;
    for (std::uint32_t row = 0; row < raster.height; ++row)
        std::memcpy(image.scanLine(static_cast<int>(row)),
                    raster.srgb.data() + static_cast<std::size_t>(row) * row_bytes, row_bytes);
    return image;
}

[[nodiscard]] ImportPreviewPolicy preview_policy(const QString &value)
{
    return value == QLatin1String("minimal")    ? ImportPreviewPolicy::kMinimal :
           value == QLatin1String("one-to-one") ? ImportPreviewPolicy::kOneToOne :
                                                  ImportPreviewPolicy::kStandard;
}

} // namespace

bool StudioPresenter::importPageOpen() const noexcept
{
    return import_page_open_;
}

bool StudioPresenter::importScanActive() const noexcept
{
    return import_scan_active_;
}

bool StudioPresenter::importPreviewWorkActive() const noexcept
{
    return import_preview_work_active_;
}

int StudioPresenter::importPreviewWorkCompleted() const noexcept
{
    return import_preview_work_completed_;
}

int StudioPresenter::importPreviewWorkTotal() const noexcept
{
    return import_preview_work_total_;
}

QString StudioPresenter::importSourceRoot() const
{
    return import_source_root_;
}

QString StudioPresenter::importDestination() const
{
    return import_destination_;
}

QString StudioPresenter::importSecondCopyDestination() const
{
    return import_second_copy_destination_;
}

QString StudioPresenter::importFilenameTemplate() const
{
    return import_filename_template_;
}

QString StudioPresenter::importMode() const
{
    return import_mode_;
}

QString StudioPresenter::importOrganization() const
{
    return import_organization_;
}

QString StudioPresenter::importPreviewPolicy() const
{
    return import_preview_policy_;
}

bool StudioPresenter::importRecursive() const noexcept
{
    return import_recursive_;
}

ImportCandidateListModel *StudioPresenter::importCandidates() noexcept
{
    return &import_candidates_;
}

void StudioPresenter::openImportPage()
{
    if (catalog_path_.isEmpty() || import_work_active_)
        return;
    import_page_open_ = true;
    emit importPageChanged();
    if (!import_source_root_.isEmpty())
        rescanImportSource();
}

void StudioPresenter::closeImportPage()
{
    if (import_work_active_)
        return;
    static_cast<void>(import_operation_.cancel("import_page_closed"));
    ++import_scan_generation_;
    import_scan_active_ = false;
    import_page_open_ = false;
    pending_import_thumbnail_rows_.clear();
    import_candidates_.setCandidates({});
    emit importPageChanged();
}

void StudioPresenter::setImportSourceRoot(const QString &path)
{
    const QString next = path.trimmed();
    if (next.isEmpty() || next == import_source_root_)
        return;
    import_source_root_ = next;
    emit importPageChanged();
    rescanImportSource();
}

void StudioPresenter::setImportDestination(const QString &path)
{
    const QString next = path.trimmed();
    if (next == import_destination_)
        return;
    import_destination_ = next;
    emit importPageChanged();
}

void StudioPresenter::setImportSecondCopyDestination(const QString &path)
{
    const QString next = path.trimmed();
    if (next == import_second_copy_destination_)
        return;
    import_second_copy_destination_ = next;
    emit importPageChanged();
}

void StudioPresenter::setImportFilenameTemplate(const QString &filename_template)
{
    if (filename_template == import_filename_template_)
        return;
    import_filename_template_ = filename_template;
    emit importPageChanged();
}

void StudioPresenter::setImportMode(const QString &mode)
{
    if (mode != QLatin1String("add") && mode != QLatin1String("copy") &&
        mode != QLatin1String("move"))
        return;
    if (import_mode_ == mode)
        return;
    import_mode_ = mode;
    emit importPageChanged();
}

void StudioPresenter::setImportOrganization(const QString &organization)
{
    if (organization != QLatin1String("single") && organization != QLatin1String("hierarchy") &&
        organization != QLatin1String("date"))
        return;
    if (import_organization_ == organization)
        return;
    import_organization_ = organization;
    emit importPageChanged();
}

void StudioPresenter::setImportPreviewPolicy(const QString &policy)
{
    if (policy != QLatin1String("minimal") && policy != QLatin1String("standard") &&
        policy != QLatin1String("one-to-one"))
        return;
    if (import_preview_policy_ == policy)
        return;
    import_preview_policy_ = policy;
    emit importPageChanged();
}

void StudioPresenter::setImportRecursive(const bool recursive)
{
    if (import_recursive_ == recursive)
        return;
    import_recursive_ = recursive;
    emit importPageChanged();
    if (!import_source_root_.isEmpty())
        rescanImportSource();
}

void StudioPresenter::rescanImportSource()
{
    if (service_ == nullptr || import_source_root_.isEmpty() || import_work_active_)
        return;
    static_cast<void>(import_operation_.cancel("import_source_changed"));
    import_operation_ = CancellationSource{};
    const auto token = import_operation_.token();
    const auto generation = ++import_scan_generation_;
    const std::string root = utf8_from_qstring(import_source_root_);
    const bool recursive = import_recursive_;
    import_scan_active_ = true;
    pending_import_thumbnail_rows_.clear();
    import_candidates_.setCandidates({});
    emit importPageChanged();
    executor_.post(
        [this, root, recursive, generation, token]()
        {
            Result<std::vector<std::string>> paths =
                service_ == nullptr ? make_error(ErrorCode::kIo, "Catalog session is closed") :
                                      service_->enumerate_import_inputs({root}, token, recursive);
            std::vector<ImportCandidate> candidates;
            TaskError failure;
            bool failed = false;
            if (!paths)
            {
                failure = paths.error();
                failed = true;
            }
            else
            {
                candidates.reserve(paths.value().size());
                for (const auto &path : paths.value())
                {
                    auto active = token.check();
                    if (!active)
                    {
                        failure = active.error();
                        failed = true;
                        break;
                    }
                    candidates.push_back(placeholder_candidate(path, root));
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, generation, candidates = std::move(candidates), failed,
                 failure = std::move(failure)]() mutable
                {
                    if (generation != import_scan_generation_ || !import_page_open_)
                        return;
                    import_scan_active_ = false;
                    if (failed)
                        setError(qstring_from_utf8(failure.message));
                    else
                        import_candidates_.setCandidates(std::move(candidates));
                    emit importPageChanged();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::ensureImportThumbnail(const int row)
{
    if (!import_page_open_ || import_work_active_ || row < 0 ||
        row >= import_candidates_.rowCount() || import_candidates_.inspected(row) ||
        !import_candidates_.thumbnail(row).isNull())
        return;
    const auto existing =
        std::find(pending_import_thumbnail_rows_.begin(), pending_import_thumbnail_rows_.end(), row);
    if (existing != pending_import_thumbnail_rows_.end())
        pending_import_thumbnail_rows_.erase(existing);
    if (pending_import_thumbnail_rows_.size() >=
        static_cast<std::size_t>(kMaximumPendingImportThumbnails))
        pending_import_thumbnail_rows_.pop_back();
    pending_import_thumbnail_rows_.push_front(row);
    kickImportCandidateWork();
}

void StudioPresenter::kickImportCandidateWork()
{
    if (import_candidate_work_in_flight_ || import_work_active_ || !import_page_open_)
        return;
    while (!pending_import_thumbnail_rows_.empty())
    {
        const int row = pending_import_thumbnail_rows_.front();
        pending_import_thumbnail_rows_.pop_front();
        if (row < 0 || row >= import_candidates_.rowCount() || import_candidates_.inspected(row) ||
            !import_candidates_.thumbnail(row).isNull())
            continue;
        startImportCandidateWork(row);
        return;
    }
}

void StudioPresenter::startImportCandidateWork(const int row)
{
    const QString source = import_candidates_.sourcePath(row);
    if (source.isEmpty())
    {
        kickImportCandidateWork();
        return;
    }
    const std::string root = utf8_from_qstring(import_source_root_);
    const auto generation = import_scan_generation_;
    const auto token = import_operation_.token();
    import_candidate_work_in_flight_ = true;
    const bool queued = executor_.post(
        [this, row, source, root, generation, token]()
        {
            ImportCandidate candidate;
            QImage image;
            if (service_ == nullptr)
            {
                candidate = placeholder_candidate(utf8_from_qstring(source), root);
                candidate.supported = false;
                candidate.error = make_error(ErrorCode::kIo, "Catalog session is closed");
            }
            else
            {
                auto inspected =
                    service_->inspect_import_candidate(utf8_from_qstring(source), root, token);
                if (!inspected)
                {
                    if (inspected.error().code != ErrorCode::kCancelled)
                    {
                        candidate = placeholder_candidate(utf8_from_qstring(source), root);
                        candidate.supported = false;
                        candidate.error = inspected.error();
                    }
                }
                else
                {
                    candidate = std::move(inspected).value();
                    if (candidate.supported)
                    {
                        auto decoded = service_->decode_import_candidate_thumbnail(
                            utf8_from_qstring(source), token);
                        if (decoded)
                            image = import_thumbnail_image(decoded.value());
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, row, source, generation, candidate = std::move(candidate),
                 image = std::move(image)]() mutable
                {
                    import_candidate_work_in_flight_ = false;
                    if (generation != import_scan_generation_ || !import_page_open_ ||
                        import_work_active_)
                    {
                        kickImportCandidateWork();
                        return;
                    }
                    if (row >= 0 && row < import_candidates_.rowCount() &&
                        import_candidates_.sourcePath(row) == source &&
                        !candidate.source_path.empty())
                    {
                        import_candidates_.updateCandidate(row, std::move(candidate));
                        if (!image.isNull())
                            import_candidates_.setThumbnail(row, std::move(image));
                    }
                    kickImportCandidateWork();
                },
                Qt::QueuedConnection);
        });
    if (!queued)
        import_candidate_work_in_flight_ = false;
}

void StudioPresenter::beginImportGalleryPlaceholders(const std::vector<std::string> &paths)
{
    import_gallery_placeholders_ = true;
    import_page_open_ = false;
    emit importPageChanged();
    std::vector<AssetRecord> placeholders;
    placeholders.reserve(paths.size());
    for (const auto &path : paths)
    {
        AssetRecord placeholder;
        placeholder.normalized_uri = path;
        placeholders.push_back(std::move(placeholder));
    }
    last_import_count_ = paths.size();
    last_import_selected_ = true;
    last_import_after_unix_ms_.reset();
    last_import_before_unix_ms_.reset();
    selected_asset_id_.clear();
    selection_anchor_id_.clear();
    selected_ids_.clear();
    applyAssets(std::move(placeholders), false, {}, {}, paths.size(), false);
}

void StudioPresenter::publishImportItem(const ImportItemResult &item, const int row)
{
    if (!import_gallery_placeholders_ || row < 0 || row >= assets_.rowCount())
        return;
    const bool last_import_was_available = lastImportAvailable();
    if (item.status == ImportItemStatus::kImported && item.asset)
    {
        const auto created = item.asset->created_unix_ms;
        last_import_after_unix_ms_ =
            last_import_after_unix_ms_ ? std::min(*last_import_after_unix_ms_, created) : created;
        last_import_before_unix_ms_ = last_import_before_unix_ms_ ?
                                          std::max(*last_import_before_unix_ms_, created) :
                                          created;
        const std::string asset_id = item.asset->id;
        assets_.replaceAssetAt(row, *item.asset);
        if (item.preview_cache_path)
            assets_.setThumbnail(asset_id,
                                 QUrl::fromLocalFile(qstring_from_utf8(*item.preview_cache_path)),
                                 QStringLiteral("ready"));
        if (item.preview_pending)
            pending_import_preview_ids_.push_back(asset_id);
        if (selected_asset_id_.isEmpty())
            selectAsset(qstring_from_utf8(asset_id));
        else
            emit thumbnailsChanged();
        if (!last_import_was_available && lastImportAvailable())
            emit folderChanged();
        return;
    }
    AssetRecord placeholder;
    placeholder.normalized_uri =
        item.input_path.empty() ? assets_.assetIdAt(row).toStdString() : item.input_path;
    placeholder.import_state = std::string(kImportStateFailed);
    if (item.error)
        placeholder.error_message = item.error->message;
    assets_.replaceAssetAt(row, std::move(placeholder));
}

void StudioPresenter::startPlannedImport()
{
    const QStringList selected = import_candidates_.selectedPaths();
    if (!import_page_open_ || import_scan_active_ || import_work_active_ || selected.isEmpty())
        return;
    if (import_mode_ != QLatin1String("add") && import_destination_.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Choose an import destination."));
        return;
    }
    ImportRequest request;
    for (const auto &path : selected)
        request.inputs.push_back(utf8_from_qstring(path));
    request.source_root = utf8_from_qstring(import_source_root_);
    request.mode = import_mode_ == QLatin1String("copy") ? ImportTransferMode::kCopy :
                   import_mode_ == QLatin1String("move") ? ImportTransferMode::kMove :
                                                           ImportTransferMode::kAdd;
    request.organization = import_organization_ == QLatin1String("hierarchy") ?
                               ImportOrganization::kPreserveHierarchy :
                           import_organization_ == QLatin1String("date") ?
                               ImportOrganization::kCaptureDate :
                               ImportOrganization::kSingleFolder;
    request.preview = preview_policy(import_preview_policy_);
    if (request.mode != ImportTransferMode::kAdd)
    {
        request.destination_directory = utf8_from_qstring(import_destination_);
        request.filename_template = utf8_from_qstring(import_filename_template_);
        request.second_copy_directory = utf8_from_qstring(import_second_copy_destination_);
    }
    request.recursive = false;
    request.defer_previews = true;
    static_cast<void>(import_operation_.cancel("planned_import_started"));
    ++import_scan_generation_;
    pending_import_thumbnail_rows_.clear();
    import_operation_ = CancellationSource{};
    request.cancellation = import_operation_.token();
    pending_import_paths_ = request.inputs;
    import_query_snapshot_ = current_query();
    pending_import_preview_policy_ = request.preview;
    import_defer_previews_ = true;
    import_results_.clear();
    import_next_index_ = 0U;
    pending_import_preview_ids_.clear();
    import_preview_operation_ = CancellationSource{};
    setImportWork(0, static_cast<int>(request.inputs.size()), true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Importing 0 / %1…")
                  .arg(request.inputs.size()));
    beginImportGalleryPlaceholders(request.inputs);
    if (request.mode == ImportTransferMode::kAdd)
    {
        startNextImportItem();
        return;
    }
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            auto batch = service_ == nullptr ?
                             Result<ImportBatchResult>{
                                 make_error(ErrorCode::kIo, "Catalog session is closed")} :
                             service_->execute_import(
                                 request,
                                 [this](const std::size_t completed, const std::size_t total,
                                        const ImportItemResult *item)
                                 {
                                     ImportItemResult copy;
                                     if (item != nullptr)
                                         copy = *item;
                                     QMetaObject::invokeMethod(
                                         this,
                                         [this, completed, total, copy = std::move(copy),
                                          has_item = item != nullptr]
                                         {
                                             setImportWork(static_cast<int>(completed),
                                                           static_cast<int>(total), true);
                                             if (has_item)
                                                 publishImportItem(copy,
                                                                   static_cast<int>(completed) - 1);
                                         },
                                         Qt::QueuedConnection);
                                 });
            QMetaObject::invokeMethod(
                this,
                [this, batch = std::move(batch)]() mutable
                {
                    if (!batch)
                    {
                        setImportWork(0, 0, false);
                        import_gallery_placeholders_ = false;
                        import_defer_previews_ = false;
                        last_import_selected_ = false;
                        last_import_count_ = 0U;
                        last_import_after_unix_ms_.reset();
                        last_import_before_unix_ms_.reset();
                        query_ = import_query_snapshot_;
                        setError(qstring_from_utf8(batch.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        reloadVisibleAssets();
                        return;
                    }
                    import_results_ = std::move(batch).value().items;
                    import_next_index_ = import_results_.size();
                    finishImportBatch();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::startNextImportPreview()
{
    if (pending_import_preview_ids_.empty())
    {
        import_preview_work_active_ = false;
        import_preview_work_completed_ = import_preview_work_total_;
        emit libraryWorkChanged();
        return;
    }
    const std::string asset_id = std::move(pending_import_preview_ids_.front());
    pending_import_preview_ids_.pop_front();
    const auto policy = pending_import_preview_policy_;
    const auto token = import_preview_operation_.token();
    executor_.post(
        [this, asset_id, policy, token]
        {
            auto preview =
                service_ == nullptr ?
                    Result<PreviewResult>{make_error(ErrorCode::kIo, "Catalog session is closed")} :
                    service_->build_import_preview(asset_id, policy, token);
            static_cast<void>(preview);
            QMetaObject::invokeMethod(
                this,
                [this]
                {
                    ++import_preview_work_completed_;
                    emit libraryWorkChanged();
                    startNextImportPreview();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::cancelImportPreviews()
{
    if (!import_preview_work_active_)
        return;
    static_cast<void>(import_preview_operation_.cancel("user_cancelled"));
    pending_import_preview_ids_.clear();
}

} // namespace ravo
