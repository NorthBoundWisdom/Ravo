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
    const auto shape_kind = state.kind_index == 6 ? state.child_kind_index : state.kind_index;
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
                             QStringLiteral("pointDensity"),
                             prefix + QStringLiteral("PointDensity"), kCanonicalMaskUnitMin,
                             kCanonicalMaskUnitMax, 0.01, 1.0, 2, kind_is(8)),
        develop_mask_control(QCoreApplication::translate("DevelopPanel", "Child opacity"),
                             QStringLiteral("childOpacity"),
                             prefix + QStringLiteral("ChildOpacity"), kCanonicalMaskUnitMin,
                             kCanonicalMaskUnitMax, 0.01, 1.0, 2, state.kind_index == 6)};

    const QStringList kind_choices{develop_mask_kind_label("none"),
                                   develop_mask_kind_label("all"),
                                   develop_mask_kind_label("linear_gradient"),
                                   develop_mask_kind_label("circle"),
                                   develop_mask_kind_label("ellipse"),
                                   develop_mask_kind_label("parametric"),
                                   develop_mask_kind_label("group"),
                                   develop_mask_kind_label("path"),
                                   develop_mask_kind_label("brush")};
    const QStringList child_kind_choices{
        develop_mask_kind_label("all"),        develop_mask_kind_label("linear_gradient"),
        develop_mask_kind_label("circle"),     develop_mask_kind_label("ellipse"),
        develop_mask_kind_label("parametric"), develop_mask_kind_label("path"),
        develop_mask_kind_label("brush")};
    const QStringList operator_choices{QCoreApplication::translate("DevelopPanel", "Replace"),
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
    const bool comparison_changed = visible && clear_comparison();
    const bool changed = mask_overlay_visible_ != visible || mask_overlay_target_ != normalized;
    mask_overlay_visible_ = visible;
    mask_overlay_target_ = normalized;
    if (!changed && !comparison_changed)
    {
        return;
    }
    if (comparison_changed)
    {
        emit editChanged();
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
    return develop_.monochrome.mix;
}

QVariantMap StudioPresenter::editMonochromeFilter() const
{
    const auto &params = develop_.monochrome;
    return {{QStringLiteral("present"), develop_.monochrome_present},
            {QStringLiteral("enabled"), develop_.monochrome_enabled},
            {QStringLiteral("masked"), develop_.monochrome_mask_id.has_value()},
            {QStringLiteral("filterA"), params.filter_a},
            {QStringLiteral("filterB"), params.filter_b},
            {QStringLiteral("size"), params.size},
            {QStringLiteral("highlights"), params.highlights},
            {QStringLiteral("mix"), params.mix}};
}

double StudioPresenter::editSplitShadowsHue() const noexcept
{
    return develop_.split_toning.shadow_hue;
}

double StudioPresenter::editSplitHighlightsHue() const noexcept
{
    return develop_.split_toning.highlight_hue;
}

double StudioPresenter::editSplitBalance() const noexcept
{
    return develop_.split_toning.balance;
}

double StudioPresenter::editSplitAmount() const noexcept
{
    return develop_.split_toning.mix;
}

QVariantMap StudioPresenter::editSplitToning() const
{
    const auto &params = develop_.split_toning;
    return {{QStringLiteral("present"), develop_.split_toning_present},
            {QStringLiteral("enabled"), develop_.split_toning_enabled},
            {QStringLiteral("masked"), develop_.split_toning_mask_id.has_value()},
            {QStringLiteral("shadowSaturation"), params.shadow_saturation},
            {QStringLiteral("highlightSaturation"), params.highlight_saturation},
            {QStringLiteral("compress"), params.compress},
            {QStringLiteral("mix"), params.mix}};
}

double StudioPresenter::editGamma() const noexcept
{
    return develop_.gamma;
}

QVariantMap StudioPresenter::editRgbLevels() const
{
    const auto &params = develop_.rgb_levels;
    int preserve_index = 1;
    const std::array<std::string_view, 7> names{
        kToneCurvePreserveColorsNone, kToneCurvePreserveColorsLuminance,
        kToneCurvePreserveColorsMax,  kToneCurvePreserveColorsAverage,
        kToneCurvePreserveColorsSum,  kToneCurvePreserveColorsNorm,
        kToneCurvePreserveColorsPower};
    for (int index = 0; index < static_cast<int>(names.size()); ++index)
    {
        if (params.preserve_colors == names[static_cast<std::size_t>(index)])
        {
            preserve_index = index;
            break;
        }
    }
    return {{QStringLiteral("modeIndex"), params.mode == kRgbLevelsModeIndependent ? 1 : 0},
            {QStringLiteral("preserveIndex"), preserve_index},
            {QStringLiteral("black"), params.levels[0][0]},
            {QStringLiteral("grey"), params.levels[0][1]},
            {QStringLiteral("white"), params.levels[0][2]},
            {QStringLiteral("blackG"), params.levels[1][0]},
            {QStringLiteral("greyG"), params.levels[1][1]},
            {QStringLiteral("whiteG"), params.levels[1][2]},
            {QStringLiteral("blackB"), params.levels[2][0]},
            {QStringLiteral("greyB"), params.levels[2][1]},
            {QStringLiteral("whiteB"), params.levels[2][2]}};
}

QVariantList StudioPresenter::editToneCurve() const
{
    return tone_curve_to_variant(develop_.tone_curve);
}

QVariantList StudioPresenter::editToneCurveSamples() const
{
    return tone_curve_sample_list(develop_.tone_curve, develop_.tone_curve_interpolation);
}

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

namespace
{

QString history_field_label(const std::string_view field)
{
    if (field == "exposure")
        return QCoreApplication::translate("DevelopPanel", "Exposure");
    if (field == "black")
        return QCoreApplication::translate("DevelopPanel", "Exposure black");
    if (field == "contrast")
        return QCoreApplication::translate("DevelopPanel", "Contrast");
    if (field == "highlights")
        return QCoreApplication::translate("DevelopPanel", "Highlights");
    if (field == "shadows")
        return QCoreApplication::translate("DevelopPanel", "Shadows");
    if (field == "whites")
        return QCoreApplication::translate("DevelopPanel", "Whites");
    if (field == "blacks")
        return QCoreApplication::translate("DevelopPanel", "Blacks");
    if (field == "vibrance")
        return QCoreApplication::translate("DevelopPanel", "Vibrance");
    if (field == "saturation")
        return QCoreApplication::translate("DevelopPanel", "Saturation");
    if (field == "velvia" || field == "velviaEnabled")
        return QCoreApplication::translate("DevelopPanel", "Velvia");
    if (field == "velviaStrength")
        return QCoreApplication::translate("DevelopPanel", "Velvia strength");
    if (field == "velviaBias")
        return QCoreApplication::translate("DevelopPanel", "Velvia mid-tones bias");
    if (field == "lut3d")
        return QCoreApplication::translate("DevelopPanel", "3D LUT");
    if (field == "gamma")
        return QCoreApplication::translate("DevelopPanel", "Gamma");
    if (field == "rgbLevels")
        return QCoreApplication::translate("DevelopPanel", "RGB levels");
    if (field == "rgbCurve")
        return QCoreApplication::translate("DevelopPanel", "RGB curve");
    if (field == "sharpen")
        return QCoreApplication::translate("DevelopPanel", "Sharpen");
    if (field == "texture")
        return QCoreApplication::translate("DevelopPanel", "Texture");
    if (field == "textureDetailThreshold")
        return QCoreApplication::translate("DevelopPanel", "Texture scale");
    if (field == "textureIterations")
        return QCoreApplication::translate("DevelopPanel", "Texture iterations");
    if (field == "retouch")
        return QCoreApplication::translate("DevelopPanel", "Retouch");
    if (field == "clarity")
        return QCoreApplication::translate("DevelopPanel", "Clarity");
    if (field == "vignette")
        return QCoreApplication::translate("DevelopPanel", "Vignette");
    if (field == "grain")
        return QCoreApplication::translate("DevelopPanel", "Grain");
    if (field == "bloom")
        return QCoreApplication::translate("DevelopPanel", "Bloom");
    if (field == "soften")
        return QCoreApplication::translate("DevelopPanel", "Soften");
    if (field == "dehaze")
        return QCoreApplication::translate("DevelopPanel", "Dehaze");
    if (field == "outputDither")
        return QCoreApplication::translate("DevelopPanel", "Output Dither");
    if (field == "outputFrame")
        return QCoreApplication::translate("DevelopPanel", "Output Frame");
    if (field == "watermark")
        return QCoreApplication::translate("DevelopPanel", "Watermark");
    if (field == "monochrome")
        return QCoreApplication::translate("DevelopPanel", "Monochrome");
    if (field == "denoise")
        return QCoreApplication::translate("DevelopPanel", "Denoise");
    if (field == "rawHighlights")
        return QCoreApplication::translate("DevelopPanel", "RAW highlights");
    if (field == "hotPixels")
        return QCoreApplication::translate("DevelopPanel", "Hot pixels");
    if (field == "rawChromaticAberration")
        return QCoreApplication::translate("DevelopPanel", "Chromatic aberration");
    if (field == "rawDenoise")
        return QCoreApplication::translate("DevelopPanel", "RAW denoise");
    if (field == "demosaic")
        return QCoreApplication::translate("DevelopPanel", "Demosaicing");
    if (field == "straighten")
        return QCoreApplication::translate("DevelopPanel", "Angle");
    if (field == "perspective")
        return QCoreApplication::translate("DevelopPanel", "Perspective");
    if (field == "toneEqBlacks")
        return QCoreApplication::translate("DevelopPanel", "Blacks");
    if (field == "toneEqShadows")
        return QCoreApplication::translate("DevelopPanel", "Shadows");
    if (field == "toneEqMidtones")
        return QCoreApplication::translate("DevelopPanel", "Midtones");
    if (field == "toneEqHighlights")
        return QCoreApplication::translate("DevelopPanel", "Highlights");
    if (field == "toneEqWhites")
        return QCoreApplication::translate("DevelopPanel", "Whites");
    if (field == "colorEqualizer")
        return QCoreApplication::translate("DevelopPanel", "Color Equalizer");
    if (field == "graduated")
        return QCoreApplication::translate("DevelopPanel", "Graduated ND");
    if (field == "rotate")
        return QCoreApplication::translate("DevelopPanel", "Rotate");
    if (field == "flip")
        return QCoreApplication::translate("DevelopPanel", "Flip");
    if (field == "crop")
        return QCoreApplication::translate("DevelopPanel", "Crop");
    if (field == "canvas")
        return QCoreApplication::translate("DevelopPanel", "Canvas");
    if (field == "lens")
        return QCoreApplication::translate("DevelopPanel", "Lens Correction");
    if (field == "toneCurve")
        return QCoreApplication::translate("DevelopPanel", "Tone curve");
    if (field == "whiteBalance")
        return QCoreApplication::translate("DevelopPanel", "White Balance");
    if (field == "inputProfile")
        return QCoreApplication::translate("DevelopPanel", "Input Profile");
    if (field == "outputProfile")
        return QCoreApplication::translate("DevelopPanel", "Output Profile");
    if (field == "primaries")
        return QCoreApplication::translate("DevelopPanel", "RGB Primaries");
    if (field == "mixer")
        return QCoreApplication::translate("DevelopPanel", "Channel Mixer");
    if (field == "calibration")
        return QCoreApplication::translate("DevelopPanel", "Camera Calibration");
    if (field == "colorBalance")
        return QCoreApplication::translate("DevelopPanel", "Color Balance");
    if (field == "colorChecker")
        return QCoreApplication::translate("DevelopPanel", "Color Checker");
    if (field == "colorBalanceRgb")
        return QCoreApplication::translate("DevelopPanel", "Color Balance RGB");
    if (field == "colorCorrection")
        return QCoreApplication::translate("DevelopPanel", "Color Correction");
    if (field == "colorContrast")
        return QCoreApplication::translate("DevelopPanel", "Color Contrast");
    if (field == "colorReconstruction")
        return QCoreApplication::translate("DevelopPanel", "Color Reconstruction");
    if (field == "colorZones")
        return QCoreApplication::translate("DevelopPanel", "Color Zones");
    if (field == "colorHarmonizer")
        return QCoreApplication::translate("DevelopPanel", "Color Harmonizer");
    if (field == "splitToning")
        return QCoreApplication::translate("DevelopPanel", "Split Toning");
    if (field == "profileGamma")
        return QCoreApplication::translate("DevelopPanel", "Unbreak input profile");
    if (field == "sigmoid")
        return QCoreApplication::translate("DevelopPanel", "Sigmoid");
    if (field == "light")
        return QCoreApplication::translate("DevelopPanel", "Light");
    if (field == "color")
        return QCoreApplication::translate("DevelopPanel", "Color");
    if (field == "detail")
        return QCoreApplication::translate("DevelopPanel", "Detail");
    if (field == "effects")
        return QCoreApplication::translate("DevelopPanel", "Effects");
    if (field == "geometry")
        return QCoreApplication::translate("DevelopPanel", "Geometry");
    if (field == "masks")
        return QCoreApplication::translate("DevelopPanel", "Masks");
    if (field.ends_with("SectionState"))
        return QCoreApplication::translate("DevelopPanel", "Section bypass state");
    if (field == "reset")
        return QCoreApplication::translate("StudioCommands", "Reset All Edits");
    return qstring_from_utf8(field);
}

QString preset_field_group(const std::string_view field)
{
    if (field == "whiteBalance" || field == "whiteBalanceSectionState")
        return QCoreApplication::translate("DevelopPanel", "White Balance");
    if (field == "profileGamma" || field == "inputProfile" || field == "inputProfileSectionState")
        return QCoreApplication::translate("DevelopPanel", "Input Profile");
    if (field == "outputProfile" || field == "outputProfileSectionState")
        return QCoreApplication::translate("DevelopPanel", "Output Profile");
    if (field == "primaries" || field == "calibration" || field == "primariesSectionState" ||
        field == "calibrationSectionState")
        return QCoreApplication::translate("DevelopPanel", "Camera Calibration");
    if (field == "exposure" || field == "contrast" || field == "highlights" || field == "shadows" ||
        field == "whites" || field == "blacks" || field == "gamma" || field == "rgbLevels" ||
        field == "sigmoid" || field == "toneEqual" || field == "lightSectionState" ||
        field == "toneEqualSectionState")
        return QCoreApplication::translate("DevelopPanel", "Light");
    if (field == "rgbCurve" || field == "toneCurve" || field == "curvesSectionState")
        return QCoreApplication::translate("DevelopPanel", "Curves");
    if (field == "vibrance" || field == "saturation" || field == "velvia" || field == "lut3d" ||
        field == "colorBalance" || field == "colorChecker" || field == "colorBalanceRgb" ||
        field == "colorCorrection" || field == "colorContrast" || field == "colorReconstruction" ||
        field == "colorZones" || field == "colorHarmonizer" || field == "monochrome" ||
        field == "splitToning" || field == "colorEqualizer" || field == "colorSectionState" ||
        field == "colorEqualizerSectionState")
        return QCoreApplication::translate("DevelopPanel", "Color");
    if (field == "sharpen" || field == "texture" || field == "retouch" || field == "clarity" ||
        field == "denoise" || field == "grain" || field == "detailSectionState")
        return QCoreApplication::translate("DevelopPanel", "Detail");
    if (field == "demosaic" || field == "rawHighlights" || field == "hotPixels" ||
        field == "rawChromaticAberration" || field == "rawDenoise" || field == "rawSectionState")
        return QCoreApplication::translate("DevelopPanel", "RAW");
    if (field == "rotate" || field == "flip" || field == "straighten" || field == "perspective" ||
        field == "crop" || field == "canvas" || field == "lens" || field == "geometrySectionState")
        return QCoreApplication::translate("DevelopPanel", "Geometry");
    if (field == "vignette" || field == "bloom" || field == "soften" || field == "dehaze" ||
        field == "outputDither" || field == "graduated" || field == "outputFrame" ||
        field == "watermark" || field == "effectsSectionState" || field == "graduatedSectionState")
        return QCoreApplication::translate("DevelopPanel", "Effects");
    return QCoreApplication::translate("DevelopPanel", "Other");
}

QString format_history_summary(const std::vector<DevelopChange> &changes)
{
    if (changes.empty())
    {
        return QCoreApplication::translate("DevelopHistoryPanel", "Original");
    }
    QStringList parts;
    constexpr int kMaxParts = 4;
    for (const auto &change : changes)
    {
        if (static_cast<int>(parts.size()) >= kMaxParts)
        {
            parts.push_back(QStringLiteral("…"));
            break;
        }
        QString part = history_field_label(change.field);
        if (change.value == "on")
        {
            part += QLatin1Char(' ') + QCoreApplication::translate("DevelopHistoryPanel", "on");
        }
        else if (change.value == "off")
        {
            part += QLatin1Char(' ') + QCoreApplication::translate("DevelopHistoryPanel", "off");
        }
        else if (!change.value.empty())
        {
            part += QLatin1Char(' ') + qstring_from_utf8(change.value);
        }
        parts.push_back(std::move(part));
    }
    return parts.join(QStringLiteral(", "));
}

DevelopParams develop_from_history_json(const std::string &recipe_json)
{
    if (recipe_json.empty())
    {
        return {};
    }
    auto recipe = parse_recipe_json(recipe_json);
    if (!recipe)
    {
        return {};
    }
    auto params = develop_from_recipe(recipe.value());
    if (!params)
    {
        return {};
    }
    return std::move(params).value();
}

} // namespace

DevelopParams StudioPresenter::baseline_develop() const
{
    DevelopParams params;
    const auto asset = assets_.assetById(selected_asset_id_);
    if (asset)
    {
        params.sigmoid_enabled = is_raw_media_type(asset->media_type);
    }
    return params;
}

QVariantList StudioPresenter::modifiedParameterChoices() const
{
    QVariantList result;
    if (selected_asset_id_.isEmpty() || !develop_loaded_)
        return result;
    const auto changes = develop_modified_fields(baseline_develop(), develop_);
    result.reserve(static_cast<qsizetype>(changes.size()));
    for (const auto &change : changes)
    {
        result.push_back(QVariantMap{{QStringLiteral("field"), qstring_from_utf8(change.field)},
                                     {QStringLiteral("label"), history_field_label(change.field)},
                                     {QStringLiteral("group"), preset_field_group(change.field)}});
    }
    return result;
}

DevelopParams StudioPresenter::develop_from_history_entry(const RecipeHistoryEntry &entry) const
{
    if (entry.recipe_json.empty())
    {
        return baseline_develop();
    }
    return develop_from_history_json(entry.recipe_json);
}

void StudioPresenter::sync_active_history()
{
    if (develop_ == baseline_develop())
    {
        active_history_id_ = 0;
        active_history_seq_ = 0;
        return;
    }
    if (recipe_history_entries_.empty())
    {
        active_history_id_ = 0;
        active_history_seq_ = 0;
        return;
    }
    for (const auto &entry : recipe_history_entries_)
    {
        if (entry.id == active_history_id_)
        {
            active_history_seq_ = entry.seq;
            return;
        }
    }
    for (const auto &entry : recipe_history_entries_)
    {
        if (develop_from_history_entry(entry) == develop_)
        {
            active_history_id_ = entry.id;
            active_history_seq_ = entry.seq;
            return;
        }
    }
    active_history_id_ = recipe_history_entries_.front().id;
    active_history_seq_ = recipe_history_entries_.front().seq;
}

void StudioPresenter::apply_recipe_history(const std::vector<RecipeHistoryEntry> &entries)
{
    recipe_history_entries_ = entries;
    std::vector<DevelopParams> steps;
    steps.reserve(entries.size());
    for (const auto &entry : entries)
    {
        steps.push_back(develop_from_history_json(entry.recipe_json));
    }
    recipe_history_.clear();
    for (std::size_t index = 0; index < entries.size(); ++index)
    {
        const auto &entry = entries[index];
        const DevelopParams &after = steps[index];
        const DevelopParams before = index + 1 < steps.size() ? steps[index + 1] : DevelopParams{};
        QString summary = format_history_summary(develop_change_summary(before, after));
        if (entry.kind == kRecipeHistoryKindSnapshot)
        {
            const QString label = entry.label ? qstring_from_utf8(*entry.label) : QString{};
            const QString snapshot = QCoreApplication::translate("DevelopHistoryPanel", "Snapshot");
            if (!label.isEmpty())
            {
                summary = snapshot + QStringLiteral(" · ") + label;
            }
            else
            {
                summary = snapshot + QStringLiteral(" · ") + summary;
            }
        }
        QVariantMap row;
        row.insert(QStringLiteral("id"), QVariant::fromValue(entry.id));
        row.insert(QStringLiteral("kind"), qstring_from_utf8(entry.kind));
        row.insert(QStringLiteral("label"),
                   entry.label ? qstring_from_utf8(*entry.label) : QString{});
        row.insert(QStringLiteral("seq"), QVariant::fromValue(entry.seq));
        row.insert(QStringLiteral("createdUnixMs"), QVariant::fromValue(entry.created_unix_ms));
        row.insert(QStringLiteral("summary"), summary);
        recipe_history_.push_back(row);
    }
}

void StudioPresenter::reload_recipe_history()
{
    if (selected_asset_id_.isEmpty())
    {
        recipe_history_.clear();
        recipe_history_entries_.clear();
        active_history_id_ = 0;
        active_history_seq_ = 0;
        emit editChanged();
        return;
    }
    const auto asset_id = utf8_from_qstring(selected_asset_id_);
    executor_.post(
        [this, asset_id]()
        {
            Result<std::vector<RecipeHistoryEntry>> history =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            if (service_ != nullptr)
            {
                history = service_->list_recipe_history(asset_id);
            }
            QMetaObject::invokeMethod(
                this,
                [this, asset_id, history = std::move(history)]() mutable
                {
                    if (utf8_from_qstring(selected_asset_id_) != asset_id)
                    {
                        return;
                    }
                    if (history)
                    {
                        apply_recipe_history(history.value());
                    }
                    else
                    {
                        recipe_history_.clear();
                        recipe_history_entries_.clear();
                    }
                    sync_active_history();
                    emit editChanged();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
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
    break_history_coalescing();
    develop_ = {};
    saved_develop_ = {};
    develop_loaded_ = false;
    develop_load_error_.clear();
    white_balance_pick_active_ = false;
    undo_stack_.clear();
    redo_stack_.clear();
    recipe_history_.clear();
    recipe_history_entries_.clear();
    active_history_id_ = 0;
    active_history_seq_ = 0;
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
                    if (history)
                    {
                        apply_recipe_history(history.value());
                    }
                    else
                    {
                        recipe_history_.clear();
                        recipe_history_entries_.clear();
                    }
                    if (!loaded)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        develop_load_error_ = qstring_from_utf8(loaded.error().message);
                        sync_active_history();
                        emit editChanged();
                        setError(qstring_from_utf8(loaded.error().message));
                        return;
                    }
                    auto params = develop_from_recipe(loaded.value());
                    if (!params)
                    {
                        develop_ = {};
                        saved_develop_ = {};
                        develop_load_error_ = qstring_from_utf8(params.error().message);
                        sync_active_history();
                        emit editChanged();
                        setError(qstring_from_utf8(params.error().message));
                        return;
                    }
                    develop_ = params.value();
                    saved_develop_ = develop_;
                    develop_loaded_ = true;
                    develop_load_error_.clear();
                    sync_curve_ui_from_develop();
                    sync_active_history();
                    emit editChanged();
                },
                Qt::QueuedConnection);
        },
        TaskPriority::kForeground);
}

