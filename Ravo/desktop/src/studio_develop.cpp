#include "ravo/desktop/studio_presenter.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <numbers>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "studio_qt.h"

namespace ravo
{
bool StudioPresenter::beforeAfter() const noexcept
{
    return before_after_;
}

bool StudioPresenter::canUndo() const noexcept
{
    return !undo_stack_.empty();
}

bool StudioPresenter::canRedo() const noexcept
{
    return !redo_stack_.empty();
}

QVariantMap StudioPresenter::editWhiteBalance() const
{
    const auto &params = develop_.temperature;
    const auto coefficients = params.coefficients.value_or(
        std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0});
    const int mode = params.mode == kTemperatureModeCameraReference   ? 1 :
                     params.mode == kTemperatureModeAsShotToReference ? 2 :
                     params.mode == kTemperatureModeManual            ? 3 :
                                                                        0;
    return {{QStringLiteral("modeIndex"), mode},
            {QStringLiteral("hasCoefficients"), params.coefficients.has_value()},
            {QStringLiteral("red"), coefficients[0]},
            {QStringLiteral("green"), coefficients[1]},
            {QStringLiteral("blue"), coefficients[2]},
            {QStringLiteral("fourth"), coefficients[3]}};
}

QVariantMap StudioPresenter::editInputColor() const
{
    const auto index_of = [](const auto &values, const std::string_view selected)
    {
        const auto found = std::find(values.begin(), values.end(), selected);
        return found == values.end() ? -1 : static_cast<int>(std::distance(values.begin(), found));
    };
    const auto &params = develop_.input_color;
    return {
        {QStringLiteral("inputProfileIndex"),
         index_of(kSelectableInputProfiles, params.input_profile)},
        {QStringLiteral("workingProfileIndex"),
         index_of(kSelectableWorkingProfiles, params.working_profile)},
        {QStringLiteral("intentIndex"), index_of(kSelectableColorIntents, params.rendering_intent)},
        {QStringLiteral("normalizeIndex"),
         index_of(kSelectableColorNormalizations, params.gamut_normalize)},
        {QStringLiteral("blueMapping"), params.blue_mapping},
        {QStringLiteral("inputProfile"), QString::fromStdString(params.input_profile)},
        {QStringLiteral("workingProfile"), QString::fromStdString(params.working_profile)}};
}

QVariantMap StudioPresenter::editProfileGamma() const
{
    const auto &params = develop_.profile_gamma;
    return {{QStringLiteral("enabled"), develop_.profile_gamma_enabled},
            {QStringLiteral("modeIndex"), params.mode == kProfileGammaModeGamma ? 1 : 0},
            {QStringLiteral("linear"), params.linear},
            {QStringLiteral("gamma"), params.gamma},
            {QStringLiteral("dynamicRange"), params.dynamic_range},
            {QStringLiteral("greyPoint"), params.grey_point},
            {QStringLiteral("shadowsRange"), params.shadows_range},
            {QStringLiteral("securityFactor"), params.security_factor}};
}

QVariantMap StudioPresenter::editOutputColor() const
{
    const auto index_of = [](const auto &values, const std::string_view selected)
    {
        const auto found = std::find(values.begin(), values.end(), selected);
        return found == values.end() ? -1 : static_cast<int>(std::distance(values.begin(), found));
    };
    const auto &params = develop_.output_color;
    return {
        {QStringLiteral("outputProfileIndex"),
         index_of(kSelectableOutputProfiles, params.output_profile)},
        {QStringLiteral("intentIndex"), index_of(kSelectableColorIntents, params.rendering_intent)},
        {QStringLiteral("proofModeIndex"), index_of(kSelectableProofModes, params.proof_mode)},
        {QStringLiteral("proofProfileIndex"),
         index_of(kSelectableProofProfiles, params.proof_profile)},
        {QStringLiteral("proofIntentIndex"),
         index_of(kSelectableColorIntents, params.proof_intent)},
        {QStringLiteral("blackPointCompensation"), params.black_point_compensation},
        {QStringLiteral("outputProfile"), QString::fromStdString(params.output_profile)},
        {QStringLiteral("proofMode"), QString::fromStdString(params.proof_mode)},
        {QStringLiteral("proofProfile"), QString::fromStdString(params.proof_profile)}};
}

double StudioPresenter::editChannelMixerRR() const noexcept
{
    return develop_.channel_mixer.red[0];
}

double StudioPresenter::editChannelMixerRG() const noexcept
{
    return develop_.channel_mixer.red[1];
}

double StudioPresenter::editChannelMixerRB() const noexcept
{
    return develop_.channel_mixer.red[2];
}

double StudioPresenter::editChannelMixerGR() const noexcept
{
    return develop_.channel_mixer.green[0];
}

double StudioPresenter::editChannelMixerGG() const noexcept
{
    return develop_.channel_mixer.green[1];
}

double StudioPresenter::editChannelMixerGB() const noexcept
{
    return develop_.channel_mixer.green[2];
}

double StudioPresenter::editChannelMixerBR() const noexcept
{
    return develop_.channel_mixer.blue[0];
}

double StudioPresenter::editChannelMixerBG() const noexcept
{
    return develop_.channel_mixer.blue[1];
}

double StudioPresenter::editChannelMixerBB() const noexcept
{
    return develop_.channel_mixer.blue[2];
}

QVariantMap StudioPresenter::editExposureParams() const
{
    return {{QStringLiteral("modeIndex"), develop_.exposure_mode == kExposureModeDeflicker ? 1 : 0},
            {QStringLiteral("black"), develop_.exposure_black},
            {QStringLiteral("exposureEv"), develop_.exposure_ev},
            {QStringLiteral("deflickerPercentile"), develop_.exposure_deflicker_percentile},
            {QStringLiteral("deflickerTargetEv"), develop_.exposure_deflicker_target_ev},
            {QStringLiteral("compensateExposureBias"), develop_.exposure_compensate_exposure_bias},
            {QStringLiteral("compensateHighlightPreservation"),
             develop_.exposure_compensate_highlight_preservation}};
}

double StudioPresenter::editExposure() const noexcept
{
    return develop_.exposure_ev;
}

double StudioPresenter::editContrast() const noexcept
{
    return develop_.contrast;
}

double StudioPresenter::editHighlights() const noexcept
{
    return develop_.highlights;
}

double StudioPresenter::editShadows() const noexcept
{
    return develop_.shadows;
}

double StudioPresenter::editWhites() const noexcept
{
    return develop_.whites;
}

double StudioPresenter::editBlacks() const noexcept
{
    return develop_.blacks;
}

double StudioPresenter::editVibrance() const noexcept
{
    return develop_.vibrance;
}

double StudioPresenter::editSaturation() const noexcept
{
    return develop_.saturation;
}

int StudioPresenter::editRotateQuarters() const noexcept
{
    return static_cast<int>(develop_.rotate_quarters);
}

double StudioPresenter::editCropX() const noexcept
{
    return develop_.crop_x;
}

double StudioPresenter::editCropY() const noexcept
{
    return develop_.crop_y;
}

double StudioPresenter::editCropWidth() const noexcept
{
    return develop_.crop_width;
}

double StudioPresenter::editCropHeight() const noexcept
{
    return develop_.crop_height;
}

double StudioPresenter::editStraighten() const noexcept
{
    return develop_.straighten_degrees;
}

QString StudioPresenter::cropAspect() const
{
    return crop_aspect_;
}

double StudioPresenter::cropAspectRatio() const noexcept
{
    if (crop_aspect_ == QLatin1String("1:1"))
    {
        return 1.0;
    }
    if (crop_aspect_ == QLatin1String("3:2"))
    {
        return 1.5;
    }
    if (crop_aspect_ == QLatin1String("4:3"))
    {
        return 4.0 / 3.0;
    }
    if (crop_aspect_ == QLatin1String("5:4"))
    {
        return 1.25;
    }
    if (crop_aspect_ == QLatin1String("16:9"))
    {
        return 16.0 / 9.0;
    }
    return 0.0;
}

