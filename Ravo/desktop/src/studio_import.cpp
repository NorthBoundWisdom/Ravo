#include "ravo/desktop/studio_presenter.h"
#include "studio_import_destination_preview_controller.h"
#include "studio_import_scan_controller.h"
#include "studio_import_workspace.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/services/import_thumbnail.h"
#include "ravo/desktop/studio_import_preferences.h"
#include "ravo/services/ingest_transport.h"
#include "studio_qt.h"
#include "studio_import_thumbnail_controller.h"

namespace ravo
{
namespace
{

[[nodiscard]] QVariantMap native_support_to_map(const NativeIngestPlatformSupport &support)
{
    return QVariantMap{
        {QStringLiteral("schema"), qstring_from_utf8(support.schema)},
        {QStringLiteral("platform"), qstring_from_utf8(support.platform)},
        {QStringLiteral("adapterPackaged"), support.adapter_packaged},
        {QStringLiteral("ptpUsb"),
         qstring_from_utf8(std::string(native_ingest_support_state_name(support.ptp_usb)))},
        {QStringLiteral("mtp"),
         qstring_from_utf8(std::string(native_ingest_support_state_name(support.mtp)))},
        {QStringLiteral("reason"), qstring_from_utf8(support.reason)},
        {QStringLiteral("ptpPlannedStack"), qstring_from_utf8(support.ptp_planned_stack)},
        {QStringLiteral("mtpPlannedStack"), qstring_from_utf8(support.mtp_planned_stack)},
    };
}

[[nodiscard]] QVariantMap ingest_report_to_map(const IngestBatchResult &detailed)
{
    const auto &batch = detailed.import;
    QVariantList items;
    items.reserve(static_cast<int>(batch.items.size()));
    for (const auto &item : batch.items)
    {
        QVariantMap row{
            {QStringLiteral("status"),
             item.status == ImportItemStatus::kImported    ? QStringLiteral("imported") :
             item.status == ImportItemStatus::kDuplicate   ? QStringLiteral("duplicate") :
             item.status == ImportItemStatus::kUnsupported ? QStringLiteral("unsupported") :
             item.status == ImportItemStatus::kSkipped     ? QStringLiteral("skipped") :
                                                             QStringLiteral("failed")},
            {QStringLiteral("inputPath"), qstring_from_utf8(item.input_path)},
            {QStringLiteral("copiesVerified"), item.copies_verified},
        };
        if (item.destination_path)
            row.insert(QStringLiteral("destinationPath"),
                       qstring_from_utf8(*item.destination_path));
        if (item.error)
        {
            row.insert(QStringLiteral("error"), qstring_from_utf8(item.error->message));
            const auto reason = item.error->context.find("reason");
            if (reason != item.error->context.end())
                row.insert(QStringLiteral("reason"), qstring_from_utf8(reason->second));
        }
        items.push_back(row);
    }
    return QVariantMap{
        {QStringLiteral("transport"), qstring_from_utf8(detailed.transport)},
        {QStringLiteral("sourceUri"), qstring_from_utf8(detailed.source_uri)},
        {QStringLiteral("imported"), static_cast<int>(batch.imported)},
        {QStringLiteral("duplicates"), static_cast<int>(batch.duplicates)},
        {QStringLiteral("unsupported"), static_cast<int>(batch.unsupported)},
        {QStringLiteral("failed"), static_cast<int>(batch.failed)},
        {QStringLiteral("skipped"), static_cast<int>(batch.skipped)},
        {QStringLiteral("verifiedSecondCopies"), static_cast<int>(batch.verified_second_copies)},
        {QStringLiteral("resumeBatchId"),
         detailed.resume_batch_id ? qstring_from_utf8(*detailed.resume_batch_id) : QString()},
        {QStringLiteral("resumeCheckpointCleared"), detailed.resume_checkpoint_cleared},
        {QStringLiteral("items"), items},
        {QStringLiteral("nativeSupport"), native_support_to_map(detailed.support)},
    };
}

[[nodiscard]] bool uses_ingest_copy_path(const QString &transport, const QString &mode)
{
    if (mode != QLatin1String("copy"))
        return false;
    return transport == QLatin1String("filesystem-card") ||
           transport == QLatin1String("ptp-stub") || transport == QLatin1String("ptp-usb") ||
           transport == QLatin1String("mtp");
}

} // namespace

bool StudioPresenter::importPageOpen() const noexcept
{
    return import_page_open_;
}

bool StudioPresenter::importScanActive() const noexcept
{
    return import_workspace_->scan && import_workspace_->scan->active();
}

int StudioPresenter::importDuplicateCount() const noexcept
{
    return import_workspace_->scan ? import_workspace_->scan->duplicateCount() : 0;
}

int StudioPresenter::importScanCompleted() const noexcept
{
    return import_workspace_->scan ? import_workspace_->scan->completed() : 0;
}

int StudioPresenter::importScanTotal() const noexcept
{
    return import_workspace_->scan ? import_workspace_->scan->total() : 0;
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

QString StudioPresenter::importDestinationError() const
{
    return import_workspace_ ? import_workspace_->draft.destination_error : QString{};
}

QString StudioPresenter::importSourceRoot() const
{
    return import_workspace_ ? import_workspace_->draft.source_root : QString{};
}

ImportDraft StudioPresenter::importDraft() const
{
    return import_workspace_ ? import_workspace_->draft : ImportDraft{};
}

QString StudioPresenter::importIngestTransport() const
{
    return import_ingest_transport_;
}

QString StudioPresenter::importIngestSourceUri() const
{
    if (import_workspace_->draft.source_root.isEmpty())
        return {};
    const auto root = utf8_from_qstring(import_workspace_->draft.source_root);
    if (import_ingest_transport_ == QLatin1String("ptp-stub"))
        return qstring_from_utf8(format_ptp_stub_ingest_uri(root));
    if (import_ingest_transport_ == QLatin1String("ptp-usb"))
        return QStringLiteral("ravo-ingest:ptp-usb:… (adapter not packaged)");
    if (import_ingest_transport_ == QLatin1String("mtp"))
        return QStringLiteral("ravo-ingest:mtp:… (adapter not packaged)");
    return qstring_from_utf8(format_filesystem_card_ingest_uri(root));
}

QVariantMap StudioPresenter::importNativeSupport() const
{
    return import_native_support_;
}

QVariantMap StudioPresenter::importIngestReport() const
{
    return import_ingest_report_;
}

QString StudioPresenter::importResumeBatchId() const
{
    return import_resume_batch_id_;
}

QString StudioPresenter::importDestination() const
{
    return import_workspace_->draft.destination;
}

bool StudioPresenter::importReady() const
{
    const bool native = import_ingest_transport_ == QLatin1String("ptp-usb") ||
                        import_ingest_transport_ == QLatin1String("mtp");
    return import_page_open_ && !native &&
           !(import_workspace_->scan && import_workspace_->scan->active()) &&
           !import_preflight_active_ && !import_work_active_ && import_workspace_->scan &&
           import_workspace_->scan->catalogRevision().has_value() &&
           import_candidates_.selectedCount() > 0 &&
           import_workspace_->draft.mode != QLatin1String("move") &&
           (import_workspace_->draft.mode == QLatin1String("add") ||
            import_workspace_->draft.destination_valid);
}

QUrl StudioPresenter::importDestinationFolderUrl() const
{
    return import_workspace_->draft.destination.isEmpty() ?
               defaultCatalogFolder() :
               QUrl::fromLocalFile(import_workspace_->draft.destination);
}

QUrl StudioPresenter::importSourceFolderUrl() const
{
    return import_workspace_->draft.source_root.isEmpty() ?
               defaultCatalogFolder() :
               QUrl::fromLocalFile(import_workspace_->draft.source_root);
}

QUrl StudioPresenter::importSecondCopyFolderUrl() const
{
    return import_workspace_->draft.second_copy_destination.isEmpty() ?
               importDestinationFolderUrl() :
               QUrl::fromLocalFile(import_workspace_->draft.second_copy_destination);
}

void StudioPresenter::validateImportDestination()
{
    const auto path = import_workspace_->draft.destination;
    import_workspace_->draft.destination_valid = false;
    import_workspace_->draft.destination_error =
        path.isEmpty() ?
            QCoreApplication::translate("StudioPresenter", "Choose an import destination.") :
            QString{};
    emit importPageChanged();
    if (path.isEmpty())
        return;
    executor_.post(
        [this, path]()
        {
            const QFileInfo directory(path);
            const bool available = directory.isDir() && directory.isWritable();
            QMetaObject::invokeMethod(
                this,
                [this, path, available]()
                {
                    if (path != import_workspace_->draft.destination || !import_page_open_)
                        return;
                    import_workspace_->draft.destination_valid = available;
                    import_workspace_->draft.destination_error =
                        available ?
                            QString{} :
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Destination unavailable. Reconnect the drive or choose another folder.");
                    emit importPageChanged();
                },
                Qt::QueuedConnection);
        });
}

QString StudioPresenter::importSecondCopyDestination() const
{
    return import_workspace_->draft.second_copy_destination;
}

QString StudioPresenter::importFilenameTemplate() const
{
    return import_workspace_->draft.filename_pattern;
}

QString StudioPresenter::importMode() const
{
    return import_workspace_->draft.mode;
}

QString StudioPresenter::importOrganization() const
{
    return import_workspace_->draft.organization;
}

QString StudioPresenter::importPreviewPolicy() const
{
    return import_workspace_->draft.preview_policy;
}

bool StudioPresenter::importRecursive() const noexcept
{
    return import_recursive_;
}

ImportCandidateListModel *StudioPresenter::importCandidates() noexcept
{
    return &import_candidates_;
}

FilesystemBrowserModel *StudioPresenter::importSourceFolders() noexcept
{
    return &import_source_folders_;
}

FilesystemBrowserModel *StudioPresenter::importDestinationFolders() noexcept
{
    return &import_destination_folders_;
}

void StudioPresenter::openImportPage()
{
    if (catalog_path_.isEmpty() || import_work_active_)
        return;
    import_page_open_ = true;
    import_workspace_->draft.mode = QStringLiteral("copy");
    const auto source = StudioImportPreferences{}.loadLastSource();
    if (source)
        import_workspace_->draft.source_root = source.value();
    else
    {
        import_workspace_->draft.source_root.clear();
        setError(qstring_from_utf8(source.error().message));
    }
    const auto destination = StudioImportPreferences{}.loadLastDestination();
    if (destination)
        import_workspace_->draft.destination = destination.value();
    else
    {
        import_workspace_->draft.destination.clear();
        setError(qstring_from_utf8(destination.error().message));
    }
    validateImportDestination();
    refreshImportNativeSupport();
    import_source_folders_.loadUserDirectory();
    import_destination_folders_.loadUserDirectory();
    if (!import_workspace_->draft.source_root.isEmpty())
        import_source_folders_.revealFolder(import_workspace_->draft.source_root);
    if (!import_workspace_->draft.destination.isEmpty())
        import_destination_folders_.revealFolder(import_workspace_->draft.destination);
    emit importPageChanged();
    if (!import_workspace_->draft.source_root.isEmpty())
        rescanImportSource();
}

void StudioPresenter::closeImportPage()
{
    if (import_work_active_)
        return;
    static_cast<void>(import_operation_.cancel("import_page_closed"));
    if (import_workspace_->thumbnails)
        import_workspace_->thumbnails->cancel("import_page_closed");
    if (import_workspace_->scan)
        import_workspace_->scan->abandon("import_page_closed");
    import_preflight_active_ = false;
    import_page_open_ = false;
    if (import_workspace_->thumbnails)
        import_workspace_->thumbnails->clearPending();
    import_candidates_.setCandidates({});
    emit importPageChanged();
}

void StudioPresenter::setImportSourceRoot(const QString &path)
{
    if (path.isEmpty() || import_work_active_ || import_preflight_active_)
        return;
    const QString next = QDir::cleanPath(path);
    const auto remembered = StudioImportPreferences{}.rememberSource(next);
    if (!remembered)
    {
        setError(qstring_from_utf8(remembered.error().message));
        if (remembered.error().code == ErrorCode::kValidation)
            return;
    }
    import_workspace_->draft.source_root = next;
    import_source_folders_.revealFolder(next);
    emit importPageChanged();
    rescanImportSource();
}

void StudioPresenter::setImportDestination(const QString &path)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = path.isEmpty() ? QString{} : QDir::cleanPath(path);
    if (next == import_workspace_->draft.destination)
    {
        if (import_workspace_->destination_preview)
            import_workspace_->destination_preview->invalidateCacheKey();
        if (!import_workspace_->draft.destination_error.isEmpty())
            import_destination_folders_.loadUserDirectory();
        import_destination_folders_.revealFolder(next);
        validateImportDestination();
        return;
    }
    import_workspace_->draft.destination = next;
    import_destination_folders_.revealFolder(next);
    validateImportDestination();
    emit importPageChanged();
}

void StudioPresenter::setImportSecondCopyDestination(const QString &path)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = path.trimmed();
    if (next == import_workspace_->draft.second_copy_destination)
        return;
    import_workspace_->draft.second_copy_destination = next;
    emit importPageChanged();
}

