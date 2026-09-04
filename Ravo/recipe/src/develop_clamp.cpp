#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"

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

namespace ravo
{
using namespace develop_internal;

void clamp_develop(DevelopParams &params) noexcept
{
    if (params.demosaic_mode != kDemosaicModeRcd && params.demosaic_mode != kDemosaicModePpg &&
        params.demosaic_mode != kDemosaicModeMarkesteijn1 &&
        params.demosaic_mode != kDemosaicModeMarkesteijn3)
    {
        params.demosaic_mode = std::string(kDemosaicModeRcd);
    }
    clamp_temperature(params.temperature);
    const auto clamp_profile_gamma_value =
        [](double &value, const double default_value, const double minimum, const double maximum)
    { value = std::isfinite(value) ? clamp_value(value, minimum, maximum) : default_value; };
    if (!params.profile_gamma_enabled &&
        params.profile_gamma.mode != kProfileGammaModeLogarithmic &&
        params.profile_gamma.mode != kProfileGammaModeGamma)
    {
        params.profile_gamma.mode = std::string(kProfileGammaModeLogarithmic);
    }
    clamp_profile_gamma_value(params.profile_gamma.linear, kProfileGammaLinearDefault,
                              kProfileGammaLinearMin, kProfileGammaLinearMax);
    clamp_profile_gamma_value(params.profile_gamma.gamma, kProfileGammaGammaDefault,
                              kProfileGammaGammaMin, kProfileGammaGammaMax);
    clamp_profile_gamma_value(params.profile_gamma.dynamic_range, kProfileGammaDynamicRangeDefault,
                              kProfileGammaDynamicRangeMin, kProfileGammaDynamicRangeMax);
    clamp_profile_gamma_value(params.profile_gamma.grey_point, kProfileGammaGreyPointDefault,
                              kProfileGammaGreyPointMin, kProfileGammaGreyPointMax);
    clamp_profile_gamma_value(params.profile_gamma.shadows_range, kProfileGammaShadowsRangeDefault,
                              kProfileGammaShadowsRangeMin, kProfileGammaShadowsRangeMax);
    clamp_profile_gamma_value(params.profile_gamma.security_factor,
                              kProfileGammaSecurityFactorDefault, kProfileGammaSecurityFactorMin,
                              kProfileGammaSecurityFactorMax);
    const auto clamp_primaries_value =
        [](double &value, const double default_value, const double minimum, const double maximum)
    { value = std::isfinite(value) ? clamp_value(value, minimum, maximum) : default_value; };
    clamp_primaries_value(params.primaries.achromatic_tint_hue, 0.0, kPrimariesHueMin,
                          kPrimariesHueMax);
    clamp_primaries_value(params.primaries.achromatic_tint_purity, 0.0,
                          kPrimariesAchromaticTintPurityMin, kPrimariesAchromaticTintPurityMax);
    clamp_primaries_value(params.primaries.red_hue, 0.0, kPrimariesHueMin, kPrimariesHueMax);
    clamp_primaries_value(params.primaries.red_purity, 1.0, kPrimariesPrimaryPurityMin,
                          kPrimariesPrimaryPurityMax);
    clamp_primaries_value(params.primaries.green_hue, 0.0, kPrimariesHueMin, kPrimariesHueMax);
    clamp_primaries_value(params.primaries.green_purity, 1.0, kPrimariesPrimaryPurityMin,
                          kPrimariesPrimaryPurityMax);
    clamp_primaries_value(params.primaries.blue_hue, 0.0, kPrimariesHueMin, kPrimariesHueMax);
    clamp_primaries_value(params.primaries.blue_purity, 1.0, kPrimariesPrimaryPurityMin,
                          kPrimariesPrimaryPurityMax);
    for (auto *channel : {&params.channel_mixer.red, &params.channel_mixer.green,
                          &params.channel_mixer.blue, &params.channel_mixer.saturation,
                          &params.channel_mixer.lightness, &params.channel_mixer.grey})
    {
        for (double &value : *channel)
        {
            value = clamp_value(value, -2.0, 2.0);
        }
    }
    if (params.channel_mixer.adaptation != kChannelMixerAdaptationRgb &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationCat16 &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationLinearBradford &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationFullBradford &&
        params.channel_mixer.adaptation != kChannelMixerAdaptationXyz)
    {
        params.channel_mixer.adaptation = std::string(kChannelMixerAdaptationRgb);
    }
    params.channel_mixer.illuminant_x =
        clamp_value(params.channel_mixer.illuminant_x, 0.000001, 0.999999);
    params.channel_mixer.illuminant_y =
        clamp_value(params.channel_mixer.illuminant_y, 0.000001, 0.999999);
    params.channel_mixer.gamut = clamp_value(params.channel_mixer.gamut, 0.0, 12.0);
    clamp_legacy_color_balance(params.color_balance);
    params.color_checker_patch =
        params.color_checker.patches.empty() ?
            0 :
            std::clamp(params.color_checker_patch, std::int64_t{0},
                       static_cast<std::int64_t>(params.color_checker.patches.size() - 1U));
    clamp_color_balance(params.color_balance_rgb);
    clamp_color_correction(params.color_correction);
    clamp_color_contrast(params.color_contrast);
    clamp_color_reconstruction(params.color_reconstruction);
    clamp_color_harmonizer(params.color_harmonizer);
    // Preserve compatibility with existing typed callers that represented an
    // active operation by setting only the historical enabled/value fields.
    // The added explicit-presence bit is needed for disabled/default masked
    // instances, but must not make a normal active round trip unequal.
    if (params.color_harmonizer_enabled)
    {
        params.color_harmonizer_present = true;
    }
    if (params.exposure_mode != kExposureModeManual &&
        params.exposure_mode != kExposureModeDeflicker)
    {
        params.exposure_mode = std::string(kExposureModeManual);
    }
    params.exposure_black =
        clamp_value(params.exposure_black, kExposureBlackMin, kExposureBlackMax);
    params.exposure_ev = clamp_value(params.exposure_ev, kExposureEvMin, kExposureEvMax);
    params.exposure_deflicker_percentile =
        clamp_value(params.exposure_deflicker_percentile, kExposureDeflickerPercentileMin,
                    kExposureDeflickerPercentileMax);
    params.exposure_deflicker_target_ev =
        clamp_value(params.exposure_deflicker_target_ev, kExposureDeflickerTargetEvMin,
                    kExposureDeflickerTargetEvMax);
    if (params.exposure_mode == kExposureModeManual)
    {
        const double white = std::exp2(-params.exposure_ev);
        if (params.exposure_black >= white)
        {
            params.exposure_black = std::max(kExposureBlackMin, white - 0.01);
        }
    }
    for (auto &instance : params.exposure_instances)
    {
        if (instance.mode != kExposureModeManual && instance.mode != kExposureModeDeflicker)
        {
            instance.mode = std::string(kExposureModeManual);
        }
        instance.black = clamp_value(instance.black, kExposureBlackMin, kExposureBlackMax);
        instance.exposure_ev = clamp_value(instance.exposure_ev, kExposureEvMin, kExposureEvMax);
        instance.deflicker_percentile =
            clamp_value(instance.deflicker_percentile, kExposureDeflickerPercentileMin,
                        kExposureDeflickerPercentileMax);
        instance.deflicker_target_ev =
            clamp_value(instance.deflicker_target_ev, kExposureDeflickerTargetEvMin,
                        kExposureDeflickerTargetEvMax);
        if (instance.mode == kExposureModeManual)
        {
            const double white = std::exp2(-instance.exposure_ev);
            if (instance.black >= white)
            {
                instance.black = std::max(kExposureBlackMin, white - 0.01);
            }
        }
    }
    if (!params.exposure_instances.empty())
    {
        const auto &front = params.exposure_instances.front();
        params.exposure_mode = front.mode;
        params.exposure_black = front.black;
        params.exposure_ev = front.exposure_ev;
        params.exposure_deflicker_percentile = front.deflicker_percentile;
        params.exposure_deflicker_target_ev = front.deflicker_target_ev;
        params.exposure_compensate_exposure_bias = front.compensate_exposure_bias;
        params.exposure_compensate_highlight_preservation = front.compensate_highlight_preservation;
        params.exposure_mask_id = front.mask_id;
    }
    params.contrast = clamp_value(params.contrast, -1.0, 1.0);
    params.highlights = clamp_value(params.highlights, -1.0, 1.0);
    params.shadows = clamp_value(params.shadows, -1.0, 1.0);
    params.whites = clamp_value(params.whites, -1.0, 1.0);
    params.blacks = clamp_value(params.blacks, -1.0, 1.0);
    params.vibrance = clamp_value(params.vibrance, -1.0, 1.0);
    params.saturation = clamp_value(params.saturation, -1.0, 1.0);
    params.rotate_quarters = ((params.rotate_quarters % 4) + 4) % 4;
    params.flip_horizontal = flag01(params.flip_horizontal);
    params.flip_vertical = flag01(params.flip_vertical);
    params.straighten_degrees =
        clamp_value(params.straighten_degrees, kDevelopStraightenMin, kDevelopStraightenMax);
    params.perspective_vertical =
        clamp_value(params.perspective_vertical, kPerspectiveShiftMin, kPerspectiveShiftMax);
    params.perspective_horizontal =
        clamp_value(params.perspective_horizontal, kPerspectiveShiftMin, kPerspectiveShiftMax);
    params.perspective_shear =
        clamp_value(params.perspective_shear, kPerspectiveShearMin, kPerspectiveShearMax);
    params.perspective_interpolation_index =
        std::clamp<std::int64_t>(params.perspective_interpolation_index, 0, 2);
    params.crop_width = clamp_value(params.crop_width, 0.01, 1.0);
    params.crop_height = clamp_value(params.crop_height, 0.01, 1.0);
    params.crop_x = clamp_value(params.crop_x, 0.0, 1.0 - params.crop_width);
    params.crop_y = clamp_value(params.crop_y, 0.0, 1.0 - params.crop_height);
    params.canvas.percent_left =
        clamp_value(params.canvas.percent_left, kCanvasPercentMin, kCanvasPercentMax);
    params.canvas.percent_right =
        clamp_value(params.canvas.percent_right, kCanvasPercentMin, kCanvasPercentMax);
    params.canvas.percent_top =
        clamp_value(params.canvas.percent_top, kCanvasPercentMin, kCanvasPercentMax);
    params.canvas.percent_bottom =
        clamp_value(params.canvas.percent_bottom, kCanvasPercentMin, kCanvasPercentMax);
    if (canvas_color_name(params.canvas.color).empty())
        params.canvas.color = CanvasColor::kGreen;
    params.sharpen = clamp_value(params.sharpen, 0.0, 2.0);
    params.sharpen_radius =
        clamp_value(params.sharpen_radius, kSharpenRadiusMin, kSharpenRadiusMax);
    params.sharpen_threshold =
        clamp_value(params.sharpen_threshold, kSharpenThresholdMin, kSharpenThresholdMax);
    params.texture.strength =
        clamp_value(params.texture.strength, kTextureStrengthMin, kTextureStrengthMax);
    params.texture.detail_threshold = clamp_value(
        params.texture.detail_threshold, kTextureDetailThresholdMin, kTextureDetailThresholdMax);
    params.texture.iterations = std::clamp<std::int64_t>(
        params.texture.iterations, kTextureIterationsMin, kTextureIterationsMax);
    params.retouch.num_scales =
        std::clamp<std::int64_t>(params.retouch.num_scales, 0, kRetouchMaxScales);
    params.retouch.merge_from_scale =
        std::clamp<std::int64_t>(params.retouch.merge_from_scale, 0, params.retouch.num_scales);
    params.retouch.max_heal_iterations =
        std::clamp<std::int64_t>(params.retouch.max_heal_iterations, 1, kRetouchMaxHealIterations);
    if (params.retouch.regions.size() > kRetouchMaxRegions)
    {
        params.retouch.regions.resize(kRetouchMaxRegions);
    }
    for (auto &region : params.retouch.regions)
    {
        region.scale = std::clamp<std::int64_t>(region.scale, 0, params.retouch.num_scales + 1);
        region.opacity = clamp_value(region.opacity, 0.0, 1.0);
        region.source_x = clamp_value(region.source_x, 0.0, 1.0);
        region.source_y = clamp_value(region.source_y, 0.0, 1.0);
        region.blur_radius =
            clamp_value(region.blur_radius, kRetouchBlurRadiusMin, kRetouchBlurRadiusMax);
        for (double &channel : region.fill_color)
        {
            channel = clamp_value(channel, 0.0, 1.0);
        }
        region.fill_brightness = clamp_value(region.fill_brightness, -1.0, 1.0);
    }
    params.clarity = clamp_value(params.clarity, -1.0, 1.0);
    params.vignette = clamp_value(params.vignette, -1.0, 1.0);
    params.vignette_midpoint = clamp_value(params.vignette_midpoint, 0.0, 1.0);
    params.vignette_falloff = clamp_value(params.vignette_falloff, 0.05, 1.0);
    params.vignette_shape = clamp_value(params.vignette_shape, 0.5, 5.0);
    params.vignette_center_x = clamp_value(params.vignette_center_x, -1.0, 1.0);
    params.vignette_center_y = clamp_value(params.vignette_center_y, -1.0, 1.0);
    params.grain = clamp_value(params.grain, 0.0, 1.0);
    params.bloom = clamp_value(params.bloom, 0.0, 1.0);
    params.soften = clamp_value(params.soften, 0.0, 1.0);
    params.dehaze = clamp_value(params.dehaze, -1.0, 1.0);
    params.dehaze_distance =
        clamp_value(params.dehaze_distance, kDehazeDistanceMin, kDehazeDistanceMax);
    if (output_dither_method_name(params.output_dither.method).empty())
        params.output_dither.method = OutputDitherMethod::kFloydSteinbergAuto;
    params.output_dither.random_damping_db = clamp_value(
        params.output_dither.random_damping_db, kOutputDitherDampingMin, kOutputDitherDampingMax);
    for (double &channel : params.frame.border_color)
        channel = clamp_value(channel, 0.0, 1.0);
    for (double &channel : params.frame.frame_color)
        channel = clamp_value(channel, 0.0, 1.0);
    params.frame.aspect = clamp_value(params.frame.aspect, -1.0, 3.0);
    if (params.frame.aspect < 0.0 && !near(params.frame.aspect, -1.0))
        params.frame.aspect = -1.0;
    params.frame.size = clamp_value(params.frame.size, 0.0, 0.5);
    params.frame.position_h = clamp_value(params.frame.position_h, 0.0, 1.0);
    params.frame.position_v = clamp_value(params.frame.position_v, 0.0, 1.0);
    params.frame.frame_size = clamp_value(params.frame.frame_size, 0.0, 1.0);
    params.frame.frame_offset = clamp_value(params.frame.frame_offset, 0.0, 1.0);
    if (frame_orientation_name(params.frame.orientation).empty())
        params.frame.orientation = FrameOrientation::kAuto;
    if (frame_basis_name(params.frame.basis).empty())
        params.frame.basis = FrameBasis::kAuto;
    for (double &channel : params.watermark.color)
        channel = clamp_value(channel, 0.0, 1.0);
    params.watermark.opacity = clamp_value(params.watermark.opacity, 0.0, 1.0);
    params.watermark.scale_percent =
        clamp_value(params.watermark.scale_percent, kWatermarkScaleMin, kWatermarkScaleMax);
    params.watermark.x_offset = clamp_value(params.watermark.x_offset, -1.0, 1.0);
    params.watermark.y_offset = clamp_value(params.watermark.y_offset, -1.0, 1.0);
    params.watermark.rotation_degrees =
        clamp_value(params.watermark.rotation_degrees, -180.0, 180.0);
    if (watermark_alignment_name(params.watermark.alignment).empty())
        params.watermark.alignment = WatermarkAlignment::kBottomRight;
    params.velvia.strength = clamp_value(params.velvia.strength, 0.0, 100.0);
    params.velvia.bias = clamp_value(params.velvia.bias, 0.0, 1.0);
    if (!lut3d_space_supported(params.lut3d.input_space))
        params.lut3d.input_space = std::string(kLut3dSpaceSrgb);
    if (!lut3d_space_supported(params.lut3d.output_space))
        params.lut3d.output_space = std::string(kLut3dSpaceSrgb);
    if (!lut3d_interpolation_supported(params.lut3d.interpolation))
        params.lut3d.interpolation = std::string(kLut3dInterpolationTetrahedral);
    params.lut3d.strength = clamp_value(params.lut3d.strength, 0.0, 1.0);
    if (params.lut3d_enabled)
        params.lut3d_present = true;
    params.monochrome.filter_a = clamp_value(params.monochrome.filter_a, -128.0, 128.0);
    params.monochrome.filter_b = clamp_value(params.monochrome.filter_b, -128.0, 128.0);
    params.monochrome.size = clamp_value(params.monochrome.size, 0.5, 3.0);
    params.monochrome.highlights = clamp_value(params.monochrome.highlights, 0.0, 1.0);
    params.monochrome.mix = clamp_value(params.monochrome.mix, 0.0, 1.0);
    params.split_toning.shadow_hue = clamp_value(params.split_toning.shadow_hue, 0.0, 1.0);
    params.split_toning.shadow_saturation =
        clamp_value(params.split_toning.shadow_saturation, 0.0, 1.0);
    params.split_toning.highlight_hue = clamp_value(params.split_toning.highlight_hue, 0.0, 1.0);
    params.split_toning.highlight_saturation =
        clamp_value(params.split_toning.highlight_saturation, 0.0, 1.0);
    params.split_toning.balance = clamp_value(params.split_toning.balance, 0.0, 1.0);
    params.split_toning.compress = clamp_value(params.split_toning.compress, 0.0, 100.0);
    params.split_toning.mix = clamp_value(params.split_toning.mix, 0.0, 1.0);
    params.gamma = clamp_value(params.gamma, 0.2, 3.0);
    clamp_rgb_levels(params.rgb_levels);
    clamp_rgb_curve(params.rgb_curve);
    params.sigmoid_contrast =
        clamp_value(params.sigmoid_contrast, kSigmoidContrastMin, kSigmoidContrastMax);
    params.sigmoid_skew = clamp_value(params.sigmoid_skew, kSigmoidSkewMin, kSigmoidSkewMax);
    params.sigmoid_display_white =
        clamp_value(params.sigmoid_display_white, kSigmoidDisplayWhiteMin, kSigmoidDisplayWhiteMax);
    params.sigmoid_display_black =
        clamp_value(params.sigmoid_display_black, kSigmoidDisplayBlackMin, kSigmoidDisplayBlackMax);
    params.sigmoid_hue_preservation = clamp_value(params.sigmoid_hue_preservation, 0.0, 1.0);
    params.raw_highlights = clamp_value(params.raw_highlights, 0.0, 1.0);
    params.raw_highlights_clip = clamp_value(params.raw_highlights_clip, 0.5, 1.0);
    if (params.raw_highlights_mode != kRawHighlightsModeClip &&
        params.raw_highlights_mode != kRawHighlightsModeInpaint &&
        params.raw_highlights_mode != kRawHighlightsModeOpposed &&
        params.raw_highlights_mode != kRawHighlightsModeLch)
    {
        params.raw_highlights_mode = std::string(kRawHighlightsModeOpposed);
    }
    params.hot_pixels_strength = clamp_value(params.hot_pixels_strength, 0.0, 1.0);
    params.hot_pixels_threshold = clamp_value(params.hot_pixels_threshold, 0.0, 1.0);
    params.raw_ca_iterations =
        std::clamp(params.raw_ca_iterations, std::int64_t{0}, std::int64_t{5});
    params.raw_denoise_threshold = clamp_value(params.raw_denoise_threshold, 0.0, 1.0);
    for (auto &channel : params.raw_denoise_bands)
    {
        for (double &band : channel)
        {
            band = clamp_value(band, 0.0, 16.0);
        }
    }
    params.denoise = clamp_value(params.denoise, 0.0, 1.0);
    params.denoise_chroma = clamp_value(params.denoise_chroma, 0.0, 1.0);
    params.denoise_radius = clamp_value(params.denoise_radius, 0.5, 8.0);
    params.lens_k1 = clamp_value(params.lens_k1, -2.0, 2.0);
    params.lens_k2 = clamp_value(params.lens_k2, -2.0, 2.0);
    params.lens_tca_r = clamp_value(params.lens_tca_r, 0.9, 1.1);
    params.lens_tca_b = clamp_value(params.lens_tca_b, 0.9, 1.1);
    params.lens_vignetting = clamp_value(params.lens_vignetting, 0.0, 1.0);
    if (params.lens_mode != kLensModeManual && params.lens_mode != kLensModeLookup)
    {
        params.lens_mode = std::string(kLensModeManual);
    }
    params.lens_focal_mm = clamp_value(params.lens_focal_mm, 1.0, 2000.0);
    for (double &value : params.color_eq_hue)
    {
        value = clamp_value(value, -0.5, 0.5);
    }
    for (double &value : params.color_eq_sat)
    {
        value = clamp_value(value, -1.0, 1.0);
    }
    for (double &value : params.color_eq_light)
    {
        value = clamp_value(value, -1.0, 1.0);
    }
    params.color_eq_band = std::clamp(params.color_eq_band, std::int64_t{0},
                                      static_cast<std::int64_t>(kColorEqualizerBandCount - 1U));
    if (color_zones_channel_name(params.color_zones.select_by).empty())
        params.color_zones.select_by = ColorZonesChannel::kHue;
    params.color_zones.strength = clamp_value(params.color_zones.strength, -200.0, 200.0);
    params.color_zones_band = std::clamp(params.color_zones_band, std::int64_t{0},
                                         static_cast<std::int64_t>(kColorEqualizerBandCount - 1U));
    for (auto &curve : params.color_zones.curves)
    {
        if (color_zones_interpolation_name(curve.interpolation).empty())
            curve.interpolation = ColorZonesInterpolation::kCatmullRom;
        for (auto &point : curve.points)
        {
            point.x = clamp_value(point.x, 0.0, 1.0);
            point.y = clamp_value(point.y, 0.0, 1.0);
        }
    }
    params.graduated_density = clamp_value(params.graduated_density, -4.0, 4.0);
    params.graduated_hardness = clamp_value(params.graduated_hardness, 0.0, 1.0);
    params.graduated_rotation = clamp_value(params.graduated_rotation, -180.0, 180.0);
    params.graduated_offset = clamp_value(params.graduated_offset, -1.0, 1.0);
    if (params.graduated_enabled)
    {
        params.graduated_present = true;
    }
    else if (!params.graduated_present && !near(params.graduated_density, 0.0))
    {
        // Preserve compatibility with callers that predate explicit presence,
        // without re-enabling a loaded disabled operation whose stored density
        // is intentionally non-zero.
        params.graduated_present = true;
        params.graduated_enabled = true;
    }
    params.tone_eq_blacks = clamp_value(params.tone_eq_blacks, -4.0, 4.0);
    params.tone_eq_shadows = clamp_value(params.tone_eq_shadows, -4.0, 4.0);
    params.tone_eq_midtones = clamp_value(params.tone_eq_midtones, -4.0, 4.0);
    params.tone_eq_highlights = clamp_value(params.tone_eq_highlights, -4.0, 4.0);
    params.tone_eq_whites = clamp_value(params.tone_eq_whites, -4.0, 4.0);
    if (params.tone_curve_working_space != kToneCurveWorkingSpaceSrgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLinearRgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceRgb &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLab &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceXyz &&
        params.tone_curve_working_space != kToneCurveWorkingSpaceLabIndependent)
    {
        params.tone_curve_working_space = std::string(kToneCurveWorkingSpaceRgb);
    }
    if (!curve_interpolation_is_supported(params.tone_curve_interpolation))
    {
        params.tone_curve_interpolation = std::string(kToneCurveInterpolationMonotoneHermite);
    }
    if (params.tone_curve_channel_mode != kToneCurveChannelModeIndependent)
    {
        params.tone_curve_channel_mode = std::string(kToneCurveChannelModeRgb);
    }
    if (params.tone_curve_working_space == kToneCurveWorkingSpaceLabIndependent)
    {
        params.tone_curve_channel_mode = std::string(kToneCurveChannelModeIndependent);
    }
    if (params.tone_curve_preserve_colors != kToneCurvePreserveColorsNone &&
        params.tone_curve_preserve_colors != kToneCurvePreserveColorsLuminance &&
        params.tone_curve_preserve_colors != kToneCurvePreserveColorsMax &&
        params.tone_curve_preserve_colors != kToneCurvePreserveColorsAverage &&
        params.tone_curve_preserve_colors != kToneCurvePreserveColorsSum &&
        params.tone_curve_preserve_colors != kToneCurvePreserveColorsNorm &&
        params.tone_curve_preserve_colors != kToneCurvePreserveColorsPower)
    {
        params.tone_curve_preserve_colors = std::string(kToneCurvePreserveColorsAverage);
    }
    clamp_tone_curve(params.tone_curve);
    clamp_tone_curve(params.tone_curve_a);
    clamp_tone_curve(params.tone_curve_b);
}

bool DevelopParams::is_identity() const noexcept
{
    return masks.empty() && !color_harmonizer_present && !color_harmonizer_mask_id.has_value() &&
           !graduated_present && !graduated_enabled && !graduated_mask_id.has_value() &&
           demosaic_mode == kDemosaicModeRcd && temperature.is_identity() &&
           !profile_gamma_enabled && input_color.is_identity() && output_color.is_identity() &&
           primaries.is_identity() && channel_mixer.is_identity() &&
           exposure_mode == kExposureModeManual && near(exposure_black, 0.0) &&
           near(exposure_ev, 0.0) &&
           near(exposure_deflicker_percentile, kExposureDeflickerPercentileDefault) &&
           near(exposure_deflicker_target_ev, kExposureDeflickerTargetEvDefault) &&
           !exposure_compensate_exposure_bias && !exposure_compensate_highlight_preservation &&
           !exposure_mask_id.has_value() && exposure_instances.empty() && near(contrast, 0.0) &&
           near(highlights, 0.0) && !highlights_mask_id.has_value() && near(shadows, 0.0) &&
           !shadows_mask_id.has_value() && near(whites, 0.0) && !whites_mask_id.has_value() &&
           near(blacks, 0.0) && !blacks_mask_id.has_value() && near(vibrance, 0.0) &&
           near(saturation, 0.0) && rotate_quarters % 4 == 0 && flip_horizontal == 0 &&
           flip_vertical == 0 && near(straighten_degrees, 0.0) && near(perspective_vertical, 0.0) &&
           near(perspective_horizontal, 0.0) && near(perspective_shear, 0.0) && near(crop_x, 0.0) &&
           near(crop_y, 0.0) && near(crop_width, 1.0) && near(crop_height, 1.0) &&
           !canvas_present && !canvas_enabled && near(sharpen, 0.0) && near(sharpen_radius, 2.0) &&
           near(sharpen_threshold, 0.5) && near(texture.strength, 0.0) &&
           near(texture.detail_threshold, 0.2) && texture.iterations == 1 &&
           retouch.is_identity() && near(clarity, 0.0) && near(vignette, 0.0) && near(grain, 0.0) &&
           near(bloom, 0.0) && near(soften, 0.0) && near(dehaze, 0.0) &&
           near(dehaze_distance, 0.2) && dehaze_adaptive && !output_dither_present &&
           !output_dither_enabled && !frame_present && !frame_enabled && !watermark_present &&
           !watermark_enabled && !velvia_present && !velvia_enabled &&
           !velvia_mask_id.has_value() && !lut3d_present && !lut3d_enabled &&
           !color_balance_enabled && !color_checker_enabled && color_balance_rgb.is_identity() &&
           !color_balance_rgb_mask_id.has_value() && !color_correction_enabled &&
           !color_contrast_enabled && !color_reconstruction_enabled && !color_zones_present &&
           !color_zones_enabled && !color_zones_mask_id.has_value() && !color_harmonizer_enabled &&
           !monochrome_present && !monochrome_enabled && !monochrome_mask_id.has_value() &&
           !split_toning_present && !split_toning_enabled && !split_toning_mask_id.has_value() &&
           near(gamma, kDevelopGammaDefault) && rgb_levels.is_identity() &&
           rgb_curve.is_identity() && !rgb_curve_mask_id.has_value() &&
           !tone_curve_mask_id.has_value() && tone_curve_is_identity(tone_curve) &&
           tone_curve_is_identity(tone_curve_a) && tone_curve_is_identity(tone_curve_b) &&
           !sigmoid_enabled && near(raw_highlights, 0.0) && near(hot_pixels_strength, 0.0) &&
           raw_ca_iterations == 0 && near(raw_denoise_threshold, 0.0) && near(denoise, 0.0) &&
           near(lens_k1, 0.0) && near(lens_k2, 0.0) && near(lens_tca_r, 1.0) &&
           near(lens_tca_b, 1.0) && near(lens_vignetting, 0.0) && lens_mode != kLensModeLookup &&
           bands_near_zero(color_eq_hue) && bands_near_zero(color_eq_sat) &&
           bands_near_zero(color_eq_light) && near(graduated_density, 0.0) &&
           near(tone_eq_blacks, 0.0) && near(tone_eq_shadows, 0.0) && near(tone_eq_midtones, 0.0) &&
           near(tone_eq_highlights, 0.0) && near(tone_eq_whites, 0.0);
}

} // namespace ravo
