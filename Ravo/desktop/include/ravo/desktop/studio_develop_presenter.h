#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QImage>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/preview_request_owner.h"
#include "ravo/desktop/studio_inspect_presenter.h"
#include "ravo/engine/engine.h"
#include "ravo/engine/mask_geometry.h"
#include "ravo/foundation/executor.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/local_adjustment.h"
#include "ravo/services/develop_service.h"

namespace ravo
{
class StudioPresenter;
class PreviewService;
class RecoveryService;
namespace testing
{
class StudioPipelineTestControl;
}

// One GUI-thread edit owner. Session/view references are read-only observations;
// queued work borrows the existing executor and specific worker-thread services.
class StudioDevelopPresenter final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool beforeAfter READ beforeAfter NOTIFY editChanged)
    Q_PROPERTY(bool comparisonActive READ comparisonActive NOTIFY editChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY editChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY editChanged)
    Q_PROPERTY(bool hasCopiedParameters READ hasCopiedParameters NOTIFY copiedParametersChanged)
    Q_PROPERTY(QVariantMap editWhiteBalance READ editWhiteBalance NOTIFY editChanged)
    Q_PROPERTY(QString activeLocalId READ activeLocalId NOTIFY editingScopeChanged)
    Q_PROPERTY(bool localEditing READ localEditing NOTIFY editingScopeChanged)
    Q_PROPERTY(QVariantList localAdjustments READ localAdjustments NOTIFY editChanged)
    Q_PROPERTY(QVariantMap editLocalMask READ editLocalMask NOTIFY editChanged)
    Q_PROPERTY(bool localDonePending READ localDonePending NOTIFY editChanged)
    Q_PROPERTY(bool maskDrawingActive READ maskDrawingActive NOTIFY editChanged)
    Q_PROPERTY(QVariantList localMaskHandles READ localMaskHandles NOTIFY editChanged)
    Q_PROPERTY(bool whiteBalancePickActive READ whiteBalancePickActive NOTIFY editChanged)
    Q_PROPERTY(bool maskPlaceActive READ maskPlaceActive NOTIFY editChanged)
    Q_PROPERTY(bool maskPlaceGeometryAllowed READ maskPlaceGeometryAllowed NOTIFY editChanged)
    Q_PROPERTY(bool maskParametricAssistActive READ maskParametricAssistActive NOTIFY editChanged)
    Q_PROPERTY(bool maskParametricAssistAllowed READ maskParametricAssistAllowed NOTIFY editChanged)
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
    Q_PROPERTY(QVariantList exposureInstances READ exposureInstances NOTIFY editChanged)
    Q_PROPERTY(
        QString selectedExposureInstanceId READ selectedExposureInstanceId NOTIFY editChanged)
    Q_PROPERTY(
        QVariantList colorBalanceRgbInstances READ colorBalanceRgbInstances NOTIFY editChanged)
    Q_PROPERTY(QString selectedColorBalanceRgbInstanceId READ selectedColorBalanceRgbInstanceId
                   NOTIFY editChanged)
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
    Q_PROPERTY(bool cropGuideReady READ cropGuideReady NOTIFY frameProjectionChanged)
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
    Q_PROPERTY(bool maskOverlayVisible READ maskOverlayVisible NOTIFY frameProjectionChanged)
    Q_PROPERTY(QString maskOverlayTarget READ maskOverlayTarget NOTIFY frameProjectionChanged)
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
    Q_PROPERTY(bool editRapidRawToneControlsEnabled READ editRapidRawToneControlsEnabled NOTIFY
                   editChanged)
    Q_PROPERTY(
        bool editRapidRawBasicToneEnabled READ editRapidRawBasicToneEnabled NOTIFY editChanged)
    Q_PROPERTY(int editToneMapperIndex READ editToneMapperIndex NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawEvShift READ editRapidRawEvShift NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawExposure READ editRapidRawExposure NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawContrast READ editRapidRawContrast NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawHighlights READ editRapidRawHighlights NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawShadows READ editRapidRawShadows NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawWhites READ editRapidRawWhites NOTIFY editChanged)
    Q_PROPERTY(double editRapidRawBlacks READ editRapidRawBlacks NOTIFY editChanged)
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
    Q_PROPERTY(QVariantList recipeHistory READ recipeHistory NOTIFY editChanged)
    Q_PROPERTY(QVariantList editPresets READ editPresets NOTIFY presetsChanged)
    Q_PROPERTY(
        QVariantList modifiedParameterChoices READ modifiedParameterChoices NOTIFY editChanged)
    Q_PROPERTY(qlonglong activeHistoryId READ activeHistoryId NOTIFY editChanged)
    Q_PROPERTY(qlonglong activeHistorySeq READ activeHistorySeq NOTIFY editChanged)
    Q_PROPERTY(bool cropToolActive READ cropToolActive NOTIFY editChanged)

