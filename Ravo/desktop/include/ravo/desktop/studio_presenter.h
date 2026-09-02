#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/folder_list_model.h"
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/preview_request_owner.h"
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/executor.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

class StudioCommandController;
class StudioLiveSessionController;

class StudioPresenter final : public QObject
{
    Q_OBJECT
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
    Q_PROPERTY(QString selectedImportState READ selectedImportState NOTIFY selectionChanged)
    Q_PROPERTY(bool canDeleteFromDisk READ canDeleteFromDisk NOTIFY selectionChanged)
    Q_PROPERTY(QUrl previewUrl READ previewUrl NOTIFY previewChanged)
    Q_PROPERTY(int previewViewportWidth READ previewViewportWidth NOTIFY previewChanged)
    Q_PROPERTY(int previewViewportHeight READ previewViewportHeight NOTIFY previewChanged)
    Q_PROPERTY(QUrl comparisonBeforeUrl READ comparisonBeforeUrl NOTIFY previewChanged)
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString scopeMode READ scopeMode WRITE setScopeMode NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramRed READ scopeHistogramRed NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramGreen READ scopeHistogramGreen NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramBlue READ scopeHistogramBlue NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramLuma READ scopeHistogramLuma NOTIFY scopesChanged)
    Q_PROPERTY(double scopeHistogramMax READ scopeHistogramMax NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeParadeUrl READ scopeParadeUrl NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeWaveformUrl READ scopeWaveformUrl NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeVectorscopeUrl READ scopeVectorscopeUrl NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeSplitUrl READ scopeSplitUrl NOTIFY scopesChanged)
    Q_PROPERTY(QString browseMode READ browseMode NOTIFY browseModeChanged)
    Q_PROPERTY(bool collapseStacks READ collapseStacks NOTIFY filterChanged)
    Q_PROPERTY(int surveySlotCount READ surveySlotCount NOTIFY surveyChanged)
    Q_PROPERTY(QVariantList surveySlots READ surveySlots NOTIFY surveyChanged)
    Q_PROPERTY(QString zoomMode READ zoomMode NOTIFY zoomChanged)
    Q_PROPERTY(double zoomFactor READ zoomFactor NOTIFY zoomChanged)
    Q_PROPERTY(
        int thumbnailSize READ thumbnailSize WRITE setThumbnailSize NOTIFY thumbnailSizeChanged)
    Q_PROPERTY(QString ratingFilterMode READ ratingFilterMode NOTIFY filterChanged)
    Q_PROPERTY(int ratingFilterValue READ ratingFilterValue NOTIFY filterChanged)
    Q_PROPERTY(QStringList colorFilters READ colorFilters NOTIFY filterChanged)
    Q_PROPERTY(QString rejectFilter READ rejectFilter NOTIFY filterChanged)
    Q_PROPERTY(QString filterText READ filterText NOTIFY filterChanged)
    Q_PROPERTY(QString mediaFilter READ mediaFilter NOTIFY filterChanged)
    Q_PROPERTY(QString editFilter READ editFilter NOTIFY filterChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY filterChanged)
    Q_PROPERTY(QString sortDirection READ sortDirection NOTIFY filterChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY filterChanged)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filterChanged)
    Q_PROPERTY(bool selectedHasEdits READ selectedHasEdits NOTIFY selectionChanged)
    Q_PROPERTY(bool beforeAfter READ beforeAfter NOTIFY editChanged)
    Q_PROPERTY(bool comparisonActive READ comparisonActive NOTIFY editChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editChanged)
    Q_PROPERTY(bool hasCopiedParameters READ hasCopiedParameters NOTIFY copiedParametersChanged)
    Q_PROPERTY(QVariantMap editWhiteBalance READ editWhiteBalance NOTIFY editChanged)
    Q_PROPERTY(bool whiteBalancePickActive READ whiteBalancePickActive NOTIFY editChanged)
    Q_PROPERTY(bool maskPlaceActive READ maskPlaceActive NOTIFY editChanged)
    Q_PROPERTY(bool maskPlaceGeometryAllowed READ maskPlaceGeometryAllowed NOTIFY editChanged)
    Q_PROPERTY(QVariantList editColorEqBands READ editColorEqBands NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editInputColor READ editInputColor NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editProfileGamma READ editProfileGamma NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editOutputColor READ editOutputColor NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerRR READ editChannelMixerRR NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerRG READ editChannelMixerRG NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerRB READ editChannelMixerRB NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerGR READ editChannelMixerGR NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerGG READ editChannelMixerGG NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerGB READ editChannelMixerGB NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerBR READ editChannelMixerBR NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerBG READ editChannelMixerBG NOTIFY editChanged)
    Q_PROPERTY(double editChannelMixerBB READ editChannelMixerBB NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editExposureParams READ editExposureParams NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editExposureMask READ editExposureMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editHighlightsMask READ editHighlightsMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editShadowsMask READ editShadowsMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editWhitesMask READ editWhitesMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editBlacksMask READ editBlacksMask NOTIFY editChanged)
    Q_PROPERTY(double editExposure READ editExposure NOTIFY editChanged)
    Q_PROPERTY(double editContrast READ editContrast NOTIFY editChanged)
    Q_PROPERTY(double editHighlights READ editHighlights NOTIFY editChanged)
    Q_PROPERTY(double editShadows READ editShadows NOTIFY editChanged)
    Q_PROPERTY(double editWhites READ editWhites NOTIFY editChanged)
    Q_PROPERTY(double editBlacks READ editBlacks NOTIFY editChanged)
    Q_PROPERTY(double editVibrance READ editVibrance NOTIFY editChanged)
    Q_PROPERTY(double editSaturation READ editSaturation NOTIFY editChanged)
    Q_PROPERTY(int editRotateQuarters READ editRotateQuarters NOTIFY editChanged)
    Q_PROPERTY(double editCropX READ editCropX NOTIFY editChanged)
    Q_PROPERTY(double editCropY READ editCropY NOTIFY editChanged)
    Q_PROPERTY(double editCropWidth READ editCropWidth NOTIFY editChanged)
    Q_PROPERTY(double editCropHeight READ editCropHeight NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editCanvas READ editCanvas NOTIFY editChanged)
    Q_PROPERTY(bool editCanvasEnabled READ editCanvasEnabled NOTIFY editChanged)
    Q_PROPERTY(double editStraighten READ editStraighten NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editPerspective READ editPerspective NOTIFY editChanged)
    Q_PROPERTY(QString cropAspect READ cropAspect NOTIFY editChanged)
    Q_PROPERTY(double cropAspectRatio READ cropAspectRatio NOTIFY editChanged)
    Q_PROPERTY(int selectedWorkingWidth READ selectedWorkingWidth NOTIFY editChanged)
    Q_PROPERTY(int selectedWorkingHeight READ selectedWorkingHeight NOTIFY editChanged)
    Q_PROPERTY(double cropMinShortEdgePixels READ cropMinShortEdgePixels CONSTANT)
    Q_PROPERTY(double cropMinShortEdgeFraction READ cropMinShortEdgeFraction CONSTANT)
    Q_PROPERTY(double validCropX READ validCropX NOTIFY editChanged)
    Q_PROPERTY(double validCropY READ validCropY NOTIFY editChanged)
    Q_PROPERTY(double validCropWidth READ validCropWidth NOTIFY editChanged)
    Q_PROPERTY(double validCropHeight READ validCropHeight NOTIFY editChanged)
    Q_PROPERTY(bool cropGuideReady READ cropGuideReady NOTIFY previewChanged)
    Q_PROPERTY(bool editFlipHorizontal READ editFlipHorizontal NOTIFY editChanged)
    Q_PROPERTY(bool editFlipVertical READ editFlipVertical NOTIFY editChanged)
    Q_PROPERTY(double editSharpen READ editSharpen NOTIFY editChanged)
    Q_PROPERTY(double editSharpenRadius READ editSharpenRadius NOTIFY editChanged)
    Q_PROPERTY(double editSharpenThreshold READ editSharpenThreshold NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editTexture READ editTexture NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editRetouch READ editRetouch NOTIFY editChanged)
    Q_PROPERTY(double editClarity READ editClarity NOTIFY editChanged)
    Q_PROPERTY(double editVignette READ editVignette NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editVignetteParams READ editVignetteParams NOTIFY editChanged)
    Q_PROPERTY(double editGrain READ editGrain NOTIFY editChanged)
    Q_PROPERTY(double editBloom READ editBloom NOTIFY editChanged)
    Q_PROPERTY(double editSoften READ editSoften NOTIFY editChanged)
    Q_PROPERTY(double editDehaze READ editDehaze NOTIFY editChanged)
    Q_PROPERTY(double editDehazeDistance READ editDehazeDistance NOTIFY editChanged)
    Q_PROPERTY(bool editDehazeAdaptive READ editDehazeAdaptive NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editOutputDither READ editOutputDither NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editOutputFrame READ editOutputFrame NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editWatermark READ editWatermark NOTIFY editChanged)
    Q_PROPERTY(double editVelvia READ editVelvia NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editVelviaParams READ editVelviaParams NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editLut3d READ editLut3d NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editLegacyColorBalance READ editLegacyColorBalance NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorChecker READ editColorChecker NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorBalanceRgb READ editColorBalanceRgb NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorBalanceRgbMask READ editColorBalanceRgbMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorCorrection READ editColorCorrection NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editPrimaries READ editPrimaries NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorContrast READ editColorContrast NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorHarmonizer READ editColorHarmonizer NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorHarmonizerMask READ editColorHarmonizerMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorReconstruction READ editColorReconstruction NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorZones READ editColorZones NOTIFY editChanged)
    Q_PROPERTY(bool maskOverlayVisible READ maskOverlayVisible NOTIFY previewChanged)
    Q_PROPERTY(QString maskOverlayTarget READ maskOverlayTarget NOTIFY previewChanged)
    Q_PROPERTY(double editMonochrome READ editMonochrome NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editMonochromeFilter READ editMonochromeFilter NOTIFY editChanged)
    Q_PROPERTY(double editSplitShadowsHue READ editSplitShadowsHue NOTIFY editChanged)
    Q_PROPERTY(double editSplitHighlightsHue READ editSplitHighlightsHue NOTIFY editChanged)
    Q_PROPERTY(double editSplitBalance READ editSplitBalance NOTIFY editChanged)
    Q_PROPERTY(double editSplitAmount READ editSplitAmount NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editSplitToning READ editSplitToning NOTIFY editChanged)
    Q_PROPERTY(double editGamma READ editGamma NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editRgbLevels READ editRgbLevels NOTIFY editChanged)
    Q_PROPERTY(QVariantList editToneCurve READ editToneCurve NOTIFY editChanged)
    Q_PROPERTY(QVariantList editToneCurveSamples READ editToneCurveSamples NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editCurve READ editCurve NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editRgbCurveMask READ editRgbCurveMask NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editToneCurveMask READ editToneCurveMask NOTIFY editChanged)
    Q_PROPERTY(QVariantList editCurvePoints READ editCurvePoints NOTIFY editChanged)
    Q_PROPERTY(QVariantList editCurveSamples READ editCurveSamples NOTIFY editChanged)
    Q_PROPERTY(bool editSigmoidEnabled READ editSigmoidEnabled NOTIFY editChanged)
    Q_PROPERTY(double editSigmoidContrast READ editSigmoidContrast NOTIFY editChanged)
    Q_PROPERTY(double editSigmoidSkew READ editSigmoidSkew NOTIFY editChanged)
    Q_PROPERTY(double editSigmoidHuePreservation READ editSigmoidHuePreservation NOTIFY editChanged)
    Q_PROPERTY(int editDemosaicModeIndex READ editDemosaicModeIndex NOTIFY editChanged)
    Q_PROPERTY(double editRawHighlights READ editRawHighlights NOTIFY editChanged)
    Q_PROPERTY(double editRawDenoiseThreshold READ editRawDenoiseThreshold NOTIFY editChanged)
    Q_PROPERTY(double editHotPixelsStrength READ editHotPixelsStrength NOTIFY editChanged)
    Q_PROPERTY(double editHotPixelsThreshold READ editHotPixelsThreshold NOTIFY editChanged)
    Q_PROPERTY(bool editHotPixelsPermissive READ editHotPixelsPermissive NOTIFY editChanged)
    Q_PROPERTY(int editRawCaIterations READ editRawCaIterations NOTIFY editChanged)
    Q_PROPERTY(bool editRawCaAvoidShift READ editRawCaAvoidShift NOTIFY editChanged)
    Q_PROPERTY(double editDenoise READ editDenoise NOTIFY editChanged)
    Q_PROPERTY(double editDenoiseChroma READ editDenoiseChroma NOTIFY editChanged)
    Q_PROPERTY(double editDenoiseRadius READ editDenoiseRadius NOTIFY editChanged)
    Q_PROPERTY(double editLensK1 READ editLensK1 NOTIFY editChanged)
    Q_PROPERTY(double editLensVignetting READ editLensVignetting NOTIFY editChanged)
    Q_PROPERTY(double editLensMode READ editLensMode NOTIFY editChanged)
    Q_PROPERTY(int editColorEqBand READ editColorEqBand NOTIFY editChanged)
    Q_PROPERTY(double editColorEqHue READ editColorEqHue NOTIFY editChanged)
    Q_PROPERTY(double editColorEqSat READ editColorEqSat NOTIFY editChanged)
    Q_PROPERTY(double editColorEqLight READ editColorEqLight NOTIFY editChanged)
    Q_PROPERTY(double editGraduatedDensity READ editGraduatedDensity NOTIFY editChanged)
    Q_PROPERTY(double editGraduatedHardness READ editGraduatedHardness NOTIFY editChanged)
    Q_PROPERTY(double editGraduatedRotation READ editGraduatedRotation NOTIFY editChanged)
    Q_PROPERTY(double editGraduatedOffset READ editGraduatedOffset NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editGraduatedMask READ editGraduatedMask NOTIFY editChanged)
    Q_PROPERTY(double editToneEqBlacks READ editToneEqBlacks NOTIFY editChanged)
    Q_PROPERTY(double editToneEqShadows READ editToneEqShadows NOTIFY editChanged)
    Q_PROPERTY(double editToneEqMidtones READ editToneEqMidtones NOTIFY editChanged)
    Q_PROPERTY(double editToneEqHighlights READ editToneEqHighlights NOTIFY editChanged)
    Q_PROPERTY(double editToneEqWhites READ editToneEqWhites NOTIFY editChanged)
    Q_PROPERTY(QString selectedTags READ selectedTags NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedTitle READ selectedTitle NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedDescription READ selectedDescription NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCreator READ selectedCreator NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCopyright READ selectedCopyright NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedCaptureSummary READ selectedCaptureSummary NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList recipeHistory READ recipeHistory NOTIFY editChanged)
    Q_PROPERTY(QVariantList editPresets READ editPresets NOTIFY presetsChanged)
    Q_PROPERTY(
        QVariantList modifiedParameterChoices READ modifiedParameterChoices NOTIFY editChanged)
    Q_PROPERTY(qlonglong activeHistoryId READ activeHistoryId NOTIFY editChanged)
    Q_PROPERTY(qlonglong activeHistorySeq READ activeHistorySeq NOTIFY editChanged)
    Q_PROPERTY(QString tagFilter READ tagFilter NOTIFY filterChanged)
    Q_PROPERTY(bool cropToolActive READ cropToolActive NOTIFY editChanged)
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
    Q_PROPERTY(bool importPageOpen READ importPageOpen NOTIFY importPageChanged)
    Q_PROPERTY(bool importScanActive READ importScanActive NOTIFY importPageChanged)
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
    Q_PROPERTY(ImportCandidateListModel *importCandidates READ importCandidates CONSTANT)