void StudioPresenter::break_history_coalescing()
{
    history_coalesce_key_.reset();
    history_coalesce_id_.reset();
}

void StudioPresenter::commit_develop(DevelopParams params, const bool push_history,
                                     const bool refresh_preview,
                                     const RecipeHistoryWrite history_write,
                                     std::optional<std::string> history_coalesce_key)
{
    if (selected_asset_id_.isEmpty() || catalog_path_.isEmpty())
    {
        return;
    }
    clamp_develop(params);
    const auto previous = saved_develop_;
    const bool same_control = push_history && params != saved_develop_ &&
                              history_write == RecipeHistoryWrite::kAppendIfNew &&
                              history_coalesce_key && history_coalesce_key_ &&
                              *history_coalesce_key == *history_coalesce_key_;
    bool pushed_undo = false;
    if (push_history && params != saved_develop_ && !same_control)
    {
        undo_stack_.push_back(saved_develop_);
        pushed_undo = true;
        if (undo_stack_.size() > 40U)
        {
            undo_stack_.erase(undo_stack_.begin());
        }
        redo_stack_.clear();
    }
    if (history_write != RecipeHistoryWrite::kAppendIfNew || !history_coalesce_key)
    {
        break_history_coalescing();
    }
    else if (!same_control)
    {
        history_coalesce_key_ = history_coalesce_key;
        history_coalesce_id_.reset();
    }
    const auto coalesce_history_id = same_control ? history_coalesce_id_ : std::nullopt;
    std::optional<std::int64_t> discard_after;
    if (push_history && history_write == RecipeHistoryWrite::kAppendIfNew && !same_control &&
        !recipe_history_entries_.empty() &&
        active_history_seq_ < recipe_history_entries_.front().seq)
    {
        discard_after = active_history_seq_;
        const auto cursor_seq = *discard_after;
        recipe_history_entries_.erase(std::remove_if(recipe_history_entries_.begin(),
                                                     recipe_history_entries_.end(),
                                                     [cursor_seq](const RecipeHistoryEntry &entry)
                                                     { return entry.seq > cursor_seq; }),
                                      recipe_history_entries_.end());
        QVariantList kept;
        kept.reserve(recipe_history_.size());
        for (const auto &row : recipe_history_)
        {
            if (row.toMap().value(QStringLiteral("seq")).toLongLong() <= cursor_seq)
            {
                kept.push_back(row);
            }
        }
        recipe_history_ = std::move(kept);
    }
    develop_ = params;
    emit editChanged();
    const bool crop_guides = crop_tool_active_ && !before_after_;
    const bool overlay = mask_overlay_visible_ && !before_after_;
    const bool needs_first_preview =
        refresh_preview && !crop_guides && !overlay && !before_after_ &&
        (!displayed_develop_.has_value() || *displayed_develop_ != params);
    preview_loading_ = refresh_preview;
    emit previewChanged();
    static_cast<void>(develop_preview_owner_.supersede("develop_save_superseded"));
    pending_save_ = PendingDevelopWork{
        .save = true,
        .interactive = crop_guides || overlay || needs_first_preview,
        .params = params,
        .previous = previous,
        .push_history = push_history,
        .pushed_undo = pushed_undo,
        .history_write = history_write,
        .discard_history_after_seq = discard_after,
        .history_coalesce_key = std::move(history_coalesce_key),
        .coalesce_history_id = coalesce_history_id,
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .refresh_preview = refresh_preview,
        .settle_preview = needs_first_preview,
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
    const bool crop_guides = crop_tool_active_ && !before_after_;
    static_cast<void>(develop_preview_owner_.supersede("interactive_preview_superseded"));
    pending_preview_ = PendingDevelopWork{
        .interactive = true,
        .params = params,
        .pushed_undo = false,
        .history_write = RecipeHistoryWrite::kUnchanged,
        .discard_history_after_seq = {},
        .history_coalesce_key = {},
        .coalesce_history_id = {},
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .overlay_mask_id = current_overlay_mask_id(params),
    };
    kick_develop_work();
    // Start the pixel job before notifying the broad inspector property set. QML may reevaluate
    // many edit bindings synchronously, while the owner-managed worker can render in parallel.
    emit editChanged();
}

bool StudioPresenter::mutate_develop(DevelopParams next, const DevelopEdit edit,
                                     const bool refresh_preview,
                                     std::optional<std::string> history_coalesce_key)
{
    clamp_develop(next);
    switch (edit)
    {
    case DevelopEdit::Overlay:
        if (next == develop_)
        {
            return false;
        }
        develop_ = std::move(next);
        emit editChanged();
        return true;
    case DevelopEdit::Preview:
        preview_develop(std::move(next));
        return true;
    case DevelopEdit::Commit:
        if (next == saved_develop_ && next == develop_)
        {
            return false;
        }
        if (next == saved_develop_)
        {
            develop_ = std::move(next);
            emit editChanged();
            if (refresh_preview)
            {
                enqueue_preview();
            }
            return true;
        }
        commit_develop(std::move(next), true, refresh_preview, RecipeHistoryWrite::kAppendIfNew,
                       std::move(history_coalesce_key));
        return true;
    case DevelopEdit::Restore:
        if (next == develop_ && next == saved_develop_)
        {
            return false;
        }
        commit_develop(std::move(next), true, refresh_preview, RecipeHistoryWrite::kUnchanged);
        return true;
    case DevelopEdit::Revert:
        commit_develop(std::move(next), false, refresh_preview, RecipeHistoryWrite::kUnchanged);
        return true;
    }
    return false;
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
    const bool progressive_develop = browse_mode_ == QLatin1String("develop") &&
                                     !mask_overlay_visible_ && !crop_guides && !before_after_;
    pending_preview_ = PendingDevelopWork{
        .interactive = mask_overlay_visible_ || crop_guides || progressive_develop,
        .params = develop_,
        .pushed_undo = false,
        .history_write = RecipeHistoryWrite::kUnchanged,
        .discard_history_after_seq = {},
        .history_coalesce_key = {},
        .coalesce_history_id = {},
        .asset_id = utf8_from_qstring(selected_asset_id_),
        .ignore_edits = before_after_,
        .ignore_crop = crop_guides,
        .ignore_straighten = false,
        .settle_preview = progressive_develop,
        .overlay_mask_id = current_overlay_mask_id(develop_),
    };
    kick_develop_work();
}

void StudioPresenter::request_comparison_before()
{
    if (!comparison_active_ || selected_asset_id_.isEmpty())
    {
        return;
    }
    comparison_before_requested_ = true;
    preview_loading_ = true;
    emit previewChanged();
    kick_develop_work();
}

void StudioPresenter::kick_develop_work()
{
    if (develop_job_in_flight_)
    {
        return;
    }
    PendingDevelopWork job;
    bool starting_comparison_before = false;
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
    else if (comparison_active_ && comparison_before_requested_)
    {
        comparison_before_requested_ = false;
        starting_comparison_before = true;
        job = PendingDevelopWork{
            .interactive = false,
            .params = develop_,
            .pushed_undo = false,
            .history_write = RecipeHistoryWrite::kUnchanged,
            .discard_history_after_seq = {},
            .history_coalesce_key = {},
            .coalesce_history_id = {},
            .asset_id = utf8_from_qstring(selected_asset_id_),
            .ignore_edits = true,
            .refresh_preview = true,
            .comparison_before = true,
            .overlay_mask_id = {},
        };
    }
    else
    {
        kickPreviewWarmup();
        return;
    }
    if (starting_comparison_before)
    {
        static_cast<void>(develop_preview_owner_.supersede("comparison_before_requested"));
        preview_loading_ = true;
        emit previewChanged();
    }
    static_cast<void>(thumbnail_work_.cancel("foreground_preview_requested"));
    develop_job_in_flight_ = true;
    const auto revision = develop_preview_owner_.revision();
    const auto cancellation = develop_preview_owner_.begin();
    executor_.post(
        [this, job, revision, cancellation]()
        {
            Result<RecipeSaveResult> saved =
                make_error(ErrorCode::kIo, "Catalog session is closed");
            Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Catalog session is closed");
            bool save_ok = !job.save;
            if (service_ != nullptr)
            {
                if (job.save)
                {
                    saved = service_->save_develop_with_history(
                        job.asset_id, job.params,
                        RecipeSaveOptions{
                            .history_write = job.history_write,
                            .discard_history_after_seq = job.discard_history_after_seq,
                            .coalesce_history_id = job.coalesce_history_id,
                            .defer_recovery_publication = true,
                        });
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
                    request.persist_preview_record =
                        job.comparison_before ? false : !job.interactive;
                    request.cancellation = cancellation;
                    if (job.overlay_mask_id)
                    {
                        request.overlay_mask_id = job.overlay_mask_id;
                        request.persist_preview_record = false;
                    }
                    preview = service_->request_preview(
                        request, job.interactive && !job.comparison_before ?
                                     std::optional<DevelopParams>{job.params} :
                                     std::optional<DevelopParams>{});
                }
            }
            const bool recovery_due = job.save && save_ok;
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
                                if (job.pushed_undo && !undo_stack_.empty())
                                {
                                    undo_stack_.pop_back();
                                }
                                if (job.history_coalesce_key &&
                                    history_coalesce_key_ == job.history_coalesce_key &&
                                    !job.coalesce_history_id)
                                {
                                    break_history_coalescing();
                                }
                                active_history_id_ = 0;
                                if (job.discard_history_after_seq)
                                {
                                    reload_recipe_history();
                                }
                                else
                                {
                                    sync_active_history();
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
                            if (job.coalesce_history_id && saved.value().history_id &&
                                *saved.value().history_id != *job.coalesce_history_id)
                            {
                                undo_stack_.push_back(job.previous);
                                if (undo_stack_.size() > 40U)
                                {
                                    undo_stack_.erase(undo_stack_.begin());
                                }
                                redo_stack_.clear();
                            }
                            saved_develop_ = job.params;
                            observed_catalog_revision_ =
                                std::max(observed_catalog_revision_, saved.value().revision);
                            assets_.updateAsset(saved.value().asset);
                            if (job.history_coalesce_key &&
                                history_coalesce_key_ == job.history_coalesce_key)
                            {
                                history_coalesce_id_ = saved.value().history_id;
                            }
                            if (pending_save_)
                            {
                                pending_save_->previous = job.params;
                                if (job.history_coalesce_key &&
                                    pending_save_->history_coalesce_key ==
                                        job.history_coalesce_key &&
                                    saved.value().history_id)
                                {
                                    pending_save_->coalesce_history_id = saved.value().history_id;
                                }
                            }
                            emit selectionChanged();
                            emit editChanged();
                            if (job.history_write == RecipeHistoryWrite::kAppendIfNew)
                            {
                                active_history_id_ = 0;
                                active_history_seq_ = 0;
                                reload_recipe_history();
                            }
                            else
                            {
                                sync_active_history();
                            }
                        }
                    }
                    if (!develop_preview_owner_.accepts(revision, job.asset_id,
                                                        utf8_from_qstring(selected_asset_id_)))
                    {
                        if (job.comparison_before && comparison_active_ &&
                            comparison_before_url_.isEmpty())
                        {
                            comparison_before_requested_ = true;
                        }
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
                            if (job.comparison_before && comparison_active_ &&
                                comparison_before_url_.isEmpty())
                            {
                                comparison_before_requested_ = true;
                            }
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
                        if (job.comparison_before && clear_comparison())
                        {
                            emit editChanged();
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
                    if (job.ignore_crop && crop_tool_active_)
                    {
                        crop_guide_ready_ = true;
                    }
                    if (job.comparison_before)
                    {
                        if (comparison_active_)
                        {
                            show_comparison_before_result(preview.value(), revision);
                            if (comparison_before_url_.isEmpty() && clear_comparison())
                            {
                                emit editChanged();
                            }
                        }
                        emit previewChanged();
                        kick_develop_work();
                        return;
                    }
                    show_preview_result(preview.value(), revision, job.interactive);
                    displayed_develop_ = job.ignore_edits ?
                                             std::optional<DevelopParams>{} :
                                             std::optional<DevelopParams>{job.params};
                    emit previewChanged();
                    if (job.settle_preview)
                    {
                        pending_preview_ = PendingDevelopWork{
                            .save = false,
                            .interactive = false,
                            .params = job.params,
                            .previous = {},
                            .push_history = false,
                            .pushed_undo = false,
                            .history_write = RecipeHistoryWrite::kUnchanged,
                            .discard_history_after_seq = {},
                            .history_coalesce_key = {},
                            .coalesce_history_id = {},
                            .asset_id = job.asset_id,
                            .ignore_edits = job.ignore_edits,
                            .ignore_crop = job.ignore_crop,
                            .ignore_straighten = job.ignore_straighten,
                            .refresh_preview = true,
                            .settle_preview = false,
                            .overlay_mask_id = {},
                        };
                    }
                    if (comparison_active_ && comparison_before_url_.isEmpty())
                    {
                        comparison_before_requested_ = true;
                    }
                    kick_develop_work();
                },
                Qt::QueuedConnection);
            if (recovery_due && service_ != nullptr)
            {
                auto synchronized = service_->sync_recovery(std::string_view{job.asset_id});
                if (!synchronized)
                {
                    const auto failure = qstring_from_utf8(synchronized.error().message);
                    QMetaObject::invokeMethod(
                        this,
                        [this, failure]
                        {
                            setError(QCoreApplication::translate(
                                         "StudioPresenter",
                                         "Edit was saved, but recovery synchronization failed: ") +
                                     failure);
                        },
                        Qt::QueuedConnection);
                }
            }
        },
        TaskPriority::kForeground);
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
    const bool keep_crop_guide =
        crop_tool_active_ && crop_guide_ready_ &&
        (name == QLatin1String("straighten") || name.startsWith(QLatin1String("perspective")));
    mutate_develop(std::move(next), DevelopEdit::Commit, !keep_crop_guide, field);
}

