#include "ravo/desktop/studio_presenter.h"

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

bool StudioPresenter::beforeAfter() const noexcept
{
    return before_after_;
}

bool StudioPresenter::comparisonActive() const noexcept
{
    return comparison_active_;
}

bool StudioPresenter::canUndo() const noexcept
{
    return !undo_stack_.empty();
}

bool StudioPresenter::canRedo() const noexcept
{
    return !redo_stack_.empty();
}

bool StudioPresenter::hasCopiedParameters() const noexcept
{
    return copied_parameters_.has_value();
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
            {QStringLiteral("fourth"), coefficients[3]},
            {QStringLiteral("canPick"), selectedMediaType() == QLatin1String("image/x-raw") &&
                                            std::abs(develop_.straighten_degrees) <= 1.0e-4 &&
                                            std::abs(develop_.perspective_vertical) <= 1.0e-4 &&
                                            std::abs(develop_.perspective_horizontal) <= 1.0e-4 &&
                                            std::abs(develop_.perspective_shear) <= 1.0e-4 &&
                                            !develop_.canvas_enabled}};
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

QVariantMap StudioPresenter::editCanvas() const
{
    static constexpr std::array<const char *, 5> labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Green"), QT_TRANSLATE_NOOP("DevelopPanel", "Red"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Blue"), QT_TRANSLATE_NOOP("DevelopPanel", "Black"),
        QT_TRANSLATE_NOOP("DevelopPanel", "White")};
    QVariantList choices;
    for (std::size_t index = 0U; index < labels.size(); ++index)
    {
        const auto color = static_cast<CanvasColor>(index);
        choices.push_back(QVariantMap{
            {QStringLiteral("id"), qstring_from_utf8(canvas_color_name(color))},
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"), QCoreApplication::translate("DevelopPanel", labels[index])},
        });
    }
    return {{QStringLiteral("present"), develop_.canvas_present},
            {QStringLiteral("enabled"), develop_.canvas_enabled},
            {QStringLiteral("left"), develop_.canvas.percent_left},
            {QStringLiteral("right"), develop_.canvas.percent_right},
            {QStringLiteral("top"), develop_.canvas.percent_top},
            {QStringLiteral("bottom"), develop_.canvas.percent_bottom},
            {QStringLiteral("colorIndex"), static_cast<int>(develop_.canvas.color)},
            {QStringLiteral("colorChoices"), choices}};
}

bool StudioPresenter::editCanvasEnabled() const noexcept
{
    return develop_.canvas_enabled;
}

double StudioPresenter::editStraighten() const noexcept
{
    return develop_.straighten_degrees;
}

QVariantMap StudioPresenter::editPerspective() const
{
    return {{QStringLiteral("vertical"), develop_.perspective_vertical},
            {QStringLiteral("horizontal"), develop_.perspective_horizontal},
            {QStringLiteral("shear"), develop_.perspective_shear},
            {QStringLiteral("constrainCrop"), develop_.perspective_constrain_crop},
            {QStringLiteral("interpolationIndex"),
             static_cast<int>(develop_.perspective_interpolation_index)}};
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
    if (crop_aspect_ == QLatin1String("locked"))
    {
        if (locked_crop_ratio_ > 0.0)
        {
            return locked_crop_ratio_;
        }
        return develop_.crop_width / std::max(develop_.crop_height, 1e-6);
    }
    return 0.0;
}

int StudioPresenter::selectedWorkingWidth() const
{
    double width = 0.0;
    double height = 0.0;
    if (!working_source_size(width, height))
    {
        return 0;
    }
    return static_cast<int>(width);
}

int StudioPresenter::selectedWorkingHeight() const
{
    double width = 0.0;
    double height = 0.0;
    if (!working_source_size(width, height))
    {
        return 0;
    }
    return static_cast<int>(height);
}

double StudioPresenter::cropMinShortEdgePixels() const noexcept
{
    return kDevelopCropMinShortEdgePixels;
}

double StudioPresenter::cropMinShortEdgeFraction() const noexcept
{
    return kDevelopCropMinShortEdgeFraction;
}

