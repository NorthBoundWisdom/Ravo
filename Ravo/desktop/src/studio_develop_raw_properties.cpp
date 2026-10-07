#include "ravo/desktop/studio_develop_presenter.h"
#include "ravo/services/preview_service.h"

#include "studio_develop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <numbers>
#include <set>
#include <string_view>
#include <utility>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <QMetaObject>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "studio_debug_info.h"
#include "studio_qt.h"

namespace ravo
{
using studio_develop_internal::develop_mask_editor_map;

namespace
{

[[nodiscard]] int preserve_colors_index(const std::string &name) noexcept
{
    static const std::array<std::string_view, 7> names{
        kToneCurvePreserveColorsNone, kToneCurvePreserveColorsLuminance,
        kToneCurvePreserveColorsMax,  kToneCurvePreserveColorsAverage,
        kToneCurvePreserveColorsSum,  kToneCurvePreserveColorsNorm,
        kToneCurvePreserveColorsPower};
    for (int index = 0; index < static_cast<int>(names.size()); ++index)
    {
        if (name == names[static_cast<std::size_t>(index)])
        {
            return index;
        }
    }
    return 1;
}

[[nodiscard]] int working_space_index(const std::string &name) noexcept
{
    if (name == kToneCurveWorkingSpaceLab)
        return 1;
    if (name == kToneCurveWorkingSpaceXyz)
        return 2;
    if (name == kToneCurveWorkingSpaceLabIndependent)
        return 3;
    if (name == kToneCurveWorkingSpaceSrgb)
        return 4;
    if (name == kToneCurveWorkingSpaceLinearRgb)
        return 5;
    return 0;
}

[[nodiscard]] const std::vector<ToneCurvePoint> &
curve_points_for(const DevelopParams &params, const int family, const int channel)
{
    if (family == 0)
    {
        const auto index = channel <= 0 ? 0 : std::clamp(channel - 1, 0, 2);
        return params.rgb_curve.channels[static_cast<std::size_t>(index)];
    }
    if (channel == 1)
        return params.tone_curve_a;
    if (channel == 2)
        return params.tone_curve_b;
    return params.tone_curve;
}

} // namespace

QVariantMap StudioDevelopPresenter::editCurve() const
{
    const bool rgb_family = state_.curve_family_ == 0;
    const bool linked =
        rgb_family ?
            edit_develop().rgb_curve.mode != kRgbLevelsModeIndependent :
            edit_develop().tone_curve_channel_mode != kToneCurveChannelModeIndependent &&
                edit_develop().tone_curve_working_space != kToneCurveWorkingSpaceLabIndependent;
    QString histogram_mode = QStringLiteral("luma");
    if (rgb_family)
    {
        if (state_.curve_channel_ == 1)
            histogram_mode = QStringLiteral("red");
        else if (state_.curve_channel_ == 2)
            histogram_mode = QStringLiteral("green");
        else if (state_.curve_channel_ == 3)
            histogram_mode = QStringLiteral("blue");
        else
            histogram_mode = QStringLiteral("rgb");
    }
    return {
        {QStringLiteral("familyIndex"), state_.curve_family_},
        {QStringLiteral("channel"), state_.curve_channel_},
        {QStringLiteral("linked"), linked},
        {QStringLiteral("histogramMode"), histogram_mode},
        {QStringLiteral("interpolationIndex"),
         curve_interpolation_index(rgb_family ? edit_develop().rgb_curve.interpolation :
                                                edit_develop().tone_curve_interpolation)},
        {QStringLiteral("preserveIndex"),
         preserve_colors_index(rgb_family ? edit_develop().rgb_curve.preserve_colors :
                                            edit_develop().tone_curve_preserve_colors)},
        {QStringLiteral("compensate"), edit_develop().rgb_curve.compensate_middle_grey},
        {QStringLiteral("workingSpaceIndex"),
         working_space_index(edit_develop().tone_curve_working_space)},
        {QStringLiteral("channelModeIndex"),
         edit_develop().tone_curve_channel_mode == kToneCurveChannelModeIndependent ? 1 : 0},
        {QStringLiteral("parametricShadows"), edit_develop().rgb_curve.parametric_shadows},
        {QStringLiteral("parametricDarks"), edit_develop().rgb_curve.parametric_darks},
        {QStringLiteral("parametricLights"), edit_develop().rgb_curve.parametric_lights},
        {QStringLiteral("parametricHighlights"), edit_develop().rgb_curve.parametric_highlights},
        {QStringLiteral("split0"), edit_develop().rgb_curve.parametric_split_shadows},
        {QStringLiteral("split1"), edit_develop().rgb_curve.parametric_split_mid},
        {QStringLiteral("split2"), edit_develop().rgb_curve.parametric_split_highlights}};
}

QVariantList StudioDevelopPresenter::editCurvePoints() const
{
    return tone_curve_to_variant(
        curve_points_for(edit_develop(), state_.curve_family_, state_.curve_channel_));
}

QVariantList StudioDevelopPresenter::editCurveSamples() const
{
    const auto interpolation = state_.curve_family_ == 0 ? edit_develop().rgb_curve.interpolation :
                                                           edit_develop().tone_curve_interpolation;
    if (state_.curve_family_ == 0 && state_.curve_channel_ <= 0 &&
        !rgb_curve_parametric_is_identity(edit_develop().rgb_curve))
    {
        constexpr int kSamples = 65;
        QVariantList samples;
        samples.reserve(kSamples);
        for (int index = 0; index < kSamples; ++index)
        {
            const double x = static_cast<double>(index) / static_cast<double>(kSamples - 1);
            samples.push_back(evaluate_tone_curve(
                edit_develop().rgb_curve.channels[0],
                evaluate_rgb_curve_parametric(edit_develop().rgb_curve, x), interpolation));
        }
        return samples;
    }
    return tone_curve_sample_list(
        curve_points_for(edit_develop(), state_.curve_family_, state_.curve_channel_),
        interpolation);
}

bool StudioDevelopPresenter::editSigmoidEnabled() const noexcept
{
    return edit_develop().sigmoid_enabled;
}

double StudioDevelopPresenter::editSigmoidContrast() const noexcept
{
    return edit_develop().sigmoid_contrast;
}

double StudioDevelopPresenter::editSigmoidSkew() const noexcept
{
    return edit_develop().sigmoid_skew;
}

double StudioDevelopPresenter::editSigmoidHuePreservation() const noexcept
{
    return edit_develop().sigmoid_hue_preservation;
}

int StudioDevelopPresenter::editDemosaicModeIndex() const noexcept
{
    if (state_.develop_.demosaic_mode == kDemosaicModePpg)
    {
        return 1;
    }
    if (state_.develop_.demosaic_mode == kDemosaicModeMarkesteijn1)
    {
        return 2;
    }
    if (state_.develop_.demosaic_mode == kDemosaicModeMarkesteijn3)
    {
        return 3;
    }
    return 0;
}

double StudioDevelopPresenter::editRawHighlights() const noexcept
{
    return state_.develop_.raw_highlights;
}

double StudioDevelopPresenter::editRawDenoiseThreshold() const noexcept
{
    return state_.develop_.raw_denoise_threshold;
}

double StudioDevelopPresenter::editHotPixelsStrength() const noexcept
{
    return state_.develop_.hot_pixels_strength;
}

double StudioDevelopPresenter::editHotPixelsThreshold() const noexcept
{
    return state_.develop_.hot_pixels_threshold;
}

bool StudioDevelopPresenter::editHotPixelsPermissive() const noexcept
{
    return state_.develop_.hot_pixels_permissive;
}

int StudioDevelopPresenter::editRawCaIterations() const noexcept
{
    return static_cast<int>(state_.develop_.raw_ca_iterations);
}

bool StudioDevelopPresenter::editRawCaAvoidShift() const noexcept
{
    return state_.develop_.raw_ca_avoid_shift;
}

double StudioDevelopPresenter::editDenoise() const noexcept
{
    return edit_develop().denoise;
}

double StudioDevelopPresenter::editDenoiseChroma() const noexcept
{
    return edit_develop().denoise_chroma;
}

double StudioDevelopPresenter::editDenoiseRadius() const noexcept
{
    return edit_develop().denoise_radius;
}

double StudioDevelopPresenter::editLensK1() const noexcept
{
    return state_.develop_.lens_k1;
}

double StudioDevelopPresenter::editLensVignetting() const noexcept
{
    return state_.develop_.lens_vignetting;
}

double StudioDevelopPresenter::editLensMode() const noexcept
{
    return state_.develop_.lens_mode == kLensModeLookup ? 1.0 : 0.0;
}

int StudioDevelopPresenter::editColorEqBand() const noexcept
{
    return static_cast<int>(edit_develop().color_eq_band);
}

double StudioDevelopPresenter::editColorEqHue() const noexcept
{
    return edit_develop().color_eq_hue[static_cast<std::size_t>(
        std::clamp(edit_develop().color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}

double StudioDevelopPresenter::editColorEqSat() const noexcept
{
    return edit_develop().color_eq_sat[static_cast<std::size_t>(
        std::clamp(edit_develop().color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}

double StudioDevelopPresenter::editColorEqLight() const noexcept
{
    return edit_develop().color_eq_light[static_cast<std::size_t>(
        std::clamp(edit_develop().color_eq_band, std::int64_t{0}, std::int64_t{7}))];
}

QVariantList StudioDevelopPresenter::editColorEqBands() const
{
    static const char *titles[] = {
        QT_TRANSLATE_NOOP("DevelopPanel", "Red"),    QT_TRANSLATE_NOOP("DevelopPanel", "Orange"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Yellow"), QT_TRANSLATE_NOOP("DevelopPanel", "Green"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Aqua"),   QT_TRANSLATE_NOOP("DevelopPanel", "Blue"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Purple"), QT_TRANSLATE_NOOP("DevelopPanel", "Magenta")};
    QVariantList bands;
    for (int index = 0; index < static_cast<int>(kColorEqualizerBandCount); ++index)
    {
        const auto i = static_cast<std::size_t>(index);
        bands.push_back(QVariantMap{
            {QStringLiteral("index"), index},
            {QStringLiteral("title"), QCoreApplication::translate("DevelopPanel", titles[index])},
            {QStringLiteral("hueField"), QStringLiteral("colorEqHue%1").arg(index)},
            {QStringLiteral("satField"), QStringLiteral("colorEqSat%1").arg(index)},
            {QStringLiteral("lightField"), QStringLiteral("colorEqLight%1").arg(index)},
            {QStringLiteral("hue"), edit_develop().color_eq_hue[i]},
            {QStringLiteral("sat"), edit_develop().color_eq_sat[i]},
            {QStringLiteral("light"), edit_develop().color_eq_light[i]}});
    }
    return bands;
}

bool StudioDevelopPresenter::whiteBalancePickActive() const noexcept
{
    return state_.white_balance_pick_active_;
}

void StudioDevelopPresenter::setWhiteBalancePickActive(const bool active)
{
    if (active && localEditing())
    {
        emit errorOccurred(QCoreApplication::translate(
            "DevelopPanel", "Finish mask editing before using global tools."));
        return;
    }
    const bool enabled = active && host_.selected_media_type() == QLatin1String("image/x-raw") &&
                         std::abs(state_.develop_.straighten_degrees) <= 1.0e-4 &&
                         std::abs(state_.develop_.perspective_vertical) <= 1.0e-4 &&
                         std::abs(state_.develop_.perspective_horizontal) <= 1.0e-4 &&
                         std::abs(state_.develop_.perspective_shear) <= 1.0e-4 &&
                         !state_.develop_.canvas_enabled;
    if (state_.white_balance_pick_active_ == enabled)
    {
        return;
    }
    const bool comparison_changed = enabled && clear_comparison();
    state_.white_balance_pick_active_ = enabled;
    if (enabled)
    {
        setCropToolActive(false);
        if (state_.mask_place_active_)
            setMaskPlaceActive(false);
        if (state_.mask_parametric_assist_active_)
            setMaskParametricAssistActive(false);
    }
    emit editChanged();
    if (comparison_changed)
    {
        emit previewChanged();
    }
}

void StudioDevelopPresenter::pickWhiteBalance(const double preview_x, const double preview_y)
{
    if (selected_asset_id_.isEmpty() || host_.develop_service() == nullptr)
    {
        return;
    }
    if (host_.selected_media_type() != QLatin1String("image/x-raw"))
    {
        emit errorOccurred(QCoreApplication::translate(
            "DevelopPanel", "White-balance pick requires a Bayer RAW original"));
        setWhiteBalancePickActive(false);
        return;
    }
    if (std::abs(state_.develop_.straighten_degrees) > 1.0e-4 ||
        std::abs(state_.develop_.perspective_vertical) > 1.0e-4 ||
        std::abs(state_.develop_.perspective_horizontal) > 1.0e-4 ||
        std::abs(state_.develop_.perspective_shear) > 1.0e-4 || state_.develop_.canvas_enabled)
    {
        emit errorOccurred(QCoreApplication::translate(
            "DevelopPanel", "White-balance pick is unavailable with Perspective or Canvas"));
        setWhiteBalancePickActive(false);
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    WhiteBalancePickRequest request;
    request.preview_x = preview_x;
    request.preview_y = preview_y;
    request.crop_x = state_.develop_.crop_x;
    request.crop_y = state_.develop_.crop_y;
    request.crop_width = state_.develop_.crop_width;
    request.crop_height = state_.develop_.crop_height;
    request.rotate_quarters = static_cast<int>(state_.develop_.rotate_quarters);
    request.flip_horizontal = state_.develop_.flip_horizontal != 0;
    request.flip_vertical = state_.develop_.flip_vertical != 0;
    executor_.post(
        [this, asset_id, request]()
        {
            Result<std::array<double, 4>> sampled =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (host_.develop_service() != nullptr)
            {
                sampled = host_.develop_service()->sample_white_balance(asset_id, request,
                                                                        CancellationToken{});
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, sampled = std::move(sampled)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    setWhiteBalancePickActive(false);
                    if (!sampled)
                    {
                        emit errorOccurred(qstring_from_utf8(sampled.error().message));
                        return;
                    }
                    DevelopParams next = state_.develop_;
                    next.temperature.mode = std::string(kTemperatureModeManual);
                    next.temperature.coefficients = sampled.value();
                    if (mutate_develop(std::move(next), DevelopEdit::Commit))
                    {
                        emit statusOccurred(QCoreApplication::translate("StudioPresenter",
                                                                        "White balance sampled."));
                    }
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

void StudioDevelopPresenter::autoPerspective(const QString &mode_name)
{
    PerspectiveAnalysisMode mode = PerspectiveAnalysisMode::kFull;
    const bool level_only = mode_name == QLatin1String("level");
    if (mode_name == QLatin1String("vertical"))
        mode = PerspectiveAnalysisMode::kVertical;
    else if (level_only)
        mode = PerspectiveAnalysisMode::kLevel;
    else if (mode_name == QLatin1String("horizontal"))
        mode = PerspectiveAnalysisMode::kHorizontal;
    else if (mode_name != QLatin1String("full"))
    {
        emit errorOccurred(QCoreApplication::translate("DevelopPanel",
                                                       "Perspective analysis mode is unsupported"));
        return;
    }
    if (selected_asset_id_.isEmpty())
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const DevelopParams analysis_develop = state_.develop_;
    const auto revision =
        state_.perspective_analysis_owner_.supersede("perspective_analysis_superseded");
    const auto cancellation = state_.perspective_analysis_owner_.begin();
    executor_.post(
        [this, asset_id, revision, analysis_develop, mode, level_only, cancellation]() mutable
        {
            Result<PerspectiveAnalysis> analysis =
                make_error(ErrorCode::kIo, "Engine session is closed");
            if (host_.develop_service() != nullptr && engine_)
            {
                PreviewRequest request;
                request.asset_id = asset_id;
                request.max_edge = 900U;
                request.request_revision = revision;
                request.ignore_crop = true;
                request.ignore_straighten = true;
                request.persist_preview_record = false;
                request.cancellation = cancellation;
                auto preview = host_.preview_service()->request_preview(request, analysis_develop);
                if (!preview)
                {
                    analysis = preview.error();
                }
                else
                {
                    RasterBuffer raster;
                    raster.width = preview.value().width;
                    raster.height = preview.value().height;
                    raster.source_width = raster.width;
                    raster.source_height = raster.height;
                    raster.srgb = std::move(preview).value().rgb;
                    const std::size_t expected =
                        static_cast<std::size_t>(raster.width) * raster.height * 3U;
                    if (raster.width == 0U || raster.height == 0U || raster.srgb.size() != expected)
                    {
                        analysis =
                            make_error(ErrorCode::kValidation,
                                       "Perspective analysis render has an invalid RGB extent",
                                       {{"reason", "invalid_perspective_analysis_raster"}});
                    }
                    else
                    {
                        analysis = engine_->analyze_perspective(raster, mode, cancellation);
                    }
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, revision, level_only, analysis = std::move(analysis)]() mutable
                {
                    if (!state_.perspective_analysis_owner_.accepts(
                            revision, asset_id, utf8_from_qstring(selected_asset_id_)))
                        return;
                    if (!analysis)
                    {
                        if (analysis.error().code != ErrorCode::kCancelled)
                            emit errorOccurred(qstring_from_utf8(analysis.error().message));
                        return;
                    }
                    DevelopParams next = state_.develop_;
                    next.straighten_degrees = analysis.value().params.rotation_degrees;
                    if (!level_only)
                    {
                        next.perspective_vertical = analysis.value().params.vertical_shift;
                        next.perspective_horizontal = analysis.value().params.horizontal_shift;
                        next.perspective_shear = analysis.value().params.shear;
                        next.perspective_constrain_crop = true;
                    }
                    if (mutate_develop(std::move(next), DevelopEdit::Commit))
                        emit statusOccurred(QCoreApplication::translate("StudioPresenter",
                                                                        "Perspective corrected."));
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

double StudioDevelopPresenter::editGraduatedDensity() const noexcept
{
    return edit_develop().graduated_density;
}

double StudioDevelopPresenter::editGraduatedHardness() const noexcept
{
    return edit_develop().graduated_hardness;
}

double StudioDevelopPresenter::editGraduatedRotation() const noexcept
{
    return edit_develop().graduated_rotation;
}

double StudioDevelopPresenter::editGraduatedOffset() const noexcept
{
    return edit_develop().graduated_offset;
}

QVariantMap StudioDevelopPresenter::editGraduatedMask() const
{
    return develop_mask_editor_map(
        develop_mask_editor_state(edit_develop(), DevelopMaskTarget::kGraduatedNd),
        DevelopMaskTarget::kGraduatedNd);
}

double StudioDevelopPresenter::editToneEqBlacks() const noexcept
{
    return edit_develop().tone_eq_blacks;
}

double StudioDevelopPresenter::editToneEqShadows() const noexcept
{
    return edit_develop().tone_eq_shadows;
}

double StudioDevelopPresenter::editToneEqMidtones() const noexcept
{
    return edit_develop().tone_eq_midtones;
}

double StudioDevelopPresenter::editToneEqHighlights() const noexcept
{
    return edit_develop().tone_eq_highlights;
}

double StudioDevelopPresenter::editToneEqWhites() const noexcept
{
    return edit_develop().tone_eq_whites;
}

QVariantList StudioDevelopPresenter::recipeHistory() const
{
    return state_.recipe_history_;
}

QVariantList StudioDevelopPresenter::editPresets() const
{
    return state_.develop_presets_;
}

qlonglong StudioDevelopPresenter::activeHistoryId() const noexcept
{
    return static_cast<qlonglong>(state_.active_history_id_);
}

qlonglong StudioDevelopPresenter::activeHistorySeq() const noexcept
{
    return static_cast<qlonglong>(state_.active_history_seq_);
}

} // namespace ravo