public:
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
    [[nodiscard]] bool importScanActive() const noexcept;
    [[nodiscard]] bool importPreviewWorkActive() const noexcept;
    [[nodiscard]] int importPreviewWorkCompleted() const noexcept;
    [[nodiscard]] int importPreviewWorkTotal() const noexcept;
    [[nodiscard]] QString importSourceRoot() const;
    [[nodiscard]] QString importDestination() const;
    [[nodiscard]] QString importSecondCopyDestination() const;
    [[nodiscard]] QString importFilenameTemplate() const;
    [[nodiscard]] QString importMode() const;
    [[nodiscard]] QString importOrganization() const;
    [[nodiscard]] QString importPreviewPolicy() const;
    [[nodiscard]] bool importRecursive() const noexcept;
    [[nodiscard]] ImportCandidateListModel *importCandidates() noexcept;
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
    [[nodiscard]] QString selectedImportState() const;
    [[nodiscard]] bool canDeleteFromDisk() const;
    [[nodiscard]] QUrl previewUrl() const;
    [[nodiscard]] int previewViewportWidth() const noexcept;
    [[nodiscard]] int previewViewportHeight() const noexcept;
    [[nodiscard]] QImage previewImage() const;
    [[nodiscard]] QUrl comparisonBeforeUrl() const;
    [[nodiscard]] QImage comparisonBeforeImage() const;
    [[nodiscard]] bool previewLoading() const noexcept;
    [[nodiscard]] QString scopeMode() const;
    void setScopeMode(const QString &mode);
    [[nodiscard]] QVariantList scopeHistogramRed() const;
    [[nodiscard]] QVariantList scopeHistogramGreen() const;
    [[nodiscard]] QVariantList scopeHistogramBlue() const;
    [[nodiscard]] QVariantList scopeHistogramLuma() const;
    [[nodiscard]] double scopeHistogramMax() const noexcept;
    [[nodiscard]] QUrl scopeParadeUrl() const;
    [[nodiscard]] QImage scopeParadeImage() const;
    [[nodiscard]] QUrl scopeWaveformUrl() const;
    [[nodiscard]] QImage scopeWaveformImage() const;
    [[nodiscard]] QUrl scopeVectorscopeUrl() const;
    [[nodiscard]] QImage scopeVectorscopeImage() const;
    [[nodiscard]] QUrl scopeSplitUrl() const;
    [[nodiscard]] QImage scopeSplitImage() const;
    [[nodiscard]] QString browseMode() const;
    [[nodiscard]] bool collapseStacks() const noexcept;
    [[nodiscard]] int surveySlotCount() const noexcept;
    [[nodiscard]] QVariantList surveySlots() const;
    [[nodiscard]] QString zoomMode() const;
    [[nodiscard]] double zoomFactor() const noexcept;
    [[nodiscard]] int thumbnailSize() const noexcept;
    [[nodiscard]] QString ratingFilterMode() const;
    [[nodiscard]] int ratingFilterValue() const noexcept;
    [[nodiscard]] QStringList colorFilters() const;
    [[nodiscard]] QString rejectFilter() const;
    [[nodiscard]] QString filterText() const;
    [[nodiscard]] QString mediaFilter() const;
    [[nodiscard]] QString editFilter() const;
    [[nodiscard]] QString sortField() const;
    [[nodiscard]] QString sortDirection() const;
    [[nodiscard]] int visibleCount() const;
    [[nodiscard]] bool filtersActive() const noexcept;
    [[nodiscard]] bool selectedHasEdits() const noexcept;
    [[nodiscard]] bool beforeAfter() const noexcept;
    [[nodiscard]] bool comparisonActive() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
    [[nodiscard]] bool hasCopiedParameters() const noexcept;
    [[nodiscard]] QVariantMap editWhiteBalance() const;
    [[nodiscard]] QVariantMap editInputColor() const;
    [[nodiscard]] QVariantMap editProfileGamma() const;
    [[nodiscard]] QVariantMap editOutputColor() const;
    [[nodiscard]] double editChannelMixerRR() const noexcept;
    [[nodiscard]] double editChannelMixerRG() const noexcept;
    [[nodiscard]] double editChannelMixerRB() const noexcept;
    [[nodiscard]] double editChannelMixerGR() const noexcept;
    [[nodiscard]] double editChannelMixerGG() const noexcept;
    [[nodiscard]] double editChannelMixerGB() const noexcept;
    [[nodiscard]] double editChannelMixerBR() const noexcept;
    [[nodiscard]] double editChannelMixerBG() const noexcept;
    [[nodiscard]] double editChannelMixerBB() const noexcept;
    [[nodiscard]] QVariantMap editExposureParams() const;
    [[nodiscard]] QVariantMap editExposureMask() const;
    [[nodiscard]] QVariantMap editHighlightsMask() const;
    [[nodiscard]] QVariantMap editShadowsMask() const;
    [[nodiscard]] QVariantMap editWhitesMask() const;
    [[nodiscard]] QVariantMap editBlacksMask() const;
    [[nodiscard]] double editExposure() const noexcept;
    [[nodiscard]] double editContrast() const noexcept;
    [[nodiscard]] double editHighlights() const noexcept;
    [[nodiscard]] double editShadows() const noexcept;
    [[nodiscard]] double editWhites() const noexcept;
    [[nodiscard]] double editBlacks() const noexcept;
    [[nodiscard]] double editVibrance() const noexcept;
    [[nodiscard]] double editSaturation() const noexcept;
    [[nodiscard]] int editRotateQuarters() const noexcept;
    [[nodiscard]] double editCropX() const noexcept;
    [[nodiscard]] double editCropY() const noexcept;
    [[nodiscard]] double editCropWidth() const noexcept;
    [[nodiscard]] double editCropHeight() const noexcept;
    [[nodiscard]] QVariantMap editCanvas() const;
    [[nodiscard]] bool editCanvasEnabled() const noexcept;
    [[nodiscard]] double editStraighten() const noexcept;
    [[nodiscard]] QVariantMap editPerspective() const;
    [[nodiscard]] QString cropAspect() const;
    [[nodiscard]] double cropAspectRatio() const noexcept;
    [[nodiscard]] int selectedWorkingWidth() const;
    [[nodiscard]] int selectedWorkingHeight() const;
    [[nodiscard]] double cropMinShortEdgePixels() const noexcept;
    [[nodiscard]] double cropMinShortEdgeFraction() const noexcept;
    [[nodiscard]] double validCropX() const;
    [[nodiscard]] double validCropY() const;
    [[nodiscard]] double validCropWidth() const;
    [[nodiscard]] double validCropHeight() const;
    [[nodiscard]] bool editFlipHorizontal() const noexcept;
    [[nodiscard]] bool editFlipVertical() const noexcept;
    [[nodiscard]] double editSharpen() const noexcept;
    [[nodiscard]] double editSharpenRadius() const noexcept;
    [[nodiscard]] double editSharpenThreshold() const noexcept;
    [[nodiscard]] QVariantMap editTexture() const;
    [[nodiscard]] QVariantMap editRetouch() const;
    [[nodiscard]] double editClarity() const noexcept;
    [[nodiscard]] double editVignette() const noexcept;
    [[nodiscard]] QVariantMap editVignetteParams() const;
    [[nodiscard]] double editGrain() const noexcept;
    [[nodiscard]] double editBloom() const noexcept;
    [[nodiscard]] double editSoften() const noexcept;
    [[nodiscard]] double editDehaze() const noexcept;
    [[nodiscard]] double editDehazeDistance() const noexcept;
    [[nodiscard]] bool editDehazeAdaptive() const noexcept;
    [[nodiscard]] QVariantMap editOutputDither() const;
    [[nodiscard]] QVariantMap editOutputFrame() const;
    [[nodiscard]] QVariantMap editWatermark() const;
    [[nodiscard]] double editVelvia() const noexcept;
    [[nodiscard]] QVariantMap editVelviaParams() const;
    [[nodiscard]] QVariantMap editLut3d() const;
    [[nodiscard]] QVariantMap editLegacyColorBalance() const;
    [[nodiscard]] QVariantMap editColorChecker() const;
    [[nodiscard]] QVariantMap editColorBalanceRgb() const;
    [[nodiscard]] QVariantMap editColorBalanceRgbMask() const;
    [[nodiscard]] QVariantMap editColorCorrection() const;
    [[nodiscard]] QVariantMap editPrimaries() const;
    [[nodiscard]] QVariantMap editColorContrast() const;
    [[nodiscard]] QVariantMap editColorHarmonizer() const;
    [[nodiscard]] QVariantMap editColorHarmonizerMask() const;
    [[nodiscard]] QVariantMap editColorReconstruction() const;
    [[nodiscard]] QVariantMap editColorZones() const;
    [[nodiscard]] double editMonochrome() const noexcept;
    [[nodiscard]] QVariantMap editMonochromeFilter() const;
    [[nodiscard]] double editSplitShadowsHue() const noexcept;
    [[nodiscard]] double editSplitHighlightsHue() const noexcept;
    [[nodiscard]] double editSplitBalance() const noexcept;
    [[nodiscard]] double editSplitAmount() const noexcept;
    [[nodiscard]] QVariantMap editSplitToning() const;
    [[nodiscard]] double editGamma() const noexcept;
    [[nodiscard]] QVariantMap editRgbLevels() const;
    [[nodiscard]] QVariantList editToneCurve() const;
    [[nodiscard]] QVariantList editToneCurveSamples() const;
    [[nodiscard]] QVariantMap editCurve() const;
    [[nodiscard]] QVariantMap editRgbCurveMask() const;
    [[nodiscard]] QVariantMap editToneCurveMask() const;
    [[nodiscard]] QVariantList editCurvePoints() const;
    [[nodiscard]] QVariantList editCurveSamples() const;
    [[nodiscard]] bool editSigmoidEnabled() const noexcept;
    [[nodiscard]] double editSigmoidContrast() const noexcept;
    [[nodiscard]] double editSigmoidSkew() const noexcept;
    [[nodiscard]] double editSigmoidHuePreservation() const noexcept;
    [[nodiscard]] int editDemosaicModeIndex() const noexcept;
    [[nodiscard]] double editRawHighlights() const noexcept;
    [[nodiscard]] double editRawDenoiseThreshold() const noexcept;
    [[nodiscard]] double editHotPixelsStrength() const noexcept;
    [[nodiscard]] double editHotPixelsThreshold() const noexcept;
    [[nodiscard]] bool editHotPixelsPermissive() const noexcept;
    [[nodiscard]] int editRawCaIterations() const noexcept;
    [[nodiscard]] bool editRawCaAvoidShift() const noexcept;
    [[nodiscard]] double editDenoise() const noexcept;
    [[nodiscard]] double editDenoiseChroma() const noexcept;
    [[nodiscard]] double editDenoiseRadius() const noexcept;
    [[nodiscard]] double editLensK1() const noexcept;
    [[nodiscard]] double editLensVignetting() const noexcept;
    [[nodiscard]] double editLensMode() const noexcept;
    [[nodiscard]] int editColorEqBand() const noexcept;
    [[nodiscard]] double editColorEqHue() const noexcept;
    [[nodiscard]] double editColorEqSat() const noexcept;
    [[nodiscard]] double editColorEqLight() const noexcept;
    [[nodiscard]] QVariantList editColorEqBands() const;
    [[nodiscard]] double editGraduatedDensity() const noexcept;
    [[nodiscard]] double editGraduatedHardness() const noexcept;
    [[nodiscard]] double editGraduatedRotation() const noexcept;
    [[nodiscard]] double editGraduatedOffset() const noexcept;
    [[nodiscard]] QVariantMap editGraduatedMask() const;
    [[nodiscard]] double editToneEqBlacks() const noexcept;
    [[nodiscard]] double editToneEqShadows() const noexcept;
    [[nodiscard]] double editToneEqMidtones() const noexcept;
    [[nodiscard]] double editToneEqHighlights() const noexcept;
    [[nodiscard]] double editToneEqWhites() const noexcept;
    [[nodiscard]] QString selectedTags() const;
    [[nodiscard]] QString selectedTitle() const;
    [[nodiscard]] QString selectedDescription() const;
    [[nodiscard]] QString selectedCreator() const;
    [[nodiscard]] QString selectedCopyright() const;
    [[nodiscard]] QString selectedCaptureSummary() const;
    [[nodiscard]] QVariantList recipeHistory() const;
    [[nodiscard]] qlonglong activeHistoryId() const noexcept;
    [[nodiscard]] qlonglong activeHistorySeq() const noexcept;
    [[nodiscard]] QString tagFilter() const;
    [[nodiscard]] bool cropToolActive() const noexcept;
    [[nodiscard]] bool cropGuideReady() const noexcept;
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
    Q_INVOKABLE void closeImportPage();
    Q_INVOKABLE void setImportSourceRoot(const QString &path);
    Q_INVOKABLE void setImportDestination(const QString &path);
    Q_INVOKABLE void setImportSecondCopyDestination(const QString &path);
    Q_INVOKABLE void setImportFilenameTemplate(const QString &filename_template);
    Q_INVOKABLE void setImportMode(const QString &mode);
    Q_INVOKABLE void setImportOrganization(const QString &organization);
    Q_INVOKABLE void setImportPreviewPolicy(const QString &policy);
    Q_INVOKABLE void setImportRecursive(bool recursive);
    Q_INVOKABLE void ensureImportThumbnail(int row);
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
    Q_INVOKABLE void exportSelectedToPath(const QString &path, const QString &format,
                                          const QVariantMap &options);
    Q_INVOKABLE void exportSelectedToDirectory(const QString &directory,
                                               const QString &filename_template,
                                               const QString &format, const QVariantMap &options);
    Q_INVOKABLE QVariantList exportFormatChoices() const;
    Q_INVOKABLE QVariantList jpegSubsamplingChoices() const;
    Q_INVOKABLE QVariantList pngBitDepthChoices() const;
    Q_INVOKABLE QVariantList tiffSampleTypeChoices() const;
    Q_INVOKABLE QVariantList tiffCompressionChoices() const;
    Q_INVOKABLE QVariantList exportMetadataModeChoices() const;
    Q_INVOKABLE QVariantMap exportDefaultOptions() const;
    Q_INVOKABLE QVariantMap exportOptionBounds() const;
    Q_INVOKABLE void selectAsset(const QString &asset_id);
    Q_INVOKABLE void selectAssetRange(const QString &asset_id);
    Q_INVOKABLE void toggleAssetSelected(const QString &asset_id);
    Q_INVOKABLE void selectAllVisible();
    Q_INVOKABLE void selectNext();
    Q_INVOKABLE void selectPrevious();
    Q_INVOKABLE void setBrowseMode(const QString &mode);
    Q_INVOKABLE void openLoupe();
    Q_INVOKABLE void openDevelop();
    Q_INVOKABLE void openSurvey();
    Q_INVOKABLE void returnToGrid();
    Q_INVOKABLE void selectSurveySlot(const QString &asset_id);
    Q_INVOKABLE void createAssetVersion();
    Q_INVOKABLE void stackSelection();
    Q_INVOKABLE void unstackSelection();
    Q_INVOKABLE void setSelectedStackPick();
    Q_INVOKABLE void setCollapseStacks(bool collapse);
    Q_INVOKABLE void setDevelopNumber(const QString &name, double value);
    Q_INVOKABLE void setDevelopText(const QString &name, const QString &value);
    Q_INVOKABLE void addRetouchRegion(const QVariantMap &region);
    Q_INVOKABLE void removeRetouchRegion(int index);
    Q_INVOKABLE void previewDevelopNumber(const QString &name, double value);
    [[nodiscard]] bool maskOverlayVisible() const noexcept;
    [[nodiscard]] QString maskOverlayTarget() const;
    Q_INVOKABLE void setMaskOverlay(const QString &target, bool visible);
    [[nodiscard]] bool maskPlaceActive() const noexcept;
    [[nodiscard]] bool maskPlaceGeometryAllowed() const noexcept;
    Q_INVOKABLE void setMaskPlaceActive(bool active);
    Q_INVOKABLE void placeMask(double preview_x, double preview_y);
    void retranslate();
    Q_INVOKABLE void setToneCurve(const QVariantList &points);
    Q_INVOKABLE void previewToneCurve(const QVariantList &points);
    Q_INVOKABLE void setCurveFamily(int family);
    Q_INVOKABLE void setCurveChannel(int channel);
    Q_INVOKABLE void setCurvePoints(const QString &family, int channel, const QVariantList &points);
    Q_INVOKABLE void previewCurvePoints(const QString &family, int channel,
                                        const QVariantList &points);
    Q_INVOKABLE void setCropRect(double x, double y, double width, double height);
    Q_INVOKABLE void previewCropRect(double x, double y, double width, double height);
    Q_INVOKABLE void setCropAspect(const QString &aspect);
    Q_INVOKABLE void rotateLeft();
    Q_INVOKABLE void rotateRight();
    Q_INVOKABLE void flipHorizontal();
    Q_INVOKABLE void flipVertical();
    Q_INVOKABLE void setCropToolActive(bool active);
    [[nodiscard]] bool whiteBalancePickActive() const noexcept;
    Q_INVOKABLE void setWhiteBalancePickActive(bool active);
    Q_INVOKABLE void pickWhiteBalance(double preview_x, double preview_y);
    Q_INVOKABLE void autoPerspective(const QString &mode);
    Q_INVOKABLE void resetControl(const QString &name);
    Q_INVOKABLE void resetSection(const QString &section);
    Q_INVOKABLE bool sectionModified(const QString &section) const;
    Q_INVOKABLE bool sectionEffectEnabled(const QString &section) const;
    Q_INVOKABLE void setSectionEffectEnabled(const QString &section, bool enabled);
    Q_INVOKABLE void resetAllEdits();
    Q_INVOKABLE void copyParametersSelected(const QVariantList &fields);
    Q_INVOKABLE void pasteParameters();
    Q_INVOKABLE void pasteParametersToSelection();
    Q_INVOKABLE void previewDevelopNumbers(const QVariantMap &fields);
    Q_INVOKABLE void setDevelopNumbers(const QVariantMap &fields);
    Q_INVOKABLE void undoEdit();
    Q_INVOKABLE void redoEdit();
    Q_INVOKABLE void toggleBeforeAfter();
    Q_INVOKABLE void toggleComparison();
    Q_INVOKABLE void setZoomMode(const QString &mode);
    Q_INVOKABLE void setZoomFactor(double factor);
    Q_INVOKABLE void adjustZoom(int wheel_delta);
    Q_INVOKABLE void toggleActualSize();
    Q_INVOKABLE void setThumbnailSize(int size);
    Q_INVOKABLE void setAssetTags(const QString &text);
    Q_INVOKABLE void setMetadataField(const QString &name, const QString &value);
    Q_INVOKABLE void refreshSelectedMetadata();
    Q_INVOKABLE void saveStyleToPath(const QString &path);
    Q_INVOKABLE void applyStyleFromPath(const QString &path);
    [[nodiscard]] QVariantList editPresets() const;
    [[nodiscard]] QVariantList modifiedParameterChoices() const;
    Q_INVOKABLE void savePreset(const QString &name, const QVariantList &fields);
    Q_INVOKABLE void importPresetFromPath(const QString &path);
    Q_INVOKABLE void renamePreset(const QString &path, const QString &name);
    Q_INVOKABLE void deletePreset(const QString &path);
    [[nodiscard]] QString selectedPhotoDebugInfo() const;
    [[nodiscard]] QString selectedPhotoParametersDebugInfo() const;
    [[nodiscard]] QString presetDebugInfo(const QString &path) const;
    Q_INVOKABLE void copySelectedPhotoDebugInfo();
    Q_INVOKABLE void copySelectedPhotoParametersDebugInfo();
    Q_INVOKABLE void revealSelectedPhotoInFileManager();
    Q_INVOKABLE void copyPresetDebugInfo(const QString &path);
    Q_INVOKABLE void createSnapshot(const QString &label);
    Q_INVOKABLE void renameSnapshot(int history_id, const QString &label);
    Q_INVOKABLE void restoreHistory(int history_id);
    Q_INVOKABLE void setTagFilter(const QString &tag);
    Q_INVOKABLE void setRating(int rating);
    Q_INVOKABLE void setColorLabel(const QString &label);
    Q_INVOKABLE void toggleRejected();
    Q_INVOKABLE void setRatingFilter(const QString &mode, int value);
    Q_INVOKABLE void toggleColorFilter(const QString &label);
    Q_INVOKABLE void setRejectFilter(const QString &mode);
    Q_INVOKABLE void setFilterText(const QString &text);
    Q_INVOKABLE void setMediaFilter(const QString &mode);
    Q_INVOKABLE void setEditFilter(const QString &mode);
    Q_INVOKABLE void setSort(const QString &field, const QString &direction);
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
    void catalogChanged();
    void busyChanged();
    void statusChanged();
    void errorChanged();
    void selectionChanged();
    void previewChanged();
    void previewIdentityChanged();
    void interactivePreviewPublished(qulonglong revision, qlonglong intentToImageMicroseconds);
    void scopesChanged();
    void browseModeChanged();
    void surveyChanged();
    void zoomChanged();
    void thumbnailSizeChanged();
    void filterChanged();
    void folderChanged();
    void editChanged();
    void copiedParametersChanged();
    void presetsChanged();
    void libraryWorkChanged();
    void thumbnailsChanged();
    void importPageChanged();