void StudioPresenter::valid_crop_rect(double &x, double &y, double &width, double &height) const
{
    const double working_aspect = selected_working_aspect();
    const double ratio = cropAspectRatio() > 0.0 ?
                             cropAspectRatio() / std::max(working_aspect, 1e-6) :
                             develop_.crop_width / std::max(develop_.crop_height, 1e-6);
    inscribed_crop_for_straighten(0.0, working_aspect, ratio, x, y, width, height);
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

double StudioPresenter::editSharpenThreshold() const noexcept
{
    return develop_.sharpen_threshold;
}

QVariantMap StudioPresenter::editTexture() const
{
    return {{QStringLiteral("strength"), develop_.texture.strength},
            {QStringLiteral("detailThreshold"), develop_.texture.detail_threshold},
            {QStringLiteral("iterations"), static_cast<qlonglong>(develop_.texture.iterations)}};
}

QVariantMap StudioPresenter::editRetouch() const
{
    QVariantList regions;
    regions.reserve(static_cast<qsizetype>(develop_.retouch.regions.size()));
    for (std::size_t index = 0U; index < develop_.retouch.regions.size(); ++index)
    {
        const auto &region = develop_.retouch.regions[index];
        const auto mask = std::find_if(develop_.masks.begin(), develop_.masks.end(),
                                       [&region](const Mask &candidate)
                                       { return candidate.id == region.mask_id; });
        regions.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("maskId"), qstring_from_utf8(region.mask_id)},
            {QStringLiteral("maskKind"), mask == develop_.masks.end() ?
                                             QStringLiteral("missing") :
                                             qstring_from_utf8(mask_kind_name(mask->kind))},
            {QStringLiteral("mode"), qstring_from_utf8(retouch_mode_name(region.mode))},
            {QStringLiteral("opacity"), region.opacity},
            {QStringLiteral("scale"), static_cast<int>(region.scale)},
        });
    }
    return {{QStringLiteral("regionCount"), static_cast<int>(develop_.retouch.regions.size())},
            {QStringLiteral("regions"), regions},
            {QStringLiteral("numScales"), static_cast<int>(develop_.retouch.num_scales)},
            {QStringLiteral("maxRegions"), static_cast<int>(kRetouchMaxRegions)}};
}

double StudioPresenter::editClarity() const noexcept
{
    return develop_.clarity;
}

double StudioPresenter::editVignette() const noexcept
{
    return develop_.vignette;
}

QVariantMap StudioPresenter::editVignetteParams() const
{
    return {{QStringLiteral("amount"), develop_.vignette},
            {QStringLiteral("midpoint"), develop_.vignette_midpoint},
            {QStringLiteral("falloff"), develop_.vignette_falloff},
            {QStringLiteral("shape"), develop_.vignette_shape},
            {QStringLiteral("centerX"), develop_.vignette_center_x},
            {QStringLiteral("centerY"), develop_.vignette_center_y}};
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

double StudioPresenter::editDehazeDistance() const noexcept
{
    return develop_.dehaze_distance;
}

bool StudioPresenter::editDehazeAdaptive() const noexcept
{
    return develop_.dehaze_adaptive;
}

QVariantMap StudioPresenter::editOutputDither() const
{
    static constexpr std::array<const char *, kOutputDitherMethodCount> labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Random noise"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 1-bit B&W"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 1-bit RGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 2-bit gray"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 2-bit RGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 4-bit gray"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 4-bit RGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 6-bit gray"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 8-bit RGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg 16-bit RGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Floyd–Steinberg auto"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 2 levels"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 3 levels"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 4 levels"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 5 levels"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 6 levels"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 7 levels"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Posterize 8 levels"),
    };
    QVariantList choices;
    choices.reserve(static_cast<qsizetype>(labels.size()));
    for (std::size_t index = 0U; index < labels.size(); ++index)
    {
        const auto method = output_dither_method_from_index(static_cast<std::int64_t>(index));
        choices.push_back(QVariantMap{
            {QStringLiteral("id"),
             method ? qstring_from_utf8(output_dither_method_name(method.value())) : QString{}},
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"), QCoreApplication::translate("DevelopPanel", labels[index])},
        });
    }
    const auto method_index = output_dither_method_index(develop_.output_dither.method);
    return {
        {QStringLiteral("present"), develop_.output_dither_present},
        {QStringLiteral("enabled"), develop_.output_dither_enabled},
        {QStringLiteral("methodIndex"), static_cast<int>(method_index)},
        {QStringLiteral("methodChoices"), choices},
        {QStringLiteral("dampingDb"), develop_.output_dither.random_damping_db},
        {QStringLiteral("dampingMinimum"), kOutputDitherDampingMin},
        {QStringLiteral("dampingMaximum"), kOutputDitherDampingMax},
        {QStringLiteral("dampingVisible"),
         develop_.output_dither.method == OutputDitherMethod::kRandom},
    };
}