void StudioPresenter::valid_crop_rect(double &x, double &y, double &width, double &height) const
{
    const double working_aspect = selected_working_aspect();
    const double ratio = cropAspectRatio() > 0.0 ?
                             cropAspectRatio() / std::max(working_aspect, 1e-6) :
                             develop_.crop_width / std::max(develop_.crop_height, 1e-6);
    inscribed_crop_for_straighten(develop_.straighten_degrees, working_aspect, ratio, x, y, width,
                                  height);
}

double StudioPresenter::validCropX() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return x;
}

double StudioPresenter::validCropY() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return y;
}

double StudioPresenter::validCropWidth() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return width;
}

double StudioPresenter::validCropHeight() const
{
    double x = 0.0;
    double y = 0.0;
    double width = 1.0;
    double height = 1.0;
    valid_crop_rect(x, y, width, height);
    return height;
}

bool StudioPresenter::editFlipHorizontal() const noexcept
{
    return develop_.flip_horizontal != 0;
}

bool StudioPresenter::editFlipVertical() const noexcept
{
    return develop_.flip_vertical != 0;
}

double StudioPresenter::editSharpen() const noexcept
{
    return develop_.sharpen;
}

double StudioPresenter::editSharpenRadius() const noexcept
{
    return develop_.sharpen_radius;
}

double StudioPresenter::editClarity() const noexcept
{
    return develop_.clarity;
}

double StudioPresenter::editVignette() const noexcept
{
    return develop_.vignette;
}

double StudioPresenter::editGrain() const noexcept
{
    return develop_.grain;
}

double StudioPresenter::editBloom() const noexcept
{
    return develop_.bloom;
}

double StudioPresenter::editSoften() const noexcept
{
    return develop_.soften;
}

double StudioPresenter::editDehaze() const noexcept
{
    return develop_.dehaze;
}

double StudioPresenter::editVelvia() const noexcept
{
    return develop_.velvia;
}

QVariantMap StudioPresenter::editLegacyColorBalance() const
{
    const auto &params = develop_.color_balance;
    return {{QStringLiteral("enabled"), develop_.color_balance_enabled},
            {QStringLiteral("modeIndex"), params.mode == kColorBalanceModeLiftGammaGain ? 0 : 1},
            {QStringLiteral("liftFactor"), params.lift[0]},
            {QStringLiteral("liftRed"), params.lift[1]},
            {QStringLiteral("liftGreen"), params.lift[2]},
            {QStringLiteral("liftBlue"), params.lift[3]},
            {QStringLiteral("gammaFactor"), params.gamma[0]},
            {QStringLiteral("gammaRed"), params.gamma[1]},
            {QStringLiteral("gammaGreen"), params.gamma[2]},
            {QStringLiteral("gammaBlue"), params.gamma[3]},
            {QStringLiteral("gainFactor"), params.gain[0]},
            {QStringLiteral("gainRed"), params.gain[1]},
            {QStringLiteral("gainGreen"), params.gain[2]},
            {QStringLiteral("gainBlue"), params.gain[3]},
            {QStringLiteral("inputSaturation"), params.input_saturation},
            {QStringLiteral("contrast"), params.contrast},
            {QStringLiteral("greyFulcrum"), params.grey_fulcrum_percent},
            {QStringLiteral("outputSaturation"), params.output_saturation}};
}

QVariantMap StudioPresenter::editColorChecker() const
{
    int preset_index = -1;
    const auto presets = color_checker_presets();
    for (std::size_t index = 0U; index < presets.size(); ++index)
    {
        auto preset = color_checker_params_for_preset(presets[index].id);
        if (preset && preset.value() == develop_.color_checker)
        {
            preset_index = static_cast<int>(index);
            break;
        }
    }
    const auto patch_count = develop_.color_checker.patches.size();
    const std::size_t patch_index =
        patch_count == 0U ?
            0U :
            static_cast<std::size_t>(std::clamp(develop_.color_checker_patch, std::int64_t{0},
                                                static_cast<std::int64_t>(patch_count - 1U)));
    const ColorCheckerPatch patch =
        patch_count == 0U ? ColorCheckerPatch{} : develop_.color_checker.patches[patch_index];
    return {{QStringLiteral("enabled"), develop_.color_checker_enabled},
            {QStringLiteral("presetIndex"), preset_index},
            {QStringLiteral("patchIndex"), static_cast<int>(patch_index)},
            {QStringLiteral("patchCount"), static_cast<int>(patch_count)},
            {QStringLiteral("sourceL"), patch.source_lab[0]},
            {QStringLiteral("sourceA"), patch.source_lab[1]},
            {QStringLiteral("sourceB"), patch.source_lab[2]},
            {QStringLiteral("targetL"), patch.target_lab[0]},
            {QStringLiteral("targetA"), patch.target_lab[1]},
            {QStringLiteral("targetB"), patch.target_lab[2]}};
}

QVariantMap StudioPresenter::editColorBalanceRgb() const
{
    const auto &params = develop_.color_balance_rgb;
    return {{QStringLiteral("shadowsY"), params.shadows_y},
            {QStringLiteral("shadowsChroma"), params.shadows_chroma},
            {QStringLiteral("shadowsHue"), params.shadows_hue},
            {QStringLiteral("midtonesY"), params.midtones_y},
            {QStringLiteral("midtonesChroma"), params.midtones_chroma},
            {QStringLiteral("midtonesHue"), params.midtones_hue},
            {QStringLiteral("highlightsY"), params.highlights_y},
            {QStringLiteral("highlightsChroma"), params.highlights_chroma},
            {QStringLiteral("highlightsHue"), params.highlights_hue},
            {QStringLiteral("globalY"), params.global_y},
            {QStringLiteral("globalChroma"), params.global_chroma},
            {QStringLiteral("globalHue"), params.global_hue},
            {QStringLiteral("shadowsFalloff"), params.shadows_falloff},
            {QStringLiteral("whiteFulcrumEv"), params.white_fulcrum_ev},
            {QStringLiteral("highlightsFalloff"), params.highlights_falloff},
            {QStringLiteral("chromaShadows"), params.chroma_shadows},
            {QStringLiteral("chromaHighlights"), params.chroma_highlights},
            {QStringLiteral("chromaGlobal"), params.chroma_global},
            {QStringLiteral("chromaMidtones"), params.chroma_midtones},
            {QStringLiteral("saturationGlobal"), params.saturation_global},
            {QStringLiteral("saturationHighlights"), params.saturation_highlights},
            {QStringLiteral("saturationMidtones"), params.saturation_midtones},
            {QStringLiteral("saturationShadows"), params.saturation_shadows},
            {QStringLiteral("hueRotation"), params.hue_rotation},
            {QStringLiteral("brillianceGlobal"), params.brilliance_global},
            {QStringLiteral("brillianceHighlights"), params.brilliance_highlights},
            {QStringLiteral("brillianceMidtones"), params.brilliance_midtones},
            {QStringLiteral("brillianceShadows"), params.brilliance_shadows},
            {QStringLiteral("maskGreyFulcrum"), params.mask_grey_fulcrum},
            {QStringLiteral("vibrance"), params.vibrance},
            {QStringLiteral("greyFulcrum"), params.grey_fulcrum},
            {QStringLiteral("contrast"), params.contrast},
            {QStringLiteral("formulaIndex"),
             params.saturation_formula == kColorBalanceRgbFormulaJzAzBz2021 ? 1 : 0}};
}

QVariantMap StudioPresenter::editColorCorrection() const
{
    const auto &params = develop_.color_correction;
    return {{QStringLiteral("enabled"), develop_.color_correction_enabled},
            {QStringLiteral("highlightA"), params.highlight_a},
            {QStringLiteral("highlightB"), params.highlight_b},
            {QStringLiteral("shadowA"), params.shadow_a},
            {QStringLiteral("shadowB"), params.shadow_b},
            {QStringLiteral("saturation"), params.saturation}};
}

