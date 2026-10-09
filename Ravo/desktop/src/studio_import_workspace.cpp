#include "ravo/desktop/studio_import_workspace.h"
#include "studio_import_worker.h"
#include "studio_import_scan_controller.h"
#include "studio_import_thumbnail_controller.h"
#include "studio_import_destination_preview_controller.h"
#include "ravo/desktop/studio_import_preferences.h"
#include "ravo/services/ingest_transport.h"
#include "studio_qt.h"
#include <algorithm>
#include <utility>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>

namespace ravo
{
[[nodiscard]] QVariantMap
StudioImportWorkspace::native_support_to_map(const NativeIngestPlatformSupport &support)
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

bool StudioImportWorkspace::importPageOpen() const noexcept
{
    return import_page_open_;
}

bool StudioImportWorkspace::importScanActive() const noexcept
{
    return scan && scan->active();
}

int StudioImportWorkspace::importDuplicateCount() const noexcept
{
    return scan ? scan->duplicateCount() : 0;
}

int StudioImportWorkspace::importScanCompleted() const noexcept
{
    return scan ? scan->completed() : 0;
}

int StudioImportWorkspace::importScanTotal() const noexcept
{
    return scan ? scan->total() : 0;
}

bool StudioImportWorkspace::importPreviewWorkActive() const noexcept
{
    return import_preview_work_active_;
}

int StudioImportWorkspace::importPreviewWorkCompleted() const noexcept
{
    return import_preview_work_completed_;
}

int StudioImportWorkspace::importPreviewWorkTotal() const noexcept
{
    return import_preview_work_total_;
}

QString StudioImportWorkspace::importDestinationError() const
{
    return draft.destination_error;
}

QString StudioImportWorkspace::importSourceRoot() const
{
    return draft.source_root;
}

ImportDraft StudioImportWorkspace::importDraft() const
{
    return draft;
}

QString StudioImportWorkspace::importIngestTransport() const
{
    return import_ingest_transport_;
}

QString StudioImportWorkspace::importMoveUnavailableReason() const
{
    if (import_ingest_transport_ == QLatin1String("folder"))
        return {};
    return QCoreApplication::translate(
        "StudioPresenter",
        "Camera and card ingest preserves source originals. Use Copy instead of Move.");
}

QString StudioImportWorkspace::importIngestSourceUri() const
{
    if (draft.source_root.isEmpty() || import_ingest_transport_ == QLatin1String("folder"))
        return {};
    const auto root = utf8_from_qstring(draft.source_root);
    if (import_ingest_transport_ == QLatin1String("ptp-stub"))
        return qstring_from_utf8(format_ptp_stub_ingest_uri(root));
    if (import_ingest_transport_ == QLatin1String("ptp-usb"))
        return QStringLiteral("ravo-ingest:ptp-usb:… (adapter not packaged)");
    if (import_ingest_transport_ == QLatin1String("mtp"))
        return QStringLiteral("ravo-ingest:mtp:… (adapter not packaged)");
    return qstring_from_utf8(format_filesystem_card_ingest_uri(root));
}

QVariantMap StudioImportWorkspace::importNativeSupport() const
{
    return import_native_support_;
}

QVariantMap StudioImportWorkspace::importIngestReport() const
{
    return import_ingest_report_;
}

QString StudioImportWorkspace::importResumeBatchId() const
{
    return import_resume_batch_id_;
}

QString StudioImportWorkspace::importDestination() const
{
    return draft.destination;
}

bool StudioImportWorkspace::importReady() const
{
    const bool native = import_ingest_transport_ == QLatin1String("ptp-usb") ||
                        import_ingest_transport_ == QLatin1String("mtp");
    return import_page_open_ && !native && !import_preflight_active_ && !import_work_active_ &&
           !importInteractionBlocked() && !importDestinationPreviewActive() && scan &&
           scan->catalogRevision().has_value() && candidates.selectedCount() > 0 &&
           (draft.mode != QLatin1String("move") || importMoveUnavailableReason().isEmpty()) &&
           (draft.mode == QLatin1String("add") ||
            (draft.destination_valid && importDestinationPreviewError().isEmpty() &&
             (!draft.second_copy_enabled || !draft.second_copy_destination.isEmpty())));
}

bool StudioImportWorkspace::importInteractionBlocked() const
{
    if (!import_page_open_ || draft.mode == QLatin1String("add") || draft.source_root.isEmpty() ||
        draft.destination.isEmpty() || !draft.destination_error.isEmpty())
        return false;
    return !draft.destination_valid || (scan && scan->active() && !scan->catalogRevision());
}

QUrl StudioImportWorkspace::importDestinationFolderUrl() const
{
    return draft.destination.isEmpty() ? QUrl::fromLocalFile(pictures_directory()) :
                                         QUrl::fromLocalFile(draft.destination);
}

QUrl StudioImportWorkspace::importSourceFolderUrl() const
{
    return draft.source_root.isEmpty() ? QUrl::fromLocalFile(pictures_directory()) :
                                         QUrl::fromLocalFile(draft.source_root);
}

QUrl StudioImportWorkspace::importSecondCopyFolderUrl() const
{
    return draft.second_copy_destination.isEmpty() ?
               importDestinationFolderUrl() :
               QUrl::fromLocalFile(draft.second_copy_destination);
}

void StudioImportWorkspace::validateImportDestination()
{
    const auto path = draft.destination;
    draft.destination_valid = false;
    draft.destination_error =
        path.isEmpty() ?
            QCoreApplication::translate("StudioPresenter", "Choose an import destination.") :
            QString{};
    emit importPageChanged();
    if (path.isEmpty())
        return;
    const bool queued = filesystem_executor_.post(
        [this, path]()
        {
            const QFileInfo directory(path);
            const bool available = directory.isDir() && directory.isWritable();
            QMetaObject::invokeMethod(
                this,
                [this, path, available]()
                {
                    if (path != draft.destination || !import_page_open_)
                        return;
                    draft.destination_valid = available;
                    if (available)
                    {
                        const auto remembered = StudioImportPreferences{}.rememberDestination(path);
                        if (!remembered)
                            setError(qstring_from_utf8(remembered.error().message));
                    }
                    draft.destination_error =
                        available ?
                            QString{} :
                            QCoreApplication::translate(
                                "StudioPresenter",
                                "Destination unavailable. Reconnect the drive or choose another folder.");
                    emit importPageChanged();
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        draft.destination_error = QStringLiteral("Import filesystem owner is closed");
        setError(draft.destination_error);
        emit importPageChanged();
    }
}

QString StudioImportWorkspace::importSecondCopyDestination() const
{
    return draft.second_copy_destination;
}

bool StudioImportWorkspace::importSecondCopyEnabled() const noexcept
{
    return draft.second_copy_enabled;
}

QString StudioImportWorkspace::importFilenameTemplate() const
{
    if (!draft.rename_enabled)
        return {};
    const std::array<QString, 4> tokens{QString{}, QStringLiteral("{stem}"),
                                        QStringLiteral("{date}"), QStringLiteral("{sequence}")};
    const std::array<QString, 3> separators{QStringLiteral("_"), QStringLiteral("-"), QString{}};
    QStringList parts;
    for (const auto component : draft.rename_parts)
        if (component != 0)
            parts.push_back(tokens[static_cast<std::size_t>(component)]);
    return parts.join(separators[static_cast<std::size_t>(draft.rename_separator)]) +
           QStringLiteral("{ext}");
}

bool StudioImportWorkspace::importRenameEnabled() const noexcept
{
    return draft.rename_enabled;
}

QVariantList StudioImportWorkspace::importRenameParts() const
{
    return {draft.rename_parts[0], draft.rename_parts[1], draft.rename_parts[2]};
}

int StudioImportWorkspace::importRenameSeparator() const noexcept
{
    return draft.rename_separator;
}

QString StudioImportWorkspace::importRenameExample() const
{
    if (!draft.rename_enabled)
        return QStringLiteral("IMG_1234.jpg");
    const auto expanded = expand_import_filename_template(
        utf8_from_qstring(importFilenameTemplate()), "IMG_1234", "20260123", 1, ".jpg");
    return expanded ? qstring_from_utf8(expanded.value()) :
                      qstring_from_utf8(expanded.error().message);
}

QString StudioImportWorkspace::importMode() const
{
    return draft.mode;
}

QString StudioImportWorkspace::importOrganization() const
{
    return draft.organization;
}

QString StudioImportWorkspace::importPreviewPolicy() const
{
    return draft.preview_policy;
}

bool StudioImportWorkspace::importRecursive() const noexcept
{
    return import_recursive_;
}

ImportCandidateListModel *StudioImportWorkspace::importCandidates() noexcept
{
    return &candidates;
}

FilesystemBrowserModel *StudioImportWorkspace::importSourceFolders() noexcept
{
    return &source_folders;
}

FilesystemBrowserModel *StudioImportWorkspace::importDestinationFolders() noexcept
{
    return &destination_folders;
}

void StudioImportWorkspace::openImportPage()
{
    if (context_.catalog_path.isEmpty() || import_work_active_)
        return;
    cancelImportPreviews();
    import_page_open_ = true;
    setError({});
    draft.mode = QStringLiteral("copy");
    const auto source = StudioImportPreferences{}.loadLastSource();
    if (source)
        draft.source_root = source.value();
    else
    {
        draft.source_root.clear();
        setError(qstring_from_utf8(source.error().message));
    }
    const auto destination = StudioImportPreferences{}.loadLastDestination();
    if (destination)
        draft.destination = destination.value();
    else
    {
        draft.destination.clear();
        setError(qstring_from_utf8(destination.error().message));
    }
    const auto organization = StudioImportPreferences{}.loadLastOrganization();
    draft.organization = organization ? organization.value() : QStringLiteral("single");
    if (!organization)
        setError(qstring_from_utf8(organization.error().message));
    validateImportDestination();
    refreshImportNativeSupport();
    source_folders.resetWithRoots({{QDir::homePath(), QDir::homePath(), true}});
    pending_source_restore_ = draft.source_root;
    destination_folders.loadUserDirectory();
    refreshImportSources();
    if (!draft.destination.isEmpty())
        destination_folders.revealFolder(draft.destination);
    emit importPageChanged();
    if (!draft.source_root.isEmpty())
        rescanImportSource();
}

void StudioImportWorkspace::cancelImportSource()
{
    if (!import_page_open_ || import_work_active_ || import_preflight_active_)
        return;
    // Clear scheduling inputs before controllers emit notifications. Late validation
    // and worker callbacks must not restart planning for the abandoned folder.
    draft.source_root.clear();
    pending_source_restore_.clear();
    source_folders.revealFolder({});
    if (scan)
        scan->abandon("import_source_cancelled");
    if (thumbnails)
    {
        thumbnails->cancel("import_source_cancelled");
        thumbnails->resetSourceSession();
    }
    if (destination_preview)
        destination_preview->clearPublished();
    candidates.setCandidates({});
    import_context_row_ = -1;
    import_context_path_.clear();
    emit importContextChanged();
    setError({});
    emit importPageChanged();
}

void StudioImportWorkspace::closeImportPage()
{
    if (import_work_active_ && !import_preflight_active_)
        return;
    static_cast<void>(import_operation_.cancel("import_page_closed"));
    if (thumbnails)
        thumbnails->cancel("import_page_closed");
    if (scan)
        scan->abandon("import_page_closed");
    if (import_preflight_active_)
        setImportWork(0, 0, false);
    import_preflight_active_ = false;
    import_page_open_ = false;
    pending_source_restore_.clear();
    source_folders.revealFolder({});
    ++import_roots_generation_;
    import_context_row_ = -1;
    import_context_path_.clear();
    if (thumbnails)
        thumbnails->resetSourceSession();
    candidates.setCandidates({});
    emit importPageChanged();
}

void StudioImportWorkspace::refreshImportSources()
{
    if (!import_page_open_ || import_work_active_ || import_preflight_active_)
        return;
    const auto generation = ++import_roots_generation_;
    const bool queued = filesystem_executor_.post(
        [this, generation]
        {
            auto roots = list_mounted_filesystem_roots();
            QMetaObject::invokeMethod(
                this,
                [this, generation, roots = std::move(roots)]() mutable
                {
                    if (!import_page_open_ || generation != import_roots_generation_)
                        return;
                    source_folders.updateMountedRoots(roots);
                    destination_folders.updateMountedRoots(std::move(roots));
                    // Wait for real mounted roots before revealing an external saved
                    // source; later refreshes must not undo the user's tree collapse.
                    if (!pending_source_restore_.isEmpty())
                        source_folders.revealFolder(pending_source_restore_);
                },
                Qt::QueuedConnection);
        });
    if (!queued)
        setError(QStringLiteral("Unable to queue import storage discovery"));
}

QString StudioImportWorkspace::importContextPath() const
{
    if (!import_page_open_ || import_work_active_ || import_preflight_active_ ||
        import_context_generation_ != candidates.generation() ||
        candidates.sourcePath(import_context_row_) != import_context_path_)
        return {};
    return import_context_path_;
}

bool StudioImportWorkspace::setImportContextRow(const int row)
{
    import_context_row_ = row;
    import_context_generation_ = candidates.generation();
    import_context_path_ = candidates.sourcePath(row);
    emit importContextChanged();
    return !importContextPath().isEmpty();
}

void StudioImportWorkspace::setImportSourceRoot(const QString &path)
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
    draft.source_root = next;
    pending_source_restore_.clear();
    refreshImportSources();
    source_folders.revealFolder(next);
    emit importPageChanged();
    rescanImportSource();
}

void StudioImportWorkspace::setImportDestination(const QString &path)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = path.isEmpty() ? QString{} : QDir::cleanPath(path);
    if (next == draft.destination)
    {
        if (destination_preview)
            destination_preview->invalidateCacheKey();
        if (!draft.destination_error.isEmpty())
            destination_folders.loadUserDirectory();
        destination_folders.revealFolder(next);
        validateImportDestination();
        return;
    }
    draft.destination = next;
    destination_folders.revealFolder(next);
    validateImportDestination();
    emit importPageChanged();
}

void StudioImportWorkspace::setImportSecondCopyEnabled(const bool enabled)
{
    if (import_work_active_ || import_preflight_active_ || enabled == draft.second_copy_enabled)
        return;
    draft.second_copy_enabled = enabled;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportSecondCopyDestination(const QString &path)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = path.trimmed();
    if (next == draft.second_copy_destination)
        return;
    draft.second_copy_destination = next;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportRenameEnabled(const bool enabled)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (enabled == draft.rename_enabled)
        return;
    draft.rename_enabled = enabled;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportRenamePart(const int position, const int component)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (position < 0 || position >= 3 || component < 0 || component > 3 ||
        (position == 0 && component == 0))
    {
        setError(QStringLiteral("Invalid import rename component"));
        return;
    }
    auto &part = draft.rename_parts[static_cast<std::size_t>(position)];
    if (part == component)
        return;
    part = component;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportRenameSeparator(const int separator)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (separator < 0 || separator > 2)
    {
        setError(QStringLiteral("Invalid import rename separator"));
        return;
    }
    if (separator == draft.rename_separator)
        return;
    draft.rename_separator = separator;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportMode(const QString &mode)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (mode != QLatin1String("add") && mode != QLatin1String("copy") &&
        mode != QLatin1String("move"))
        return;
    if (mode == QLatin1String("move") && !importMoveUnavailableReason().isEmpty())
    {
        setError(importMoveUnavailableReason());
        return;
    }
    if (draft.mode == mode)
        return;
    draft.mode = mode;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportOrganization(const QString &organization)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (organization != QLatin1String("single") && organization != QLatin1String("hierarchy") &&
        organization != QLatin1String("date") && organization != QLatin1String("month"))
        return;
    if (draft.organization == organization)
        return;
    const auto remembered = StudioImportPreferences{}.rememberOrganization(organization);
    if (!remembered)
        setError(qstring_from_utf8(remembered.error().message));
    draft.organization = organization;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportPreviewPolicy(const QString &policy)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (policy != QLatin1String("minimal") && policy != QLatin1String("standard") &&
        policy != QLatin1String("one-to-one"))
        return;
    if (draft.preview_policy == policy)
        return;
    draft.preview_policy = policy;
    emit importPageChanged();
}

void StudioImportWorkspace::setImportRecursive(const bool recursive)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    if (import_recursive_ == recursive)
        return;
    import_recursive_ = recursive;
    emit importPageChanged();
    if (!draft.source_root.isEmpty())
        rescanImportSource();
}

void StudioImportWorkspace::setImportIngestTransport(const QString &transport)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = transport.trimmed();
    if (next != QLatin1String("folder") && next != QLatin1String("filesystem-card") &&
        next != QLatin1String("ptp-stub") && next != QLatin1String("ptp-usb") &&
        next != QLatin1String("mtp"))
        return;
    if (import_ingest_transport_ == next)
        return;
    import_ingest_transport_ = next;
    if ((draft.mode == QLatin1String("move") && !importMoveUnavailableReason().isEmpty()) ||
        ((next == QLatin1String("ptp-stub") || next == QLatin1String("ptp-usb") ||
          next == QLatin1String("mtp")) &&
         draft.mode != QLatin1String("copy")))
        draft.mode = QStringLiteral("copy");
    import_resume_batch_id_.clear();
    refreshImportNativeSupport();
    emit importPageChanged();
}