QVariantMap StudioPresenter::editOutputFrame() const
{
    static constexpr std::array<const char *, 3> orientation_labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Auto"), QT_TRANSLATE_NOOP("DevelopPanel", "Portrait"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Landscape")};
    static constexpr std::array<const char *, 5> basis_labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Auto"), QT_TRANSLATE_NOOP("DevelopPanel", "Width"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Height"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Shorter side"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Longer side")};
    QVariantList orientations;
    QVariantList bases;
    for (std::size_t index = 0U; index < orientation_labels.size(); ++index)
    {
        orientations.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"),
             QCoreApplication::translate("DevelopPanel", orientation_labels[index])},
        });
    }
    for (std::size_t index = 0U; index < basis_labels.size(); ++index)
    {
        bases.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"),
             QCoreApplication::translate("DevelopPanel", basis_labels[index])},
        });
    }
    const auto &frame = develop_.frame;
    return {{QStringLiteral("present"), develop_.frame_present},
            {QStringLiteral("enabled"), develop_.frame_enabled},
            {QStringLiteral("borderRed"), frame.border_color[0]},
            {QStringLiteral("borderGreen"), frame.border_color[1]},
            {QStringLiteral("borderBlue"), frame.border_color[2]},
            {QStringLiteral("aspect"), frame.aspect},
            {QStringLiteral("orientationIndex"), static_cast<int>(frame.orientation)},
            {QStringLiteral("orientationChoices"), orientations},
            {QStringLiteral("size"), frame.size},
            {QStringLiteral("positionH"), frame.position_h},
            {QStringLiteral("positionV"), frame.position_v},
            {QStringLiteral("lineSize"), frame.frame_size},
            {QStringLiteral("lineOffset"), frame.frame_offset},
            {QStringLiteral("lineRed"), frame.frame_color[0]},
            {QStringLiteral("lineGreen"), frame.frame_color[1]},
            {QStringLiteral("lineBlue"), frame.frame_color[2]},
            {QStringLiteral("basisIndex"), static_cast<int>(frame.basis)},
            {QStringLiteral("basisChoices"), bases}};
}

QVariantMap StudioPresenter::editWatermark() const
{
    static constexpr std::array<const char *, 9> labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Top left"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Top center"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Top right"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Center left"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Center"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Center right"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Bottom left"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Bottom center"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Bottom right"),
    };
    QVariantList alignments;
    alignments.reserve(static_cast<qsizetype>(labels.size()));
    for (std::size_t index = 0U; index < labels.size(); ++index)
    {
        alignments.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"), QCoreApplication::translate("DevelopPanel", labels[index])},
        });
    }
    const auto &watermark = develop_.watermark;
    return {{QStringLiteral("present"), develop_.watermark_present},
            {QStringLiteral("enabled"), develop_.watermark_enabled},
            {QStringLiteral("text"), qstring_from_utf8(watermark.text)},
            {QStringLiteral("red"), watermark.color[0]},
            {QStringLiteral("green"), watermark.color[1]},
            {QStringLiteral("blue"), watermark.color[2]},
            {QStringLiteral("opacity"), watermark.opacity},
            {QStringLiteral("scale"), watermark.scale_percent},
            {QStringLiteral("offsetX"), watermark.x_offset},
            {QStringLiteral("offsetY"), watermark.y_offset},
            {QStringLiteral("rotation"), watermark.rotation_degrees},
            {QStringLiteral("alignmentIndex"), static_cast<int>(watermark.alignment)},
            {QStringLiteral("alignmentChoices"), alignments}};
}

double StudioPresenter::editVelvia() const noexcept
{
    return develop_.velvia_enabled ? develop_.velvia.strength / 100.0 : 0.0;
}

QVariantMap StudioPresenter::editVelviaParams() const
{
    return {{QStringLiteral("present"), develop_.velvia_present},
            {QStringLiteral("enabled"), develop_.velvia_enabled},
            {QStringLiteral("masked"), develop_.velvia_mask_id.has_value()},
            {QStringLiteral("strength"), develop_.velvia.strength},
            {QStringLiteral("bias"), develop_.velvia.bias}};
}

QVariantMap StudioPresenter::editLut3d() const
{
    static constexpr std::array<const char *, 6> space_labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "sRGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Adobe RGB"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Rec. 709"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Linear Rec. 709"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Linear Rec. 2020"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Linear ProPhoto RGB"),
    };
    static constexpr std::array<const char *, 2> interpolation_labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Tetrahedral"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Trilinear"),
    };
    const auto index_of = [](const auto &values, const std::string_view selected)
    {
        const auto found = std::find(values.begin(), values.end(), selected);
        return found == values.end() ? 0 : static_cast<int>(std::distance(values.begin(), found));
    };
    QVariantList spaces;
    spaces.reserve(static_cast<qsizetype>(space_labels.size()));
    for (const char *label : space_labels)
        spaces.push_back(QCoreApplication::translate("DevelopPanel", label));
    QVariantList interpolations;
    interpolations.reserve(static_cast<qsizetype>(interpolation_labels.size()));
    for (const char *label : interpolation_labels)
        interpolations.push_back(QCoreApplication::translate("DevelopPanel", label));
    return {{QStringLiteral("present"), develop_.lut3d_present},
            {QStringLiteral("enabled"), develop_.lut3d_enabled},
            {QStringLiteral("filePath"), qstring_from_utf8(develop_.lut3d.file_path)},
            {QStringLiteral("hasFile"), !develop_.lut3d.file_path.empty()},
            {QStringLiteral("inputSpaceIndex"),
             index_of(kLut3dSelectableSpaces, develop_.lut3d.input_space)},
            {QStringLiteral("outputSpaceIndex"),
             index_of(kLut3dSelectableSpaces, develop_.lut3d.output_space)},
            {QStringLiteral("interpolationIndex"),
             index_of(kLut3dSelectableInterpolations, develop_.lut3d.interpolation)},
            {QStringLiteral("strength"), develop_.lut3d.strength},
            {QStringLiteral("spaceChoices"), spaces},
            {QStringLiteral("interpolationChoices"), interpolations}};
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

