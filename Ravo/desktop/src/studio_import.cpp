#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QCoreApplication>
#include <QImage>
#include <QMetaObject>

#include "studio_qt.h"

namespace ravo
{
namespace
{

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
    import_thumbnail_requests_.clear();
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
    import_thumbnail_requests_.clear();
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
                        break;
                    auto candidate = service_->inspect_import_candidate(path, root, token);
                    if (!candidate)
                    {
                        ImportCandidate unavailable;
                        unavailable.source_path = path;
                        unavailable.display_name =
                            qstring_from_utf8(path).section('/', -1).toStdString();
                        unavailable.supported = false;
                        unavailable.error = candidate.error();
                        candidates.push_back(std::move(unavailable));
                    }
                    else
                        candidates.push_back(std::move(candidate).value());
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
    if (!import_page_open_ || row < 0 || row >= import_candidates_.rowCount() ||
        !import_thumbnail_requests_.insert(row).second)
        return;
    const QString source = import_candidates_.sourcePath(row);
    const auto generation = import_scan_generation_;
    const auto token = import_operation_.token();
    executor_.post(
        [this, row, source, generation, token]()
        {
            Result<RasterBuffer> decoded =
                service_ == nullptr ?
                    make_error(ErrorCode::kIo, "Catalog session is closed") :
                    service_->decode_import_candidate_thumbnail(utf8_from_qstring(source), token);
            QImage image;
            if (decoded)
                image = import_thumbnail_image(decoded.value());
            QMetaObject::invokeMethod(
                this,
                [this, row, generation, image = std::move(image)]() mutable
                {
                    import_thumbnail_requests_.erase(row);
                    if (generation == import_scan_generation_ && import_page_open_ &&
                        !image.isNull())
                        import_candidates_.setThumbnail(row, std::move(image));
                },
                Qt::QueuedConnection);
        });
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
    request.destination_directory = utf8_from_qstring(import_destination_);
    request.recursive = false;
    request.defer_previews = true;
    import_operation_ = CancellationSource{};
    request.cancellation = import_operation_.token();
    pending_import_paths_ = request.inputs;
    import_query_snapshot_ = current_query();
    setImportWork(0, static_cast<int>(request.inputs.size()), true);
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Importing 0 / %1…")
                  .arg(request.inputs.size()));
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            auto batch = service_ == nullptr ?
                             Result<ImportBatchResult>{
                                 make_error(ErrorCode::kIo, "Catalog session is closed")} :
                             service_->execute_import(
                                 request,
                                 [this](const std::size_t completed, const std::size_t total,
                                        const ImportItemResult *)
                                 {
                                     QMetaObject::invokeMethod(
                                         this,
                                         [this, completed, total]
                                         {
                                             setImportWork(static_cast<int>(completed),
                                                           static_cast<int>(total), true);
                                         },
                                         Qt::QueuedConnection);
                                 });
            QMetaObject::invokeMethod(
                this,
                [this, batch = std::move(batch), policy = request.preview]() mutable
                {
                    if (!batch)
                    {
                        setImportWork(0, 0, false);
                        setError(qstring_from_utf8(batch.error().message));
                        setStatus(QCoreApplication::translate("StudioPresenter", "Import failed."));
                        return;
                    }
                    import_results_ = std::move(batch).value().items;
                    import_next_index_ = import_results_.size();
                    pending_import_preview_policy_ = policy;
                    import_preview_operation_ = CancellationSource{};
                    pending_import_preview_ids_.clear();
                    for (const auto &item : import_results_)
                        if (item.status == ImportItemStatus::kImported && item.asset &&
                            item.preview_pending)
                            pending_import_preview_ids_.push_back(item.asset->id);
                    if (!pending_import_preview_ids_.empty())
                        import_page_open_ = false;
                    emit importPageChanged();
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
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, preview = std::move(preview)]() mutable
                {
                    ++import_preview_work_completed_;
                    if (preview)
                    {
                        assets_.setThumbnail(
                            asset_id,
                            QUrl::fromLocalFile(qstring_from_utf8(preview.value().cache_path)),
                            QStringLiteral("ready"));
                        emit thumbnailsChanged();
                    }
                    else if (preview.error().code != ErrorCode::kCancelled)
                        setError(qstring_from_utf8(preview.error().message));
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
