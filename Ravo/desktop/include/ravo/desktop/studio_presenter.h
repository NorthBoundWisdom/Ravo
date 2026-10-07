#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QCache>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/filesystem_browser_model.h"
#include "ravo/desktop/studio_import_draft.h"
#include "ravo/desktop/folder_list_model.h"
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_library_presenter.h"
#include "ravo/desktop/studio_inspect_presenter.h"
#include "ravo/desktop/studio_develop_presenter.h"
#include "ravo/desktop/studio_export_presenter.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/preview_request_owner.h"
#include "ravo/domain/types.h"
#include "ravo/services/cull_assistance.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/local_adjustment.h"
#include "ravo/engine/mask_geometry.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

class StudioCommandController;
class StudioLiveSessionController;
class StudioDisplayPresentation;
class StudioImportWorker;
struct DisplayPresentationState;
namespace testing
{
class StudioImportTestControl;
class StudioPipelineTestControl;
} // namespace testing

class StudioImportThumbnailController;
class StudioImportDestinationPreviewController;
class StudioImportScanController;
struct StudioImportWorkspace;
struct StudioLibraryResume;
class StudioStartupController;

class StudioPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(StudioExportPresenter *exports READ exports CONSTANT)
    Q_PROPERTY(StudioDevelopPresenter *develop READ develop CONSTANT)
    Q_PROPERTY(StudioInspectPresenter *inspect READ inspect CONSTANT)
    Q_PROPERTY(StudioLibraryPresenter *library READ library CONSTANT)
    Q_PROPERTY(bool catalogOpen READ catalogOpen NOTIFY catalogChanged)
    Q_PROPERTY(QString catalogPath READ catalogPath NOTIFY catalogChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(QString selectedAssetId READ selectedAssetId NOTIFY selectionChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectionChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(int selectedRating READ selectedRating NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedColorLabel READ selectedColorLabel NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedRejected READ selectedRejected NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedPicked READ selectedPicked NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedImportState READ selectedImportState NOTIFY selectionChanged)
    Q_PROPERTY(bool canDeleteFromDisk READ canDeleteFromDisk NOTIFY selectionChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(int previewViewportWidth READ previewViewportWidth NOTIFY previewChanged)
    Q_PROPERTY(int previewViewportHeight READ previewViewportHeight NOTIFY previewChanged)
    Q_PROPERTY(QUrl inspectRoiUrl READ inspectRoiUrl NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiX READ inspectRoiX NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiY READ inspectRoiY NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiWidth READ inspectRoiWidth NOTIFY inspectRoiChanged)
    Q_PROPERTY(double inspectRoiHeight READ inspectRoiHeight NOTIFY inspectRoiChanged)
    Q_PROPERTY(quint64 gpuPreviewGeneration READ gpuPreviewGeneration NOTIFY previewChanged)
    Q_PROPERTY(quint64 gpuPreviewNativeSurface READ gpuPreviewNativeSurface NOTIFY previewChanged)
    Q_PROPERTY(int gpuPreviewWidth READ gpuPreviewWidth NOTIFY previewChanged)
    Q_PROPERTY(int gpuPreviewHeight READ gpuPreviewHeight NOTIFY previewChanged)
    Q_PROPERTY(quint64 gpuRoiGeneration READ gpuRoiGeneration NOTIFY inspectRoiChanged)
    Q_PROPERTY(quint64 gpuRoiNativeSurface READ gpuRoiNativeSurface NOTIFY inspectRoiChanged)
    Q_PROPERTY(int gpuRoiWidth READ gpuRoiWidth NOTIFY inspectRoiChanged)
    Q_PROPERTY(int gpuRoiHeight READ gpuRoiHeight NOTIFY inspectRoiChanged)
    Q_PROPERTY(QUrl comparisonBeforeUrl READ comparisonBeforeUrl NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString browseMode READ browseMode NOTIFY browseModeChanged)
    Q_PROPERTY(bool collapseStacks READ collapseStacks NOTIFY filterChanged)
    Q_PROPERTY(int surveySlotCount READ surveySlotCount NOTIFY surveyChanged)
    Q_PROPERTY(QVariantList surveySlots READ surveySlots NOTIFY surveyChanged)
    Q_PROPERTY(QString zoomMode READ zoomMode NOTIFY zoomChanged)
    Q_PROPERTY(double zoomFactor READ zoomFactor NOTIFY zoomChanged)
    Q_PROPERTY(
        int thumbnailSize READ thumbnailSize WRITE setThumbnailSize NOTIFY thumbnailSizeChanged)
    Q_PROPERTY(QString cullSuggestionFilter READ cullSuggestionFilter NOTIFY filterChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY filterChanged)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filterChanged)
    Q_PROPERTY(bool selectedHasEdits READ selectedHasEdits NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedTags READ selectedTags NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedDescription READ selectedDescription NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCreator READ selectedCreator NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCopyright READ selectedCopyright NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCountry READ selectedCountry NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedProvinceState READ selectedProvinceState NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCity READ selectedCity NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedSublocation READ selectedSublocation NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedHeadline READ selectedHeadline NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCredit READ selectedCredit NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedSource READ selectedSource NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedInstructions READ selectedInstructions NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedUsageTerms READ selectedUsageTerms NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedJobId READ selectedJobId NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCaptureSummary READ selectedCaptureSummary NOTIFY selectionChanged)
    Q_PROPERTY(AssetListModel *assets READ assets CONSTANT)
    Q_PROPERTY(FolderListModel *folders READ folders CONSTANT)
    Q_PROPERTY(LibrarySetListModel *librarySets READ librarySets CONSTANT)
    Q_PROPERTY(QUrl selectedThumbnailUrl READ selectedThumbnailUrl NOTIFY thumbnailsChanged)
    Q_PROPERTY(QString selectedFolderUri READ selectedFolderUri NOTIFY folderChanged)
    Q_PROPERTY(QString selectedLibrarySetId READ selectedLibrarySetId NOTIFY folderChanged)
    Q_PROPERTY(bool lastImportAvailable READ lastImportAvailable NOTIFY folderChanged)
    Q_PROPERTY(bool lastImportSelected READ lastImportSelected NOTIFY folderChanged)
    Q_PROPERTY(int lastImportCount READ lastImportCount NOTIFY folderChanged)
    Q_PROPERTY(QString selectedDisplayName READ selectedDisplayName NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedFolderPath READ selectedFolderPath NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedMediaType READ selectedMediaType NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedDimensions READ selectedDimensions NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedFileSize READ selectedFileSize NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedUri READ selectedUri NOTIFY selectionChanged)
    Q_PROPERTY(QUrl defaultCatalogFolder READ defaultCatalogFolder CONSTANT)
    Q_PROPERTY(QUrl defaultCatalogFile READ defaultCatalogFile CONSTANT)
    Q_PROPERTY(QString startupCatalogPath READ startupCatalogPath CONSTANT)
    Q_PROPERTY(bool importWorkActive READ importWorkActive NOTIFY libraryWorkChanged)
    Q_PROPERTY(int importWorkCompleted READ importWorkCompleted NOTIFY libraryWorkChanged)
    Q_PROPERTY(int importWorkTotal READ importWorkTotal NOTIFY libraryWorkChanged)
    Q_PROPERTY(bool previewWorkActive READ previewWorkActive NOTIFY libraryWorkChanged)
    Q_PROPERTY(int previewWorkCompleted READ previewWorkCompleted NOTIFY libraryWorkChanged)
    Q_PROPERTY(int previewWorkTotal READ previewWorkTotal NOTIFY libraryWorkChanged)
    Q_PROPERTY(bool catalogOperationActive READ catalogOperationActive NOTIFY libraryWorkChanged)
    Q_PROPERTY(QString catalogOperationStage READ catalogOperationStage NOTIFY libraryWorkChanged)
    Q_PROPERTY(
        int catalogOperationCompleted READ catalogOperationCompleted NOTIFY libraryWorkChanged)
    Q_PROPERTY(int catalogOperationTotal READ catalogOperationTotal NOTIFY libraryWorkChanged)
    Q_PROPERTY(int recoveryPendingCount READ recoveryPendingCount NOTIFY libraryWorkChanged)
    Q_PROPERTY(int libraryTotal READ libraryTotal NOTIFY filterChanged)
    Q_PROPERTY(bool libraryHasMore READ libraryHasMore NOTIFY filterChanged)
    Q_PROPERTY(QVariantMap backupScheduleStatus READ backupScheduleStatus NOTIFY libraryWorkChanged)
    Q_PROPERTY(QVariantMap externalEditorSession READ externalEditorSession NOTIFY
                   externalEditorSessionChanged)
    Q_PROPERTY(QVariantMap offlineEditMediaStatus READ offlineEditMediaStatus NOTIFY
                   offlineEditMediaStatusChanged)
    Q_PROPERTY(QVariantList offlineEditProxyList READ offlineEditProxyList NOTIFY
                   offlineEditProxyListChanged)
    Q_PROPERTY(
        QVariantMap selectedAiProposal READ selectedAiProposal NOTIFY selectedAiProposalChanged)
    Q_PROPERTY(QVariantList aiProposals READ aiProposals NOTIFY aiProposalsChanged)
    Q_PROPERTY(bool importPageOpen READ importPageOpen NOTIFY importPageChanged)
    Q_PROPERTY(bool importScanActive READ importScanActive NOTIFY importPageChanged)
    Q_PROPERTY(int importDuplicateCount READ importDuplicateCount NOTIFY importPageChanged)
    Q_PROPERTY(int importScanCompleted READ importScanCompleted NOTIFY importPageChanged)
    Q_PROPERTY(int importScanTotal READ importScanTotal NOTIFY importPageChanged)
    Q_PROPERTY(bool importPreflightActive READ importPreflightActive NOTIFY importPageChanged)
    Q_PROPERTY(bool importReady READ importReady NOTIFY importPageChanged)
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
    Q_PROPERTY(QString importFilenameTemplate READ importFilenameTemplate NOTIFY importPageChanged)
    Q_PROPERTY(QString importMode READ importMode NOTIFY importPageChanged)
    Q_PROPERTY(QString importOrganization READ importOrganization NOTIFY importPageChanged)
    Q_PROPERTY(QString importPreviewPolicy READ importPreviewPolicy NOTIFY importPageChanged)
    Q_PROPERTY(bool importRecursive READ importRecursive NOTIFY importPageChanged)
    Q_PROPERTY(QString importIngestTransport READ importIngestTransport NOTIFY importPageChanged)
    Q_PROPERTY(QString importIngestSourceUri READ importIngestSourceUri NOTIFY importPageChanged)
    Q_PROPERTY(QVariantMap importNativeSupport READ importNativeSupport NOTIFY importPageChanged)
    Q_PROPERTY(QVariantMap importIngestReport READ importIngestReport NOTIFY importPageChanged)
    Q_PROPERTY(QVariantMap iqQualityPolicy READ iqQualityPolicy CONSTANT)
    Q_PROPERTY(QString importResumeBatchId READ importResumeBatchId NOTIFY importPageChanged)
    Q_PROPERTY(ImportCandidateListModel *importCandidates READ importCandidates CONSTANT)
    Q_PROPERTY(FilesystemBrowserModel *importSourceFolders READ importSourceFolders CONSTANT)
    Q_PROPERTY(
        FilesystemBrowserModel *importDestinationFolders READ importDestinationFolders CONSTANT)

public:
    [[nodiscard]] StudioExportPresenter *exports() noexcept
    {
        return export_presenter_.get();
    }
    [[nodiscard]] const StudioExportPresenter *exports() const noexcept
    {
        return export_presenter_.get();
    }
    [[nodiscard]] StudioDevelopPresenter *develop() noexcept
    {
        return develop_presenter_.get();
    }
    [[nodiscard]] const StudioDevelopPresenter *develop() const noexcept
    {
        return develop_presenter_.get();
    }
    [[nodiscard]] StudioInspectPresenter *inspect() noexcept
    {
        return &inspect_;
    }
    [[nodiscard]] StudioLibraryPresenter *library() noexcept
    {
        return &library_;
    }
    explicit StudioPresenter(QObject *parent = nullptr);
    ~StudioPresenter() override;

    [[nodiscard]] bool catalogOpen() const noexcept;
    [[nodiscard]] QString catalogPath() const;
    [[nodiscard]] QUrl defaultCatalogFolder() const;
    [[nodiscard]] QUrl defaultCatalogFile() const;
    [[nodiscard]] QString startupCatalogPath() const;
    void setStartupCatalogPath(const QString &path);
    Q_INVOKABLE bool defaultCatalogExists() const;
    [[nodiscard]] bool importWorkActive() const noexcept;
    [[nodiscard]] int importWorkCompleted() const noexcept;
    [[nodiscard]] int importWorkTotal() const noexcept;
    [[nodiscard]] bool previewWorkActive() const noexcept;
    [[nodiscard]] int previewWorkCompleted() const noexcept;
    [[nodiscard]] int previewWorkTotal() const noexcept;
    [[nodiscard]] bool catalogOperationActive() const noexcept;
    [[nodiscard]] QString catalogOperationStage() const;
    [[nodiscard]] int catalogOperationCompleted() const noexcept;
    [[nodiscard]] int catalogOperationTotal() const noexcept;
    [[nodiscard]] int recoveryPendingCount() const noexcept;
    [[nodiscard]] int libraryTotal() const noexcept;
    [[nodiscard]] bool libraryHasMore() const noexcept;
    [[nodiscard]] QVariantMap backupScheduleStatus() const;
    [[nodiscard]] bool importPageOpen() const noexcept;
    [[nodiscard]] QString importContextPath() const;
    Q_INVOKABLE bool setImportContextRow(int row);
    [[nodiscard]] bool importScanActive() const noexcept;
    [[nodiscard]] int importDuplicateCount() const noexcept;
    [[nodiscard]] int importScanCompleted() const noexcept;
    [[nodiscard]] int importScanTotal() const noexcept;
    [[nodiscard]] bool importPreflightActive() const noexcept
    {
        return import_preflight_active_;
    }
    [[nodiscard]] bool importReady() const;
    [[nodiscard]] QString importDestinationError() const;
    [[nodiscard]] QUrl importDestinationFolderUrl() const;
    [[nodiscard]] QUrl importSourceFolderUrl() const;
    [[nodiscard]] QUrl importSecondCopyFolderUrl() const;
    [[nodiscard]] bool importPreviewWorkActive() const noexcept;
    [[nodiscard]] int importPreviewWorkCompleted() const noexcept;
    [[nodiscard]] int importPreviewWorkTotal() const noexcept;
    [[nodiscard]] QString importSourceRoot() const;
    [[nodiscard]] ImportDraft importDraft() const;
    [[nodiscard]] QString importDestination() const;
    [[nodiscard]] QString importSecondCopyDestination() const;
    [[nodiscard]] QString importFilenameTemplate() const;
    [[nodiscard]] QString importMode() const;
    [[nodiscard]] QString importOrganization() const;
    [[nodiscard]] QString importPreviewPolicy() const;
    [[nodiscard]] bool importRecursive() const noexcept;
    [[nodiscard]] QVariantList importDestinationPreview() const;
    [[nodiscard]] QString importDestinationPreviewError() const;
    [[nodiscard]] bool importDestinationPreviewActive() const;
    [[nodiscard]] QString importIngestTransport() const;
    [[nodiscard]] QString importIngestSourceUri() const;
    [[nodiscard]] QVariantMap importNativeSupport() const;
    [[nodiscard]] QVariantMap importIngestReport() const;
    [[nodiscard]] QVariantMap iqQualityPolicy() const;
    Q_INVOKABLE QVariantMap evaluateIqQuality(const QString &corpusRoot = QString(),
                                              double strength = 0.35) const;
    [[nodiscard]] QString importResumeBatchId() const;
    [[nodiscard]] ImportCandidateListModel *importCandidates() noexcept;
    [[nodiscard]] FilesystemBrowserModel *importSourceFolders() noexcept;
    [[nodiscard]] FilesystemBrowserModel *importDestinationFolders() noexcept;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] QString selectedAssetId() const;
    [[nodiscard]] int selectedIndex() const;
    [[nodiscard]] int selectedCount() const noexcept;
    Q_INVOKABLE bool isAssetSelected(const QString &asset_id) const;
    [[nodiscard]] int selectedRating() const;
    [[nodiscard]] QString selectedColorLabel() const;
    [[nodiscard]] bool selectedRejected() const noexcept;
    [[nodiscard]] bool selectedPicked() const noexcept;
    [[nodiscard]] QString selectedImportState() const;
    [[nodiscard]] bool canDeleteFromDisk() const;
    [[nodiscard]] QUrl previewUrl() const;
    // ADR-0144: bind presentation owner so Loupe/Develop/comparison apply monitor ICC.
    void bindDisplayPresentation(StudioDisplayPresentation *owner);
    [[nodiscard]] int previewViewportWidth() const noexcept;
    [[nodiscard]] int previewViewportHeight() const noexcept;
    [[nodiscard]] QUrl inspectRoiUrl() const;
    [[nodiscard]] QImage inspectRoiImage() const;
    [[nodiscard]] double inspectRoiX() const noexcept;
    [[nodiscard]] double inspectRoiY() const noexcept;
    [[nodiscard]] double inspectRoiWidth() const noexcept;
    [[nodiscard]] double inspectRoiHeight() const noexcept;
    [[nodiscard]] quint64 gpuPreviewGeneration() const noexcept;
    [[nodiscard]] quint64 gpuPreviewNativeSurface() const noexcept;
    [[nodiscard]] int gpuPreviewWidth() const noexcept;
    [[nodiscard]] int gpuPreviewHeight() const noexcept;
    [[nodiscard]] quint64 gpuRoiGeneration() const noexcept;
    [[nodiscard]] quint64 gpuRoiNativeSurface() const noexcept;
    [[nodiscard]] int gpuRoiWidth() const noexcept;
    [[nodiscard]] int gpuRoiHeight() const noexcept;
    [[nodiscard]] QImage previewImage() const;
    [[nodiscard]] QUrl comparisonBeforeUrl() const;
    [[nodiscard]] QImage comparisonBeforeImage() const;
    [[nodiscard]] bool previewLoading() const noexcept;

    [[nodiscard]] QString browseMode() const;
    [[nodiscard]] bool collapseStacks() const noexcept;
    [[nodiscard]] int surveySlotCount() const noexcept;
    [[nodiscard]] QVariantList surveySlots() const;
    [[nodiscard]] QString zoomMode() const;
    [[nodiscard]] double zoomFactor() const noexcept;
    [[nodiscard]] int thumbnailSize() const noexcept;

    [[nodiscard]] int visibleCount() const;
    [[nodiscard]] bool filtersActive() const noexcept;
    [[nodiscard]] bool selectedHasEdits() const noexcept;

    [[nodiscard]] QString selectedTags() const;
    [[nodiscard]] QString selectedTitle() const;
    [[nodiscard]] QString selectedDescription() const;
    [[nodiscard]] QString selectedCreator() const;
    [[nodiscard]] QString selectedCopyright() const;
    [[nodiscard]] QString selectedCountry() const;
    [[nodiscard]] QString selectedProvinceState() const;
    [[nodiscard]] QString selectedCity() const;
    [[nodiscard]] QString selectedSublocation() const;
    [[nodiscard]] QString selectedHeadline() const;
    [[nodiscard]] QString selectedCredit() const;
    [[nodiscard]] QString selectedSource() const;
    [[nodiscard]] QString selectedInstructions() const;
    [[nodiscard]] QString selectedUsageTerms() const;
    [[nodiscard]] QString selectedJobId() const;
    [[nodiscard]] QString selectedCaptureSummary() const;

    [[nodiscard]] AssetListModel *assets() noexcept;
    [[nodiscard]] FolderListModel *folders() noexcept;
    [[nodiscard]] LibrarySetListModel *librarySets() noexcept;
    [[nodiscard]] QUrl selectedThumbnailUrl() const;
    [[nodiscard]] QString selectedFolderUri() const;
    [[nodiscard]] QString selectedLibrarySetId() const;
    [[nodiscard]] bool lastImportAvailable() const noexcept;
    [[nodiscard]] bool lastImportSelected() const noexcept;
    [[nodiscard]] int lastImportCount() const noexcept;
    [[nodiscard]] QString selectedDisplayName() const;
    [[nodiscard]] QString selectedFolderPath() const;
    [[nodiscard]] QString selectedMediaType() const;
    [[nodiscard]] QString selectedDimensions() const;
    [[nodiscard]] QString selectedFileSize() const;
    [[nodiscard]] QString selectedUri() const;

    Q_INVOKABLE void createCatalog(const QUrl &file_url);
    Q_INVOKABLE void openCatalog(const QUrl &file_url);
    Q_INVOKABLE void importFiles(const QList<QUrl> &files);
    Q_INVOKABLE void importFolder(const QUrl &folder_url);
    Q_INVOKABLE void createCatalogFromPath(const QString &path);
    Q_INVOKABLE void openCatalogFromPath(const QString &path);
    Q_INVOKABLE void importFilePaths(const QStringList &paths);
    Q_INVOKABLE void importFolderFromPath(const QString &path);
    Q_INVOKABLE void openImportPage();
    Q_INVOKABLE void refreshImportSources();
    Q_INVOKABLE void closeImportPage();
    Q_INVOKABLE void setImportSourceRoot(const QString &path);
    Q_INVOKABLE void setImportDestination(const QString &path);
    Q_INVOKABLE void setImportSecondCopyDestination(const QString &path);
    Q_INVOKABLE void setImportFilenameTemplate(const QString &filename_template);
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
    Q_INVOKABLE void refreshRecoveryStatus();
    Q_INVOKABLE void synchronizeRecovery();
    Q_INVOKABLE void createBackupAtPath(const QString &path);
    Q_INVOKABLE void verifyBackupAtPath(const QString &path);
    Q_INVOKABLE void restoreBackupToPath(const QString &backup_path, const QString &catalog_path);
    Q_INVOKABLE void rebuildSelectedPreviews();
    Q_INVOKABLE void rebuildAllPreviews();
    Q_INVOKABLE void cancelCatalogOperation();
    Q_INVOKABLE void configureBackupSchedule(const QString &directory, int interval_minutes,
                                             int retention_count, bool enabled);
    Q_INVOKABLE void runScheduledBackupNow();
    Q_INVOKABLE void disableBackupSchedule();
    Q_INVOKABLE void relinkFolder(const QString &folder_id, const QString &replacement_directory);
    Q_INVOKABLE void revealFolderInFileManager(const QString &folder_uri);
    Q_INVOKABLE void removeFolderFromCatalog(const QString &folder_uri);
    Q_INVOKABLE QString folderLocalPath(const QString &folder_uri) const;
    void checkScheduledBackup();

    Q_INVOKABLE QVariantMap externalEditorSession() const;
    Q_INVOKABLE void preparePhotoMerge(const QString &kind);
    Q_INVOKABLE void applyPhotoMerge(const QVariantMap &options);
    Q_INVOKABLE QVariantMap externalEditorDefaultOptions() const;
    Q_INVOKABLE QVariantList externalEditorTiffSampleTypeChoices() const;
    Q_INVOKABLE void prepareExternalEditorWorkingCopy(const QVariantMap &options);
    Q_INVOKABLE void openExternalEditorWorkingCopy(const QString &working_path,
                                                   const QString &application_path = QString());
    Q_INVOKABLE void checkExternalEditorReturned(const QString &working_copy_id = QString(),
                                                 const QString &returned_path = QString());
    Q_INVOKABLE void clearExternalEditorSession();
    Q_INVOKABLE void abandonExternalEditorWorkingCopy(const QString &working_copy_id = QString());
    Q_INVOKABLE void reopenExternalEditorWorkingCopy(const QString &working_copy_id = QString(),
                                                     bool open_after = true,
                                                     const QString &application_path = QString());
    Q_INVOKABLE void
    refreshExternalEditorWorkingCopyStatus(const QString &working_copy_id = QString());
    Q_INVOKABLE QVariantMap offlineEditMediaStatus() const;
    Q_INVOKABLE QVariantList offlineEditProxyList() const;
    Q_INVOKABLE void refreshOfflineEditMediaStatus();
    Q_INVOKABLE void refreshOfflineEditProxyList();
    Q_INVOKABLE void createOfflineEditProxy(unsigned int max_edge = 0);
    Q_INVOKABLE void reconnectOfflineEditProxy(bool clear_proxy = false);
    Q_INVOKABLE void deleteOfflineEditProxy(bool force = false);
    Q_INVOKABLE void pinOfflineEditProxy(bool pinned = true);
    Q_INVOKABLE void evictOfflineEditProxies(qulonglong max_total_bytes);
    Q_INVOKABLE QVariantMap selectedAiProposal() const;
    Q_INVOKABLE QVariantList aiProposals() const;
    Q_INVOKABLE void refreshAiProposals();
    Q_INVOKABLE void createAiStubProposal(const QString &kind = QStringLiteral("global"),
                                          const QString &semantic_label = QString());
    Q_INVOKABLE void selectAiProposal(const QString &proposal_id);
    Q_INVOKABLE void clearSelectedAiProposal();
    Q_INVOKABLE void applySelectedAiProposal();
    Q_INVOKABLE void rejectSelectedAiProposal();
    Q_INVOKABLE void cancelSelectedAiProposal();
    Q_INVOKABLE void selectAsset(const QString &asset_id);
    Q_INVOKABLE void selectLibraryRow(int row, const QString &mode = QStringLiteral("single"),
                                      bool open_loupe = false);
    Q_INVOKABLE void selectAssetRange(const QString &asset_id);
    Q_INVOKABLE void toggleAssetSelected(const QString &asset_id);
    Q_INVOKABLE void selectAllVisible();
    Q_INVOKABLE void selectNext();
    Q_INVOKABLE void selectPrevious();
    Q_INVOKABLE void setBrowseMode(const QString &mode);
    Q_INVOKABLE void openLoupe();
    Q_INVOKABLE void openDevelop();
    Q_INVOKABLE void openSurvey();
    // ADR-0155: Survey pair from selected asset stack; step previous/next in burst.
    [[nodiscard]] bool canOpenBurstCompare() const;
    [[nodiscard]] bool burstCompareActive() const;
    [[nodiscard]] bool burstComparePending() const noexcept;
    Q_INVOKABLE void openBurstCompare();
    Q_INVOKABLE void stepBurstComparePrevious();
    Q_INVOKABLE void stepBurstCompareNext();
    Q_INVOKABLE void returnToGrid();
    Q_INVOKABLE void selectSurveySlot(const QString &asset_id);
    Q_INVOKABLE void createAssetVersion();
    Q_INVOKABLE void stackSelection();
    Q_INVOKABLE void unstackSelection();
    Q_INVOKABLE void setSelectedStackPick();
    Q_INVOKABLE void setCollapseStacks(bool collapse);

    Q_INVOKABLE void setZoomMode(const QString &mode);
    Q_INVOKABLE void setZoomFactor(double factor);
    Q_INVOKABLE void adjustZoom(int wheel_delta);
    Q_INVOKABLE void toggleActualSize();
    Q_INVOKABLE void requestInspectRoi(double x, double y, double width, double height);
    Q_INVOKABLE void setThumbnailSize(int size);
    Q_INVOKABLE void setAssetTags(const QString &text);
    Q_INVOKABLE void setMetadataField(const QString &name, const QString &value);
    Q_INVOKABLE void refreshSelectedMetadata();

    [[nodiscard]] QString selectedPhotoDebugInfo() const;
    [[nodiscard]] QString selectedPhotoParametersDebugInfo() const;
    [[nodiscard]] QString presetDebugInfo(const QString &path) const;
    Q_INVOKABLE void copySelectedPhotoDebugInfo();
    Q_INVOKABLE void copySelectedPhotoParametersDebugInfo();
    Q_INVOKABLE void revealSelectedPhotoInFileManager();
    Q_INVOKABLE void copyPresetDebugInfo(const QString &path);
    Q_INVOKABLE void createSnapshot(const QString &label);
    Q_INVOKABLE void renameSnapshot(int history_id, const QString &label);

    Q_INVOKABLE void setRating(int rating);
    Q_INVOKABLE void setColorLabel(const QString &label);
    Q_INVOKABLE void toggleRejected();
    Q_INVOKABLE void togglePicked();
    Q_INVOKABLE void applyCullReview(const QString &flagAction, const QVariant &rating,
                                     const QString &colorLabel, bool autoAdvance);

    Q_INVOKABLE void setCullSuggestionFilter(const QString &mode);

    [[nodiscard]] QString cullSuggestionFilter() const;

    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void selectFolder(const QString &folder_uri);
    Q_INVOKABLE void selectLastImport();
    Q_INVOKABLE void selectLibrarySet(const QString &set_id);
    Q_INVOKABLE void createManualLibrarySet(const QString &name);
    Q_INVOKABLE void createSmartLibrarySet(const QString &name);
    Q_INVOKABLE void renameLibrarySet(const QString &set_id, const QString &name);
    Q_INVOKABLE void deleteLibrarySet(const QString &set_id);
    Q_INVOKABLE void addSelectionToLibrarySet(const QString &set_id);
    Q_INVOKABLE void removeSelectionFromLibrarySet(const QString &set_id);
    Q_INVOKABLE void ensureThumbnail(const QString &asset_id);

    Q_INVOKABLE void ensureLibraryRow(int row);
    Q_INVOKABLE void loadNextLibraryPage();
    void pollCatalogRevision();
signals:
    void importContextChanged();
    void catalogChanged();
    void busyChanged();
    void statusChanged();
    void errorChanged();
    void selectionChanged();
    void previewChanged();
    void inspectRoiChanged();
    void interactivePreviewPublished(qulonglong revision, qlonglong intentToImageMicroseconds);
    void inspectContextChanged();
    void browseModeChanged();
    void surveyChanged();
    void zoomChanged();
    void thumbnailSizeChanged();
    void filterChanged();
    void folderChanged();
    void libraryWorkChanged();
    void externalEditorSessionChanged();
    void photoMergeDialogRequested(const QVariantMap &context);
    void offlineEditMediaStatusChanged();
    void offlineEditProxyListChanged();
    void selectedAiProposalChanged();
    void aiProposalsChanged();
    void thumbnailsChanged();
    void importPageChanged();
    void importDestinationPreviewChanged();

private:
    friend class StudioCommandController;
    friend class testing::StudioImportTestControl;
    friend class testing::StudioPipelineTestControl;
    friend class StudioLiveSessionController;

    void setBusy(bool busy);
    void setStatus(QString text);
    void setError(QString text);
    void applyAssets(std::vector<AssetRecord> assets, bool restore_selection,
                     std::unordered_map<std::string, QUrl> thumbnail_urls = {},
                     std::unordered_map<std::string, QString> thumbnail_states = {},
                     std::size_t total = 0U, bool has_more = false, std::size_t offset = 0U);
    friend class StudioStartupController;
    void initializeLibraryResume();
    void persistLibraryPosition();
    [[nodiscard]] Result<LibraryQuery> beginLibraryResume(const QString &path, LibraryQuery query);
    [[nodiscard]] std::optional<std::string> resumeAssetId() const;
    [[nodiscard]] bool resumeCollapseStacks() const;
    void finishLibraryResume(bool success);
    void applyFolders(std::vector<FolderRecord> folders);
    void applyLibrarySets(std::vector<LibrarySetRecord> sets);
    void clearLastImportQuery();
    void requestPreviewForSelection();
    void requestSurveyPreviews();
    void startSurveyPreviewRequest(std::string asset_id);
    void finishSurveyPreviewRequest(bool success);
    void rebuild_survey_slots();
    void apply_burst_compare_pair(const BurstComparePair &pair, bool preserve_inspect_roi);
    void request_burst_compare(BurstCompareStep step, bool preserve_inspect_roi);
    void reloadVisibleAssets();
    void start_catalog_revision_watch(std::int64_t revision);
    void resetThumbnailDemand();
    void requestLibraryPage(std::size_t offset, std::optional<std::string> cursor, bool sequential);
    void kickThumbnailDemand();
    void startThumbnailRequest(std::string asset_id);
    void setImportWork(int completed, int total, bool active);
    void beginImportGalleryPlaceholders(const std::vector<std::string> &paths);
    void publishImportItem(const ImportItemResult &item, int row);
    void startNextImportItem();
    void requestFilesystemListing(FilesystemBrowserModel *browser, const QString &path,
                                  quint64 generation);
    void finishImportBatch();
    void setCatalogOperation(QString stage, int completed, int total, bool active);
    void startPreviewRebuild(std::vector<std::string> asset_ids, std::size_t expected_total);
    void startScheduledBackup(bool force);
    void finishThumbnailRequest(bool success);
    void rescanImportSource();
    void beginPlannedImport(ImportRequest request);
    void validateImportDestination();
    void startNextImportPreview();

    void clear_displayed_preview();
    [[nodiscard]] QImage
    apply_display_presentation_image(const QImage &output_referred,
                                     const ColorProfileState &source_profile) const;
    void handle_display_presentation_changed();
    void reapply_display_presentation_to_cached_previews();
    void reapply_display_presentation_to_cached_thumbnails();
    void release_gpu_preview_presented_surface();
    void release_gpu_roi_presented_surface();
    [[nodiscard]] bool publish_gpu_preview_presented_surface(const QImage &presented);
    [[nodiscard]] bool publish_gpu_roi_presented_surface(const QImage &presented);
    void clear_thumbnail_presentation_cache();
    void startNextThumbnailPresentation();
    void remember_thumbnail_base(const std::string &asset_id, const QString &base_path,
                                 const ColorProfileState &source_profile,
                                 const QString &thumb_state);

    void show_preview_result(const PreviewResult &preview, std::uint64_t revision,
                             bool preserve_viewport_extent);
    void show_comparison_before_result(const PreviewResult &preview, std::uint64_t revision);

    void refresh_scopes_from_thumbnail(const QString &asset_id);

    [[nodiscard]] LibraryQuery current_query() const;
    struct PreparedCatalogSession
    {
        std::unique_ptr<CatalogService> service;
        std::shared_ptr<PreviewCache> cache;
        std::shared_ptr<std::mutex> recovery_publication_mutex;
    };
    [[nodiscard]] Result<PreparedCatalogSession> make_catalog_service(const std::string &path,
                                                                      bool create);
    void mutate_selected_review(
        const std::function<Result<AssetRecord>(CatalogService &, std::string_view)> &action);
    void apply_cull_review_request(CullReviewFlagAction flag_action, std::optional<int> rating,
                                   std::optional<ColorLabel> color_label, bool auto_advance);
    void remove_selected_from_catalog();
    void remove_selected_from_disk();
    void publish_selection();
    void activate_primary(const QString &asset_id, bool reload_preview);
    [[nodiscard]] std::vector<std::string> selected_asset_ids() const;

    [[nodiscard]] ImportRequest plannedImportRequest() const;
    void refreshImportDestinationPreview();
    void startImportDestinationPreview();

    SerialExecutor executor_;
    SerialExecutor filesystem_executor_;
    std::uint64_t import_roots_generation_ = 0;
    SerialExecutor thumbnail_presentation_executor_;
    CancellationSource thumbnail_presentation_cancel_;
    CancellationSource library_reload_cancel_;
    std::map<std::string, std::function<void()>> pending_thumbnail_presentations_;
    bool thumbnail_presentation_in_flight_ = false;
    std::shared_ptr<const DisplayPresentationState> thumbnail_display_state_;
    std::uint64_t thumbnail_presentation_revision_ = 0;
    std::unordered_map<std::string, std::uint64_t> thumbnail_presentation_revisions_;
    // One cache-miss repair per presentation chain; cleared on success or listing replacement.
    std::unordered_set<std::string> thumbnail_repair_attempts_;
    std::optional<EngineFacade> engine_;
    std::unique_ptr<CatalogService> service_;
    CancellationSource shutdown_;
    CancellationSource thumbnail_work_;
    CancellationSource catalog_operation_;
    CancellationSource import_operation_;
    CancellationSource import_preview_operation_;
    QTimer *catalog_revision_timer_ = nullptr;
    QTimer *backup_schedule_timer_ = nullptr;
    bool catalog_poll_in_flight_ = false;
    std::int64_t observed_catalog_revision_ = -1;
    AssetListModel assets_;
    FolderListModel folders_;
    LibrarySetListModel library_sets_;
    int import_context_row_ = -1;
    std::uint64_t import_context_generation_ = 0;
    QString import_context_path_;
    QString cull_suggestion_filter_{QStringLiteral("none")};
    std::unordered_set<std::string> cull_suggestion_asset_ids_;
    StudioLibraryPresenter library_;
    StudioInspectPresenter inspect_;
    QString catalog_path_;
    QString startup_catalog_path_;
    std::unique_ptr<StudioLibraryResume> library_resume_;
    bool import_work_active_ = false;
    int import_work_completed_ = 0;
    int import_work_total_ = 0;
    std::vector<std::string> pending_import_paths_;
    std::vector<ImportItemResult> import_results_;
    std::size_t import_next_index_ = 0U;
    bool import_gallery_placeholders_ = false;
    bool import_defer_previews_ = false;
    bool import_skip_existing_ = false;
    std::unordered_map<std::string, std::string> pending_import_content_hashes_;
    LibraryQuery import_query_snapshot_;
    bool import_page_open_ = false;
    std::unique_ptr<StudioImportWorkspace> import_workspace_;
    bool import_preview_work_active_ = false;
    std::uint64_t import_generation_ = 0;
    std::uint64_t import_preview_generation_ = 0;
    int import_preview_work_completed_ = 0;
    int import_preview_work_total_ = 0;
    bool import_preflight_active_ = false;
    QString pending_import_destination_;
    QString import_preference_error_;
    bool import_destination_remembered_ = false;
    bool import_recursive_ = true;
    QString import_ingest_transport_{QStringLiteral("filesystem-card")};
    QVariantMap import_native_support_;
    QVariantMap import_ingest_report_;
    QString import_resume_batch_id_;
    std::deque<std::string> pending_import_preview_ids_;
    ImportPreviewPolicy pending_import_preview_policy_ = ImportPreviewPolicy::kStandard;
    std::optional<std::int64_t> last_import_after_unix_ms_;
    std::optional<std::int64_t> last_import_before_unix_ms_;
    std::size_t last_import_count_ = 0U;
    bool last_import_selected_ = false;
    bool preview_work_active_ = false;
    int preview_work_completed_ = 0;
    int preview_work_total_ = 0;
    bool catalog_operation_active_ = false;
    QString catalog_operation_stage_;
    int catalog_operation_completed_ = 0;
    int catalog_operation_total_ = 0;
    int recovery_pending_count_ = 0;
    std::size_t library_total_ = 0U;
    bool library_has_more_ = false;
    bool library_page_in_flight_ = false;
    std::uint64_t library_query_generation_ = 0U;
    std::size_t library_next_offset_ = 0U;
    std::optional<std::size_t> pending_library_page_offset_;
    struct PendingLibrarySelection
    {
        int row = -1;
        std::uint64_t generation = 0;
        QString primary;
        std::unordered_set<std::string> selection;
        QString mode;
        bool open_loupe = false;
    };
    std::optional<PendingLibrarySelection> pending_library_selection_;
    void completePendingLibrarySelection();
    std::optional<CatalogBackupPolicy> backup_policy_;
    bool thumbnail_request_in_flight_ = false;
    std::deque<std::string> pending_thumbnail_ids_;
    QString status_text_{QStringLiteral("Create or open a library to import photos.")};
    QString error_text_;
    QString selected_asset_id_;
    QString selection_anchor_id_;
    std::unordered_set<std::string> selected_ids_;
    QUrl preview_url_;
    QUrl comparison_before_url_;
    QImage preview_image_;
    QImage comparison_before_image_;
    QImage preview_base_image_;
    // UI-thread-owned, unpresented pixels. KiB costs bound bytes and a 1 MiB
    // minimum cost also caps entry/key overhead at 64 recent resources.
    QCache<QString, QImage> decoded_preview_images_{64 * 1024};
    QImage comparison_before_base_image_;
    ColorProfileState preview_output_profile_;
    ColorProfileState comparison_before_output_profile_;
    QPointer<StudioDisplayPresentation> display_presentation_;
    int preview_viewport_width_ = 0;
    int preview_viewport_height_ = 0;
    std::uint64_t live_preview_revision_ = 0;
    std::uint32_t live_preview_width_ = 0;
    std::uint32_t live_preview_height_ = 0;
    QString live_preview_color_profile_id_;
    std::vector<float> preview_mask_alpha_;
    mutable QMutex preview_image_mutex_;
    QString browse_mode_{QStringLiteral("grid")};
    bool collapse_stacks_ = true;
    std::vector<std::string> survey_slot_ids_;
    std::vector<std::string> burst_compare_slot_ids_;
    std::uint64_t burst_compare_generation_ = 0;
    bool burst_compare_request_in_flight_ = false;
    std::uint64_t burst_compare_context_revision_ = 0;
    std::unordered_map<std::string, QUrl> survey_preview_urls_;
    std::deque<std::string> pending_survey_ids_;
    std::unordered_map<std::string, std::uint64_t> survey_preview_requests_;
    bool survey_preview_in_flight_ = false;
    std::uint64_t survey_preview_revision_ = 0;
    QString zoom_mode_{QStringLiteral("fit")};
    double zoom_factor_ = 1.0;
    QString last_non_actual_zoom_mode_{QStringLiteral("fit")};
    double last_non_actual_zoom_factor_ = 1.0;
    int thumbnail_size_ = 180;
    QVariantMap external_editor_session_;
    std::uint64_t photo_merge_token_ = 0;
    std::vector<std::string> photo_merge_assets_;
    QString photo_merge_catalog_;
    QString photo_merge_kind_;
    std::int64_t photo_merge_revision_ = -1;
    QVariantMap offline_edit_media_status_;
    QVariantList offline_edit_proxy_list_;
    QVariantMap selected_ai_proposal_;
    QVariantList ai_proposals_;
    bool busy_ = false;
    bool preview_loading_ = false;
    PreviewRequestOwner inspect_roi_owner_;
    QUrl inspect_roi_url_;
    QImage inspect_roi_image_;
    double inspect_roi_x_ = 0.0;
    double inspect_roi_y_ = 0.0;
    double inspect_roi_width_ = 0.0;
    double inspect_roi_height_ = 0.0;
    quint64 gpu_preview_generation_ = 0;
    quint64 gpu_preview_native_surface_ = 0;
    quint64 gpu_preview_presented_surface_ = 0;
    int gpu_preview_width_ = 0;
    int gpu_preview_height_ = 0;
    quint64 gpu_roi_generation_ = 0;
    quint64 gpu_roi_native_surface_ = 0;
    quint64 gpu_roi_presented_surface_ = 0;
    int gpu_roi_width_ = 0;
    int gpu_roi_height_ = 0;
    void clear_inspect_roi();
    void refresh_inspect_roi();
    std::uint64_t thumbnail_revision_ = 0;
    std::unordered_map<std::string, std::uint64_t> thumbnail_requests_;
    std::unordered_map<std::string, QString> thumbnail_base_paths_;
    std::unordered_map<std::string, ColorProfileState> thumbnail_base_profiles_;
    QString thumbnail_presented_root_;

    // Borrowed session/view slots and executors outlive the edit owner.
    std::unique_ptr<StudioDevelopPresenter> develop_presenter_;
    std::unique_ptr<StudioExportPresenter> export_presenter_;
};

} // namespace ravo