QVariantMap StudioPresenter::editColorReconstruction() const
{
    const auto &params = develop_.color_reconstruction;
    const QStringList precedence_choices{
        QCoreApplication::translate("DevelopPanel", "None"),
        QCoreApplication::translate("DevelopPanel", "Saturated colors"),
        QCoreApplication::translate("DevelopPanel", "Hue"),
    };
    return {{QStringLiteral("enabled"), develop_.color_reconstruction_enabled},
            {QStringLiteral("threshold"), params.threshold},
            {QStringLiteral("spatial"), params.spatial},
            {QStringLiteral("range"), params.range},
            {QStringLiteral("hueDegrees"), params.hue * 360.0},
            {QStringLiteral("precedenceIndex"), static_cast<int>(params.precedence)},
            {QStringLiteral("precedenceChoices"), precedence_choices}};
}

QVariantMap StudioPresenter::editColorZones() const
{
    static constexpr std::array<const char *, 3> channel_labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Lightness"), QT_TRANSLATE_NOOP("DevelopPanel", "Chroma"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Hue")};
    static constexpr std::array<const char *, 3> interpolation_labels{
        QT_TRANSLATE_NOOP("DevelopPanel", "Cubic spline"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Catmull–Rom"),
        QT_TRANSLATE_NOOP("DevelopPanel", "Monotone Hermite")};
    QVariantList channels;
    QVariantList interpolations;
    QVariantList bands;
    for (std::size_t index = 0U; index < channel_labels.size(); ++index)
    {
        channels.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"),
             QCoreApplication::translate("DevelopPanel", channel_labels[index])},
        });
        interpolations.push_back(QVariantMap{
            {QStringLiteral("index"), static_cast<int>(index)},
            {QStringLiteral("label"),
             QCoreApplication::translate("DevelopPanel", interpolation_labels[index])},
        });
    }
    for (int index = 0; index < static_cast<int>(kColorEqualizerBandCount); ++index)
        bands.push_back(QVariantMap{{QStringLiteral("index"), index},
                                    {QStringLiteral("label"), QString::number(index + 1)}});
    const auto &params = develop_.color_zones;
    const std::size_t band = static_cast<std::size_t>(
        std::clamp(develop_.color_zones_band, std::int64_t{0}, std::int64_t{7}));
    const bool editable =
        std::all_of(params.curves.begin(), params.curves.end(), [](const ColorZonesCurve &curve)
                    { return curve.points.size() == kColorEqualizerBandCount; });
    const auto value = [&](const std::size_t channel)
    { return editable ? params.curves[channel].points[band].y : 0.5; };
    return {
        {QStringLiteral("present"), develop_.color_zones_present},
        {QStringLiteral("enabled"), develop_.color_zones_enabled},
        {QStringLiteral("editable"), editable},
        {QStringLiteral("masked"), develop_.color_zones_mask_id.has_value()},
        {QStringLiteral("selectByIndex"), static_cast<int>(params.select_by)},
        {QStringLiteral("selectByChoices"), channels},
        {QStringLiteral("bandIndex"), static_cast<int>(band)},
        {QStringLiteral("bandChoices"), bands},
        {QStringLiteral("lightness"), value(0U)},
        {QStringLiteral("chroma"), value(1U)},
        {QStringLiteral("hue"), value(2U)},
        {QStringLiteral("lightnessInterpolationIndex"),
         static_cast<int>(params.curves[0].interpolation)},
        {QStringLiteral("chromaInterpolationIndex"),
         static_cast<int>(params.curves[1].interpolation)},
        {QStringLiteral("hueInterpolationIndex"), static_cast<int>(params.curves[2].interpolation)},
        {QStringLiteral("interpolationChoices"), interpolations},
        {QStringLiteral("strength"), params.strength}};
}

} // namespace ravo