void StudioPresenter::setDevelopText(const QString &name, const QString &value)
{
    DevelopParams next = develop_;
    auto applied =
        apply_develop_text_field_strict(next, utf8_from_qstring(name), utf8_from_qstring(value));
    if (!applied)
    {
        setError(qstring_from_utf8(applied.error().message));
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit, true, utf8_from_qstring(name));
}

void StudioPresenter::saveStyleToPath(const QString &path)
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !engine_)
        return;
    QString output_path = path.trimmed();
    if (output_path.startsWith(QStringLiteral("file:")))
        output_path = QUrl(output_path).toLocalFile();
    if (!output_path.endsWith(QStringLiteral(".rstyle.json"), Qt::CaseInsensitive))
        output_path += QStringLiteral(".rstyle.json");
    if (output_path.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Style path must not be empty."));
        return;
    }
    auto recipe = recipe_from_develop(
        {asset->id, asset->normalized_uri, asset->content_fingerprint}, develop_);
    if (!recipe)
    {
        setError(qstring_from_utf8(recipe.error().message));
        return;
    }
    auto valid = engine_->validate(recipe.value());
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    QString style_name = QFileInfo(output_path).completeBaseName();
    if (style_name.endsWith(QStringLiteral(".rstyle"), Qt::CaseInsensitive))
        style_name.chop(7);
    auto style = recipe_style_from_recipe(utf8_from_qstring(style_name), {}, recipe.value());
    if (!style)
    {
        setError(qstring_from_utf8(style.error().message));
        return;
    }
    auto serialized = serialize_recipe_style(style.value());
    if (!serialized)
    {
        setError(qstring_from_utf8(serialized.error().message));
        return;
    }
    auto written =
        write_utf8_text_file_atomically(utf8_from_qstring(output_path), serialized.value());
    if (!written)
    {
        setError(qstring_from_utf8(written.error().message));
        return;
    }
    setStatus(QCoreApplication::translate("StudioPresenter", "Recipe style saved."));
}