void StudioImportWorkspace::setImportResumeBatchId(const QString &batch_id)
{
    if (import_work_active_ || import_preflight_active_)
        return;
    const QString next = batch_id.trimmed();
    if (import_resume_batch_id_ == next)
        return;
    import_resume_batch_id_ = next;
    emit importPageChanged();
}

void StudioImportWorkspace::refreshImportNativeSupport()
{
    import_native_support_ = native_support_to_map(probe_native_ingest_support());
    emit importPageChanged();
}

void StudioImportWorkspace::rescanImportSource()
{
    if (context_.catalog_path.isEmpty() || !scan || draft.source_root.isEmpty() ||
        import_work_active_)
        return;
    import_context_row_ = -1;
    import_context_path_.clear();
    emit importContextChanged();
    static_cast<void>(import_operation_.cancel("import_source_changed"));
    import_operation_ = CancellationSource{};
    scan->startRescan();
}

void StudioImportWorkspace::ensureImportThumbnail(const int row)
{
    if (thumbnails)
        thumbnails->ensure(row);
}

void StudioImportWorkspace::setImportThumbnailViewportDemand(const QVariantList &rows,
                                                             const int prefetch,
                                                             const int current_row)
{
    if (!thumbnails)
        return;
    std::vector<int> visible;
    visible.reserve(static_cast<std::size_t>(rows.size()));
    for (const auto &value : rows)
        visible.push_back(value.toInt());
    thumbnails->setViewportDemand(visible, prefetch, current_row);
}