private:
    friend class StudioCommandController;
    friend class StudioLiveSessionController;

    void setBusy(bool busy);
    void setStatus(QString text);
    void setError(QString text);
    void applyAssets(std::vector<AssetRecord> assets, bool restore_selection,
                     std::unordered_map<std::string, QUrl> thumbnail_urls = {},
                     std::unordered_map<std::string, QString> thumbnail_states = {},
                     std::size_t total = 0U, bool has_more = false);
    void applyFolders(std::vector<FolderRecord> folders);
    void applyLibrarySets(std::vector<LibrarySetRecord> sets);
    void clearLastImportQuery();
    void requestPreviewForSelection();
    void requestSurveyPreviews();
    void startSurveyPreviewRequest(std::string asset_id);
    void finishSurveyPreviewRequest(bool success);
    void rebuild_survey_slots();
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
    void finishImportBatch();
    void setCatalogOperation(QString stage, int completed, int total, bool active);
    void startPreviewRebuild(std::vector<std::string> asset_ids, std::size_t expected_total);
    void startScheduledBackup(bool force);
    void finishThumbnailRequest(bool success);
    void rescanImportSource();
    void kickImportCandidateWork();
    void startImportCandidateWork(int row);
    void startNextImportPreview();
    void load_develop_for_selection();
    void apply_recipe_history(const std::vector<RecipeHistoryEntry> &entries);
    void reload_recipe_history();
    void reload_presets();
    [[nodiscard]] QString presets_directory() const;
    void sync_active_history();
    [[nodiscard]] DevelopParams baseline_develop() const;
    [[nodiscard]] DevelopParams develop_from_history_entry(const RecipeHistoryEntry &entry) const;
    enum class DevelopEdit : std::uint8_t
    {
        Preview,
        Overlay,
        Commit,
        Restore,
        Revert
    };
    bool mutate_develop(DevelopParams next, DevelopEdit edit, bool refresh_preview = true,
                        std::optional<std::string> history_coalesce_key = {});
    void applyDevelopNumbers(const QVariantMap &fields, DevelopEdit edit);
    void commit_develop(DevelopParams params, bool push_history, bool refresh_preview = true,
                        RecipeHistoryWrite history_write = RecipeHistoryWrite::kAppendIfNew,
                        std::optional<std::string> history_coalesce_key = {});
    void preview_develop(DevelopParams params);
    void break_history_coalescing();
    void enqueue_preview();
    void request_comparison_before();
    [[nodiscard]] double selected_source_aspect() const;
    [[nodiscard]] double selected_working_aspect() const;
    [[nodiscard]] bool working_source_size(double &width, double &height) const;
    void clamp_selected_crop(DevelopParams &params) const;
    void constrain_geometry_crop(DevelopParams &params) const;
    void fit_geometry_crop(DevelopParams &params) const;
    void valid_crop_rect(double &x, double &y, double &width, double &height) const;
    [[nodiscard]] std::optional<std::string>
    current_overlay_mask_id(const DevelopParams &params) const;
    void kick_develop_work();
    void clear_displayed_preview();
    [[nodiscard]] bool clear_comparison();
    void show_preview_result(const PreviewResult &preview, std::uint64_t revision,
                             bool preserve_viewport_extent);
    void show_comparison_before_result(const PreviewResult &preview, std::uint64_t revision);
    void schedule_preview_analysis(const QImage &identity_image, const QImage &scope_image,
                                   std::uint64_t preview_revision, const std::string &asset_id,
                                   const QString &profile_id);
    void drain_preview_analysis();
    void cancel_preview_analysis(std::string reason);
    void refresh_scopes(const QImage &image);
    void refresh_scopes_from_thumbnail(const QString &asset_id);
    void clear_scopes();
    [[nodiscard]] static QVariantList
    histogram_channel_list(const std::array<std::uint32_t, kRgbHistogramBins> &channel);
    struct PendingDevelopWork
    {
        bool save = false;
        bool interactive = false;
        DevelopParams params{};
        DevelopParams previous{};
        bool push_history = false;
        bool pushed_undo = false;
        RecipeHistoryWrite history_write = RecipeHistoryWrite::kAppendIfNew;
        std::optional<std::int64_t> discard_history_after_seq;
        std::optional<std::string> history_coalesce_key;
        std::optional<std::int64_t> coalesce_history_id;
        std::string asset_id;
        bool ignore_edits = false;
        bool ignore_crop = false;
        bool ignore_straighten = false;
        bool refresh_preview = true;
        bool settle_preview = false;
        bool comparison_before = false;
        std::optional<std::string> overlay_mask_id;
        std::optional<std::uint64_t> request_revision;
        std::chrono::steady_clock::time_point intent_started_at{};
    };
    [[nodiscard]] LibraryQuery current_query() const;
    [[nodiscard]] Result<std::unique_ptr<CatalogService>>
    make_catalog_service(const std::string &path, bool create);
    void mutate_selected_review(
        const std::function<Result<AssetRecord>(CatalogService &, std::string_view)> &action);
    void remove_selected_from_catalog();
    void remove_selected_from_disk();
    void publish_selection();
    void activate_primary(const QString &asset_id, bool reload_preview);
    [[nodiscard]] std::vector<std::string> selected_asset_ids() const;

    SerialExecutor executor_;
    SerialExecutor preview_analysis_executor_;
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
    ImportCandidateListModel import_candidates_;
    LibraryQuery query_;
    QString catalog_path_;
    QVariantList develop_presets_;
    QString startup_catalog_path_;
    bool import_work_active_ = false;
    int import_work_completed_ = 0;
    int import_work_total_ = 0;
    std::vector<std::string> pending_import_paths_;
    std::vector<ImportItemResult> import_results_;
    std::size_t import_next_index_ = 0U;
    bool import_gallery_placeholders_ = false;
    bool import_defer_previews_ = false;
    LibraryQuery import_query_snapshot_;
    bool import_page_open_ = false;
    bool import_scan_active_ = false;
    bool import_preview_work_active_ = false;
    int import_preview_work_completed_ = 0;
    int import_preview_work_total_ = 0;
    QString import_source_root_;
    QString import_destination_;
    QString import_second_copy_destination_;
    QString import_filename_template_;
    QString import_mode_{QStringLiteral("add")};
    QString import_organization_{QStringLiteral("single")};
    QString import_preview_policy_{QStringLiteral("standard")};
    bool import_recursive_ = true;
    std::uint64_t import_scan_generation_ = 0U;
    std::deque<int> pending_import_thumbnail_rows_;
    bool import_candidate_work_in_flight_ = false;
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
    int preview_viewport_width_ = 0;
    int preview_viewport_height_ = 0;
    std::uint64_t live_preview_revision_ = 0;
    std::uint32_t live_preview_width_ = 0;
    std::uint32_t live_preview_height_ = 0;
    QString live_preview_color_profile_id_;
    QString live_preview_pixel_sha256_;
    bool preview_identity_pending_ = false;
    std::vector<float> preview_mask_alpha_;
    bool mask_overlay_visible_ = false;
    QString mask_overlay_target_{QStringLiteral("color_harmonizer")};
    mutable QMutex preview_image_mutex_;
    QMutex preview_analysis_queue_mutex_;
    std::optional<std::function<void()>> pending_preview_analysis_;
    bool preview_analysis_worker_active_ = false;
    QString scope_mode_{QStringLiteral("parade")};
    RgbHistogram scope_histogram_{};
    QImage scope_parade_image_;
    QUrl scope_parade_url_;
    QImage scope_waveform_image_;
    QUrl scope_waveform_url_;
    QImage scope_vectorscope_image_;
    QUrl scope_vectorscope_url_;
    QImage scope_split_image_;
    QUrl scope_split_url_;
    std::uint64_t scope_revision_ = 0;
    QString browse_mode_{QStringLiteral("grid")};
    bool collapse_stacks_ = true;
    std::vector<std::string> survey_slot_ids_;
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
    bool busy_ = false;
    bool preview_loading_ = false;
    PreviewRequestOwner develop_preview_owner_;
    PreviewRequestOwner preview_analysis_owner_;
    PreviewRequestOwner perspective_analysis_owner_;
    std::uint64_t thumbnail_revision_ = 0;
    std::unordered_map<std::string, std::uint64_t> thumbnail_requests_;
    void apply_curve_points(const QString &family, int channel, const QVariantList &points,
                            DevelopEdit edit);
    void sync_curve_ui_from_develop();

    DevelopParams develop_{};
    bool develop_loaded_ = false;
    QString develop_load_error_;
    int curve_family_ = 0;
    int curve_channel_ = 0;
    DevelopParams saved_develop_{};
    std::optional<DevelopParams> displayed_develop_;
    std::vector<DevelopParams> undo_stack_;
    std::vector<DevelopParams> redo_stack_;
    struct CopiedDevelopParameters
    {
        DevelopParams source;
        std::vector<std::string> fields;
    };
    std::optional<CopiedDevelopParameters> copied_parameters_;
    bool before_after_ = false;
    bool comparison_active_ = false;
    bool comparison_before_requested_ = false;
    bool crop_tool_active_ = false;
    bool white_balance_pick_active_ = false;
    bool mask_place_active_ = false;
    bool crop_guide_ready_ = false;
    QString crop_aspect_{QStringLiteral("free")};
    double locked_crop_ratio_ = 0.0;
    bool develop_job_in_flight_ = false;
    bool develop_interactive_job_in_flight_ = false;
    std::optional<PendingDevelopWork> pending_save_;
    std::optional<PendingDevelopWork> pending_preview_;
    QVariantList recipe_history_;
    std::vector<RecipeHistoryEntry> recipe_history_entries_;
    std::int64_t active_history_id_ = 0;
    std::int64_t active_history_seq_ = 0;
    std::optional<std::string> history_coalesce_key_;
    std::optional<std::int64_t> history_coalesce_id_;
};

} // namespace ravo