QVariantMap StudioPresenter::editPrimaries() const
{
    constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi_v<double>;
    const auto &params = develop_.primaries;
    return {{QStringLiteral("achromaticTintHueDegrees"),
             params.achromatic_tint_hue * kDegreesPerRadian},
            {QStringLiteral("achromaticTintPurity"), params.achromatic_tint_purity},
            {QStringLiteral("redHueDegrees"), params.red_hue * kDegreesPerRadian},
            {QStringLiteral("redPurity"), params.red_purity},
            {QStringLiteral("greenHueDegrees"), params.green_hue * kDegreesPerRadian},
            {QStringLiteral("greenPurity"), params.green_purity},
            {QStringLiteral("blueHueDegrees"), params.blue_hue * kDegreesPerRadian},
            {QStringLiteral("bluePurity"), params.blue_purity}};
}

QVariantMap StudioPresenter::editColorContrast() const
{
    const auto &params = develop_.color_contrast;
    return {{QStringLiteral("enabled"), develop_.color_contrast_enabled},
            {QStringLiteral("aSteepness"), params.a_steepness},
            {QStringLiteral("aOffset"), params.a_offset},
            {QStringLiteral("bSteepness"), params.b_steepness},
            {QStringLiteral("bOffset"), params.b_offset},
            {QStringLiteral("unbound"), params.unbound}};
}

