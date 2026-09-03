#include "ravo/desktop/studio_presenter.h"

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

QVariantMap StudioPresenter::editCurve() const
{
    const bool rgb_family = curve_family_ == 0;
    const bool linked =
        rgb_family ? develop_.rgb_curve.mode != kRgbLevelsModeIndependent :
                     develop_.tone_curve_channel_mode != kToneCurveChannelModeIndependent &&
                         develop_.tone_curve_working_space != kToneCurveWorkingSpaceLabIndependent;
    QString histogram_mode = QStringLiteral("luma");
    if (rgb_family)
    {
        if (curve_channel_ == 1)
            histogram_mode = QStringLiteral("red");
        else if (curve_channel_ == 2)
            histogram_mode = QStringLiteral("green");
        else if (curve_channel_ == 3)
            histogram_mode = QStringLiteral("blue");
        else
            histogram_mode = QStringLiteral("rgb");
    }
    return {{QStringLiteral("familyIndex"), curve_family_},
            {QStringLiteral("channel"), curve_channel_},
            {QStringLiteral("linked"), linked},
            {QStringLiteral("histogramMode"), histogram_mode},
            {QStringLiteral("interpolationIndex"),
             curve_interpolation_index(rgb_family ? develop_.rgb_curve.interpolation :
                                                    develop_.tone_curve_interpolation)},
            {QStringLiteral("preserveIndex"),
             preserve_colors_index(rgb_family ? develop_.rgb_curve.preserve_colors :
                                                develop_.tone_curve_preserve_colors)},
            {QStringLiteral("compensate"), develop_.rgb_curve.compensate_middle_grey},
            {QStringLiteral("workingSpaceIndex"),
             working_space_index(develop_.tone_curve_working_space)},
            {QStringLiteral("channelModeIndex"),
             develop_.tone_curve_channel_mode == kToneCurveChannelModeIndependent ? 1 : 0},
            {QStringLiteral("parametricShadows"), develop_.rgb_curve.parametric_shadows},
            {QStringLiteral("parametricDarks"), develop_.rgb_curve.parametric_darks},
            {QStringLiteral("parametricLights"), develop_.rgb_curve.parametric_lights},
            {QStringLiteral("parametricHighlights"), develop_.rgb_curve.parametric_highlights},
            {QStringLiteral("split0"), develop_.rgb_curve.parametric_split_shadows},
            {QStringLiteral("split1"), develop_.rgb_curve.parametric_split_mid},
            {QStringLiteral("split2"), develop_.rgb_curve.parametric_split_highlights}};
}

QVariantList StudioPresenter::editCurvePoints() const
{
    return tone_curve_to_variant(curve_points_for(develop_, curve_family_, curve_channel_));
}

QVariantList StudioPresenter::editCurveSamples() const
{
    const auto interpolation =
        curve_family_ == 0 ? develop_.rgb_curve.interpolation : develop_.tone_curve_interpolation;
    if (curve_family_ == 0 && curve_channel_ <= 0 &&
        !rgb_curve_parametric_is_identity(develop_.rgb_curve))
    {
        constexpr int kSamples = 65;
        QVariantList samples;
        samples.reserve(kSamples);
        for (int index = 0; index < kSamples; ++index)
        {
            const double x = static_cast<double>(index) / static_cast<double>(kSamples - 1);
            samples.push_back(evaluate_tone_curve(
                develop_.rgb_curve.channels[0],
                evaluate_rgb_curve_parametric(develop_.rgb_curve, x), interpolation));
        }
        return samples;
    }
    return tone_curve_sample_list(curve_points_for(develop_, curve_family_, curve_channel_),
                                  interpolation);
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

int StudioPresenter::editDemosaicModeIndex() const noexcept
{
    if (develop_.demosaic_mode == kDemosaicModePpg)
    {
        return 1;
    }
    if (develop_.demosaic_mode == kDemosaicModeMarkesteijn1)
    {
        return 2;
    }
    if (develop_.demosaic_mode == kDemosaicModeMarkesteijn3)
    {
        return 3;
    }
    return 0;
}

double StudioPresenter::editRawHighlights() const noexcept
{
    return develop_.raw_highlights;
}

double StudioPresenter::editRawDenoiseThreshold() const noexcept
{
    return develop_.raw_denoise_threshold;
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

QVariantList StudioPresenter::editColorEqBands() const
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
            {QStringLiteral("hue"), develop_.color_eq_hue[i]},
            {QStringLiteral("sat"), develop_.color_eq_sat[i]},
            {QStringLiteral("light"), develop_.color_eq_light[i]}});
    }
    return bands;
}

bool StudioPresenter::whiteBalancePickActive() const noexcept
{
    return white_balance_pick_active_;
}

void StudioPresenter::setWhiteBalancePickActive(const bool active)
{
    const bool enabled = active && selectedMediaType() == QLatin1String("image/x-raw") &&
                         std::abs(develop_.straighten_degrees) <= 1.0e-4 &&
                         std::abs(develop_.perspective_vertical) <= 1.0e-4 &&
                         std::abs(develop_.perspective_horizontal) <= 1.0e-4 &&
                         std::abs(develop_.perspective_shear) <= 1.0e-4 && !develop_.canvas_enabled;
    if (white_balance_pick_active_ == enabled)
    {
        return;
    }
    const bool comparison_changed = enabled && clear_comparison();
    white_balance_pick_active_ = enabled;
    if (enabled)
    {
        setCropToolActive(false);
        if (mask_place_active_)
            setMaskPlaceActive(false);
        if (mask_parametric_assist_active_)
            setMaskParametricAssistActive(false);
    }
    emit editChanged();
    if (comparison_changed)
    {
        emit previewChanged();
    }
}