void StudioPresenter::applyStyleFromPath(const QString &path)
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !engine_)
        return;
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    auto text = read_utf8_text_file(utf8_from_qstring(input_path), kRecipeStyleFileMaxBytes);
    if (!text)
    {
        setError(qstring_from_utf8(text.error().message));
        return;
    }
    if (is_crs_xmp_document(text.value()))
    {
        auto imported = import_crs_xmp(
            {text.value(), {asset->id, asset->normalized_uri, asset->content_fingerprint}});
        if (!imported)
        {
            setError(qstring_from_utf8(imported.error().message));
            return;
        }
        auto params = develop_;
        apply_crs_look(params, imported.value().look, imported.value().mask);
        mutate_develop(std::move(params), DevelopEdit::Commit);
        const auto name = imported.value().name.empty() ?
                              QString() :
                              QString::fromStdString(imported.value().name);
        setStatus(
            name.isEmpty() ?
                QCoreApplication::translate("StudioPresenter", "Lightroom preset applied.") :
                QCoreApplication::translate("StudioPresenter", "Lightroom preset “%1” applied.")
                    .arg(name));
        return;
    }
    auto style = parse_recipe_style_json(text.value());
    if (!style)
    {
        setError(qstring_from_utf8(style.error().message));
        return;
    }
    auto valid_template = engine_->validate(style.value().recipe);
    if (!valid_template)
    {
        setError(qstring_from_utf8(valid_template.error().message));
        return;
    }
    auto target_recipe = recipe_from_develop(
        {asset->id, asset->normalized_uri, asset->content_fingerprint}, develop_);
    if (!target_recipe)
    {
        setError(qstring_from_utf8(target_recipe.error().message));
        return;
    }
    auto recipe = apply_recipe_style(style.value(), std::move(target_recipe).value());
    if (!recipe)
    {
        setError(qstring_from_utf8(recipe.error().message));
        return;
    }
    auto valid = engine_->validate(recipe.value());
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    auto params = develop_from_recipe(recipe.value());
    if (!params)
    {
        setError(qstring_from_utf8(params.error().message));
        return;
    }
    mutate_develop(std::move(params).value(), DevelopEdit::Commit);
}