void StudioPresenter::setImportFilenameTemplate(const QString &filename_template)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (filename_template == import_workspace_->draft.filename_pattern)
        return;
    import_workspace_->draft.filename_pattern = filename_template;
    emit importPageChanged();
}

void StudioPresenter::setImportMode(const QString &mode)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (mode != QLatin1String("add") && mode != QLatin1String("copy") &&
        mode != QLatin1String("move"))
        return;
    if (import_workspace_->draft.mode == mode)
        return;
    import_workspace_->draft.mode = mode;
    emit importPageChanged();
}

void StudioPresenter::setImportOrganization(const QString &organization)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (organization != QLatin1String("single") && organization != QLatin1String("hierarchy") &&
        organization != QLatin1String("date") && organization != QLatin1String("month"))
        return;
    if (import_workspace_->draft.organization == organization)
        return;
    import_workspace_->draft.organization = organization;
    emit importPageChanged();
}

void StudioPresenter::setImportPreviewPolicy(const QString &policy)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (policy != QLatin1String("minimal") && policy != QLatin1String("standard") &&
        policy != QLatin1String("one-to-one"))
        return;
    if (import_workspace_->draft.preview_policy == policy)
        return;
    import_workspace_->draft.preview_policy = policy;
    emit importPageChanged();
}

