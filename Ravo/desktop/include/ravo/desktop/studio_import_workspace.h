#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/desktop/studio_import_draft.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/foundation/error.h"

namespace ravo
{
struct NativeIngestPlatformSupport;
struct IngestBatchResult;
class StudioImportWorker;
class PreviewCache;
class StudioImportScanController;
class StudioImportThumbnailController;
class StudioImportDestinationPreviewController;
namespace testing
{
class StudioImportTestControl;
}

// GUI-thread Import owner. Context is borrowed from the composition root for this
// object's lifetime; callbacks publish Library/selection intents without owning them.
// Worker sessions and Engines retain their original serial owner threads.
class StudioImportWorkspace final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool importWorkActive READ importWorkActive NOTIFY libraryWorkChanged)
    Q_PROPERTY(int importWorkCompleted READ importWorkCompleted NOTIFY libraryWorkChanged)
    Q_PROPERTY(int importWorkTotal READ importWorkTotal NOTIFY libraryWorkChanged)
    Q_PROPERTY(QString importWorkTitle READ importWorkTitle NOTIFY importProgressChanged)
    Q_PROPERTY(QString importWorkCountText READ importWorkCountText NOTIFY importProgressChanged)
    Q_PROPERTY(QString importWorkDetailText READ importWorkDetailText NOTIFY importProgressChanged)
    Q_PROPERTY(double importWorkFraction READ importWorkFraction NOTIFY importProgressChanged)
    Q_PROPERTY(bool importPageOpen READ importPageOpen NOTIFY importPageChanged)
    Q_PROPERTY(bool importScanActive READ importScanActive NOTIFY importPageChanged)
    Q_PROPERTY(int importDuplicateCount READ importDuplicateCount NOTIFY importPageChanged)
    Q_PROPERTY(int importScanCompleted READ importScanCompleted NOTIFY importPageChanged)
    Q_PROPERTY(int importScanTotal READ importScanTotal NOTIFY importPageChanged)
    Q_PROPERTY(bool importPreflightActive READ importPreflightActive NOTIFY importPageChanged)
    Q_PROPERTY(bool importReady READ importReady NOTIFY importPageChanged)
    Q_PROPERTY(bool importInteractionBlocked READ importInteractionBlocked NOTIFY importPageChanged)
    Q_PROPERTY(QString importDestinationError READ importDestinationError NOTIFY importPageChanged)
    Q_PROPERTY(QVariantList importDestinationPreview READ importDestinationPreview NOTIFY
                   importDestinationPreviewChanged)
    Q_PROPERTY(QString importDestinationPreviewError READ importDestinationPreviewError NOTIFY
                   importDestinationPreviewChanged)
    Q_PROPERTY(bool importDestinationPreviewActive READ importDestinationPreviewActive NOTIFY
                   importDestinationPreviewChanged)
    Q_PROPERTY(
        QUrl importDestinationFolderUrl READ importDestinationFolderUrl NOTIFY importPageChanged)
    Q_PROPERTY(QUrl importSourceFolderUrl READ importSourceFolderUrl NOTIFY importPageChanged)
    Q_PROPERTY(
        QUrl importSecondCopyFolderUrl READ importSecondCopyFolderUrl NOTIFY importPageChanged)
    Q_PROPERTY(bool importPreviewWorkActive READ importPreviewWorkActive NOTIFY libraryWorkChanged)
    Q_PROPERTY(
        int importPreviewWorkCompleted READ importPreviewWorkCompleted NOTIFY libraryWorkChanged)
    Q_PROPERTY(int importPreviewWorkTotal READ importPreviewWorkTotal NOTIFY libraryWorkChanged)
    Q_PROPERTY(QString importSourceRoot READ importSourceRoot NOTIFY importPageChanged)
    Q_PROPERTY(QString importDestination READ importDestination NOTIFY importPageChanged)
    Q_PROPERTY(QString importSecondCopyDestination READ importSecondCopyDestination NOTIFY
                   importPageChanged)
    Q_PROPERTY(bool importSecondCopyEnabled READ importSecondCopyEnabled NOTIFY importPageChanged)
    Q_PROPERTY(QString importFilenameTemplate READ importFilenameTemplate NOTIFY importPageChanged)
    Q_PROPERTY(bool importRenameEnabled READ importRenameEnabled NOTIFY importPageChanged)
    Q_PROPERTY(QVariantList importRenameParts READ importRenameParts NOTIFY importPageChanged)
    Q_PROPERTY(int importRenameSeparator READ importRenameSeparator NOTIFY importPageChanged)
    Q_PROPERTY(QString importRenameExample READ importRenameExample NOTIFY importPageChanged)
    Q_PROPERTY(QString importMode READ importMode NOTIFY importPageChanged)
    Q_PROPERTY(QString importMoveUnavailableReason READ importMoveUnavailableReason NOTIFY
                   importPageChanged)
    Q_PROPERTY(QString importOrganization READ importOrganization NOTIFY importPageChanged)
    Q_PROPERTY(QString importPreviewPolicy READ importPreviewPolicy NOTIFY importPageChanged)
    Q_PROPERTY(bool importRecursive READ importRecursive NOTIFY importPageChanged)
    Q_PROPERTY(QString importIngestTransport READ importIngestTransport NOTIFY importPageChanged)
    Q_PROPERTY(QString importIngestSourceUri READ importIngestSourceUri NOTIFY importPageChanged)
    Q_PROPERTY(QVariantMap importNativeSupport READ importNativeSupport NOTIFY importPageChanged)
    Q_PROPERTY(QVariantMap importIngestReport READ importIngestReport NOTIFY importPageChanged)
    Q_PROPERTY(QString importResumeBatchId READ importResumeBatchId NOTIFY importPageChanged)
    Q_PROPERTY(ImportCandidateListModel *importCandidates READ importCandidates CONSTANT)
    Q_PROPERTY(FilesystemBrowserModel *importSourceFolders READ importSourceFolders CONSTANT)
    Q_PROPERTY(
        FilesystemBrowserModel *importDestinationFolders READ importDestinationFolders CONSTANT)

