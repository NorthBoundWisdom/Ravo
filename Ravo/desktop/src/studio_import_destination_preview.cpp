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
StudioImportWorkspace::StudioImportWorkspace(Context context, Host host, QObject *parent)
    : QObject(parent)
    , context_(context)
    , host_(std::move(host))
    , worker(std::make_unique<StudioImportWorker>())
    , candidates(this)
    , source_folders(this)
    , destination_folders(this)
{
    const auto bind_browser = [this](FilesystemBrowserModel *browser)
    {
        QObject::connect(browser, &FilesystemBrowserModel::directoryListingRequested, this,
                         [this, browser](const QString &path, quint64 generation)
                         { requestFilesystemListing(browser, path, generation); });
    };
    bind_browser(&source_folders);
    bind_browser(&destination_folders);
    connect(this, &StudioImportWorkspace::importPageChanged, this,
            &StudioImportWorkspace::refreshImportDestinationPreview);
    connect(&candidates, &ImportCandidateListModel::selectionChanged, this,
            &StudioImportWorkspace::importPageChanged);
    scan = std::make_unique<StudioImportScanController>(
        StudioImportScanController::Host{
            this,
            &worker->executor(),
            [this] { return worker->service(); },
            [this] { return import_page_open_; },
            [this] { return import_work_active_; },
            [this] { return &candidates; },
            [this] { return draft.source_root; },
            [this](const QString &root)
            { return import_source_recursion(root, QDir::homePath(), import_recursive_); },
            [this] { emit importPageChanged(); },
            [this](QString message) { setError(std::move(message)); },
            [this]
            {
                if (thumbnails)
                {
                    thumbnails->cancel("import_source_changed");
                    thumbnails->resetOperation();
                    thumbnails->resetSourceSession();
                }
            },
            [this] { import_preflight_active_ = false; },
            [this]
            {
                if (thumbnails)
                    thumbnails->startBackgroundPass();
            },
        },
        this);
    thumbnails = std::make_unique<StudioImportThumbnailController>(
        StudioImportThumbnailController::Host{
            &candidates,
            this,
            [this] { return import_page_open_; },
            [this] { return import_work_active_; },
            [this] { return import_preflight_active_; },
            [this] { return scan ? scan->generation() : 0U; },
            [this](QString message) { setError(std::move(message)); },
        },
        this);
    destination_preview = std::make_unique<StudioImportDestinationPreviewController>(
        StudioImportDestinationPreviewController::Host{
            this,
            &worker->executor(),
            [this]() -> CatalogService * { return worker->service(); },
            [this] { return import_page_open_; },
            [this]
            {
                return import_page_open_ && scan && !scan->active() && !import_work_active_ &&
                       !import_preflight_active_ && scan->catalogRevision() &&
                       draft.mode != QLatin1String("add") && !draft.destination.isEmpty() &&
                       draft.destination_error.isEmpty() && candidates.selectedCount() > 0;
            },
            [this]
            {
                // Lightweight invalidation key only — owning path snapshots are built
                // after the destination-preview debounce timer fires (build_request).
                return QJsonDocument(
                           QJsonObject{{QStringLiteral("catalog"), context_.catalog_path},
                                       {QStringLiteral("revision"),
                                        QString::number(*scan->catalogRevision())},
                                       {QStringLiteral("source"), draft.source_root},
                                       {QStringLiteral("destination"), draft.destination},
                                       {QStringLiteral("second"), draft.second_copy_destination},
                                       {QStringLiteral("mode"), draft.mode},
                                       {QStringLiteral("organization"), draft.organization},
                                       {QStringLiteral("name"), draft.filename_pattern},
                                       {QStringLiteral("generation"),
                                        QString::number(candidates.generation())},
                                       {QStringLiteral("selectionRevision"),
                                        QString::number(candidates.selectionRevision())}})
                    .toJson(QJsonDocument::Compact);
            },
            [this] { return plannedImportRequest(); },
        },
        this);
    connect(destination_preview.get(), &StudioImportDestinationPreviewController::changed, this,
            [this]
            {
                destination_folders.setPreviewFolders(destination_preview->treeFolders(),
                                                      importDestination());
                emit importDestinationPreviewChanged();
            });
}

