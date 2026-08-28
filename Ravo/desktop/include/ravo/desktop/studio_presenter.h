#pragma once

#include <cstdint>
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
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/folder_list_model.h"
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
    Q_PROPERTY(bool previewLoading READ previewLoading NOTIFY previewChanged)
    Q_PROPERTY(QString scopeMode READ scopeMode WRITE setScopeMode NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramRed READ scopeHistogramRed NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramGreen READ scopeHistogramGreen NOTIFY scopesChanged)
    Q_PROPERTY(QVariantList scopeHistogramBlue READ scopeHistogramBlue NOTIFY scopesChanged)
    Q_PROPERTY(double scopeHistogramMax READ scopeHistogramMax NOTIFY scopesChanged)
    Q_PROPERTY(QUrl scopeParadeUrl READ scopeParadeUrl NOTIFY scopesChanged)
    Q_PROPERTY(QString browseMode READ browseMode NOTIFY browseModeChanged)
    Q_PROPERTY(QString zoomMode READ zoomMode NOTIFY zoomChanged)
    Q_PROPERTY(double zoomFactor READ zoomFactor NOTIFY zoomChanged)
    Q_PROPERTY(
        int thumbnailSize READ thumbnailSize WRITE setThumbnailSize NOTIFY thumbnailSizeChanged)
    Q_PROPERTY(QString ratingFilterMode READ ratingFilterMode NOTIFY filterChanged)
    Q_PROPERTY(int ratingFilterValue READ ratingFilterValue NOTIFY filterChanged)
    Q_PROPERTY(QStringList colorFilters READ colorFilters NOTIFY filterChanged)
    Q_PROPERTY(QString rejectFilter READ rejectFilter NOTIFY filterChanged)
    Q_PROPERTY(QString sortField READ sortField NOTIFY filterChanged)
    Q_PROPERTY(QString sortDirection READ sortDirection NOTIFY filterChanged)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY filterChanged)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filterChanged)
    Q_PROPERTY(bool selectedHasEdits READ selectedHasEdits NOTIFY selectionChanged)
    Q_PROPERTY(bool beforeAfter READ beforeAfter NOTIFY editChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editWhiteBalance READ editWhiteBalance NOTIFY editChanged)
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
    Q_PROPERTY(double editStraighten READ editStraighten NOTIFY editChanged)
    Q_PROPERTY(QString cropAspect READ cropAspect NOTIFY editChanged)
    Q_PROPERTY(double cropAspectRatio READ cropAspectRatio NOTIFY editChanged)
    Q_PROPERTY(double validCropX READ validCropX NOTIFY editChanged)
    Q_PROPERTY(double validCropY READ validCropY NOTIFY editChanged)
    Q_PROPERTY(double validCropWidth READ validCropWidth NOTIFY editChanged)
    Q_PROPERTY(double validCropHeight READ validCropHeight NOTIFY editChanged)
    Q_PROPERTY(bool cropGuideReady READ cropGuideReady NOTIFY previewChanged)
    Q_PROPERTY(bool editFlipHorizontal READ editFlipHorizontal NOTIFY editChanged)
    Q_PROPERTY(bool editFlipVertical READ editFlipVertical NOTIFY editChanged)
    Q_PROPERTY(double editSharpen READ editSharpen NOTIFY editChanged)
    Q_PROPERTY(double editSharpenRadius READ editSharpenRadius NOTIFY editChanged)
    Q_PROPERTY(double editClarity READ editClarity NOTIFY editChanged)
    Q_PROPERTY(double editVignette READ editVignette NOTIFY editChanged)
    Q_PROPERTY(double editGrain READ editGrain NOTIFY editChanged)
    Q_PROPERTY(double editBloom READ editBloom NOTIFY editChanged)
    Q_PROPERTY(double editSoften READ editSoften NOTIFY editChanged)
    Q_PROPERTY(double editDehaze READ editDehaze NOTIFY editChanged)
    Q_PROPERTY(double editVelvia READ editVelvia NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editLegacyColorBalance READ editLegacyColorBalance NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorChecker READ editColorChecker NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorBalanceRgb READ editColorBalanceRgb NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorCorrection READ editColorCorrection NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editPrimaries READ editPrimaries NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorContrast READ editColorContrast NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editColorHarmonizer READ editColorHarmonizer NOTIFY editChanged)
    Q_PROPERTY(double editMonochrome READ editMonochrome NOTIFY editChanged)
    Q_PROPERTY(double editSplitShadowsHue READ editSplitShadowsHue NOTIFY editChanged)
    Q_PROPERTY(double editSplitHighlightsHue READ editSplitHighlightsHue NOTIFY editChanged)
    Q_PROPERTY(double editSplitBalance READ editSplitBalance NOTIFY editChanged)
    Q_PROPERTY(double editSplitAmount READ editSplitAmount NOTIFY editChanged)
    Q_PROPERTY(double editGamma READ editGamma NOTIFY editChanged)
    Q_PROPERTY(QVariantList editToneCurve READ editToneCurve NOTIFY editChanged)
    Q_PROPERTY(QVariantList editToneCurveSamples READ editToneCurveSamples NOTIFY editChanged)
    Q_PROPERTY(bool editSigmoidEnabled READ editSigmoidEnabled NOTIFY editChanged)
    Q_PROPERTY(double editSigmoidContrast READ editSigmoidContrast NOTIFY editChanged)
    Q_PROPERTY(double editSigmoidSkew READ editSigmoidSkew NOTIFY editChanged)
    Q_PROPERTY(double editSigmoidHuePreservation READ editSigmoidHuePreservation NOTIFY editChanged)
    Q_PROPERTY(double editRawHighlights READ editRawHighlights NOTIFY editChanged)
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
    Q_PROPERTY(QString tagFilter READ tagFilter NOTIFY filterChanged)
    Q_PROPERTY(bool cropToolActive READ cropToolActive NOTIFY editChanged)
    Q_PROPERTY(AssetListModel *assets READ assets CONSTANT)
    Q_PROPERTY(FolderListModel *folders READ folders CONSTANT)
    Q_PROPERTY(QUrl selectedThumbnailUrl READ selectedThumbnailUrl NOTIFY thumbnailsChanged)
    Q_PROPERTY(QString selectedFolderUri READ selectedFolderUri NOTIFY folderChanged)
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
    [[nodiscard]] QImage previewImage() const;
    [[nodiscard]] bool previewLoading() const noexcept;
    [[nodiscard]] QString scopeMode() const;
    void setScopeMode(const QString &mode);
    [[nodiscard]] QVariantList scopeHistogramRed() const;
    [[nodiscard]] QVariantList scopeHistogramGreen() const;
    [[nodiscard]] QVariantList scopeHistogramBlue() const;
    [[nodiscard]] double scopeHistogramMax() const noexcept;
    [[nodiscard]] QUrl scopeParadeUrl() const;
    [[nodiscard]] QImage scopeParadeImage() const;
    [[nodiscard]] QString browseMode() const;
    [[nodiscard]] QString zoomMode() const;
    [[nodiscard]] double zoomFactor() const noexcept;
    [[nodiscard]] int thumbnailSize() const noexcept;
    [[nodiscard]] QString ratingFilterMode() const;
    [[nodiscard]] int ratingFilterValue() const noexcept;
    [[nodiscard]] QStringList colorFilters() const;
    [[nodiscard]] QString rejectFilter() const;
    [[nodiscard]] QString sortField() const;
    [[nodiscard]] QString sortDirection() const;
    [[nodiscard]] int visibleCount() const;
    [[nodiscard]] bool filtersActive() const noexcept;
    [[nodiscard]] bool selectedHasEdits() const noexcept;
    [[nodiscard]] bool beforeAfter() const noexcept;
    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] bool canRedo() const noexcept;
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
    [[nodiscard]] double editStraighten() const noexcept;
    [[nodiscard]] QString cropAspect() const;
    [[nodiscard]] double cropAspectRatio() const noexcept;
    [[nodiscard]] double validCropX() const;
    [[nodiscard]] double validCropY() const;
    [[nodiscard]] double validCropWidth() const;
    [[nodiscard]] double validCropHeight() const;
    [[nodiscard]] bool editFlipHorizontal() const noexcept;
    [[nodiscard]] bool editFlipVertical() const noexcept;
    [[nodiscard]] double editSharpen() const noexcept;
    [[nodiscard]] double editSharpenRadius() const noexcept;
    [[nodiscard]] double editClarity() const noexcept;
    [[nodiscard]] double editVignette() const noexcept;
    [[nodiscard]] double editGrain() const noexcept;
    [[nodiscard]] double editBloom() const noexcept;
    [[nodiscard]] double editSoften() const noexcept;
    [[nodiscard]] double editDehaze() const noexcept;
    [[nodiscard]] double editVelvia() const noexcept;
    [[nodiscard]] QVariantMap editLegacyColorBalance() const;
    [[nodiscard]] QVariantMap editColorChecker() const;
    [[nodiscard]] QVariantMap editColorBalanceRgb() const;
    [[nodiscard]] QVariantMap editColorCorrection() const;
    [[nodiscard]] QVariantMap editPrimaries() const;
    [[nodiscard]] QVariantMap editColorContrast() const;
    [[nodiscard]] QVariantMap editColorHarmonizer() const;
    [[nodiscard]] double editMonochrome() const noexcept;
    [[nodiscard]] double editSplitShadowsHue() const noexcept;
    [[nodiscard]] double editSplitHighlightsHue() const noexcept;
    [[nodiscard]] double editSplitBalance() const noexcept;
    [[nodiscard]] double editSplitAmount() const noexcept;
    [[nodiscard]] double editGamma() const noexcept;
    [[nodiscard]] QVariantList editToneCurve() const;
    [[nodiscard]] QVariantList editToneCurveSamples() const;
    [[nodiscard]] bool editSigmoidEnabled() const noexcept;
    [[nodiscard]] double editSigmoidContrast() const noexcept;
    [[nodiscard]] double editSigmoidSkew() const noexcept;
    [[nodiscard]] double editSigmoidHuePreservation() const noexcept;
    [[nodiscard]] double editRawHighlights() const noexcept;
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
    [[nodiscard]] double editGraduatedDensity() const noexcept;
    [[nodiscard]] double editGraduatedHardness() const noexcept;
    [[nodiscard]] double editGraduatedRotation() const noexcept;
    [[nodiscard]] double editGraduatedOffset() const noexcept;
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
    [[nodiscard]] QString tagFilter() const;
    [[nodiscard]] bool cropToolActive() const noexcept;
    [[nodiscard]] bool cropGuideReady() const noexcept;
    [[nodiscard]] AssetListModel *assets() noexcept;
    [[nodiscard]] FolderListModel *folders() noexcept;
    [[nodiscard]] QUrl selectedThumbnailUrl() const;
    [[nodiscard]] QString selectedFolderUri() const;
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
    Q_INVOKABLE void exportSelectedToPath(const QString &path, const QString &format,
                                          const QVariantMap &options);
    Q_INVOKABLE QVariantList exportFormatChoices() const;
    Q_INVOKABLE QVariantList jpegSubsamplingChoices() const;
    Q_INVOKABLE QVariantList pngBitDepthChoices() const;
    Q_INVOKABLE QVariantList tiffSampleTypeChoices() const;
    Q_INVOKABLE QVariantList tiffCompressionChoices() const;
    Q_INVOKABLE QVariantMap exportDefaultOptions() const;
    Q_INVOKABLE QVariantMap exportOptionBounds() const;
    Q_INVOKABLE void selectAsset(const QString &asset_id);
    Q_INVOKABLE void selectAssetRange(const QString &asset_id);
    Q_INVOKABLE void toggleAssetSelected(const QString &asset_id);
    Q_INVOKABLE void selectNext();
    Q_INVOKABLE void selectPrevious();
    Q_INVOKABLE void setBrowseMode(const QString &mode);
    Q_INVOKABLE void openLoupe();
    Q_INVOKABLE void openDevelop();
    Q_INVOKABLE void returnToGrid();
    Q_INVOKABLE void setDevelopNumber(const QString &name, double value);
    Q_INVOKABLE void previewDevelopNumber(const QString &name, double value);
    void retranslate();
    Q_INVOKABLE void setToneCurve(const QVariantList &points);
    Q_INVOKABLE void previewToneCurve(const QVariantList &points);
    Q_INVOKABLE void setCropRect(double x, double y, double width, double height);
    Q_INVOKABLE void previewCropRect(double x, double y, double width, double height);
    Q_INVOKABLE void setCropAspect(const QString &aspect);
    Q_INVOKABLE void rotateLeft();
    Q_INVOKABLE void rotateRight();
    Q_INVOKABLE void flipHorizontal();
    Q_INVOKABLE void flipVertical();
    Q_INVOKABLE void setCropToolActive(bool active);
    Q_INVOKABLE void resetControl(const QString &name);
    Q_INVOKABLE void resetSection(const QString &section);
    Q_INVOKABLE void resetAllEdits();
    Q_INVOKABLE void undoEdit();
    Q_INVOKABLE void redoEdit();
    Q_INVOKABLE void toggleBeforeAfter();
    Q_INVOKABLE void setZoomMode(const QString &mode);
    Q_INVOKABLE void setZoomFactor(double factor);
    Q_INVOKABLE void adjustZoom(int wheel_delta);
    Q_INVOKABLE void setThumbnailSize(int size);
    Q_INVOKABLE void setAssetTags(const QString &text);
    Q_INVOKABLE void setMetadataField(const QString &name, const QString &value);
    Q_INVOKABLE void createSnapshot(const QString &label);
    Q_INVOKABLE void restoreHistory(int history_id);
    Q_INVOKABLE void setTagFilter(const QString &tag);
    Q_INVOKABLE void setRating(int rating);
    Q_INVOKABLE void setColorLabel(const QString &label);
    Q_INVOKABLE void toggleRejected();
    Q_INVOKABLE void setRatingFilter(const QString &mode, int value);
    Q_INVOKABLE void toggleColorFilter(const QString &label);
    Q_INVOKABLE void setRejectFilter(const QString &mode);
    Q_INVOKABLE void setSort(const QString &field, const QString &direction);
    Q_INVOKABLE void clearFilters();
    Q_INVOKABLE void selectFolder(const QString &folder_uri);
    Q_INVOKABLE void ensureThumbnail(const QString &asset_id);