public:
    struct BatchCompletion
    {
        std::uint64_t generation;
        std::vector<ImportItemResult> results;
        LibraryQuery query;
        std::optional<std::int64_t> imported_after;
        std::optional<std::int64_t> imported_before;
        std::size_t imported_count;
        bool cancelled;
        std::size_t completed;
        std::size_t total;
        QString preference_error;
        std::optional<TaskError> failure = {};
    };
    struct Context
    {
        const QString &catalog_path;
        const bool &busy;
    };
    struct Host
    {
        std::function<LibraryQuery()> current_query;
        std::function<void(const std::vector<std::string> &)> begin_placeholders;
        std::function<void(const ImportItemResult &, int)> publish_item;
        std::function<void(BatchCompletion)> finish_batch;
        std::function<void()> enter_gallery;
    };
    explicit StudioImportWorkspace(Context context, Host host, QObject *parent = nullptr);
    ~StudioImportWorkspace() override;
    // Cancel GUI-owned sources and join folder/thumbnail work before the foreground
    // composition barrier. The import worker is stopped after that barrier.
    void shutdown();
    void shutdownWorker();
    void cancelImport(std::string reason);
    // Abandon this source's preparation without closing the Import workspace.
    void cancelImportSource();
    void catalogReplaced();
    // Final model reset occurs outside import; notify idle after the GUI transaction.
    [[nodiscard]] bool beginPublication(std::uint64_t generation) noexcept;
    [[nodiscard]] bool finishPublication(std::uint64_t generation, std::size_t completed,
                                         std::size_t total);
    void startDeferredPreviews();
    [[nodiscard]] bool galleryPlaceholders() const noexcept;
    [[nodiscard]] const bool &workActiveState() const noexcept;
    [[nodiscard]] StudioImportWorker &importWorker() noexcept;
    [[nodiscard]] Result<void>
    openImportWorkers(const std::string &catalog, std::shared_ptr<PreviewCache> cache,
                      std::shared_ptr<std::mutex> recovery_publication_mutex);
    [[nodiscard]] QString contextDebugInfo() const;
    [[nodiscard]] bool importPageOpen() const noexcept;
    [[nodiscard]] bool importScanActive() const noexcept;
    [[nodiscard]] int importDuplicateCount() const noexcept;
    [[nodiscard]] int importScanCompleted() const noexcept;
    [[nodiscard]] int importScanTotal() const noexcept;
    [[nodiscard]] bool importPreviewWorkActive() const noexcept;
    [[nodiscard]] int importPreviewWorkCompleted() const noexcept;
    [[nodiscard]] int importPreviewWorkTotal() const noexcept;
    [[nodiscard]] QString importDestinationError() const;
    [[nodiscard]] QString importSourceRoot() const;
    [[nodiscard]] ImportDraft importDraft() const;
    [[nodiscard]] QString importIngestTransport() const;
    [[nodiscard]] QString importMoveUnavailableReason() const;
    [[nodiscard]] QString importIngestSourceUri() const;
    [[nodiscard]] QVariantMap importNativeSupport() const;
    [[nodiscard]] QVariantMap importIngestReport() const;
    [[nodiscard]] QString importResumeBatchId() const;
    [[nodiscard]] QString importDestination() const;
    [[nodiscard]] bool importReady() const;
    [[nodiscard]] bool importInteractionBlocked() const;
    [[nodiscard]] QUrl importDestinationFolderUrl() const;
    [[nodiscard]] QUrl importSourceFolderUrl() const;
    [[nodiscard]] QUrl importSecondCopyFolderUrl() const;
    [[nodiscard]] QString importSecondCopyDestination() const;
    [[nodiscard]] bool importSecondCopyEnabled() const noexcept;
    [[nodiscard]] QString importFilenameTemplate() const;
    [[nodiscard]] bool importRenameEnabled() const noexcept;
    [[nodiscard]] QVariantList importRenameParts() const;
    [[nodiscard]] int importRenameSeparator() const noexcept;
    [[nodiscard]] QString importRenameExample() const;
    [[nodiscard]] QString importMode() const;
    [[nodiscard]] QString importOrganization() const;
    [[nodiscard]] QString importPreviewPolicy() const;
    [[nodiscard]] bool importRecursive() const noexcept;
    [[nodiscard]] ImportCandidateListModel *importCandidates() noexcept;
    [[nodiscard]] FilesystemBrowserModel *importSourceFolders() noexcept;
    [[nodiscard]] FilesystemBrowserModel *importDestinationFolders() noexcept;
    Q_INVOKABLE void openImportPage();
    Q_INVOKABLE void closeImportPage();
    Q_INVOKABLE void refreshImportSources();
    [[nodiscard]] QString importContextPath() const;
    Q_INVOKABLE bool setImportContextRow(int row);
    Q_INVOKABLE void setImportSourceRoot(const QString &path);
    Q_INVOKABLE void setImportDestination(const QString &path);
    Q_INVOKABLE void setImportSecondCopyDestination(const QString &path);
    Q_INVOKABLE void setImportSecondCopyEnabled(bool enabled);
    Q_INVOKABLE void setImportRenameEnabled(bool enabled);
    Q_INVOKABLE void setImportRenamePart(int position, int component);
    Q_INVOKABLE void setImportRenameSeparator(int separator);
    Q_INVOKABLE void setImportMode(const QString &mode);
    Q_INVOKABLE void setImportOrganization(const QString &organization);
    Q_INVOKABLE void setImportPreviewPolicy(const QString &policy);
    Q_INVOKABLE void setImportRecursive(bool recursive);
    Q_INVOKABLE void setImportIngestTransport(const QString &transport);
    Q_INVOKABLE void setImportResumeBatchId(const QString &batch_id);
    Q_INVOKABLE void refreshImportNativeSupport();
    Q_INVOKABLE void ensureImportThumbnail(int row);
    Q_INVOKABLE void setImportThumbnailViewportDemand(const QVariantList &rows, int prefetch = 2,
                                                      int current_row = -1);
    Q_INVOKABLE void startPlannedImport();
    Q_INVOKABLE void cancelImportPreviews();
    [[nodiscard]] QVariantList importDestinationPreview() const;
    [[nodiscard]] QString importDestinationPreviewError() const;
    [[nodiscard]] bool importDestinationPreviewActive() const;
    [[nodiscard]] bool importWorkActive() const noexcept;
    [[nodiscard]] int importWorkCompleted() const noexcept;
    [[nodiscard]] QString importWorkTitle() const;
    [[nodiscard]] QString importWorkCountText() const;
    [[nodiscard]] QString importWorkDetailText() const;
    [[nodiscard]] double importWorkFraction() const;
    [[nodiscard]] JsonValue jsonSnapshot() const;
    [[nodiscard]] int importWorkTotal() const noexcept;
    Q_INVOKABLE void importFolder(const QUrl &folder_url);
    Q_INVOKABLE void importFilePaths(const QStringList &paths);
    Q_INVOKABLE void importFolderFromPath(const QString &path);
    Q_INVOKABLE void importFiles(const QList<QUrl> &files);
    [[nodiscard]] bool importPreflightActive() const noexcept
    {
        return import_preflight_active_;
    }
