#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/rapidraw_tone_controls.h"

#include "develop_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <iomanip>
#include <map>
#include <new>
#include <numbers>
#include <set>
#include <sstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ravo::develop_internal
{

bool assign_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
    const auto selected = [value](const auto &options) -> std::optional<std::string>
    {
        if (!std::isfinite(value))
        {
            return std::nullopt;
        }
        const auto index = static_cast<std::int64_t>(std::llround(value));
        if (std::abs(value - static_cast<double>(index)) > 1.0e-9 || index < 0 ||
            index >= static_cast<std::int64_t>(options.size()))
        {
            return std::nullopt;
        }
        return std::string(options[static_cast<std::size_t>(index)]);
    };
    if (name == "demosaicModeIndex")
    {
        const auto mode = selected(
            std::array<std::string_view, 4>{kDemosaicModeRcd, kDemosaicModePpg,
                                            kDemosaicModeMarkesteijn1, kDemosaicModeMarkesteijn3});
        if (!mode)
        {
            return false;
        }
        params.demosaic_mode = std::move(*mode);
        return true;
    }
    if (apply_temperature_field(params.temperature, name, value))
    {
        return true;
    }
    if (name == "profileGammaEnabled")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma_enabled = value >= 0.5;

        return true;
    }
    if (name == "profileGammaModeIndex")
    {
        auto mode = selected(kSelectableProfileGammaModes);
        if (!mode)
        {
            return false;
        }
        params.profile_gamma.mode = std::move(*mode);

        return true;
    }
    if (name == "profileGammaLinear")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.linear = value;

        return true;
    }
    if (name == "profileGammaGamma")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.gamma = value;

        return true;
    }
    if (name == "profileGammaDynamicRange")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.dynamic_range = value;

        return true;
    }
    if (name == "profileGammaGreyPoint")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.grey_point = value;

        return true;
    }
    if (name == "profileGammaShadowsRange")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.shadows_range = value;

        return true;
    }
    if (name == "profileGammaSecurityFactor")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.profile_gamma.security_factor = value;

        return true;
    }
    if (name == "inputProfile")
    {
        auto profile = selected(kSelectableInputProfiles);
        if (!profile)
        {
            return false;
        }
        params.input_color.input_profile = std::move(*profile);
        params.input_color.input_profile_filename.clear();

        return true;
    }
    if (name == "workingProfile")
    {
        auto profile = selected(kSelectableWorkingProfiles);
        if (!profile)
        {
            return false;
        }
        params.input_color.working_profile = std::move(*profile);
        params.input_color.working_profile_filename.clear();

        return true;
    }
    if (name == "renderingIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.input_color.rendering_intent = std::move(*intent);

        return true;
    }
    if (name == "gamutNormalize")
    {
        auto normalize = selected(kSelectableColorNormalizations);
        if (!normalize)
        {
            return false;
        }
        params.input_color.gamut_normalize = std::move(*normalize);

        return true;
    }
    if (name == "blueMapping")
    {
        params.input_color.blue_mapping = value >= 0.5;

        return true;
    }
    if (name == "outputProfile")
    {
        auto profile = selected(kSelectableOutputProfiles);
        if (!profile)
        {
            return false;
        }
        params.output_color.output_profile = std::move(*profile);
        params.output_color.output_profile_filename.clear();

        return true;
    }
    if (name == "outputRenderingIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.output_color.rendering_intent = std::move(*intent);

        return true;
    }
    if (name == "proofMode")
    {
        auto mode = selected(kSelectableProofModes);
        if (!mode)
        {
            return false;
        }
        params.output_color.proof_mode = std::move(*mode);

        return true;
    }
    if (name == "proofProfile")
    {
        auto profile = selected(kSelectableProofProfiles);
        if (!profile)
        {
            return false;
        }
        params.output_color.proof_profile = std::move(*profile);
        params.output_color.proof_profile_filename.clear();

        return true;
    }
    if (name == "proofIntent")
    {
        auto intent = selected(kSelectableColorIntents);
        if (!intent)
        {
            return false;
        }
        params.output_color.proof_intent = std::move(*intent);

        return true;
    }
    if (name == "outputBlackPointCompensation")
    {
        params.output_color.black_point_compensation = value >= 0.5;

        return true;
    }
    if (name == "primariesAchromaticHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.achromatic_tint_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesAchromaticPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.achromatic_tint_purity = value;

        return true;
    }
    if (name == "primariesRedHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.red_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesRedPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.red_purity = value;

        return true;
    }
    if (name == "primariesGreenHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.green_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesGreenPurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.green_purity = value;

        return true;
    }
    if (name == "primariesBlueHueDegrees")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.blue_hue = value * std::numbers::pi / 180.0;

        return true;
    }
    if (name == "primariesBluePurity")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.primaries.blue_purity = value;

        return true;
    }
    if (name == "channelMixerRR")
    {
        params.channel_mixer.red[0] = value;

        return true;
    }
    if (name == "channelMixerRG")
    {
        params.channel_mixer.red[1] = value;

        return true;
    }
    if (name == "channelMixerRB")
    {
        params.channel_mixer.red[2] = value;

        return true;
    }
    if (name == "channelMixerGR")
    {
        params.channel_mixer.green[0] = value;

        return true;
    }
    if (name == "channelMixerGG")
    {
        params.channel_mixer.green[1] = value;

        return true;
    }
    if (name == "channelMixerGB")
    {
        params.channel_mixer.green[2] = value;

        return true;
    }
    if (name == "channelMixerBR")
    {
        params.channel_mixer.blue[0] = value;

        return true;
    }
    if (name == "channelMixerBG")
    {
        params.channel_mixer.blue[1] = value;

        return true;
    }
    if (name == "channelMixerBB")
    {
        params.channel_mixer.blue[2] = value;

        return true;
    }
    if (name == "exposureMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.exposure_mode =
            value == 0.0 ? std::string(kExposureModeManual) : std::string(kExposureModeDeflicker);
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "exposureBlack")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_black = value;
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "exposure")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_ev = value;
        const double white = std::exp2(-value);
        if (params.exposure_black >= white)
        {
            params.exposure_black = std::max(kExposureBlackMin, white - 0.01);
        }
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "exposureDeflickerPercentile")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_deflicker_percentile = value;
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "exposureDeflickerTarget")
    {
        if (!std::isfinite(value))
        {
            return false;
        }
        params.exposure_deflicker_target_ev = value;
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "exposureCompensateBias")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.exposure_compensate_exposure_bias = value == 1.0;
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "exposureCompensateHighlight")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.exposure_compensate_highlight_preservation = value == 1.0;
        mirror_legacy_exposure_into_instance(params, 0);

        return true;
    }
    if (name == "contrast")
    {
        params.contrast = value;

        return true;
    }
    if (name == "rapidrawEvShift")
    {
        if (!std::isfinite(value) || value < kRapidRawExposureMin ||
            value > kRapidRawExposureMax)
            return false;
        params.rapidraw_basic_tone_enabled = true;
        params.rapidraw_tone_controls_enabled = true;
        params.sigmoid_enabled = false;
        params.rapidraw_ev_shift = value;
        return true;
    }
    if (name == "toneMapperIndex")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        const bool rapidraw = value == 0.0;
        params.rapidraw_basic_tone_enabled = rapidraw;
        params.rapidraw_tone_controls_enabled = rapidraw;
        params.sigmoid_enabled = !rapidraw;
        if (!rapidraw)
        {
            params.rapidraw_ev_shift = 0.0;
            params.rapidraw_exposure = 0.0;
            params.rapidraw_contrast = 0.0;
            params.rapidraw_highlights = 0.0;
            params.rapidraw_shadows = 0.0;
            params.rapidraw_whites = 0.0;
            params.rapidraw_blacks = 0.0;
        }
        return true;
    }
    if (name == "rapidrawExposure")
    {
        if (!std::isfinite(value) || value < kRapidRawExposureMin ||
            value > kRapidRawExposureMax)
            return false;
        params.rapidraw_basic_tone_enabled = true;
        params.rapidraw_tone_controls_enabled = true;
        params.sigmoid_enabled = false;
        params.rapidraw_exposure = value;
        return true;
    }
    if (name == "rapidrawContrast" || name == "rapidrawHighlights" ||
        name == "rapidrawShadows" || name == "rapidrawWhites" || name == "rapidrawBlacks")
    {
        if (!std::isfinite(value) || value < kRapidRawToneMin || value > kRapidRawToneMax)
            return false;
        params.rapidraw_basic_tone_enabled = true;
        params.rapidraw_tone_controls_enabled = true;
        params.sigmoid_enabled = false;
        if (name == "rapidrawContrast")
            params.rapidraw_contrast = value;
        else if (name == "rapidrawHighlights")
            params.rapidraw_highlights = value;
        else if (name == "rapidrawShadows")
            params.rapidraw_shadows = value;
        else if (name == "rapidrawWhites")
            params.rapidraw_whites = value;
        else
            params.rapidraw_blacks = value;
        return true;
    }
    if (name == "highlights")
    {
        params.highlights = value;

        return true;
    }
    if (name == "shadows")
    {
        params.shadows = value;

        return true;
    }
    if (name == "whites")
    {
        params.whites = value;

        return true;
    }
    if (name == "blacks")
    {
        params.blacks = value;

        return true;
    }
    if (name == "vibrance")
    {
        params.vibrance = value;

        return true;
    }
    if (name == "saturation")
    {
        params.saturation = value;

        return true;
    }
    if (name == "straighten")
    {
        params.straighten_degrees = value;

        return true;
    }
    if (name == "perspectiveVertical")
    {
        if (!std::isfinite(value))
            return false;
        params.perspective_vertical = value;
        return true;
    }
    if (name == "perspectiveHorizontal")
    {
        if (!std::isfinite(value))
            return false;
        params.perspective_horizontal = value;
        return true;
    }
    if (name == "perspectiveShear")
    {
        if (!std::isfinite(value))
            return false;
        params.perspective_shear = value;
        return true;
    }
    if (name == "perspectiveConstrainCrop")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.perspective_constrain_crop = value == 1.0;
        return true;
    }
    if (name == "perspectiveInterpolationIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        params.perspective_interpolation_index = static_cast<std::int64_t>(value);
        return true;
    }
    if (name == "cropX")
    {
        params.crop_x = value;

        return true;
    }
    if (name == "cropY")
    {
        params.crop_y = value;

        return true;
    }
    if (name == "cropWidth")
    {
        params.crop_width = value;

        return true;
    }
    if (name == "cropHeight")
    {
        params.crop_height = value;

        return true;
    }
    if (name == "canvasEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.canvas_enabled = value == 1.0;
        if (params.canvas_enabled)
        {
            params.canvas_present = true;
            params.geometry_effect_enabled = true;
        }
        else if (params.canvas.is_identity())
        {
            params.canvas_present = false;
        }

        return true;
    }
    if (name == "canvasLeft" || name == "canvasRight" || name == "canvasTop" ||
        name == "canvasBottom")
    {
        params.canvas_present = true;
        params.canvas_enabled = true;
        params.geometry_effect_enabled = true;
        double *target = name == "canvasLeft"  ? &params.canvas.percent_left :
                         name == "canvasRight" ? &params.canvas.percent_right :
                         name == "canvasTop"   ? &params.canvas.percent_top :
                                                 &params.canvas.percent_bottom;
        *target = value;

        return true;
    }
    if (name == "canvasColorIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 4.0)
            return false;
        params.canvas_present = true;
        params.canvas_enabled = true;
        params.geometry_effect_enabled = true;
        params.canvas.color = static_cast<CanvasColor>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "sharpen")
    {
        params.sharpen = value;

        return true;
    }
    if (name == "sharpenRadius")
    {
        params.sharpen_radius = value;

        return true;
    }
    if (name == "sharpenThreshold")
    {
        params.sharpen_threshold = value;

        return true;
    }
    if (name == "texture")
    {
        params.texture.strength = value;

        return true;
    }
    if (name == "textureDetailThreshold")
    {
        params.texture.detail_threshold = value;

        return true;
    }
    if (name == "textureIterations")
    {
        if (!std::isfinite(value) || std::floor(value) != value)
        {
            return false;
        }
        params.texture.iterations = static_cast<std::int64_t>(value);

        return true;
    }
    if (name == "clarity")
    {
        params.clarity = value;

        return true;
    }
    if (name == "vignette")
    {
        params.vignette = value;

        return true;
    }
    if (name == "vignetteMidpoint")
    {
        params.vignette_midpoint = value;

        return true;
    }
    if (name == "vignetteFalloff")
    {
        params.vignette_falloff = value;

        return true;
    }
    if (name == "vignetteShape")
    {
        params.vignette_shape = value;

        return true;
    }
    if (name == "vignetteCenterX")
    {
        params.vignette_center_x = value;

        return true;
    }
    if (name == "vignetteCenterY")
    {
        params.vignette_center_y = value;

        return true;
    }
    if (name == "grain")
    {
        params.grain = value;

        return true;
    }
    if (name == "bloom")
    {
        params.bloom = value;

        return true;
    }
    if (name == "soften")
    {
        params.soften = value;

        return true;
    }
    if (name == "dehaze")
    {
        params.dehaze = value;

        return true;
    }
    if (name == "dehazeDistance")
    {
        params.dehaze_distance = value;

        return true;
    }
    if (name == "dehazeAdaptive")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.dehaze_adaptive = value == 1.0;

        return true;
    }
    if (name == "outputDitherEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.output_dither_present = true;
        params.output_dither_enabled = value == 1.0;
        if (params.output_dither_enabled)
            params.effects_effect_enabled = true;

        return true;
    }
    if (name == "outputDitherMethodIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value)
            return false;
        auto method = output_dither_method_from_index(static_cast<std::int64_t>(value));
        if (!method)
            return false;
        params.output_dither_present = true;
        params.output_dither_enabled = true;
        params.effects_effect_enabled = true;
        params.output_dither.method = method.value();

        return true;
    }
    if (name == "outputDitherDamping")
    {
        if (!std::isfinite(value))
            return false;
        params.output_dither_present = true;
        params.output_dither_enabled = true;
        params.effects_effect_enabled = true;
        params.output_dither.random_damping_db = value;

        return true;
    }
    if (name == "outputFrameEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.frame_present = true;
        params.frame_enabled = value == 1.0;
        if (params.frame_enabled)
            params.effects_effect_enabled = true;

        return true;
    }
    if (name == "outputFrameBorderRed" || name == "outputFrameBorderGreen" ||
        name == "outputFrameBorderBlue" || name == "outputFrameLineRed" ||
        name == "outputFrameLineGreen" || name == "outputFrameLineBlue")
    {
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        const bool line = name.starts_with("outputFrameLine");
        auto &color = line ? params.frame.frame_color : params.frame.border_color;
        const std::size_t channel = name.ends_with("Red") ? 0U : name.ends_with("Green") ? 1U : 2U;
        color[channel] = value;

        return true;
    }
    if (name == "outputFrameAspect")
    {
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        params.frame.aspect = value;

        return true;
    }
    if (name == "outputFrameOrientationIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        params.frame.orientation = static_cast<FrameOrientation>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "outputFrameSize" || name == "outputFramePositionH" ||
        name == "outputFramePositionV" || name == "outputFrameLineSize" ||
        name == "outputFrameLineOffset")
    {
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        double *target = name == "outputFrameSize"      ? &params.frame.size :
                         name == "outputFramePositionH" ? &params.frame.position_h :
                         name == "outputFramePositionV" ? &params.frame.position_v :
                         name == "outputFrameLineSize"  ? &params.frame.frame_size :
                                                          &params.frame.frame_offset;
        *target = value;

        return true;
    }
    if (name == "outputFrameBasisIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 4.0)
            return false;
        params.frame_present = true;
        params.frame_enabled = true;
        params.effects_effect_enabled = true;
        params.frame.basis = static_cast<FrameBasis>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "watermarkEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.watermark_present = true;
        params.watermark_enabled = value == 1.0;
        if (params.watermark_enabled)
            params.effects_effect_enabled = true;

        return true;
    }
    if (name == "watermarkRed" || name == "watermarkGreen" || name == "watermarkBlue")
    {
        params.watermark_present = true;
        params.watermark_enabled = true;
        params.effects_effect_enabled = true;
        const std::size_t channel = name.ends_with("Red") ? 0U : name.ends_with("Green") ? 1U : 2U;
        params.watermark.color[channel] = value;

        return true;
    }
    if (name == "watermarkOpacity" || name == "watermarkScale" || name == "watermarkOffsetX" ||
        name == "watermarkOffsetY" || name == "watermarkRotation")
    {
        params.watermark_present = true;
        params.watermark_enabled = true;
        params.effects_effect_enabled = true;
        double *target = name == "watermarkOpacity" ? &params.watermark.opacity :
                         name == "watermarkScale"   ? &params.watermark.scale_percent :
                         name == "watermarkOffsetX" ? &params.watermark.x_offset :
                         name == "watermarkOffsetY" ? &params.watermark.y_offset :
                                                      &params.watermark.rotation_degrees;
        *target = value;

        return true;
    }
    if (name == "watermarkAlignmentIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 8.0)
            return false;
        params.watermark_present = true;
        params.watermark_enabled = true;
        params.effects_effect_enabled = true;
        params.watermark.alignment =
            static_cast<WatermarkAlignment>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "velvia")
    {
        if (value == 0.0)
        {
            const DevelopParams identity;
            params.velvia_present = identity.velvia_present;
            params.velvia_enabled = identity.velvia_enabled;
            params.velvia = identity.velvia;
            params.velvia_mask_id = identity.velvia_mask_id;
            return true;
        }
        params.velvia_present = true;
        params.velvia_enabled = true;
        params.color_effect_enabled = true;
        params.velvia.strength = value * 100.0;

        return true;
    }
    if (name == "velviaEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.velvia_present = true;
        params.velvia_enabled = value == 1.0;
        if (params.velvia_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "velviaStrength" || name == "velviaBias")
    {
        params.velvia_present = true;
        params.velvia_enabled = true;
        params.color_effect_enabled = true;
        double *target = name == "velviaStrength" ? &params.velvia.strength : &params.velvia.bias;
        *target = value;

        return true;
    }
    if (name == "lut3dEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.lut3d_enabled = value == 1.0;
        params.lut3d_present = params.lut3d_enabled || !params.lut3d.file_path.empty();
        if (params.lut3d_enabled)
            params.color_effect_enabled = true;
        return true;
    }
    if (name == "lut3dInputSpaceIndex" || name == "lut3dOutputSpaceIndex")
    {
        const auto space = selected(kLut3dSelectableSpaces);
        if (!space)
            return false;
        params.lut3d_present = true;
        params.lut3d_enabled = true;
        params.color_effect_enabled = true;
        (name == "lut3dInputSpaceIndex" ? params.lut3d.input_space : params.lut3d.output_space) =
            *space;
        return true;
    }
    if (name == "lut3dInterpolationIndex")
    {
        const auto interpolation = selected(kLut3dSelectableInterpolations);
        if (!interpolation)
            return false;
        params.lut3d_present = true;
        params.lut3d_enabled = true;
        params.color_effect_enabled = true;
        params.lut3d.interpolation = *interpolation;
        return true;
    }
    if (name == "lut3dStrength")
    {
        params.lut3d_present = true;
        params.lut3d_enabled = true;
        params.color_effect_enabled = true;
        params.lut3d.strength = value;
        return true;
    }
    if (apply_legacy_color_balance_field(params.color_balance, name, value))
    {
        params.color_balance_enabled = true;

        return true;
    }
    if (apply_color_checker_field(params, name, value))
    {
        return true;
    }
    if (apply_color_balance_field(params.color_balance_rgb, name, value))
    {
        mirror_legacy_color_balance_rgb_into_instance(params, 0);
        return true;
    }
    if (apply_color_correction_field(params, name, value))
    {
        return true;
    }
    if (apply_color_contrast_field(params, name, value))
    {
        return true;
    }
    if (apply_color_reconstruction_field(params, name, value))
    {
        return true;
    }
    if (apply_color_harmonizer_field(params, name, value))
    {
        return true;
    }
    if (name == "monochrome")
    {
        params.monochrome_present = true;
        params.monochrome_enabled = value > 0.0;
        params.monochrome.mix = value;
        if (params.monochrome_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "monochromeEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.monochrome_present = true;
        params.monochrome_enabled = value == 1.0;
        if (params.monochrome_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "monochromeFilterA" || name == "monochromeFilterB" || name == "monochromeSize" ||
        name == "monochromeHighlights" || name == "monochromeMix")
    {
        params.monochrome_present = true;
        params.monochrome_enabled = true;
        params.color_effect_enabled = true;
        double *target = name == "monochromeFilterA"    ? &params.monochrome.filter_a :
                         name == "monochromeFilterB"    ? &params.monochrome.filter_b :
                         name == "monochromeSize"       ? &params.monochrome.size :
                         name == "monochromeHighlights" ? &params.monochrome.highlights :
                                                          &params.monochrome.mix;
        *target = value;

        return true;
    }
    if (name == "splitShadowsHue")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        params.split_toning.shadow_hue = value;

        return true;
    }
    if (name == "splitHighlightsHue")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        params.split_toning.highlight_hue = value;

        return true;
    }
    if (name == "splitBalance")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        params.split_toning.balance = value;

        return true;
    }
    if (name == "splitAmount")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = value > 0.0;
        params.split_toning.mix = value;
        if (params.split_toning_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "splitToningEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        params.split_toning_present = true;
        params.split_toning_enabled = value == 1.0;
        if (params.split_toning_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "splitShadowSaturation" || name == "splitHighlightSaturation" ||
        name == "splitCompress" || name == "splitMix")
    {
        params.split_toning_present = true;
        params.split_toning_enabled = true;
        params.color_effect_enabled = true;
        double *target =
            name == "splitShadowSaturation"    ? &params.split_toning.shadow_saturation :
            name == "splitHighlightSaturation" ? &params.split_toning.highlight_saturation :
            name == "splitCompress"            ? &params.split_toning.compress :
                                                 &params.split_toning.mix;
        *target = value;

        return true;
    }
    if (name == "gamma")
    {
        params.gamma = value;

        return true;
    }
    if (apply_rgb_levels_field(params, name, value))
    {
        return true;
    }
    if (apply_rgb_curve_field(params, name, value))
    {
        return true;
    }
    if (name == "toneCurveInterpolation")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 2, index))
        {
            return false;
        }
        params.tone_curve_interpolation =
            std::string(curve_interpolation_from_index(static_cast<int>(index)));
        return true;
    }
    if (name == "toneCurveChannelMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.tone_curve_channel_mode = value == 1.0 ?
                                             std::string(kToneCurveChannelModeIndependent) :
                                             std::string(kToneCurveChannelModeRgb);
        return true;
    }
    if (name == "toneCurvePreserve")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 6, index))
        {
            return false;
        }
        params.tone_curve_preserve_colors =
            std::string(rgb_levels_preserve_names()[static_cast<std::size_t>(index)]);
        return true;
    }
    if (name == "toneCurveWorkingSpace")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 5, index))
        {
            return false;
        }
        static constexpr std::array<std::string_view, 6> spaces{
            kToneCurveWorkingSpaceRgb,  kToneCurveWorkingSpaceLab,
            kToneCurveWorkingSpaceXyz,  kToneCurveWorkingSpaceLabIndependent,
            kToneCurveWorkingSpaceSrgb, kToneCurveWorkingSpaceLinearRgb};
        params.tone_curve_working_space = std::string(spaces[static_cast<std::size_t>(index)]);
        return true;
    }
    if (name == "sigmoidContrast")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_contrast = value;

        return true;
    }
    if (name == "sigmoidSkew")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_skew = value;

        return true;
    }
    if (name == "sigmoidHuePreservation")
    {
        params.sigmoid_enabled = true;
        params.sigmoid_hue_preservation = value;

        return true;
    }
    if (name == "rawHighlights")
    {
        params.raw_highlights = value;

        return true;
    }
    if (name == "rawHighlightsClip")
    {
        params.raw_highlights_clip = value;

        return true;
    }
    if (name == "rawHighlightsMode")
    {
        params.raw_highlights_mode = value >= 0.5 ? std::string(kRawHighlightsModeInpaint) :
                                                    std::string(kRawHighlightsModeClip);

        return true;
    }
    if (name == "rawDenoiseThreshold")
    {
        params.raw_denoise_threshold = value;

        return true;
    }
    if (name == "hotPixelsStrength")
    {
        params.hot_pixels_strength = value;

        return true;
    }
    if (name == "hotPixelsThreshold")
    {
        params.hot_pixels_threshold = value;

        return true;
    }
    if (name == "hotPixelsPermissive")
    {
        params.hot_pixels_permissive = value >= 0.5;

        return true;
    }
    if (name == "rawCaIterations")
    {
        params.raw_ca_iterations = static_cast<std::int64_t>(std::llround(value));

        return true;
    }
    if (name == "rawCaAvoidShift")
    {
        params.raw_ca_avoid_shift = value >= 0.5;

        return true;
    }
    if (name == "denoise")
    {
        params.denoise = value;

        return true;
    }
    if (name == "denoiseChroma")
    {
        params.denoise_chroma = value;

        return true;
    }
    if (name == "denoiseRadius")
    {
        params.denoise_radius = value;

        return true;
    }
    if (name == "lensK1")
    {
        params.lens_k1 = value;

        return true;
    }
    if (name == "lensK2")
    {
        params.lens_k2 = value;

        return true;
    }
    if (name == "lensTcaR")
    {
        params.lens_tca_r = value;

        return true;
    }
    if (name == "lensTcaB")
    {
        params.lens_tca_b = value;

        return true;
    }
    if (name == "lensVignetting")
    {
        params.lens_vignetting = value;

        return true;
    }
    if (name == "lensMode")
    {
        params.lens_mode =
            value >= 0.5 ? std::string(kLensModeLookup) : std::string(kLensModeManual);

        return true;
    }
    if (name == "lensFocal")
    {
        params.lens_focal_mm = value;

        return true;
    }
    if (name == "colorZonesEnabled")
    {
        if (value != 0.0 && value != 1.0)
            return false;
        if (!params.color_zones_present && value == 1.0)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = value == 1.0;
        if (params.color_zones_enabled)
            params.color_effect_enabled = true;

        return true;
    }
    if (name == "colorZonesSelectByIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        params.color_zones.select_by =
            static_cast<ColorZonesChannel>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "colorZonesBandIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 7.0)
            return false;
        params.color_zones_band = static_cast<std::int64_t>(value);

        return true;
    }
    if (name == "colorZonesStrength")
    {
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        params.color_zones.strength = value;

        return true;
    }
    if (name == "colorZonesLightnessInterpolationIndex" ||
        name == "colorZonesChromaInterpolationIndex" || name == "colorZonesHueInterpolationIndex")
    {
        if (!std::isfinite(value) || std::floor(value) != value || value < 0.0 || value > 2.0)
            return false;
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        const std::size_t channel = name.starts_with("colorZonesLightness") ? 0U :
                                    name.starts_with("colorZonesChroma")    ? 1U :
                                                                              2U;
        params.color_zones.curves[channel].interpolation =
            static_cast<ColorZonesInterpolation>(static_cast<std::uint8_t>(value));

        return true;
    }
    if (name == "colorZonesLightness" || name == "colorZonesChroma" || name == "colorZonesHue")
    {
        if (!params.color_zones_present)
            make_studio_color_zones_curves(params.color_zones);
        if (!studio_color_zones_curves(params.color_zones))
            return false;
        params.color_zones_present = true;
        params.color_zones_enabled = true;
        params.color_effect_enabled = true;
        const std::size_t channel = name == "colorZonesLightness" ? 0U :
                                    name == "colorZonesChroma"    ? 1U :
                                                                    2U;
        const auto band = static_cast<std::size_t>(
            std::clamp(params.color_zones_band, std::int64_t{0}, std::int64_t{7}));
        params.color_zones.curves[channel].points[band].y = value;

        return true;
    }
    if (name == "colorEqBand")
    {
        params.color_eq_band = static_cast<std::int64_t>(std::llround(value));
        params.color_eq_effect_enabled = true;

        return true;
    }
    if (name == "colorEqHue")
    {
        params.color_eq_hue[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;
        params.color_eq_effect_enabled = true;

        return true;
    }
    if (name == "colorEqSat")
    {
        params.color_eq_sat[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;
        params.color_eq_effect_enabled = true;

        return true;
    }
    if (name == "colorEqLight")
    {
        params.color_eq_light[static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}))] = value;
        params.color_eq_effect_enabled = true;

        return true;
    }
    if (name == "graduatedDensity")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_density = value;

        return true;
    }
    if (name == "graduatedHardness")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_hardness = value;

        return true;
    }
    if (name == "graduatedRotation")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_rotation = value;

        return true;
    }
    if (name == "graduatedOffset")
    {
        params.graduated_present = true;
        params.graduated_enabled = true;
        params.graduated_offset = value;

        return true;
    }
    if (name == "toneEqBlacks")
    {
        params.tone_eq_blacks = value;

        return true;
    }
    if (name == "toneEqShadows")
    {
        params.tone_eq_shadows = value;

        return true;
    }
    if (name == "toneEqMidtones")
    {
        params.tone_eq_midtones = value;

        return true;
    }
    if (name == "toneEqHighlights")
    {
        params.tone_eq_highlights = value;

        return true;
    }
    if (name == "toneEqWhites")
    {
        params.tone_eq_whites = value;

        return true;
    }
    std::size_t band = 0;
    if (parse_band_field(name, "colorEqHue", band))
    {
        params.color_eq_hue[band] = value;
        return true;
    }
    if (parse_band_field(name, "colorEqSat", band))
    {
        params.color_eq_sat[band] = value;
        return true;
    }
    if (parse_band_field(name, "colorEqLight", band))
    {
        params.color_eq_light[band] = value;
        return true;
    }
    return false;
}

} // namespace ravo::develop_internal