namespace
{

QString local_file_path(QString path)
{
    path = path.trimmed();
    if (path.startsWith(QStringLiteral("file:")))
        path = QUrl(path).toLocalFile();
    return path;
}

QString preset_name_from_filename(const QFileInfo &info)
{
    QString name = info.fileName();
    const QString style_suffix = QStringLiteral(".rstyle.json");
    const QString xmp_suffix = QStringLiteral(".xmp");
    if (name.endsWith(style_suffix, Qt::CaseInsensitive))
        name.chop(style_suffix.size());
    else if (name.endsWith(xmp_suffix, Qt::CaseInsensitive))
        name.chop(xmp_suffix.size());
    return name;
}

QString preset_suffix_from_filename(const QFileInfo &info)
{
    const QString file_name = info.fileName();
    const QString style_suffix = QStringLiteral(".rstyle.json");
    const QString xmp_suffix = QStringLiteral(".xmp");
    if (file_name.endsWith(style_suffix, Qt::CaseInsensitive))
        return file_name.right(style_suffix.size());
    if (file_name.endsWith(xmp_suffix, Qt::CaseInsensitive))
        return file_name.right(xmp_suffix.size());
    return {};
}

QString canonical_or_absolute(const QFileInfo &info)
{
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool is_managed_preset(const QVariantList &presets, const QString &directory,
                       const QFileInfo &candidate)
{
    if (directory.isEmpty() || !candidate.exists() || !candidate.isFile() || candidate.isSymLink())
        return false;
    const QString directory_path = QFileInfo(directory).canonicalFilePath();
    const QString parent_path = QFileInfo(candidate.absolutePath()).canonicalFilePath();
    if (directory_path.isEmpty() || parent_path != directory_path)
        return false;
    const QString candidate_path = canonical_or_absolute(candidate);
    return std::any_of(
        presets.cbegin(), presets.cend(),
        [&](const QVariant &entry)
        {
            const QFileInfo listed(entry.toMap().value(QStringLiteral("path")).toString());
            return !listed.isSymLink() && canonical_or_absolute(listed) == candidate_path;
        });
}

QString preset_name_validation_error(const QString &name)
{
    if (name.isEmpty())
        return QCoreApplication::translate("StudioPresenter", "Preset name must not be empty.");
    if (name != name.trimmed())
        return QCoreApplication::translate("StudioPresenter",
                                           "Preset name must not start or end with whitespace.");
    if (name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('.')) ||
        name == QLatin1String(".."))
        return QCoreApplication::translate("StudioPresenter",
                                           "Preset name must not start or end with a period.");
    if (name.toUtf8().size() > 200)
        return QCoreApplication::translate("StudioPresenter", "Preset name is too long.");
    static const QRegularExpression invalid_characters(QStringLiteral(R"([\x00-\x1f\\/:*?"<>|])"));
    if (name.contains(invalid_characters))
        return QCoreApplication::translate(
            "StudioPresenter",
            "Preset name contains characters that cannot be used in a file name.");
    const QString base_name = name.section(QLatin1Char('.'), 0, 0);
    static const QRegularExpression reserved_name(
        QStringLiteral(R"(^(?:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (reserved_name.match(base_name).hasMatch())
        return QCoreApplication::translate("StudioPresenter",
                                           "Preset name is reserved by the operating system.");
    return {};
}

QString collect_modified_parameter_selection(const QVariantList &fields,
                                             const DevelopParams &baseline,
                                             const DevelopParams &current,
                                             std::vector<std::string> &selected_fields)
{
    if (fields.isEmpty())
        return QCoreApplication::translate("StudioPresenter",
                                           "Select at least one modified parameter.");
    if (fields.size() > static_cast<qsizetype>(develop_selectable_field_names().size()))
        return QCoreApplication::translate("StudioPresenter", "Parameter selection is invalid.");

    selected_fields.clear();
    selected_fields.reserve(static_cast<std::size_t>(fields.size()));
    std::set<std::string, std::less<>> selected_set;
    for (const auto &field : fields)
    {
        if (field.metaType().id() != QMetaType::QString || field.toString().isEmpty())
            return QCoreApplication::translate("StudioPresenter",
                                               "Parameter selection is invalid.");
        auto selected = utf8_from_qstring(field.toString());
        if (!is_develop_selectable_field(selected) || !selected_set.insert(selected).second)
            return QCoreApplication::translate("StudioPresenter",
                                               "Parameter selection is invalid.");
        selected_fields.push_back(std::move(selected));
    }

    std::set<std::string, std::less<>> current_fields;
    for (const auto &change : develop_modified_fields(baseline, current))
        current_fields.insert(change.field);
    if (!std::all_of(selected_fields.cbegin(), selected_fields.cend(),
                     [&current_fields](const std::string &field)
                     { return current_fields.contains(field); }))
        return QCoreApplication::translate("StudioPresenter",
                                           "The selected parameters are no longer modified.");
    std::sort(selected_fields.begin(), selected_fields.end());
    return {};
}

} // namespace

QString StudioPresenter::presets_directory() const
{
    if (catalog_path_.isEmpty())
        return {};
    return QDir(QFileInfo(catalog_path_).absolutePath()).filePath(QStringLiteral("Ravo Presets"));
}

void StudioPresenter::savePreset(const QString &name, const QVariantList &fields)
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !engine_)
        return;
    const QString checked_name = name;
    const QString name_error = preset_name_validation_error(checked_name);
    if (!name_error.isEmpty())
    {
        setError(name_error);
        return;
    }
    if (static_cast<std::size_t>(checked_name.toUtf8().size()) > kRecipeStyleNameMaxBytes)
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset name is too long."));
        return;
    }
    std::vector<std::string> selected_fields;
    const QString selection_error =
        collect_modified_parameter_selection(fields, baseline_develop(), develop_, selected_fields);
    if (!selection_error.isEmpty())
    {
        setError(selection_error);
        return;
    }

    const QString directory = presets_directory();
    if (directory.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Open a library to save presets."));
        return;
    }
    if (!QDir().mkpath(directory))
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Preset folder could not be created."));
        return;
    }
    const bool duplicate_name =
        std::any_of(develop_presets_.cbegin(), develop_presets_.cend(),
                    [&checked_name](const QVariant &entry)
                    {
                        return entry.toMap()
                                   .value(QStringLiteral("name"))
                                   .toString()
                                   .compare(checked_name, Qt::CaseInsensitive) == 0;
                    });
    if (duplicate_name)
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "A preset with that name already exists."));
        return;
    }

    auto recipe = recipe_from_develop(
        {asset->id, asset->normalized_uri, asset->content_fingerprint}, develop_);
    if (!recipe)
    {
        setError(qstring_from_utf8(recipe.error().message));
        return;
    }
    auto valid = engine_->validate(recipe.value());
    if (!valid)
    {
        setError(qstring_from_utf8(valid.error().message));
        return;
    }
    auto style = recipe_style_from_selected_fields(
        utf8_from_qstring(checked_name), {}, std::move(recipe).value(), std::move(selected_fields));
    if (!style)
    {
        setError(qstring_from_utf8(style.error().message));
        return;
    }
    auto serialized = serialize_recipe_style(style.value());
    if (!serialized)
    {
        setError(qstring_from_utf8(serialized.error().message));
        return;
    }
    const QString output = QDir(directory).filePath(checked_name + QStringLiteral(".rstyle.json"));
    auto written = write_utf8_text_file_atomically(utf8_from_qstring(output), serialized.value());
    if (!written)
    {
        setError(qstring_from_utf8(written.error().message));
        return;
    }
    reload_presets();
    setStatus(
        QCoreApplication::translate("StudioPresenter", "Preset “%1” saved.").arg(checked_name));
}