signals:
    void importProgressChanged();
    void importPageChanged();
    void importDestinationPreviewChanged();
    void importContextChanged();
    void libraryWorkChanged();
    void errorOccurred(QString error);
    void statusOccurred(QString status);

private:
    friend class testing::StudioImportTestControl;
    static QVariantMap native_support_to_map(const NativeIngestPlatformSupport &support);
    static QVariantMap ingest_report_to_map(const IngestBatchResult &detailed);
    void setError(QString error);
    void setStatus(QString status);
    void beginImportGalleryPlaceholders(const std::vector<std::string> &paths);
    void publishImportItem(const ImportItemResult &item, int row);
    void finishImportBatch(std::optional<TaskError> failure = {});
    void validateImportDestination();
    void rescanImportSource();
    void beginPlannedImport(ImportRequest request);
    void startNextImportPreview();
    void requestFilesystemListing(FilesystemBrowserModel *browser, const QString &path,
                                  quint64 generation);
    [[nodiscard]] ImportRequest plannedImportRequest() const;
    void refreshImportDestinationPreview();
    void setImportWork(int completed, int total, bool active);
    [[nodiscard]] ImportWorkProgressCallback progressObserver(std::uint64_t generation,
                                                              CancellationToken token);
    void startNextImportItem();
    Context context_;
    Host host_;
    std::unique_ptr<StudioImportWorker> worker;
    std::unique_ptr<StudioImportWorker> destination_preview_worker;
    ImportCandidateListModel candidates;
    FilesystemBrowserModel source_folders;
    FilesystemBrowserModel destination_folders;
    ImportDraft draft;
    // One page-entry restore intent, consumed before expanding the selected folder.
    QString pending_source_restore_;
    std::unique_ptr<StudioImportScanController> scan;
    std::unique_ptr<StudioImportThumbnailController> thumbnails;
    std::unique_ptr<StudioImportDestinationPreviewController> destination_preview;
    SerialExecutor filesystem_executor_;
    std::uint64_t import_roots_generation_ = 0;
    CancellationSource import_operation_;
    CancellationSource import_preview_operation_;
    int import_context_row_ = -1;
    std::uint64_t import_context_generation_ = 0;
    QString import_context_path_;
    bool import_work_active_ = false;
    int import_work_completed_ = 0;
    int import_work_total_ = 0;
    ImportWorkProgress import_work_progress_;
    std::uint64_t import_progress_revision_ = 0;
    std::vector<std::string> pending_import_paths_;
    std::vector<ImportItemResult> import_results_;
    LibraryQuery import_query_snapshot_;
    std::size_t import_next_index_ = 0U;
    bool import_gallery_placeholders_ = false;
    bool import_defer_previews_ = false;
    bool import_skip_existing_ = false;
    std::unordered_map<std::string, std::string> pending_import_content_hashes_;
    bool import_page_open_ = false;
    bool import_preview_work_active_ = false;
    std::uint64_t import_generation_ = 0;
    std::uint64_t import_preview_generation_ = 0;
    int import_preview_work_completed_ = 0;
    int import_preview_work_total_ = 0;
    bool import_preflight_active_ = false;
    bool import_publishing_ = false;
    QString pending_import_destination_;
    QString import_preference_error_;
    bool import_destination_remembered_ = false;
    bool import_recursive_ = true;
    QString import_ingest_transport_{QStringLiteral("folder")};
    QVariantMap import_native_support_;
    QVariantMap import_ingest_report_;
    QString import_resume_batch_id_;
    std::deque<std::string> pending_import_preview_ids_;
    ImportPreviewPolicy pending_import_preview_policy_ = ImportPreviewPolicy::kStandard;
    bool stopped_ = false;
};
} // namespace ravo