void StudioPresenter::setImportRecursive(const bool recursive)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (import_recursive_ == recursive)
        return;
    import_recursive_ = recursive;
    emit importPageChanged();
    if (!import_workspace_->draft.source_root.isEmpty())
        rescanImportSource();
}

void StudioPresenter::setImportIngestTransport(const QString &transport)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = transport.trimmed();
    if (next != QLatin1String("filesystem-card") && next != QLatin1String("ptp-stub") &&
        next != QLatin1String("ptp-usb") && next != QLatin1String("mtp"))
        return;
    if (import_ingest_transport_ == next)
        return;
    import_ingest_transport_ = next;
    if ((next == QLatin1String("ptp-stub") || next == QLatin1String("ptp-usb") ||
         next == QLatin1String("mtp")) &&
        import_workspace_->draft.mode != QLatin1String("copy"))
        import_workspace_->draft.mode = QStringLiteral("copy");
    refreshImportNativeSupport();
    emit importPageChanged();
}

void StudioPresenter::setImportResumeBatchId(const QString &batch_id)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = batch_id.trimmed();
    if (import_resume_batch_id_ == next)
        return;
    import_resume_batch_id_ = next;
    emit importPageChanged();
}

void StudioPresenter::refreshImportNativeSupport()
{
    import_native_support_ = native_support_to_map(probe_native_ingest_support());
    emit importPageChanged();
}