public:
    StudioDevelopPresenter(const StudioDevelopPresenter &) = delete;
    StudioDevelopPresenter &operator=(const StudioDevelopPresenter &) = delete;
    enum class DevelopEdit : std::uint8_t
    {
        Preview,
        Overlay,
        Commit,
        Restore,
        Revert
    };
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
        bool prefer_cached_settled_preview = false;
        bool comparison_before = false;
        std::optional<std::string> overlay_mask_id;
        std::optional<std::uint64_t> request_revision;
        std::chrono::steady_clock::time_point intent_started_at{};
        std::optional<AssetDescriptor> expected_source;
        std::optional<std::int64_t> expected_history_head;
    };
    struct State
    {
        DevelopParams develop_{};
        QString active_local_id_;
        DevelopParams local_projection_;
        bool local_done_pending_ = false;
        bool mask_drawing_active_ = false;
        bool mask_gesture_updating_ = false;
        QString mask_gesture_token_;
        QString mask_gesture_handle_;
        QString mask_gesture_scope_;
        QString mask_gesture_asset_;
        std::optional<DevelopParams> mask_gesture_before_;
        std::optional<DevelopParams> local_creation_before_;
        DevelopParams mask_gesture_local_;
        std::vector<LocalMaskPoint> mask_gesture_points_;
        std::optional<MaskGeometryMapping> mask_gesture_mapping_;
        std::size_t selected_exposure_instance_index_ = 0;
        std::size_t selected_color_balance_rgb_instance_index_ = 0;
        std::optional<DevelopExposureInstance> exposure_front_restore_;
        std::optional<DevelopColorBalanceRgbInstance> color_balance_rgb_front_restore_;
        bool develop_loaded_ = false;
        bool develop_preview_deferred_ = false;
        QString develop_load_error_;
        int curve_family_ = 0;
        int curve_channel_ = 0;
        DevelopParams saved_develop_{};
        std::optional<AssetDescriptor> loaded_recipe_asset_;
        // Durable head, updated with save results; independent of async history rows.
        std::int64_t loaded_recipe_history_head_ = 0;
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
        bool mask_parametric_assist_active_ = false;
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
        QVariantList develop_presets_;
        PreviewRequestOwner develop_preview_owner_;
        PreviewRequestOwner perspective_analysis_owner_;
        bool mask_overlay_visible_ = false;
        QString mask_overlay_target_{QStringLiteral("color_harmonizer")};
    };
    // GUI-thread borrowed view, valid until this presenter is destroyed. Do not
    // retain references into replaced recipe/history/pending values.
    [[nodiscard]] const State &state() const noexcept;
    void shutdown();
    void selectionInvalidated();
    void acceptLoadedHistory(const Result<std::vector<RecipeHistoryEntry>> &history);
    void acceptExternalRecipe(const Recipe &recipe, DevelopParams params);
    void setCropGuideReady(bool ready);
    void refreshContextProjection();
    [[nodiscard]] bool liveBatchBusy() const noexcept;
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
    [[nodiscard]] bool editRapidRawToneControlsEnabled() const noexcept;
    [[nodiscard]] bool editRapidRawBasicToneEnabled() const noexcept;
    [[nodiscard]] int editToneMapperIndex() const noexcept;
    [[nodiscard]] double editRapidRawEvShift() const noexcept;
    [[nodiscard]] double editRapidRawExposure() const noexcept;
    [[nodiscard]] double editRapidRawContrast() const noexcept;
    [[nodiscard]] double editRapidRawHighlights() const noexcept;
    [[nodiscard]] double editRapidRawShadows() const noexcept;
    [[nodiscard]] double editRapidRawWhites() const noexcept;
    [[nodiscard]] double editRapidRawBlacks() const noexcept;
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
    [[nodiscard]] QVariantList recipeHistory() const;
    [[nodiscard]] qlonglong activeHistoryId() const noexcept;
    [[nodiscard]] qlonglong activeHistorySeq() const noexcept;
    [[nodiscard]] bool cropToolActive() const noexcept;
    [[nodiscard]] bool cropGuideReady() const noexcept;
    Q_INVOKABLE void setDevelopNumber(const QString &name, double value);
    [[nodiscard]] QString activeLocalId() const;
    [[nodiscard]] bool localEditing() const noexcept;
    [[nodiscard]] QVariantList localAdjustments() const;
    [[nodiscard]] QVariantMap editLocalMask() const;
    [[nodiscard]] bool localDonePending() const noexcept;
    [[nodiscard]] bool maskDrawingActive() const noexcept;
    [[nodiscard]] QVariantList localMaskHandles() const;
    [[nodiscard]] Result<bool> applyLocalAdjustmentCommand(const QString &action,
                                                           const QVariantMap &arguments);
    [[nodiscard]] QVariantList exposureInstances() const;
    [[nodiscard]] QString selectedExposureInstanceId() const;
    [[nodiscard]] QVariantList colorBalanceRgbInstances() const;
    [[nodiscard]] QString selectedColorBalanceRgbInstanceId() const;
    Q_INVOKABLE void selectExposureInstance(const QString &instanceId);
    Q_INVOKABLE void selectColorBalanceRgbInstance(const QString &instanceId);
    Q_INVOKABLE void addExposureInstance();
    Q_INVOKABLE void addColorBalanceRgbInstance();
    Q_INVOKABLE void duplicateExposureInstance();
    Q_INVOKABLE void duplicateColorBalanceRgbInstance();
    Q_INVOKABLE void deleteExposureInstance(const QString &instanceId);
    Q_INVOKABLE void deleteColorBalanceRgbInstance(const QString &instanceId);
    Q_INVOKABLE void renameExposureInstance(const QString &instanceId, const QString &name);
    Q_INVOKABLE void renameColorBalanceRgbInstance(const QString &instanceId, const QString &name);
    Q_INVOKABLE void setExposureInstanceBypass(const QString &instanceId, bool bypass);
    Q_INVOKABLE void setColorBalanceRgbInstanceBypass(const QString &instanceId, bool bypass);
    Q_INVOKABLE void setExposureInstanceEnabled(const QString &instanceId, bool enabled);
    Q_INVOKABLE void setColorBalanceRgbInstanceEnabled(const QString &instanceId, bool enabled);
    Q_INVOKABLE void reorderExposureInstance(int from, int to);
    Q_INVOKABLE void reorderColorBalanceRgbInstance(int from, int to);
    Q_INVOKABLE void setDevelopText(const QString &name, const QString &value);
    [[nodiscard]] bool maskOverlayVisible() const noexcept;
    [[nodiscard]] QString maskOverlayTarget() const;
    Q_INVOKABLE void setMaskOverlay(const QString &target, bool visible);
    [[nodiscard]] bool maskPlaceActive() const noexcept;
    [[nodiscard]] bool maskPlaceGeometryAllowed() const noexcept;
    Q_INVOKABLE void setMaskPlaceActive(bool active);
    Q_INVOKABLE void placeMask(double preview_x, double preview_y);
    [[nodiscard]] bool maskParametricAssistActive() const noexcept;
    [[nodiscard]] bool maskParametricAssistAllowed() const noexcept;
    Q_INVOKABLE void setMaskParametricAssistActive(bool active);
    Q_INVOKABLE void assistParametricMask(double preview_x, double preview_y);
    void retranslate();
    [[nodiscard]] bool whiteBalancePickActive() const noexcept;
    Q_INVOKABLE void setWhiteBalancePickActive(bool active);
    Q_INVOKABLE void pickWhiteBalance(double preview_x, double preview_y);
    Q_INVOKABLE void autoPerspective(const QString &mode);
    Q_INVOKABLE void saveStyleToPath(const QString &path);
    Q_INVOKABLE void applyStyleFromPath(const QString &path);
    [[nodiscard]] QVariantList editPresets() const;
    [[nodiscard]] QVariantList modifiedParameterChoices() const;
    Q_INVOKABLE void restoreHistory(int history_id);
    void load_develop_for_selection();
    void apply_recipe_history(const std::vector<RecipeHistoryEntry> &entries);
    void reload_recipe_history();
    void sync_active_history();
    [[nodiscard]] DevelopParams baseline_develop() const;
    [[nodiscard]] DevelopParams develop_from_history_entry(const RecipeHistoryEntry &entry) const;
    bool mutate_develop(DevelopParams next, DevelopEdit edit, bool refresh_preview = true,
                        std::optional<std::string> history_coalesce_key = {});
    void sync_local_edit_scope();
    void clear_local_edit_scope();
    [[nodiscard]] Result<bool> applyMaskGesture(const QString &action,
                                                const QVariantMap &arguments);
    bool mutate_scoped_develop(DevelopParams next, DevelopEdit edit, bool refresh_preview = true,
                               std::optional<std::string> history_coalesce_key = {});
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
    [[nodiscard]] bool clear_comparison();
    void capture_instance_front_for_field(const DevelopParams &params, std::string_view field);
    void retarget_instance_edit_after_field(DevelopParams &params, std::string_view field);
    void sync_selected_instance_edit_buffers(DevelopParams &params);

    Q_INVOKABLE void addRetouchRegion(const QVariantMap &region);
    void applyDevelopNumbers(const QVariantMap &fields, DevelopEdit edit);
    void apply_curve_points(const QString &family, int channel, const QVariantList &points,
                            DevelopEdit edit);
    Q_INVOKABLE void copyParametersSelected(const QVariantList &fields);
    Q_INVOKABLE void deletePreset(const QString &path);
    [[nodiscard]] const DevelopParams &edit_develop() const noexcept;
    Q_INVOKABLE void flipHorizontal();
    Q_INVOKABLE void flipVertical();
    Q_INVOKABLE void importPresetFromPath(const QString &path);
    Q_INVOKABLE void pasteParameters();
    Q_INVOKABLE void pasteParametersToSelection();
    [[nodiscard]] QString presets_directory() const;
    Q_INVOKABLE void previewCropRect(double x, double y, double width, double height);
    Q_INVOKABLE void previewCurvePoints(const QString &family, int channel,
                                        const QVariantList &points);
    Q_INVOKABLE void previewDevelopNumber(const QString &name, double value);
    Q_INVOKABLE void previewDevelopNumbers(const QVariantMap &fields);
    Q_INVOKABLE void previewToneCurve(const QVariantList &points);
    Q_INVOKABLE void redoEdit();
    void reload_presets();
    Q_INVOKABLE void removeRetouchRegion(int index);
    Q_INVOKABLE void renamePreset(const QString &path, const QString &name);
    Q_INVOKABLE void resetAllEdits();
    Q_INVOKABLE void resetControl(const QString &name);
    Q_INVOKABLE void resetSection(const QString &section);
    Q_INVOKABLE void rotateLeft();
    Q_INVOKABLE void rotateRight();
    Q_INVOKABLE void savePreset(const QString &name, const QVariantList &fields);
    Q_INVOKABLE bool sectionEffectEnabled(const QString &section) const;
    Q_INVOKABLE bool sectionModified(const QString &section) const;
    Q_INVOKABLE void setCropAspect(const QString &aspect);
    Q_INVOKABLE void setCropRect(double x, double y, double width, double height);
    Q_INVOKABLE void setCropToolActive(bool active);
    Q_INVOKABLE void setCurveChannel(int channel);
    Q_INVOKABLE void setCurveFamily(int family);
    Q_INVOKABLE void setCurvePoints(const QString &family, int channel, const QVariantList &points);
    Q_INVOKABLE void setDevelopNumbers(const QVariantMap &fields);
    Q_INVOKABLE void setSectionEffectEnabled(const QString &section, bool enabled);
    Q_INVOKABLE void setToneCurve(const QVariantList &points);
    void sync_curve_ui_from_develop();
    Q_INVOKABLE void toggleBeforeAfter();
    Q_INVOKABLE void toggleComparison();
    Q_INVOKABLE void undoEdit();