void StudioImportWorkspace::requestFilesystemListing(FilesystemBrowserModel *browser,
                                                     const QString &path, const quint64 generation)
{
    if (browser == nullptr)
        return;
    const bool queued = filesystem_executor_.post(
        [this, browser, path, generation]()
        {
            auto listed = list_filesystem_folders(path);
            QMetaObject::invokeMethod(
                this,
                [this, browser, path, generation, listed = std::move(listed)]() mutable
                {
                    if (browser != &source_folders && browser != &destination_folders)
                        return;
                    browser->applyChildren(path, generation, std::move(listed));
                },
                Qt::QueuedConnection);
        });
    if (!queued)
    {
        const auto failure = make_error(ErrorCode::kCancelled, "Import filesystem owner is closed");
        browser->applyChildren(path, generation, failure);
        setError(qstring_from_utf8(failure.message));
    }
}

bool StudioImportWorkspace::importWorkActive() const noexcept
{
    return import_work_active_;
}

int StudioImportWorkspace::importWorkCompleted() const noexcept
{
    return import_work_completed_;
}

int StudioImportWorkspace::importWorkTotal() const noexcept
{
    return import_work_total_;
}

QString StudioImportWorkspace::contextDebugInfo() const
{
    const QString path = importContextPath();
    if (path.isEmpty())
        return {};
    const auto index = candidates.index(import_context_row_, 0);
    return QStringLiteral(
               "ravo.debug.import-photo 1\npath=%1\nuri=%2\ndisplay_name=%3\nsize_bytes=%4\nduplicate=%5")
        .arg(path, QUrl::fromLocalFile(path).toString(),
             candidates.data(index, ImportCandidateListModel::DisplayNameRole).toString(),
             candidates.data(index, ImportCandidateListModel::SizeBytesRole).toString(),
             candidates.data(index, ImportCandidateListModel::DuplicateRole).toBool() ?
                 QStringLiteral("true") :
                 QStringLiteral("false"));
}
} // namespace ravo