void StudioPresenter::rescanImportSource()
{
    if (service_ == nullptr || !import_workspace_ || !import_workspace_->scan ||
        import_workspace_->draft.source_root.isEmpty() || import_work_active_)
        return;
    static_cast<void>(import_operation_.cancel("import_source_changed"));
    import_operation_ = CancellationSource{};
    import_workspace_->scan->startRescan();
}

void StudioPresenter::ensureImportThumbnail(const int row)
{
    if (import_workspace_->thumbnails)
        import_workspace_->thumbnails->ensure(row);
}

void StudioPresenter::setImportThumbnailViewportDemand(const QVariantList &rows, const int prefetch,
                                                       const int current_row)
{
    if (!import_workspace_->thumbnails)
        return;
    std::vector<int> visible;
    visible.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto &value : rows)
        visible.push_back(value.toInt());
    import_workspace_->thumbnails->setViewportDemand(visible, prefetch, current_row);
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
        if (!pending_import_destination_.isEmpty() && !import_destination_remembered_)
        {
            import_destination_remembered_ = true;
            const auto remembered =
                StudioImportPreferences{}.rememberDestination(pending_import_destination_);
            if (!remembered)
            {
                import_preference_error_ = QCoreApplication::translate(
                    "StudioPresenter",
                    "Photos imported, but the destination could not be remembered.");
                setError(import_preference_error_);
            }
        }
        const auto created = item.asset->created_unix_ms;
        last_import_after_unix_ms_ =
            last_import_after_unix_ms_ ? std::min(*last_import_after_unix_ms_, created) : created;
        last_import_before_unix_ms_ =
            last_import_before_unix_ms_ ? std::max(*last_import_before_unix_ms_, created) : created;
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
    if (!import_page_open_ || (import_workspace_->scan && import_workspace_->scan->active()) ||
        import_work_active_ || import_preflight_active_ || selected.isEmpty())
        return;
    if (!(import_workspace_->scan && import_workspace_->scan->catalogRevision()))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Scan the source folder again."));
        return;
    }
    if (import_workspace_->draft.mode != QLatin1String("add") &&
        import_workspace_->draft.destination.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Choose an import destination."));
        return;
    }
    if ((import_ingest_transport_ == QLatin1String("filesystem-card") ||
         import_ingest_transport_ == QLatin1String("ptp-stub") ||
         import_ingest_transport_ == QLatin1String("ptp-usb") ||
         import_ingest_transport_ == QLatin1String("mtp")) &&
        import_workspace_->draft.mode == QLatin1String("move"))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter",
            "Ingest transports are Copy-only; Move and camera delete stay rejected."));
        return;
    }
    if (import_ingest_transport_ == QLatin1String("ptp-usb") ||
        import_ingest_transport_ == QLatin1String("mtp"))
    {
        refreshImportNativeSupport();
        const auto reason = import_native_support_.value(QStringLiteral("reason")).toString();
        setError(QCoreApplication::translate(
                     "StudioPresenter",
                     "Native PTP/MTP adapter is not packaged (%1). Use filesystem-card or the "
                     "ptp-stub fixture.")
                     .arg(reason.isEmpty() ? QStringLiteral("native_ingest_adapter_not_packaged") :
                                             reason));
        return;
    }

    ImportRequest request = plannedImportRequest();
    const auto generation = import_workspace_->scan ? import_workspace_->scan->generation() : 0U;
    import_preflight_active_ = true;
    setError({});
    emit importPageChanged();
    executor_.post(
        [this, generation, request = std::move(request)]() mutable
        {
            auto ready = service_ == nullptr ?
                             Result<void>{make_error(ErrorCode::kIo, "Catalog session is closed")} :
                             service_->preflight_import(request);
            QMetaObject::invokeMethod(
                this,
                [this, generation, ready = std::move(ready), request = std::move(request)]() mutable
                {
                    if (!import_workspace_->scan->matches(generation) || !import_page_open_)
                        return;
                    import_preflight_active_ = false;
                    emit importPageChanged();
                    if (!ready)
                    {
                        setError(qstring_from_utf8(ready.error().message));
                        if (import_workspace_->thumbnails)
                            import_workspace_->thumbnails->kick();
                        return;
                    }
                    beginPlannedImport(std::move(request));
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::beginPlannedImport(ImportRequest request)
{
    if (import_workspace_->thumbnails)
        import_workspace_->thumbnails->cancel("planned_import_started");
    const bool ingest_copy =
        uses_ingest_copy_path(import_ingest_transport_, import_workspace_->draft.mode);
    pending_import_destination_ = request.mode == ImportTransferMode::kAdd ?
                                      QString{} :
                                      qstring_from_utf8(request.destination_directory);
    import_destination_remembered_ = false;
    import_preference_error_.clear();
    static_cast<void>(import_operation_.cancel("planned_import_started"));
    if (import_workspace_->scan)
        import_workspace_->scan->bumpGeneration("planned_import_started");
    if (import_workspace_->thumbnails)
        import_workspace_->thumbnails->clearPending();
    import_operation_ = CancellationSource{};
    request.cancellation = import_operation_.token();
    pending_import_paths_ = request.inputs;
    import_query_snapshot_ = current_query();
    pending_import_preview_policy_ = request.preview;
    import_defer_previews_ = true;
    import_skip_existing_ = true;
    pending_import_content_hashes_.clear();
    for (const auto &[path, digest] : request.expected_content_hashes)
        pending_import_content_hashes_.emplace(path, digest);
    import_results_.clear();
    import_ingest_report_ = {};
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
    if (ingest_copy)
    {
        IngestRequest ingest;
        ingest.source_root = utf8_from_qstring(import_workspace_->draft.source_root);
        ingest.transport = utf8_from_qstring(import_ingest_transport_);
        ingest.mode = ImportTransferMode::kCopy;
        ingest.organization = request.organization;
        ingest.preview = request.preview;
        ingest.destination_directory = request.destination_directory;
        ingest.filename_template = request.filename_template;
        ingest.second_copy_directory = request.second_copy_directory;
        ingest.recursive = import_source_recursion(import_workspace_->draft.source_root,
                                                   QDir::homePath(), import_recursive_);
        ingest.include_xmp_sidecars = true;
        ingest.defer_previews = true;
        ingest.skip_existing = true;
        ingest.expected_catalog_revision = request.expected_catalog_revision;
        ingest.expected_content_hashes = request.expected_content_hashes;
        ingest.selected_paths = request.inputs;
        ingest.cancellation = request.cancellation;
        if (!import_resume_batch_id_.isEmpty())
            ingest.resume_batch_id = utf8_from_qstring(import_resume_batch_id_);
        executor_.post(
            [this, ingest = std::move(ingest)]() mutable
            {
                auto detailed = service_ == nullptr ?
                                    Result<IngestBatchResult>{
                                        make_error(ErrorCode::kIo, "Catalog session is closed")} :
                                    service_->execute_ingest_detailed(
                                        ingest,
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
                                                        publishImportItem(
                                                            copy, static_cast<int>(completed) - 1);
                                                },
                                                Qt::QueuedConnection);
                                        });
                QMetaObject::invokeMethod(
                    this,
                    [this, detailed = std::move(detailed)]() mutable
                    {
                        if (!detailed)
                        {
                            setImportWork(0, 0, false);
                            import_gallery_placeholders_ = false;
                            import_defer_previews_ = false;
                            last_import_selected_ = false;
                            last_import_count_ = 0U;
                            last_import_after_unix_ms_.reset();
                            last_import_before_unix_ms_.reset();
                            query_ = import_query_snapshot_;
                            import_ingest_report_ = {};
                            setError(qstring_from_utf8(detailed.error().message));
                            setStatus(
                                QCoreApplication::translate("StudioPresenter", "Ingest failed."));
                            emit importPageChanged();
                            reloadVisibleAssets();
                            return;
                        }
                        auto value = std::move(detailed).value();
                        import_ingest_report_ = ingest_report_to_map(value);
                        if (value.resume_batch_id && !value.resume_checkpoint_cleared)
                            import_resume_batch_id_ = qstring_from_utf8(*value.resume_batch_id);
                        else if (value.resume_checkpoint_cleared)
                            import_resume_batch_id_.clear();
                        emit importPageChanged();
                        import_results_ = std::move(value.import.items);
                        import_next_index_ = import_results_.size();
                        finishImportBatch();
                    },
                    Qt::QueuedConnection);
            });
        return;
    }
    executor_.post(
        [this, request = std::move(request)]() mutable
        {
            auto batch =
                service_ == nullptr ?
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
                                        publishImportItem(copy, static_cast<int>(completed) - 1);
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

void StudioPresenter::requestFilesystemListing(FilesystemBrowserModel *browser, const QString &path,
                                               const quint64 generation)
{
    if (browser == nullptr)
        return;
    filesystem_executor_.post(
        [this, browser, path, generation]()
        {
            auto listed = list_filesystem_folders(path);
            QMetaObject::invokeMethod(
                this,
                [this, browser, path, generation, listed = std::move(listed)]() mutable
                {
                    if (browser != &import_source_folders_ &&
                        browser != &import_destination_folders_)
                        return;
                    browser->applyChildren(path, generation, std::move(listed));
                },
                Qt::QueuedConnection);
        });
}

} // namespace ravo