signals:
    void editChanged();
    void editingScopeChanged();
    void copiedParametersChanged();
    void presetsChanged();
    void previewChanged();
    void frameProjectionChanged();
    void selectionChanged();
    void interactivePreviewPublished(qulonglong revision, qlonglong intentToImageMicroseconds);
    void errorOccurred(QString error);
    void statusOccurred(QString status);

private:
    friend class StudioPresenter;
    friend class testing::StudioPipelineTestControl;
    struct Host
    {
        std::function<DevelopService *()> develop_service;
        std::function<PreviewService *()> preview_service;
        std::function<RecoveryService *()> recovery_service;
        std::function<void(bool)> preview_loading;
        std::function<void(std::int64_t)> observed_revision;
        std::function<void()> begin_catalog_operation;
        std::function<CancellationToken()> catalog_operation_token;
        std::function<void(QString, int, int, bool)> catalog_progress;
        std::function<void()> clear_comparison_images;
        std::function<void()> restore_preview_base;
        std::function<bool()> adopt_preview_as_comparison;
        std::function<void()> refresh_roi;
        std::function<void()> reload_library;
        std::function<void()> request_selection_preview;
        std::function<void()> kick_thumbnails;
        std::function<void(std::string)> cancel_thumbnails;
        std::function<void(QString)> zoom_mode;
        std::function<void(const PreviewResult &, std::uint64_t, bool)> publish_preview;
        std::function<void(const PreviewResult &, std::uint64_t)> publish_before;
        std::function<std::vector<std::string>()> selected_assets;
        std::function<QString()> selected_media_type;
    };
    struct Context
    {
        const QString &selected_asset_id_;
        const QString &catalog_path_;
        const QString &browse_mode_;
        const bool &busy_;
        const bool &catalog_operation_active_;
        const bool &import_work_active_;
        const std::int64_t &observed_catalog_revision_;
        const std::uint64_t &live_preview_revision_;
        const bool &preview_loading_;
        const QImage &preview_image_;
        const QImage &preview_base_image_;
        QMutex &preview_image_mutex_;
        const QUrl &comparison_before_url_;
        AssetListModel &assets_;
        StudioInspectPresenter &inspect_;
        const std::optional<EngineFacade> &engine_;
        SerialExecutor &executor_;
    };
    StudioDevelopPresenter(Context context, Host host, QObject *parent);
    using CopiedDevelopParameters = State::CopiedDevelopParameters;
    State state_;
    Host host_;
    bool stopped_ = false;
    const QString &selected_asset_id_;
    const QString &catalog_path_;
    const QString &browse_mode_;
    const bool &busy_;
    const bool &catalog_operation_active_;
    const bool &import_work_active_;
    const std::int64_t &observed_catalog_revision_;
    const std::uint64_t &live_preview_revision_;
    const bool &preview_loading_;
    const QImage &preview_image_;
    const QImage &preview_base_image_;
    QMutex &preview_image_mutex_;
    const QUrl &comparison_before_url_;
    AssetListModel &assets_;
    StudioInspectPresenter &inspect_;
    const std::optional<EngineFacade> &engine_;
    SerialExecutor &executor_;
};

} // namespace ravo