void StudioPresenter::reload_presets()
{
    QVariantList presets;
    const QString directory = presets_directory();
    if (!directory.isEmpty())
    {
        const QDir dir(directory);
        const auto entries = dir.entryInfoList(
            {QStringLiteral("*.xmp"), QStringLiteral("*.XMP"), QStringLiteral("*.rstyle.json")},
            QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const auto &entry : entries)
        {
            if (entry.isSymLink())
                continue;
            const QString name = preset_name_from_filename(entry);
            QString kind = QStringLiteral("style");
            const auto text = read_utf8_text_file(utf8_from_qstring(entry.absoluteFilePath()),
                                                  kRecipeStyleFileMaxBytes);
            if (text && is_crs_xmp_document(text.value()))
            {
                kind = QStringLiteral("crs");
            }
            else if (text)
            {
                auto style = parse_recipe_style_json(text.value());
                if (!style)
                    continue;
            }
            else
            {
                continue;
            }
            presets.push_back(QVariantMap{{QStringLiteral("name"), name},
                                          {QStringLiteral("path"), entry.absoluteFilePath()},
                                          {QStringLiteral("kind"), kind}});
        }
    }
    develop_presets_ = std::move(presets);
    emit presetsChanged();
}

void StudioPresenter::importPresetFromPath(const QString &path)
{
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    const QFileInfo source(input_path);
    if (!source.exists() || !source.isFile())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    const QString directory = presets_directory();
    if (directory.isEmpty())
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Open a library to import presets."));
        return;
    }
    if (!QDir().mkpath(directory))
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Preset folder could not be created."));
        return;
    }
    auto text = read_utf8_text_file(utf8_from_qstring(input_path), kRecipeStyleFileMaxBytes);
    if (!text)
    {
        setError(qstring_from_utf8(text.error().message));
        return;
    }
    QString stem = source.completeBaseName();
    QString suffix = QStringLiteral(".rstyle.json");
    if (is_crs_xmp_document(text.value()))
    {
        auto imported =
            import_crs_xmp({text.value(), {"preset", "ravo-preset://library", std::nullopt}});
        if (!imported)
        {
            setError(qstring_from_utf8(imported.error().message));
            return;
        }
        if (!imported.value().name.empty())
            stem = QString::fromStdString(imported.value().name);
        suffix = QStringLiteral(".xmp");
    }
    else
    {
        auto style = parse_recipe_style_json(text.value());
        if (!style)
        {
            setError(qstring_from_utf8(style.error().message));
            return;
        }
        stem = QString::fromStdString(style.value().name);
    }
    stem.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|])")), QStringLiteral("-"));
    if (stem.trimmed().isEmpty())
        stem = QStringLiteral("preset");
    QString destination = QDir(directory).filePath(stem + suffix);
    if (QFileInfo::exists(destination) &&
        QFileInfo(destination).canonicalFilePath() != source.canonicalFilePath())
    {
        int serial = 2;
        while (QFileInfo::exists(QDir(directory).filePath(stem + QStringLiteral("-") +
                                                          QString::number(serial) + suffix)))
            ++serial;
        destination =
            QDir(directory).filePath(stem + QStringLiteral("-") + QString::number(serial) + suffix);
    }
    if (QFileInfo(destination).canonicalFilePath() != source.canonicalFilePath())
    {
        QFile::remove(destination);
        if (!QFile::copy(input_path, destination))
        {
            setError(QCoreApplication::translate("StudioPresenter", "Preset could not be copied."));
            return;
        }
    }
    reload_presets();
    applyStyleFromPath(destination);
}

void StudioPresenter::renamePreset(const QString &path, const QString &name)
{
    const QString input_path = local_file_path(path);
    const QFileInfo source(input_path);
    if (!source.exists() || !source.isFile())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    if (!is_managed_preset(develop_presets_, presets_directory(), source))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Only presets imported into this library can be renamed."));
        return;
    }
    const QString name_error = preset_name_validation_error(name);
    if (!name_error.isEmpty())
    {
        setError(name_error);
        return;
    }
    const QString suffix = preset_suffix_from_filename(source);
    if (suffix.isEmpty())
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Preset file type is not supported."));
        return;
    }
    const QString destination = QDir(source.absolutePath()).filePath(name + suffix);
    if (QDir::cleanPath(destination) == QDir::cleanPath(source.absoluteFilePath()))
    {
        setError({});
        setStatus(QCoreApplication::translate("StudioPresenter", "Preset name is unchanged."));
        return;
    }
    const QFileInfo target(destination);
    if (target.exists() || target.isSymLink())
    {
        if (canonical_or_absolute(target) == canonical_or_absolute(source))
        {
            setError(QCoreApplication::translate(
                "StudioPresenter", "This filesystem cannot rename a preset by letter case only."));
        }
        else
        {
            setError(QCoreApplication::translate("StudioPresenter",
                                                 "A preset with that name already exists."));
        }
        return;
    }
    QFile file(source.absoluteFilePath());
    if (!file.rename(destination))
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset could not be renamed: %1")
                     .arg(file.errorString()));
        return;
    }
    setError({});
    reload_presets();
    setStatus(QCoreApplication::translate("StudioPresenter", "Preset renamed to “%1”.").arg(name));
}

void StudioPresenter::deletePreset(const QString &path)
{
    const QString input_path = local_file_path(path);
    const QFileInfo source(input_path);
    if (!source.exists() || !source.isFile())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    if (!is_managed_preset(develop_presets_, presets_directory(), source))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Only presets imported into this library can be deleted."));
        return;
    }
    QFile file(source.absoluteFilePath());
    if (!file.remove())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset could not be deleted: %1")
                     .arg(file.errorString()));
        return;
    }
    setError({});
    reload_presets();
    setStatus(QCoreApplication::translate("StudioPresenter", "Preset deleted."));
}

QString StudioPresenter::selectedPhotoDebugInfo() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset)
        return {};
    PhotoDebugIdentity identity;
    identity.catalog = catalog_path_;
    identity.asset_id = selected_asset_id_;
    identity.uri = qstring_from_utf8(asset->normalized_uri);
    identity.path = QUrl(identity.uri).toLocalFile();
    if (asset->content_fingerprint)
        identity.fingerprint = qstring_from_utf8(*asset->content_fingerprint);
    identity.media_type = qstring_from_utf8(asset->media_type);
    identity.display_name = qstring_from_utf8(asset_display_name(*asset));
    if (asset->width)
        identity.width = QString::number(*asset->width);
    if (asset->height)
        identity.height = QString::number(*asset->height);
    identity.size_bytes = QString::number(asset->size_bytes);
    identity.has_edits = asset->has_edits;
    identity.import_state = qstring_from_utf8(asset->import_state);
    return format_photo_debug_info(identity);
}

QString StudioPresenter::selectedPhotoParametersDebugInfo() const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !develop_loaded_)
        return {};
    const AssetDescriptor descriptor{asset->id, asset->normalized_uri, asset->content_fingerprint};
    auto recipe = recipe_from_develop(descriptor, develop_);
    if (!recipe)
        return {};
    auto serialized = serialize_recipe(recipe.value());
    if (!serialized)
        return {};

    PhotoParametersDebugInfo parameters;
    parameters.catalog = catalog_path_;
    parameters.asset_id = selected_asset_id_;
    parameters.display_name = qstring_from_utf8(asset_display_name(*asset));
    parameters.recipe_state =
        develop_ == saved_develop_ ? QStringLiteral("saved") : QStringLiteral("pending");
    parameters.recipe_json = qstring_from_utf8(serialized.value());
    return format_photo_parameters_debug_info(parameters);
}

QString StudioPresenter::presetDebugInfo(const QString &path) const
{
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    const QFileInfo info(input_path);
    if (!info.exists() || !info.isFile())
        return {};
    if (info.size() > static_cast<qint64>(kRecipeStyleFileMaxBytes))
        return {};
    const QString canonical =
        info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
    QString name = info.completeBaseName();
    QString kind;
    for (const auto &entry : develop_presets_)
    {
        const auto listed = entry.toMap();
        const QFileInfo listed_info(listed.value(QStringLiteral("path")).toString());
        const QString listed_path = listed_info.canonicalFilePath().isEmpty() ?
                                        listed_info.absoluteFilePath() :
                                        listed_info.canonicalFilePath();
        if (listed_path == canonical)
        {
            name = listed.value(QStringLiteral("name")).toString();
            kind = listed.value(QStringLiteral("kind")).toString();
            break;
        }
    }
    if (kind.isEmpty())
    {
        auto text = read_utf8_text_file(utf8_from_qstring(canonical), kRecipeStyleFileMaxBytes);
        if (!text)
            return {};
        if (is_crs_xmp_document(text.value()))
        {
            kind = QStringLiteral("crs");
            auto parsed_name = crs_xmp_preset_name(text.value());
            if (parsed_name && !parsed_name.value().empty())
                name = QString::fromStdString(parsed_name.value());
        }
        else
        {
            auto style = parse_recipe_style_json(text.value());
            if (!style)
                return {};
            kind = QStringLiteral("style");
            name = QString::fromStdString(style.value().name);
        }
    }
    QFile file(canonical);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray bytes = file.readAll();
    if (bytes.size() != info.size())
        return {};
    PresetDebugIdentity identity;
    identity.name = name;
    identity.path = canonical;
    identity.kind = kind;
    identity.sha256 =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    identity.size_bytes = QString::number(bytes.size());
    identity.mtime_unix_ms = QString::number(info.lastModified().toMSecsSinceEpoch());
    return format_preset_debug_info(identity);
}