StudioImportWorkspace::~StudioImportWorkspace()
{
    shutdown();
    shutdownWorker();
}
void StudioImportWorkspace::shutdown()
{
    if (stopped_)
        return;
    stopped_ = true;
    static_cast<void>(import_operation_.cancel("window_closed"));
    static_cast<void>(import_preview_operation_.cancel("window_closed"));
    filesystem_executor_.request_stop();
    filesystem_executor_.wait();
    if (destination_preview)
        destination_preview->shutdown();
    if (thumbnails)
        thumbnails->shutdown();
    if (scan)
        scan->abandon("workspace_shutdown");
}
void StudioImportWorkspace::shutdownWorker()
{
    worker->shutdown();
}
void StudioImportWorkspace::cancelImport(std::string reason)
{
    static_cast<void>(import_operation_.cancel(std::move(reason)));
}
void StudioImportWorkspace::catalogReplaced()
{
    ++import_generation_;
}
bool StudioImportWorkspace::finishPublication(std::uint64_t generation, std::size_t completed,
                                              std::size_t total)
{
    if (generation != import_generation_)
        return false;
    setImportWork(static_cast<int>(completed), static_cast<int>(total), false);
    return true;
}
void StudioImportWorkspace::startDeferredPreviews()
{
    if (pending_import_preview_ids_.empty())
        return;
    import_preview_work_active_ = true;
    import_preview_work_completed_ = 0;
    import_preview_work_total_ = static_cast<int>(pending_import_preview_ids_.size());
    emit libraryWorkChanged();
    startNextImportPreview();
}
bool StudioImportWorkspace::galleryPlaceholders() const noexcept
{
    return import_gallery_placeholders_;
}
const bool &StudioImportWorkspace::workActiveState() const noexcept
{
    return import_work_active_;
}
StudioImportWorker &StudioImportWorkspace::importWorker() noexcept
{
    return *worker;
}
void StudioImportWorkspace::setError(QString error)
{
    emit errorOccurred(std::move(error));
}
void StudioImportWorkspace::setStatus(QString status)
{
    emit statusOccurred(std::move(status));
}

QVariantList StudioImportWorkspace::importDestinationPreview() const
{
    return destination_preview ? destination_preview->folders() : QVariantList{};
}

QString StudioImportWorkspace::importDestinationPreviewError() const
{
    return destination_preview ? destination_preview->error() : QString{};
}

bool StudioImportWorkspace::importDestinationPreviewActive() const
{
    return destination_preview && destination_preview->active();
}

ImportRequest StudioImportWorkspace::plannedImportRequest() const
{
    ImportRequest request;
    for (const auto &path : candidates.selectedPaths())
        request.inputs.push_back(utf8_from_qstring(path));
    request.source_root = utf8_from_qstring(draft.source_root);
    request.mode = draft.mode == QLatin1String("copy") ? ImportTransferMode::kCopy :
                   draft.mode == QLatin1String("move") ? ImportTransferMode::kMove :
                                                         ImportTransferMode::kAdd;
    request.organization =
        draft.organization == QLatin1String("hierarchy") ? ImportOrganization::kPreserveHierarchy :
        draft.organization == QLatin1String("date")      ? ImportOrganization::kCaptureDate :
        draft.organization == QLatin1String("month")     ? ImportOrganization::kCaptureMonth :
                                                           ImportOrganization::kSingleFolder;
    request.preview =
        draft.preview_policy == QLatin1String("minimal")    ? ImportPreviewPolicy::kMinimal :
        draft.preview_policy == QLatin1String("one-to-one") ? ImportPreviewPolicy::kOneToOne :
                                                              ImportPreviewPolicy::kStandard;
    if (request.mode != ImportTransferMode::kAdd)
    {
        request.destination_directory = utf8_from_qstring(draft.destination);
        request.filename_template = utf8_from_qstring(draft.filename_pattern);
        request.second_copy_directory = utf8_from_qstring(draft.second_copy_destination);
    }
    request.recursive = false;
    request.defer_previews = true;
    request.skip_existing = true;
    // Selected paths/hashes and destination conflicts are revalidated by the
    // import service. A photo edit while this worker runs is not an import conflict.
    request.expected_content_hashes = candidates.selectedContentHashes();
    request.cancellation = import_operation_.token();
    return request;
}

void StudioImportWorkspace::refreshImportDestinationPreview()
{
    if (destination_preview)
        destination_preview->refresh();
}
} // namespace ravo