signals:
    void catalogChanged();
    void busyChanged();
    void statusChanged();
    void errorChanged();
    void selectionChanged();
    void previewChanged();
    void scopesChanged();
    void browseModeChanged();
    void zoomChanged();
    void thumbnailSizeChanged();
    void filterChanged();
    void folderChanged();
    void editChanged();
    void libraryWorkChanged();
    void thumbnailsChanged();

private:
    friend class StudioCommandController;

    void setBusy(bool busy);
    void setStatus(QString text);
    void setError(QString text);
    void applyAssets(std::vector<AssetRecord> assets, bool restore_selection,
                     std::unordered_map<std::string, QUrl> thumbnail_urls = {},
                     std::unordered_map<std::string, QString> thumbnail_states = {});
    void applyFolders(std::vector<FolderRecord> folders);
    void requestPreviewForSelection();
    void reloadVisibleAssets();
    void queuePreviewWarmup();
    void kickPreviewWarmup();
    void setImportWork(int completed, int total, bool active);
    void ingestImportedItem(const ImportItemResult &item);
    void finishPreviewJob(bool success);
    void load_develop_for_selection();
    void commit_develop(DevelopParams params, bool push_history, bool refresh_preview = true);
    void preview_develop(DevelopParams params);
    void enqueue_preview();
    [[nodiscard]] double selected_source_aspect() const;
    [[nodiscard]] double selected_working_aspect() const;
    void constrain_geometry_crop(DevelopParams &params) const;
    void fit_geometry_crop(DevelopParams &params) const;
    void valid_crop_rect(double &x, double &y, double &width, double &height) const;
    void kick_develop_work();
    void clear_displayed_preview();
    void show_preview_result(const PreviewResult &preview, std::uint64_t revision);
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
        std::string asset_id;
        bool ignore_edits = false;
        bool ignore_crop = false;
        bool ignore_straighten = false;
        bool refresh_preview = true;
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
    std::optional<EngineFacade> engine_;
    std::unique_ptr<CatalogService> service_;
    CancellationSource shutdown_;
    AssetListModel assets_;
    FolderListModel folders_;
    LibraryQuery query_;
    QString catalog_path_;
    QString startup_catalog_path_;
    bool import_work_active_ = false;
    int import_work_completed_ = 0;
    int import_work_total_ = 0;
    bool preview_work_active_ = false;
    int preview_work_completed_ = 0;
    int preview_work_total_ = 0;
    bool preview_warmup_in_flight_ = false;
    std::vector<std::string> pending_preview_ids_;
    QString status_text_{QStringLiteral("Create or open a library to import photos.")};
    QString error_text_;
    QString selected_asset_id_;
    QString selection_anchor_id_;
    std::unordered_set<std::string> selected_ids_;
    QUrl preview_url_;
    QImage preview_image_;
    mutable QMutex preview_image_mutex_;
    QString scope_mode_{QStringLiteral("histogram")};
    RgbHistogram scope_histogram_{};
    QImage scope_parade_image_;
    QUrl scope_parade_url_;
    std::uint64_t scope_revision_ = 0;
    QString browse_mode_{QStringLiteral("grid")};
    QString zoom_mode_{QStringLiteral("fit")};
    double zoom_factor_ = 1.0;
    int thumbnail_size_ = 180;
    bool busy_ = false;
    bool preview_loading_ = false;
    PreviewRequestOwner develop_preview_owner_;
    std::uint64_t thumbnail_revision_ = 0;
    std::unordered_map<std::string, std::uint64_t> thumbnail_requests_;
    DevelopParams develop_{};
    DevelopParams saved_develop_{};
    std::vector<DevelopParams> undo_stack_;
    std::vector<DevelopParams> redo_stack_;
    bool before_after_ = false;
    bool crop_tool_active_ = false;
    bool crop_guide_ready_ = false;
    QString crop_aspect_{QStringLiteral("free")};
    bool develop_job_in_flight_ = false;
    std::optional<PendingDevelopWork> pending_save_;
    std::optional<PendingDevelopWork> pending_preview_;
    QVariantList recipe_history_;
};

} // namespace ravo