void StudioPresenter::copySelectedPhotoDebugInfo()
{
    const QString text = selectedPhotoDebugInfo();
    if (text.isEmpty())
        return;
    if (!write_clipboard_text(text))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Photo information could not be copied to the clipboard."));
        return;
    }
    setStatus(QCoreApplication::translate("StudioPresenter", "Photo information copied."));
}

void StudioPresenter::copySelectedPhotoParametersDebugInfo()
{
    const QString text = selectedPhotoParametersDebugInfo();
    if (text.isEmpty())
    {
        setError(
            QCoreApplication::translate("StudioPresenter", "Photo parameters could not be read."));
        return;
    }
    if (!write_clipboard_text(text))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Photo parameters could not be copied to the clipboard."));
        return;
    }
    setError({});
    setStatus(QCoreApplication::translate("StudioPresenter", "Photo parameters copied."));
}

void StudioPresenter::copyPresetDebugInfo(const QString &path)
{
    QString input_path = path.trimmed();
    if (input_path.startsWith(QStringLiteral("file:")))
        input_path = QUrl(input_path).toLocalFile();
    if (!QFileInfo::exists(input_path) || !QFileInfo(input_path).isFile())
    {
        setError(QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
        return;
    }
    const QString text = presetDebugInfo(input_path);
    if (text.isEmpty())
    {
        setError(QCoreApplication::translate("StudioPresenter",
                                             "Preset information could not be read."));
        return;
    }
    if (!write_clipboard_text(text))
    {
        setError(QCoreApplication::translate(
            "StudioPresenter", "Preset information could not be copied to the clipboard."));
        return;
    }
    setStatus(QCoreApplication::translate("StudioPresenter", "Preset information copied."));
}

void StudioPresenter::addRetouchRegion(const QVariantMap &values)
{
    const auto reject = [&](const QString &reason)
    {
        setError(QCoreApplication::translate("DevelopPanel", "Retouch region was rejected") +
                 QStringLiteral(" [") + reason + QStringLiteral("]"));
    };
    if (develop_.retouch.regions.size() >= kRetouchMaxRegions)
    {
        reject(QStringLiteral("region_limit"));
        return;
    }
    const auto number = [&](const char *name, const double minimum,
                            const double maximum) -> std::optional<double>
    {
        const auto found = values.constFind(QString::fromLatin1(name));
        if (found == values.cend())
            return std::nullopt;
        bool ok = false;
        const double value = found.value().toDouble(&ok);
        return ok && std::isfinite(value) && value >= minimum && value <= maximum ?
                   std::optional<double>{value} :
                   std::nullopt;
    };
    const QString mode_text = values.value(QStringLiteral("mode")).toString();
    RetouchMode mode = RetouchMode::kHeal;
    if (mode_text == QLatin1String("clone"))
        mode = RetouchMode::kClone;
    else if (mode_text == QLatin1String("heal"))
        mode = RetouchMode::kHeal;
    else if (mode_text == QLatin1String("blur"))
        mode = RetouchMode::kBlur;
    else if (mode_text == QLatin1String("fill"))
        mode = RetouchMode::kFill;
    else
    {
        reject(QStringLiteral("unsupported_mode"));
        return;
    }
    const auto center_x = number("centerX", 0.0, 1.0);
    const auto center_y = number("centerY", 0.0, 1.0);
    const auto radius = number("radius", kCanonicalMaskPositiveMin, 1.0);
    const auto feather = number("feather", 0.0, 1.0);
    const auto opacity = number("opacity", 0.0, 1.0);
    const auto source_x = number("sourceX", 0.0, 1.0);
    const auto source_y = number("sourceY", 0.0, 1.0);
    const auto blur_radius = number("blurRadius", kRetouchBlurRadiusMin, kRetouchBlurRadiusMax);
    const auto fill_r = number("fillR", 0.0, 1.0);
    const auto fill_g = number("fillG", 0.0, 1.0);
    const auto fill_b = number("fillB", 0.0, 1.0);
    const auto fill_brightness = number("fillBrightness", -1.0, 1.0);
    if (!center_x || !center_y || !radius || !feather || !opacity || !source_x || !source_y ||
        !blur_radius || !fill_r || !fill_g || !fill_b || !fill_brightness)
    {
        reject(QStringLiteral("invalid_numeric_field"));
        return;
    }
    const QString blur_text = values.value(QStringLiteral("blurType")).toString();
    const QString fill_text = values.value(QStringLiteral("fillMode")).toString();
    if (blur_text != QLatin1String("gaussian") && blur_text != QLatin1String("bilateral"))
    {
        reject(QStringLiteral("unsupported_blur_type"));
        return;
    }
    if (fill_text != QLatin1String("erase") && fill_text != QLatin1String("color"))
    {
        reject(QStringLiteral("unsupported_fill_mode"));
        return;
    }

    DevelopParams next = develop_;
    std::size_t suffix = next.retouch.regions.size() + 1U;
    std::string mask_id;
    do
    {
        mask_id = "studio-retouch-" + std::to_string(suffix++);
    } while (std::any_of(next.masks.begin(), next.masks.end(),
                         [&mask_id](const Mask &mask) { return mask.id == mask_id; }));
    Mask mask{mask_id, kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    mask.payload = CircleMask{*center_x, *center_y, *radius, *feather};
    next.masks.push_back(std::move(mask));
    RetouchRegion region;
    region.mask_id = mask_id;
    region.mode = mode;
    region.opacity = *opacity;
    region.source_x = *source_x;
    region.source_y = *source_y;
    region.blur_type = blur_text == QLatin1String("gaussian") ? RetouchBlurType::kGaussian :
                                                                RetouchBlurType::kBilateral;
    region.blur_radius = *blur_radius;
    region.fill_mode =
        fill_text == QLatin1String("erase") ? RetouchFillMode::kErase : RetouchFillMode::kColor;
    region.fill_color = {*fill_r, *fill_g, *fill_b};
    region.fill_brightness = *fill_brightness;
    next.retouch.regions.push_back(std::move(region));
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::removeRetouchRegion(const int index)
{
    if (index < 0 || static_cast<std::size_t>(index) >= develop_.retouch.regions.size())
    {
        setError(QCoreApplication::translate("DevelopPanel", "Retouch region was rejected") +
                 QStringLiteral(" [invalid_region_index]"));
        return;
    }
    DevelopParams next = develop_;
    const std::string mask_id = next.retouch.regions[static_cast<std::size_t>(index)].mask_id;
    next.retouch.regions.erase(next.retouch.regions.begin() + index);
    const bool group_references_mask = std::any_of(
        next.masks.begin(), next.masks.end(),
        [&mask_id](const Mask &mask)
        {
            const auto *group = std::get_if<MaskGroup>(&mask.payload);
            return group != nullptr && std::any_of(group->children.begin(), group->children.end(),
                                                   [&mask_id](const MaskGroupChild &child)
                                                   { return child.mask_id == mask_id; });
        });
    if (mask_id.starts_with("studio-retouch-") &&
        std::none_of(next.retouch.regions.begin(), next.retouch.regions.end(),
                     [&mask_id](const RetouchRegion &region)
                     { return region.mask_id == mask_id; }) &&
        (!next.color_harmonizer_mask_id || *next.color_harmonizer_mask_id != mask_id) &&
        (!next.graduated_mask_id || *next.graduated_mask_id != mask_id) && !group_references_mask)
    {
        next.masks.erase(std::remove_if(next.masks.begin(), next.masks.end(),
                                        [&mask_id](const Mask &mask)
                                        { return mask.id == mask_id; }),
                         next.masks.end());
    }
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::setToneCurve(const QVariantList &points)
{
    setCurvePoints(QStringLiteral("tone"), 0, points);
}

void StudioPresenter::previewToneCurve(const QVariantList &points)
{
    previewCurvePoints(QStringLiteral("tone"), 0, points);
}

void StudioPresenter::setCurveFamily(const int family)
{
    const int next = family == 1 ? 1 : 0;
    if (curve_family_ == next)
        return;
    curve_family_ = next;
    curve_channel_ = 0;
    emit editChanged();
}

void StudioPresenter::setCurveChannel(const int channel)
{
    const int max_channel = curve_family_ == 0 ? 3 : 2;
    const int next = std::clamp(channel, 0, max_channel);
    if (curve_channel_ == next)
        return;
    curve_channel_ = next;
    emit editChanged();
}

void StudioPresenter::apply_curve_points(const QString &family, const int channel,
                                         const QVariantList &points, const DevelopEdit edit)
{
    DevelopParams next = develop_;
    const int family_index = family == QLatin1String("tone") ? 1 : 0;
    curve_family_ = family_index;
    if (family_index == 0)
    {
        curve_channel_ = std::clamp(channel, 0, 3);
        if (curve_channel_ <= 0)
        {
            next.rgb_curve.mode = std::string(kRgbLevelsModeLinked);
            next.rgb_curve.channels[0] = tone_curve_from_variant(points);
        }
        else
        {
            next.rgb_curve.mode = std::string(kRgbLevelsModeIndependent);
            next.rgb_curve.channels[static_cast<std::size_t>(curve_channel_ - 1)] =
                tone_curve_from_variant(points);
        }
    }
    else
    {
        curve_channel_ = std::clamp(channel, 0, 2);
        if (curve_channel_ == 1)
        {
            next.tone_curve_channel_mode = std::string(kToneCurveChannelModeIndependent);
            next.tone_curve_a = tone_curve_from_variant(points);
        }
        else if (curve_channel_ == 2)
        {
            next.tone_curve_channel_mode = std::string(kToneCurveChannelModeIndependent);
            next.tone_curve_b = tone_curve_from_variant(points);
        }
        else
        {
            next.tone_curve = tone_curve_from_variant(points);
        }
    }
    mutate_develop(std::move(next), edit, true,
                   edit == DevelopEdit::Commit ?
                       std::optional<std::string>{"curve:" + utf8_from_qstring(family) + ":" +
                                                  std::to_string(channel)} :
                       std::nullopt);
}

void StudioPresenter::setCurvePoints(const QString &family, const int channel,
                                     const QVariantList &points)
{
    apply_curve_points(family, channel, points, DevelopEdit::Commit);
}

void StudioPresenter::previewCurvePoints(const QString &family, const int channel,
                                         const QVariantList &points)
{
    apply_curve_points(family, channel, points, DevelopEdit::Preview);
}

void StudioPresenter::sync_curve_ui_from_develop()
{
    if (!develop_.rgb_curve.is_identity())
        curve_family_ = 0;
    else if (!tone_curve_is_identity(develop_.tone_curve) ||
             !tone_curve_is_identity(develop_.tone_curve_a) ||
             !tone_curve_is_identity(develop_.tone_curve_b))
        curve_family_ = 1;
    else
        curve_family_ = 0;
    curve_channel_ = 0;
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
        if (crop_tool_active_)
        {
            mutate_develop(std::move(next), DevelopEdit::Overlay);
            return;
        }
    }
    mutate_develop(std::move(next), DevelopEdit::Preview);
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
    clamp_selected_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit, true, std::string{"cropRect"});
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
    clamp_selected_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Overlay);
}

void StudioPresenter::setCropAspect(const QString &aspect)
{
    if (aspect == QLatin1String("locked"))
    {
        locked_crop_ratio_ = develop_.crop_width / std::max(develop_.crop_height, 1e-6);
        crop_aspect_ = QStringLiteral("locked");
        emit editChanged();
        return;
    }
    DevelopParams next = develop_;
    if (!apply_crop_aspect(next, utf8_from_qstring(aspect)))
    {
        return;
    }
    crop_aspect_ = aspect;
    locked_crop_ratio_ = 0.0;
    fit_geometry_crop(next);
    clamp_selected_crop(next);
    if (!mutate_develop(std::move(next), DevelopEdit::Commit))
    {
        emit editChanged();
    }
}

void StudioPresenter::rotateLeft()
{
    DevelopParams next = develop_;
    next.rotate_quarters = (next.rotate_quarters + 3) % 4;
    transform_crop_for_quarter_turns(next, 3);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::rotateRight()
{
    DevelopParams next = develop_;
    next.rotate_quarters = (next.rotate_quarters + 1) % 4;
    transform_crop_for_quarter_turns(next, 1);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::flipHorizontal()
{
    DevelopParams next = develop_;
    next.flip_horizontal = next.flip_horizontal == 0 ? 1 : 0;
    transform_crop_for_flip(next, true, false);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::flipVertical()
{
    DevelopParams next = develop_;
    next.flip_vertical = next.flip_vertical == 0 ? 1 : 0;
    transform_crop_for_flip(next, false, true);
    fit_geometry_crop(next);
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::setCropToolActive(const bool active)
{
    if (crop_tool_active_ == active)
    {
        return;
    }
    if (active)
    {
        static_cast<void>(clear_comparison());
    }
    crop_tool_active_ = active;
    if (active)
    {
        if (white_balance_pick_active_)
        {
            white_balance_pick_active_ = false;
        }
        setZoomMode(QStringLiteral("fit"));
        DevelopParams next = develop_;
        // Geometry is rendered by the canonical Perspective owner while Crop
        // is stripped. The overlay therefore edits normalized coordinates in
        // the actual post-perspective frame without reproducing a homography
        // in QML.
        crop_guide_ready_ = false;
        if (mutate_develop(std::move(next), DevelopEdit::Commit))
        {
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
    mutate_develop(std::move(next), DevelopEdit::Commit, true, field);
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
        locked_crop_ratio_ = 0.0;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

bool StudioPresenter::sectionModified(const QString &section) const
{
    return develop_section_modified(develop_, utf8_from_qstring(section));
}

bool StudioPresenter::sectionEffectEnabled(const QString &section) const
{
    return develop_section_effect_enabled(develop_, utf8_from_qstring(section));
}

void StudioPresenter::setSectionEffectEnabled(const QString &section, const bool enabled)
{
    DevelopParams next = develop_;
    if (!set_develop_section_effect_enabled(next, utf8_from_qstring(section), enabled))
    {
        return;
    }
    mutate_develop(std::move(next), DevelopEdit::Commit);
}

void StudioPresenter::resetAllEdits()
{
    crop_aspect_ = QStringLiteral("free");
    locked_crop_ratio_ = 0.0;
    DevelopParams reset;
    reset.sigmoid_enabled = develop_.sigmoid_enabled;
    mutate_develop(std::move(reset), DevelopEdit::Commit);
}

void StudioPresenter::copyParametersSelected(const QVariantList &fields)
{
    if (selected_asset_id_.isEmpty())
        return;
    std::vector<std::string> selected_fields;
    const QString selection_error =
        collect_modified_parameter_selection(fields, baseline_develop(), develop_, selected_fields);
    if (!selection_error.isEmpty())
    {
        setError(selection_error);
        return;
    }
    copied_parameters_ = CopiedDevelopParameters{develop_, std::move(selected_fields)};
    emit copiedParametersChanged();
    setStatus(QCoreApplication::translate("StudioPresenter", "Parameters copied."));
}

void StudioPresenter::pasteParameters()
{
    if (selected_asset_id_.isEmpty() || !copied_parameters_)
        return;
    DevelopParams next = develop_;
    auto applied =
        apply_develop_selected_fields(next, copied_parameters_->source, copied_parameters_->fields);
    if (!applied)
    {
        setError(qstring_from_utf8(applied.error().message));
        return;
    }
    if (mutate_develop(std::move(next), DevelopEdit::Commit))
        setStatus(QCoreApplication::translate("StudioPresenter", "Parameters pasted."));
}

void StudioPresenter::applyDevelopNumbers(const QVariantMap &fields, const DevelopEdit edit)
{
    if (fields.isEmpty())
    {
        return;
    }
    DevelopParams next = develop_;
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
    {
        if (it.key().trimmed().isEmpty())
        {
            return;
        }
        bool ok = false;
        const double value = it.value().toDouble(&ok);
        if (!ok || !std::isfinite(value))
        {
            return;
        }
        if (!apply_develop_field(next, utf8_from_qstring(it.key()), value))
        {
            return;
        }
    }
    std::optional<std::string> history_coalesce_key;
    if (edit == DevelopEdit::Commit && fields.size() == 1)
    {
        history_coalesce_key = utf8_from_qstring(fields.cbegin().key());
    }
    mutate_develop(std::move(next), edit, true, std::move(history_coalesce_key));
}

void StudioPresenter::previewDevelopNumbers(const QVariantMap &fields)
{
    applyDevelopNumbers(fields, DevelopEdit::Preview);
}

void StudioPresenter::setDevelopNumbers(const QVariantMap &fields)
{
    applyDevelopNumbers(fields, DevelopEdit::Commit);
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
    mutate_develop(previous, DevelopEdit::Revert);
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
    mutate_develop(next, DevelopEdit::Revert);
}

void StudioPresenter::toggleBeforeAfter()
{
    static_cast<void>(clear_comparison());
    before_after_ = !before_after_;
    emit editChanged();
    enqueue_preview();
}

void StudioPresenter::toggleComparison()
{
    if (comparison_active_)
    {
        if (clear_comparison())
        {
            emit editChanged();
            emit previewChanged();
        }
        return;
    }
    if (selected_asset_id_.isEmpty() || browse_mode_ != QLatin1String("develop"))
    {
        return;
    }
    if (crop_tool_active_)
    {
        setCropToolActive(false);
    }
    if (white_balance_pick_active_)
    {
        setWhiteBalancePickActive(false);
    }
    if (mask_overlay_visible_)
    {
        setMaskOverlay(mask_overlay_target_, false);
    }

    comparison_active_ = true;
    comparison_before_requested_ = true;
    if (before_after_)
    {
        QImage before;
        {
            const QMutexLocker lock(&preview_image_mutex_);
            before = preview_image_;
            comparison_before_image_ = before;
        }
        if (!before.isNull())
        {
            comparison_before_url_ = QUrl(
                QStringLiteral("image://studioPreview/before?r=%1").arg(live_preview_revision_));
            comparison_before_requested_ = false;
        }
        before_after_ = false;
        emit editChanged();
        enqueue_preview();
        return;
    }
    emit editChanged();
    request_comparison_before();
}

bool StudioPresenter::working_source_size(double &width, double &height) const
{
    const auto asset = assets_.assetById(selected_asset_id_);
    if (!asset || !asset->width || !asset->height || *asset->width == 0 || *asset->height == 0)
    {
        return false;
    }
    width = static_cast<double>(*asset->width);
    height = static_cast<double>(*asset->height);
    const auto turns = ((develop_.rotate_quarters % 4) + 4) % 4;
    if (turns == 1 || turns == 3)
    {
        std::swap(width, height);
    }
    return true;
}

void StudioPresenter::clamp_selected_crop(DevelopParams &params) const
{
    double width = 0.0;
    double height = 0.0;
    if (!working_source_size(width, height))
    {
        return;
    }
    clamp_develop_crop_min_extent(params, width, height);
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
    const double rotation = params.straighten_degrees;
    params.straighten_degrees = 0.0;
    constrain_crop_to_straighten(params, selected_working_aspect());
    params.straighten_degrees = rotation;
}

void StudioPresenter::fit_geometry_crop(DevelopParams &params) const
{
    const double rotation = params.straighten_degrees;
    params.straighten_degrees = 0.0;
    fit_crop_to_straighten(params, selected_working_aspect());
    params.straighten_degrees = rotation;
}

} // namespace ravo