void StudioPresenter::pickWhiteBalance(const double preview_x, const double preview_y)
{
    if (selected_asset_id_.isEmpty() || service_ == nullptr)
    {
        return;
    }
    if (selectedMediaType() != QLatin1String("image/x-raw"))
    {
        setError(QCoreApplication::translate("DevelopPanel",
                                             "White-balance pick requires a Bayer RAW original"));
        setWhiteBalancePickActive(false);
        return;
    }
    if (std::abs(develop_.straighten_degrees) > 1.0e-4 ||
        std::abs(develop_.perspective_vertical) > 1.0e-4 ||
        std::abs(develop_.perspective_horizontal) > 1.0e-4 ||
        std::abs(develop_.perspective_shear) > 1.0e-4 || develop_.canvas_enabled)
    {
        setError(QCoreApplication::translate(
            "DevelopPanel", "White-balance pick is unavailable with Perspective or Canvas"));
        setWhiteBalancePickActive(false);
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    WhiteBalancePickRequest request;
    request.preview_x = preview_x;
    request.preview_y = preview_y;
    request.crop_x = develop_.crop_x;
    request.crop_y = develop_.crop_y;
    request.crop_width = develop_.crop_width;
    request.crop_height = develop_.crop_height;
    request.rotate_quarters = static_cast<int>(develop_.rotate_quarters);
    request.flip_horizontal = develop_.flip_horizontal != 0;
    request.flip_vertical = develop_.flip_vertical != 0;
    executor_.post(
        [this, asset_id, request]()
        {
            Result<std::array<double, 4>> sampled =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                sampled = service_->sample_white_balance(asset_id, request, CancellationToken{});
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
                        setError(qstring_from_utf8(sampled.error().message));
                        return;
                    }
                    DevelopParams next = develop_;
                    next.temperature.mode = std::string(kTemperatureModeManual);
                    next.temperature.coefficients = sampled.value();
                    if (mutate_develop(std::move(next), DevelopEdit::Commit))
                    {
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "White balance sampled."));
                    }
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

void StudioPresenter::autoPerspective(const QString &mode_name)
{
    PerspectiveAnalysisMode mode = PerspectiveAnalysisMode::kFull;
    if (mode_name == QLatin1String("vertical"))
        mode = PerspectiveAnalysisMode::kVertical;
    else if (mode_name == QLatin1String("horizontal"))
        mode = PerspectiveAnalysisMode::kHorizontal;
    else if (mode_name != QLatin1String("full"))
    {
        setError(QCoreApplication::translate("DevelopPanel",
                                             "Perspective analysis mode is unsupported"));
        return;
    }
    if (selected_asset_id_.isEmpty())
        return;
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    const DevelopParams analysis_develop = develop_;
    const auto revision = perspective_analysis_owner_.supersede("perspective_analysis_superseded");
    const auto cancellation = perspective_analysis_owner_.begin();
    executor_.post(
        [this, asset_id, revision, analysis_develop, mode, cancellation]() mutable
        {
            Result<PerspectiveAnalysis> analysis =
                make_error(ErrorCode::kIo, "Engine session is closed");
            if (service_ != nullptr && engine_)
            {
                PreviewRequest request;
                request.asset_id = asset_id;
                request.max_edge = 900U;
                request.request_revision = revision;
                request.ignore_crop = true;
                request.ignore_straighten = true;
                request.persist_preview_record = false;
                request.cancellation = cancellation;
                auto preview = service_->request_preview(request, analysis_develop);
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
                [this, asset_id, revision, analysis = std::move(analysis)]() mutable
                {
                    if (!perspective_analysis_owner_.accepts(revision, asset_id,
                                                             utf8_from_qstring(selected_asset_id_)))
                        return;
                    if (!analysis)
                    {
                        if (analysis.error().code != ErrorCode::kCancelled)
                            setError(qstring_from_utf8(analysis.error().message));
                        return;
                    }
                    DevelopParams next = develop_;
                    next.straighten_degrees = analysis.value().params.rotation_degrees;
                    next.perspective_vertical = analysis.value().params.vertical_shift;
                    next.perspective_horizontal = analysis.value().params.horizontal_shift;
                    next.perspective_shear = analysis.value().params.shear;
                    next.perspective_constrain_crop = true;
                    if (mutate_develop(std::move(next), DevelopEdit::Commit))
                        setStatus(QCoreApplication::translate("StudioPresenter",
                                                              "Perspective corrected."));
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
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

QVariantList StudioPresenter::editPresets() const
{
    return develop_presets_;
}

qlonglong StudioPresenter::activeHistoryId() const noexcept
{
    return static_cast<qlonglong>(active_history_id_);
}

qlonglong StudioPresenter::activeHistorySeq() const noexcept
{
    return static_cast<qlonglong>(active_history_seq_);
}

} // namespace ravo