namespace
{

constexpr double kColorHarmonizerHueStepDegrees = 0.1;
constexpr double kColorHarmonizerLinearStep = 0.01;
constexpr double kColorHarmonizerCustomNodesStep = 1.0;
constexpr int kColorHarmonizerHueDecimals = 1;
constexpr int kColorHarmonizerLinearDecimals = 2;
constexpr int kColorHarmonizerCustomNodesDecimals = 0;
// This is an interaction minimum only. Canonical recipes retain the exact
// strictly-positive hard lower bound exported by mask.h.
constexpr double kDevelopMaskRadiusSoftMin = 0.01;

[[nodiscard]] QString color_harmonizer_rule_label(const std::string_view name)
{
    if (name == "monochromatic")
    {
        return QCoreApplication::translate("DevelopPanel", "Monochromatic");
    }
    if (name == "analogous")
    {
        return QCoreApplication::translate("DevelopPanel", "Analogous");
    }
    if (name == "analogous_complementary")
    {
        return QCoreApplication::translate("DevelopPanel", "Analogous complementary");
    }
    if (name == "complementary")
    {
        return QCoreApplication::translate("DevelopPanel", "Complementary");
    }
    if (name == "split_complementary")
    {
        return QCoreApplication::translate("DevelopPanel", "Split complementary");
    }
    if (name == "dyad")
    {
        return QCoreApplication::translate("DevelopPanel", "Dyad");
    }
    if (name == "triad")
    {
        return QCoreApplication::translate("DevelopPanel", "Triad");
    }
    if (name == "tetrad")
    {
        return QCoreApplication::translate("DevelopPanel", "Tetrad");
    }
    if (name == "square")
    {
        return QCoreApplication::translate("DevelopPanel", "Square");
    }
    if (name == "custom")
    {
        return QCoreApplication::translate("DevelopPanel", "Custom");
    }
    return {};
}

[[nodiscard]] QVariantMap color_harmonizer_control(const QString &title, const QString &key,
                                                   const QString &field, const double minimum,
                                                   const double maximum, const double step,
                                                   const double reset, const int decimals,
                                                   const bool visible)
{
    return {{QStringLiteral("title"), title},     {QStringLiteral("key"), key},
            {QStringLiteral("field"), field},     {QStringLiteral("minimum"), minimum},
            {QStringLiteral("maximum"), maximum}, {QStringLiteral("step"), step},
            {QStringLiteral("reset"), reset},     {QStringLiteral("decimals"), decimals},
            {QStringLiteral("visible"), visible}};
}

[[nodiscard]] QString develop_mask_field_prefix(const DevelopMaskTarget target)
{
    return target == DevelopMaskTarget::kColorHarmonizer ? QStringLiteral("colorHarmonizerMask") :
                                                           QStringLiteral("graduatedMask");
}

[[nodiscard]] QString develop_mask_kind_label(const std::string_view name)
{
    if (name == "none")
        return QCoreApplication::translate("DevelopPanel", "None");
    if (name == "all")
        return QCoreApplication::translate("DevelopPanel", "All");
    if (name == "linear_gradient")
        return QCoreApplication::translate("DevelopPanel", "Linear gradient");
    if (name == "circle")
        return QCoreApplication::translate("DevelopPanel", "Circle");
    if (name == "ellipse")
        return QCoreApplication::translate("DevelopPanel", "Ellipse");
    if (name == "parametric")
        return QCoreApplication::translate("DevelopPanel", "Parametric");
    if (name == "group")
        return QCoreApplication::translate("DevelopPanel", "Group");
    if (name == "path")
        return QCoreApplication::translate("DevelopPanel", "Path");
    if (name == "brush")
        return QCoreApplication::translate("DevelopPanel", "Brush");
    return QCoreApplication::translate("DevelopPanel", "Unknown");
}

[[nodiscard]] QString develop_mask_status_label(const DevelopMaskAttachmentStatus status)
{
    switch (status)
    {
    case DevelopMaskAttachmentStatus::kNoMask:
        return QCoreApplication::translate("DevelopPanel", "No mask attached");
    case DevelopMaskAttachmentStatus::kEditable:
        return QCoreApplication::translate("DevelopPanel", "Mask is editable");
    case DevelopMaskAttachmentStatus::kExternalReadOnly:
        return QCoreApplication::translate("DevelopPanel", "External mask is read-only");
    case DevelopMaskAttachmentStatus::kSharedReadOnly:
        return QCoreApplication::translate("DevelopPanel", "Shared mask is read-only");
    case DevelopMaskAttachmentStatus::kGroupReadOnly:
        return QCoreApplication::translate("DevelopPanel", "Group mask is read-only");
    case DevelopMaskAttachmentStatus::kInvalid:
        return QCoreApplication::translate("DevelopPanel", "Mask attachment is invalid");
    }
    return QCoreApplication::translate("DevelopPanel", "Mask attachment is invalid");
}

[[nodiscard]] QVariantMap develop_mask_control(const QString &title, const QString &key,
                                               const QString &field, const double minimum,
                                               const double maximum, const double step,
                                               const double reset, const int decimals,
                                               const bool visible)
{
    return {{QStringLiteral("title"), title},    {QStringLiteral("key"), key},
            {QStringLiteral("field"), field},    {QStringLiteral("min"), minimum},
            {QStringLiteral("max"), maximum},    {QStringLiteral("step"), step},
            {QStringLiteral("reset"), reset},    {QStringLiteral("decimals"), decimals},
            {QStringLiteral("visible"), visible}};
}

[[nodiscard]] QVariantMap develop_mask_editor_map(const DevelopMaskEditorState &state,
                                                  const DevelopMaskTarget target)
{
    const auto prefix = develop_mask_field_prefix(target);
    const auto shape_kind =
        state.kind_index == 6 ? state.child_kind_index : state.kind_index;
    const auto kind_is = [shape_kind](const std::int64_t index) { return shape_kind == index; };
    const bool attached = state.attached;
    const double threshold0_min = kCanonicalMaskUnitMin;
    const double threshold0_max = state.threshold1;
    const double threshold1_min = state.threshold0;
    const double threshold1_max = state.threshold2;
    const double threshold2_min = state.threshold1;
    const double threshold2_max = state.threshold3;
    const double threshold3_min = state.threshold2;
    const double threshold3_max = kCanonicalMaskUnitMax;
    const double radius_min = std::min(kDevelopMaskRadiusSoftMin, state.radius);
    const double radius_x_min = std::min(kDevelopMaskRadiusSoftMin, state.radius_x);
    const double radius_y_min = std::min(kDevelopMaskRadiusSoftMin, state.radius_y);
    const QVariantList controls{
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Opacity"),
                             QStringLiteral("opacity"), prefix + QStringLiteral("Opacity"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 1.0, 2, attached),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Anchor X"),
                             QStringLiteral("anchorX"), prefix + QStringLiteral("AnchorX"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(2)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Anchor Y"),
                             QStringLiteral("anchorY"), prefix + QStringLiteral("AnchorY"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(2)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Rotation"),
                             QStringLiteral("rotationDegrees"),
                             prefix + QStringLiteral("RotationDegrees"), kCanonicalMaskAngleMin,
                             kCanonicalMaskAngleMax, 1.0, 0.0, 0, kind_is(2) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Transition"),
                             QStringLiteral("transition"), prefix + QStringLiteral("Transition"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.1, 2,
                             kind_is(2)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Center X"),
                             QStringLiteral("centerX"), prefix + QStringLiteral("CenterX"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(3) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Center Y"),
                             QStringLiteral("centerY"), prefix + QStringLiteral("CenterY"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(3) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Radius"),
                             QStringLiteral("radius"), prefix + QStringLiteral("Radius"),
                             radius_min, kCanonicalMaskUnitMax, 0.01, 0.25, 2, kind_is(3)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Radius X"),
                             QStringLiteral("radiusX"), prefix + QStringLiteral("RadiusX"),
                             radius_x_min, kCanonicalMaskUnitMax, 0.01, 0.25, 2, kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Radius Y"),
                             QStringLiteral("radiusY"), prefix + QStringLiteral("RadiusY"),
                             radius_y_min, kCanonicalMaskUnitMax, 0.01, 0.25, 2, kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Feather"),
                             QStringLiteral("feather"), prefix + QStringLiteral("Feather"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.0, 2,
                             kind_is(3) || kind_is(4)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 1"),
                             QStringLiteral("threshold0"), prefix + QStringLiteral("Threshold0"),
                             threshold0_min, threshold0_max, 0.01, 0.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 2"),
                             QStringLiteral("threshold1"), prefix + QStringLiteral("Threshold1"),
                             threshold1_min, threshold1_max, 0.01, 0.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 3"),
                             QStringLiteral("threshold2"), prefix + QStringLiteral("Threshold2"),
                             threshold2_min, threshold2_max, 0.01, 1.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Threshold 4"),
                             QStringLiteral("threshold3"), prefix + QStringLiteral("Threshold3"),
                             threshold3_min, threshold3_max, 0.01, 1.0, 2, kind_is(5)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Path feather"),
                             QStringLiteral("pathFeather"), prefix + QStringLiteral("PathFeather"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.05, 2,
                             kind_is(7)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point X"),
                             QStringLiteral("pointX"), prefix + QStringLiteral("PointX"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(7) || kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point Y"),
                             QStringLiteral("pointY"), prefix + QStringLiteral("PointY"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 0.5, 2,
                             kind_is(7) || kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point radius"),
                             QStringLiteral("pointRadius"), prefix + QStringLiteral("PointRadius"),
                             radius_min, kCanonicalMaskUnitMax, 0.01, 0.05, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point hardness"),
                             QStringLiteral("pointHardness"),
                             prefix + QStringLiteral("PointHardness"), kCanonicalMaskUnitMin,
                             kCanonicalMaskUnitMax, 0.01, 0.5, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Point density"),
                             QStringLiteral("pointDensity"), prefix + QStringLiteral("PointDensity"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 1.0, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Child opacity"),
                             QStringLiteral("childOpacity"), prefix + QStringLiteral("ChildOpacity"),
                             kCanonicalMaskUnitMin, kCanonicalMaskUnitMax, 0.01, 1.0, 2,
                             state.kind_index == 6)};

    const QStringList kind_choices{develop_mask_kind_label("none"),
                                   develop_mask_kind_label("all"),
                                   develop_mask_kind_label("linear_gradient"),
                                   develop_mask_kind_label("circle"),
                                   develop_mask_kind_label("ellipse"),
                                   develop_mask_kind_label("parametric"),
                                   develop_mask_kind_label("group"),
                                   develop_mask_kind_label("path"),
                                   develop_mask_kind_label("brush")};
    const QStringList child_kind_choices{develop_mask_kind_label("all"),
                                         develop_mask_kind_label("linear_gradient"),
                                         develop_mask_kind_label("circle"),
                                         develop_mask_kind_label("ellipse"),
                                         develop_mask_kind_label("parametric"),
                                         develop_mask_kind_label("path"),
                                         develop_mask_kind_label("brush")};
    const QStringList operator_choices{
        QCoreApplication::translate("DevelopPanel", "Replace"),
        QCoreApplication::translate("DevelopPanel", "Union"),
        QCoreApplication::translate("DevelopPanel", "Intersection"),
        QCoreApplication::translate("DevelopPanel", "Difference"),
        QCoreApplication::translate("DevelopPanel", "Exclusion")};
    const QStringList source_choices{
        QCoreApplication::translate("DevelopPanel", "Input"),
        QCoreApplication::translate("DevelopPanel", "Operation output")};
    const QStringList channel_choices{QCoreApplication::translate("DevelopPanel", "Luminance"),
                                      QCoreApplication::translate("DevelopPanel", "Red"),
                                      QCoreApplication::translate("DevelopPanel", "Green"),
                                      QCoreApplication::translate("DevelopPanel", "Blue")};
    return {{QStringLiteral("target"), qstring_from_utf8(develop_mask_target_name(target))},
            {QStringLiteral("attached"), state.attached},
            {QStringLiteral("editable"), state.editable},
            {QStringLiteral("canDetach"), state.can_detach},
            {QStringLiteral("kindIndex"), static_cast<int>(state.kind_index)},
            {QStringLiteral("kindName"), qstring_from_utf8(state.kind_name)},
            {QStringLiteral("kindLabel"), develop_mask_kind_label(state.kind_name)},
            {QStringLiteral("kindChoices"), kind_choices},
            {QStringLiteral("status"), develop_mask_status_label(state.status)},
            {QStringLiteral("statusCode"),
             qstring_from_utf8(develop_mask_attachment_status_name(state.status))},
            {QStringLiteral("kindField"), prefix + QStringLiteral("Kind")},
            {QStringLiteral("detachField"), prefix},
            {QStringLiteral("invertedField"), prefix + QStringLiteral("Inverted")},
            {QStringLiteral("sourceField"), prefix + QStringLiteral("Source")},
            {QStringLiteral("channelField"), prefix + QStringLiteral("Channel")},
            {QStringLiteral("selectorsVisible"), kind_is(5)},
            {QStringLiteral("opacity"), state.opacity},
            {QStringLiteral("inverted"), state.inverted},
            {QStringLiteral("anchorX"), state.anchor_x},
            {QStringLiteral("anchorY"), state.anchor_y},
            {QStringLiteral("rotationDegrees"), state.rotation_degrees},
            {QStringLiteral("transition"), state.transition},
            {QStringLiteral("centerX"), state.center_x},
            {QStringLiteral("centerY"), state.center_y},
            {QStringLiteral("radius"), state.radius},
            {QStringLiteral("radiusX"), state.radius_x},
            {QStringLiteral("radiusY"), state.radius_y},
            {QStringLiteral("feather"), state.feather},
            {QStringLiteral("sourceIndex"), static_cast<int>(state.source_index)},
            {QStringLiteral("sourceChoices"), source_choices},
            {QStringLiteral("channelIndex"), static_cast<int>(state.channel_index)},
            {QStringLiteral("channelChoices"), channel_choices},
            {QStringLiteral("threshold0"), state.threshold0},
            {QStringLiteral("threshold1"), state.threshold1},
            {QStringLiteral("threshold2"), state.threshold2},
            {QStringLiteral("threshold3"), state.threshold3},
            {QStringLiteral("pathFeather"), state.path_feather},
            {QStringLiteral("pointX"), state.point_x},
            {QStringLiteral("pointY"), state.point_y},
            {QStringLiteral("pointRadius"), state.point_radius},
            {QStringLiteral("pointHardness"), state.point_hardness},
            {QStringLiteral("pointDensity"), state.point_density},
            {QStringLiteral("pointCount"), static_cast<int>(state.point_count)},
            {QStringLiteral("pointIndex"), static_cast<int>(state.point_index)},
            {QStringLiteral("pointIndexField"), prefix + QStringLiteral("PointIndex")},
            {QStringLiteral("addPointField"), prefix + QStringLiteral("AddPoint")},
            {QStringLiteral("removePointField"), prefix + QStringLiteral("RemovePoint")},
            {QStringLiteral("childCount"), static_cast<int>(state.child_count)},
            {QStringLiteral("childIndex"), static_cast<int>(state.child_index)},
            {QStringLiteral("childIndexField"), prefix + QStringLiteral("ChildIndex")},
            {QStringLiteral("childKindIndex"),
             state.child_kind_index <= 5 ? static_cast<int>(state.child_kind_index - 1) :
             state.child_kind_index >= 7 ? static_cast<int>(state.child_kind_index - 2) :
                                           0},
            {QStringLiteral("childKindField"), prefix + QStringLiteral("ChildKind")},
            {QStringLiteral("childKindChoices"), child_kind_choices},
            {QStringLiteral("childKindValues"), QVariantList{1, 2, 3, 4, 5, 7, 8}},
            {QStringLiteral("childOperatorIndex"), static_cast<int>(state.child_operator_index)},
            {QStringLiteral("childOperatorField"), prefix + QStringLiteral("ChildOperator")},
            {QStringLiteral("operatorChoices"), operator_choices},
            {QStringLiteral("childOpacity"), state.child_opacity},
            {QStringLiteral("childInverted"), state.child_inverted},
            {QStringLiteral("childInvertedField"), prefix + QStringLiteral("ChildInverted")},
            {QStringLiteral("addChildField"), prefix + QStringLiteral("AddChild")},
            {QStringLiteral("removeChildField"), prefix + QStringLiteral("RemoveChild")},
            {QStringLiteral("groupVisible"), state.kind_index == 6},
            {QStringLiteral("pointsVisible"), kind_is(7) || kind_is(8)},
            {QStringLiteral("numericControls"), controls}};
}

} // namespace

QVariantMap StudioPresenter::editColorHarmonizer() const
{
    const auto &params = develop_.color_harmonizer;
    const ColorHarmonizerParams defaults;
    const bool custom_rule = params.rule == ColorHarmonizerRule::kCustom;
    const int active_node_count = static_cast<int>(color_harmonizer_active_node_count(params));
    const bool anchor_visible = color_harmonizer_uses_anchor_hue(params.rule);

    QStringList rule_choices;
    rule_choices.reserve(static_cast<int>(kColorHarmonizerRuleCount));
    for (std::size_t index = 0U; index < kColorHarmonizerRuleCount; ++index)
    {
        const auto rule = color_harmonizer_rule_from_index(static_cast<std::int64_t>(index));
        const auto name = rule ? color_harmonizer_rule_name(rule.value()) : std::string_view{};
        rule_choices.push_back(color_harmonizer_rule_label(name));
    }

    const QVariantList shared_controls{
        color_harmonizer_control(QCoreApplication::translate("DevelopPanel", "Anchor hue"),
                                 QStringLiteral("anchorHueDegrees"),
                                 QStringLiteral("colorHarmonizerAnchorHueDegrees"),
                                 kColorHarmonizerHueDegreesMin, kColorHarmonizerHueDegreesMax,
                                 kColorHarmonizerHueStepDegrees,
                                 color_harmonizer_hue_turns_to_degrees(defaults.anchor_hue),
                                 kColorHarmonizerHueDecimals, anchor_visible),
        color_harmonizer_control(QCoreApplication::translate("DevelopPanel", "Pull strength"),
                                 QStringLiteral("pullStrength"),
                                 QStringLiteral("colorHarmonizerPullStrength"),
                                 kColorHarmonizerPullStrengthMin, kColorHarmonizerPullStrengthMax,
                                 kColorHarmonizerLinearStep, defaults.pull_strength,
                                 kColorHarmonizerLinearDecimals, true),
        color_harmonizer_control(QCoreApplication::translate("DevelopPanel", "Neutral protection"),
                                 QStringLiteral("neutralProtection"),
                                 QStringLiteral("colorHarmonizerNeutralProtection"),
                                 kColorHarmonizerNeutralProtectionMin,
                                 kColorHarmonizerNeutralProtectionMax, kColorHarmonizerLinearStep,
                                 defaults.neutral_protection, kColorHarmonizerLinearDecimals, true),
        color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", "Pull width"), QStringLiteral("pullWidth"),
            QStringLiteral("colorHarmonizerPullWidth"), kColorHarmonizerPullWidthMin,
            kColorHarmonizerPullWidthMax, kColorHarmonizerLinearStep, defaults.pull_width,
            kColorHarmonizerLinearDecimals, true),
        color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", "Smoothing"), QStringLiteral("smoothing"),
            QStringLiteral("colorHarmonizerSmoothing"), kColorHarmonizerSmoothingMin,
            kColorHarmonizerSmoothingMax, kColorHarmonizerLinearStep, defaults.smoothing,
            kColorHarmonizerLinearDecimals, true),
    };

    const QVariantMap custom_node_control = color_harmonizer_control(
        QCoreApplication::translate("DevelopPanel", "Custom nodes"),
        QStringLiteral("customNodeCount"), QStringLiteral("colorHarmonizerCustomNodeCount"),
        static_cast<double>(kColorHarmonizerCustomNodesMin),
        static_cast<double>(kColorHarmonizerCustomNodesMax), kColorHarmonizerCustomNodesStep,
        static_cast<double>(defaults.num_custom_nodes), kColorHarmonizerCustomNodesDecimals,
        custom_rule);

    QVariantList custom_hue_controls;
    QVariantList node_saturation_controls;
    custom_hue_controls.reserve(static_cast<int>(kColorHarmonizerNodeSlotCount));
    node_saturation_controls.reserve(static_cast<int>(kColorHarmonizerNodeSlotCount));
    static const std::array<const char *, 4> hue_titles{
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 1"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 2"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 3"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Custom hue 4")};
    static const std::array<const char *, 4> sat_titles{
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 1"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 2"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 3"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Node saturation 4")};
    for (std::size_t index = 0U; index < kColorHarmonizerNodeSlotCount; ++index)
    {
        const auto suffix = QString::number(static_cast<int>(index));
        custom_hue_controls.push_back(color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", hue_titles[index]),
            QStringLiteral("customHue%1Degrees").arg(suffix),
            QStringLiteral("colorHarmonizerCustomHue%1Degrees").arg(suffix),
            kColorHarmonizerHueDegreesMin, kColorHarmonizerHueDegreesMax,
            kColorHarmonizerHueStepDegrees,
            color_harmonizer_hue_turns_to_degrees(defaults.custom_hue[index]),
            kColorHarmonizerHueDecimals, color_harmonizer_uses_custom_hue(params, index)));
        node_saturation_controls.push_back(color_harmonizer_control(
            QCoreApplication::translate("DevelopPanel", sat_titles[index]),
            QStringLiteral("nodeSaturation%1").arg(suffix),
            QStringLiteral("colorHarmonizerNodeSaturation%1").arg(suffix),
            kColorHarmonizerNodeSaturationMin, kColorHarmonizerNodeSaturationMax,
            kColorHarmonizerLinearStep, defaults.node_saturation[index],
            kColorHarmonizerLinearDecimals, color_harmonizer_uses_node_saturation(params, index)));
    }

    return {
        {QStringLiteral("enabled"), develop_.color_harmonizer_enabled},
        {QStringLiteral("ruleIndex"), static_cast<int>(color_harmonizer_rule_index(params.rule))},
        {QStringLiteral("ruleChoices"), rule_choices},
        {QStringLiteral("customRule"), custom_rule},
        {QStringLiteral("activeNodeCount"), active_node_count},
        {QStringLiteral("anchorVisible"), anchor_visible},
        {QStringLiteral("anchorHueDegrees"),
         color_harmonizer_hue_turns_to_degrees(params.anchor_hue)},
        {QStringLiteral("pullStrength"), params.pull_strength},
        {QStringLiteral("neutralProtection"), params.neutral_protection},
        {QStringLiteral("pullWidth"), params.pull_width},
        {QStringLiteral("smoothing"), params.smoothing},
        {QStringLiteral("customNodeCount"), static_cast<int>(params.num_custom_nodes)},
        {QStringLiteral("customHue0Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[0])},
        {QStringLiteral("customHue1Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[1])},
        {QStringLiteral("customHue2Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[2])},
        {QStringLiteral("customHue3Degrees"),
         color_harmonizer_hue_turns_to_degrees(params.custom_hue[3])},
        {QStringLiteral("nodeSaturation0"), params.node_saturation[0]},
        {QStringLiteral("nodeSaturation1"), params.node_saturation[1]},
        {QStringLiteral("nodeSaturation2"), params.node_saturation[2]},
        {QStringLiteral("nodeSaturation3"), params.node_saturation[3]},
        {QStringLiteral("sharedControls"), shared_controls},
        {QStringLiteral("customNodeControl"), custom_node_control},
        {QStringLiteral("customHueControls"), custom_hue_controls},
        {QStringLiteral("nodeSaturationControls"), node_saturation_controls}};
}

QVariantMap StudioPresenter::editColorHarmonizerMask() const
{
    return develop_mask_editor_map(
        develop_mask_editor_state(develop_, DevelopMaskTarget::kColorHarmonizer),
        DevelopMaskTarget::kColorHarmonizer);
}

bool StudioPresenter::maskOverlayVisible() const noexcept
{
    return mask_overlay_visible_;
}

QString StudioPresenter::maskOverlayTarget() const
{
    return mask_overlay_target_;
}

void StudioPresenter::setMaskOverlay(const QString &target, const bool visible)
{
    const QString normalized = target == QLatin1String("graduatednd") ?
                                   QStringLiteral("graduatednd") :
                                   QStringLiteral("color_harmonizer");
    const bool changed = mask_overlay_visible_ != visible || mask_overlay_target_ != normalized;
    mask_overlay_visible_ = visible;
    mask_overlay_target_ = normalized;
    if (!changed)
    {
        return;
    }
    emit previewChanged();
    if (!visible)
    {
        if (!preview_base_image_.isNull())
        {
            const QMutexLocker lock(&preview_image_mutex_);
            preview_image_ = preview_base_image_;
        }
        emit previewChanged();
        return;
    }
    enqueue_preview();
}

void StudioPresenter::retranslate()
{
    emit editChanged();
}

double StudioPresenter::editMonochrome() const noexcept
{
    return develop_.monochrome;
}

double StudioPresenter::editSplitShadowsHue() const noexcept
{
    return develop_.split_shadows_hue;
}

double StudioPresenter::editSplitHighlightsHue() const noexcept
{
    return develop_.split_highlights_hue;
}

double StudioPresenter::editSplitBalance() const noexcept
{
    return develop_.split_balance;
}

double StudioPresenter::editSplitAmount() const noexcept
{
    return develop_.split_amount;
}

double StudioPresenter::editGamma() const noexcept
{
    return develop_.gamma;
}

QVariantList StudioPresenter::editToneCurve() const
{
    return tone_curve_to_variant(develop_.tone_curve);
}

QVariantList StudioPresenter::editToneCurveSamples() const
{
    return tone_curve_sample_list(develop_.tone_curve);
}

bool StudioPresenter::editSigmoidEnabled() const noexcept
{
    return develop_.sigmoid_enabled;
}

double StudioPresenter::editSigmoidContrast() const noexcept
{
    return develop_.sigmoid_contrast;
}

double StudioPresenter::editSigmoidSkew() const noexcept
{
    return develop_.sigmoid_skew;
}

double StudioPresenter::editSigmoidHuePreservation() const noexcept
{
    return develop_.sigmoid_hue_preservation;
}

double StudioPresenter::editRawHighlights() const noexcept
{
    return develop_.raw_highlights;
}

double StudioPresenter::editHotPixelsStrength() const noexcept
{
    return develop_.hot_pixels_strength;
}

double StudioPresenter::editHotPixelsThreshold() const noexcept
{
    return develop_.hot_pixels_threshold;
}

bool StudioPresenter::editHotPixelsPermissive() const noexcept
{
    return develop_.hot_pixels_permissive;
}

int StudioPresenter::editRawCaIterations() const noexcept
{
    return static_cast<int>(develop_.raw_ca_iterations);
}

bool StudioPresenter::editRawCaAvoidShift() const noexcept
{
    return develop_.raw_ca_avoid_shift;
}

double StudioPresenter::editDenoise() const noexcept
{
    return develop_.denoise;
}

double StudioPresenter::editDenoiseChroma() const noexcept
{
    return develop_.denoise_chroma;
}

double StudioPresenter::editDenoiseRadius() const noexcept
{
    return develop_.denoise_radius;
}

double StudioPresenter::editLensK1() const noexcept
{
    return develop_.lens_k1;
}

double StudioPresenter::editLensVignetting() const noexcept
{
    return develop_.lens_vignetting;
}

double StudioPresenter::editLensMode() const noexcept
{
    return develop_.lens_mode == kLensModeLookup ? 1.0 : 0.0;
}

int StudioPresenter::editColorEqBand() const noexcept
{
    return static_cast<int>(develop_.color_eq_band);
}

double StudioPresenter::editColorEqHue() const noexcept
{
    return develop_.color_eq_hue[static_cast<std::size_t>(
        std::clamp(develop_.color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}

double StudioPresenter::editColorEqSat() const noexcept
{
    return develop_.color_eq_sat[static_cast<std::size_t>(
        std::clamp(develop_.color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}

double StudioPresenter::editColorEqLight() const noexcept
{
    return develop_.color_eq_light[static_cast<std::size_t>(
        std::clamp(develop_.color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}

double StudioPresenter::editGraduatedDensity() const noexcept
{
    return develop_.graduated_density;
}

double StudioPresenter::editGraduatedHardness() const noexcept
{
    return develop_.graduated_hardness;
}

double StudioPresenter::editGraduatedRotation() const noexcept
{
    return develop_.graduated_rotation;
}

double StudioPresenter::editGraduatedOffset() const noexcept
{
    return develop_.graduated_offset;
}

QVariantMap StudioPresenter::editGraduatedMask() const
{
    return develop_mask_editor_map(
        develop_mask_editor_state(develop_, DevelopMaskTarget::kGraduatedNd),
        DevelopMaskTarget::kGraduatedNd);
}

double StudioPresenter::editToneEqBlacks() const noexcept
{
    return develop_.tone_eq_blacks;
}

double StudioPresenter::editToneEqShadows() const noexcept
{
    return develop_.tone_eq_shadows;
}

double StudioPresenter::editToneEqMidtones() const noexcept
{
    return develop_.tone_eq_midtones;
}

double StudioPresenter::editToneEqHighlights() const noexcept
{
    return develop_.tone_eq_highlights;
}

double StudioPresenter::editToneEqWhites() const noexcept
{
    return develop_.tone_eq_whites;
}

QVariantList StudioPresenter::recipeHistory() const
{
    return recipe_history_;
}

bool StudioPresenter::cropToolActive() const noexcept
{
    return crop_tool_active_;
}

bool StudioPresenter::cropGuideReady() const noexcept
{
    return crop_guide_ready_;
}

void StudioPresenter::load_develop_for_selection()
{
    develop_ = {};
    saved_develop_ = {};
    undo_stack_.clear();
    redo_stack_.clear();
    recipe_history_.clear();
    if (selected_asset_id_.isEmpty())
    {
        emit editChanged();
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<Recipe> loaded = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                loaded = service_->load_recipe(asset_id);
                history = service_->list_recipe_history(asset_id);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, loaded = std::move(loaded), history = std::move(history)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    recipe_history_.clear();
                    if (history)
                    {
                        for (const auto &entry : history.value())
                        {
                            QVariantMap row;
                            row.insert(QStringLiteral("id"), QVariant::fromValue(entry.id));
                            row.insert(QStringLiteral("kind"), qstring_from_utf8(entry.kind));
                            row.insert(QStringLiteral("label"),
                                       entry.label ? qstring_from_utf8(*entry.label) : QString{});
                            row.insert(QStringLiteral("seq"), QVariant::fromValue(entry.seq));
                            recipe_history_.push_back(row);
                        }
                    }
                    if (!loaded)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        emit editChanged();
                        setError(qstring_from_utf8(loaded.error().message));
                        return;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (!params)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        emit editChanged();
                        setError(qstring_from_utf8(params.error().message));
                        return;
                    }
                    develop_ = params.value();
                    saved_develop_ = develop_;
                    emit editChanged();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::commit_develop(DevelopParams params, const bool push_history,
                                     const bool refresh_preview)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    clamp_develop(params);
    const auto previous = saved_develop_;
    if (push_history && params != saved_develop_)
    {
        undo_stack_.push_back(saved_develop_);
        if (undo_stack_.size() > 40U)
        {
            undo_stack_.erase(undo_stack_.begin());
        }
        redo_stack_.clear();
    }
    develop_ = params;
    emit editChanged();
    const bool crop_guides = crop_tool_active_ && !before_after_;
    preview_loading_ = refresh_preview;
    emit previewChanged();
    static_cast<void>(develop_preview_owner_.supersede("develop_save_superseded"));
    pending_save_ = PendingDevelopWork{
        .save = true,
        .params = params,
        .previous = previous,
        .push_history = push_history,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = crop_guides,
        .refresh_preview = refresh_preview,
        .overlay_mask_id = current_overlay_mask_id(params),
    };
    pending_preview_.reset();
    kick_develop_work();
}

[[nodiscard]] std::optional<std::string>
StudioPresenter::current_overlay_mask_id(const DevelopParams &params) const
{
    if (!mask_overlay_visible_ || before_after_)
    {
        return std::nullopt;
    }
    const auto &attachment = mask_overlay_target_ == QLatin1String("graduatednd") ?
                                 params.graduated_mask_id :
                                 params.color_harmonizer_mask_id;
    return attachment;
}

void StudioPresenter::preview_develop(DevelopParams params)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    clamp_develop(params);
    if (params == develop_)
    {
        return;
    }
    develop_ = params;
    emit editChanged();
    const bool crop_guides = crop_tool_active_ && !before_after_;
    static_cast<void>(develop_preview_owner_.supersede("interactive_preview_superseded"));
    pending_preview_ = PendingDevelopWork{
        .interactive = true,
        .params = params,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = crop_guides,
        .overlay_mask_id = current_overlay_mask_id(params),
    };
    kick_develop_work();
}

void StudioPresenter::enqueue_preview()
{
    if (selected_asset_id_.isEmpty())
    {
        preview_loading_ = false;
        emit previewChanged();
        return;
    }
    preview_loading_ = true;
    emit previewChanged();
    static_cast<void>(develop_preview_owner_.supersede("preview_superseded"));
    const bool crop_guides = crop_tool_active_ && !before_after_;
    pending_preview_ = PendingDevelopWork{
        .interactive = mask_overlay_visible_,
        .params = develop_,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = crop_guides,
        .overlay_mask_id = current_overlay_mask_id(develop_),
    };
    kick_develop_work();
}

void StudioPresenter::kick_develop_work()
{
    if (develop_job_in_flight_)
    {
        return;
    }
    PendingDevelopWork job;
    if (pending_save_.has_value())
    {
        job = *pending_save_;
        pending_save_.reset();
    }
    else if (pending_preview_.has_value())
    {
        job = *pending_preview_;
        pending_preview_.reset();
    }
    else
    {
        return;
    }
    develop_job_in_flight_ = true;
    const auto revision = develop_preview_owner_.revision();
    const auto cancellation = develop_preview_owner_.begin();
    executor_.post(
        [this, job, revision, cancellation]()
        {
            Result<AssetRecord> saved = make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool save_ok = !job.save;
            if (service_ != nullptr)
            {
                if (job.save)
                {
                    saved = service_->save_develop(job.asset_id, job.params);
                    save_ok = static_cast<bool>(saved);
                }
                if (save_ok && job.refresh_preview)
                {
                    PreviewRequest request;
                    request.asset_id = job.asset_id;
                    request.max_edge =
                        job.interactive ? kInteractivePreviewMaxEdge : kDefaultPreviewMaxEdge;
                    request.request_revision = revision;
                    request.ignore_edits = job.ignore_edits;
                    request.ignore_crop = job.ignore_crop;
                    request.ignore_straighten = job.ignore_straighten;
                    request.persist_preview_record = !job.interactive;
                    request.cancellation = cancellation;
                    if (job.overlay_mask_id)
                    {
                        request.overlay_mask_id = job.overlay_mask_id;
                        request.persist_preview_record = false;
                    }
                    preview = service_->request_preview(
                        request, job.interactive ? std::optional<DevelopParams>{job.params} :
                                                   std::optional<DevelopParams>{});
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, job, revision, saved = std::move(saved),
                 preview = std::move(preview)]() mutable
                {
                    develop_job_in_flight_ = false;
                    const bool selected_matches =
                        utf8_from_qstring(selected_asset_id_) == job.asset_id;
                    if (job.save)
                    {
                        if (!saved)
                        {
                            if (selected_matches && !pending_save_.has_value() &&
                                develop_ == job.params)
                            {
                                develop_ = job.previous;
                                saved_develop_ = job.previous;
                                if (job.push_history && !undo_stack_.empty())
                                {
                                    undo_stack_.pop_back();
                                }
                                preview_loading_ = false;
                                emit editChanged();
                                emit previewChanged();
                            }
                            setError(qstring_from_utf8(saved.error().message));
                            kick_develop_work();
                            return;
                        }
                        if (selected_matches)
                        {
                            saved_develop_ = job.params;
                            assets_.updateAsset(saved.value());
                            emit selectionChanged();
                            emit editChanged();
                        }
                    }
                    if (!develop_preview_owner_.accepts(revision, job.asset_id,
                                                        utf8_from_qstring(selected_asset_id_)))
                    {
                        kick_develop_work();
                        return;
                    }
                    if (job.save && !job.refresh_preview)
                    {
                        preview_loading_ = false;
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    preview_loading_ = false;
                    if (!preview)
                    {
                        if (preview.error().code == ErrorCode::kCancelled)
                        {
                            kick_develop_work();
                            return;
                        }
                        if (preview.error().code == ErrorCode::kNotFound)
                        {
                            assets_.markOriginalMissing(job.asset_id);
                            emit selectionChanged();
                        }
                        else
                        {
                            setError(qstring_from_utf8(preview.error().message));
                        }
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    if (preview.value().original_missing)
                    {
                        assets_.markOriginalMissing(job.asset_id);
                        emit selectionChanged();
                    }
                    if (job.ignore_crop && job.ignore_straighten && crop_tool_active_)
                    {
                        crop_guide_ready_ = true;
                    }
                    show_preview_result(preview.value(), revision);
                    emit previewChanged();
                    kick_develop_work();
                },
                Qt::QueuedConnection);
        });
}

void StudioPresenter::setDevelopNumber(const QString &name, const double value)
{
    DevelopParams next = develop_;
    const auto field = utf8_from_qstring(name);
    if (is_develop_mask_field(field))
    {
        auto applied = apply_develop_field_strict(next, field, value);
        if (!applied)
        {
            const auto reason = applied.error().context.find("reason");
            const auto reason_text = reason == applied.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            setError(QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                     QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    else if (!apply_develop_field(next, field, value))
    {
        return;
    }
    if (name == QLatin1String("straighten"))
    {
        fit_geometry_crop(next);
    }
    clamp_develop(next);
    if (next == saved_develop_ && next == develop_)
    {
        return;
    }
    if (next == saved_develop_)
    {
        develop_ = next;
        emit editChanged();
        enqueue_preview();
        return;
    }
    const bool keep_crop_guide =
        crop_tool_active_ && crop_guide_ready_ && name == QLatin1String("straighten");
    commit_develop(next, true, !keep_crop_guide);
}

void StudioPresenter::setToneCurve(const QVariantList &points)
{
    DevelopParams next = develop_;
    next.tone_curve = tone_curve_from_variant(points);
    clamp_develop(next);
    if (next == saved_develop_ && next == develop_)
    {
        return;
    }
    if (next == saved_develop_)
    {
        develop_ = next;
        emit editChanged();
        enqueue_preview();
        return;
    }
    commit_develop(next, true);
}

void StudioPresenter::previewToneCurve(const QVariantList &points)
{
    DevelopParams next = develop_;
    next.tone_curve = tone_curve_from_variant(points);
    preview_develop(next);
}

void StudioPresenter::previewDevelopNumber(const QString &name, const double value)
{
    DevelopParams next = develop_;
    const auto field = utf8_from_qstring(name);
    if (is_develop_mask_field(field))
    {
        auto applied = apply_develop_field_strict(next, field, value);
        if (!applied)
        {
            const auto reason = applied.error().context.find("reason");
            const auto reason_text = reason == applied.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            setError(QCoreApplication::translate("DevelopPanel", "Mask edit was rejected") +
                     QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    else if (!apply_develop_field(next, field, value))
    {
        return;
    }
    if (name == QLatin1String("straighten"))
    {
        fit_geometry_crop(next);
        if (crop_tool_active_)
        {
            clamp_develop(next);
            if (next == develop_)
            {
                return;
            }
            develop_ = next;
            emit editChanged();
            return;
        }
    }
    preview_develop(next);
}

void StudioPresenter::setCropRect(const double x, const double y, const double width,
                                  const double height)
{
    DevelopParams next = develop_;
    next.crop_x = x;
    next.crop_y = y;
    next.crop_width = width;
    next.crop_height = height;
    clamp_develop(next);
    constrain_geometry_crop(next);
    if (next == develop_)
    {
        return;
    }
    commit_develop(next, true);
}

void StudioPresenter::previewCropRect(const double x, const double y, const double width,
                                      const double height)
{
    DevelopParams next = develop_;
    next.crop_x = x;
    next.crop_y = y;
    next.crop_width = width;
    next.crop_height = height;
    clamp_develop(next);
    constrain_geometry_crop(next);
    if (next == develop_)
    {
        return;
    }
    develop_ = next;
    emit editChanged();
}

void StudioPresenter::setCropAspect(const QString &aspect)
{
    DevelopParams next = develop_;
    if (!apply_crop_aspect(next, utf8_from_qstring(aspect)))
    {
        return;
    }
    crop_aspect_ = aspect;
    fit_geometry_crop(next);
    if (next == develop_)
    {
        emit editChanged();
        return;
    }
    commit_develop(next, true);
}

void StudioPresenter::rotateLeft()
{
    DevelopParams next = develop_;
    next.rotate_quarters = (next.rotate_quarters + 3) % 4;
    transform_crop_for_quarter_turns(next, 3);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::rotateRight()
{
    DevelopParams next = develop_;
    next.rotate_quarters = (next.rotate_quarters + 1) % 4;
    transform_crop_for_quarter_turns(next, 1);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::flipHorizontal()
{
    DevelopParams next = develop_;
    next.flip_horizontal = next.flip_horizontal == 0 ? 1 : 0;
    transform_crop_for_flip(next, true, false);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::flipVertical()
{
    DevelopParams next = develop_;
    next.flip_vertical = next.flip_vertical == 0 ? 1 : 0;
    transform_crop_for_flip(next, false, true);
    fit_geometry_crop(next);
    commit_develop(next, true);
}

void StudioPresenter::setCropToolActive(const bool active)
{
    if (crop_tool_active_ == active)
    {
        return;
    }
    crop_tool_active_ = active;
    if (active)
    {
        setZoomMode(QStringLiteral("fit"));
        DevelopParams next = develop_;
        fit_geometry_crop(next);
        crop_guide_ready_ = std::abs(next.straighten_degrees) < 1e-4 && next.crop_width >= 0.999 &&
                            next.crop_height >= 0.999 && std::abs(next.crop_x) < 1e-6 &&
                            std::abs(next.crop_y) < 1e-6;
        if (next != develop_)
        {
            emit editChanged();
            emit previewChanged();
            commit_develop(next, true);
            return;
        }
    }
    else
    {
        crop_guide_ready_ = false;
    }
    emit editChanged();
    emit previewChanged();
    enqueue_preview();
}

void StudioPresenter::resetControl(const QString &name)
{
    DevelopParams next = develop_;
    const auto field = utf8_from_qstring(name);
    if (is_develop_mask_field(field))
    {
        auto reset = reset_develop_mask_field(next, field);
        if (!reset)
        {
            const auto reason = reset.error().context.find("reason");
            const auto reason_text = reason == reset.error().context.end() ?
                                         QStringLiteral("unknown") :
                                         qstring_from_utf8(reason->second);
            setError(QCoreApplication::translate("DevelopPanel", "Mask reset was rejected") +
                     QStringLiteral(" [") + reason_text + QStringLiteral("]"));
            return;
        }
    }
    else if (!reset_develop_field(next, field))
    {
        return;
    }
    if (name == QLatin1String("straighten"))
    {
        fit_geometry_crop(next);
    }
    commit_develop(next, true);
}

void StudioPresenter::resetSection(const QString &section)
{
    DevelopParams next = develop_;
    if (!reset_develop_section(next, utf8_from_qstring(section)))
    {
        return;
    }
    if (section == QLatin1String("geometry"))
    {
        crop_aspect_ = QStringLiteral("free");
    }
    commit_develop(next, true);
}

void StudioPresenter::resetAllEdits()
{
    crop_aspect_ = QStringLiteral("free");
    DevelopParams reset;
    reset.sigmoid_enabled = develop_.sigmoid_enabled;
    commit_develop(reset, true);
}

void StudioPresenter::undoEdit()
{
    if (undo_stack_.empty())
    {
        return;
    }
    redo_stack_.push_back(develop_);
    const auto previous = undo_stack_.back();
    undo_stack_.pop_back();
    commit_develop(previous, false);
}

void StudioPresenter::redoEdit()
{
    if (redo_stack_.empty())
    {
        return;
    }
    undo_stack_.push_back(develop_);
    const auto next = redo_stack_.back();
    redo_stack_.pop_back();
    commit_develop(next, false);
}

void StudioPresenter::toggleBeforeAfter()
{
    before_after_ = !before_after_;
    emit editChanged();
    enqueue_preview();
}

double StudioPresenter::selected_source_aspect() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && asset->width && asset->height && *asset->height > 0)
    {
        return static_cast<double>(*asset->width) / static_cast<double>(*asset->height);
    }
    return 1.5;
}

double StudioPresenter::selected_working_aspect() const
{
    if (crop_tool_active_)
    {
        const QMutexLocker lock(&preview_image_mutex_);
        if (!preview_image_.isNull() && preview_image_.height() > 0)
        {
            return static_cast<double>(preview_image_.width()) /
                   static_cast<double>(preview_image_.height());
        }
    }
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset && asset->width && asset->height && *asset->height > 0)
    {
        return working_image_aspect(develop_.rotate_quarters, selected_source_aspect());
    }
    const QMutexLocker lock(&preview_image_mutex_);
    if (!preview_image_.isNull() && preview_image_.height() > 0)
    {
        return static_cast<double>(preview_image_.width()) /
               static_cast<double>(preview_image_.height());
    }
    return working_image_aspect(develop_.rotate_quarters, selected_source_aspect());
}

void StudioPresenter::constrain_geometry_crop(DevelopParams &params) const
{
    constrain_crop_to_straighten(params, selected_working_aspect());
}

void StudioPresenter::fit_geometry_crop(DevelopParams &params) const
{
    fit_crop_to_straighten(params, selected_working_aspect());
}

} // namespace ravo
