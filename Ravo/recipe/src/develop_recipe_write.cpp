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

Result<Recipe> recipe_from_develop(AssetDescriptor asset, const DevelopParams &params)
{
    DevelopParams clamped = params;
    clamp_develop(clamped);
    Recipe recipe;
    recipe.asset = std::move(asset);
    recipe.masks = clamped.masks;
    if (clamped.demosaic_mode != kDemosaicModeRcd)
    {
        add_operation(recipe, std::string(kDemosaicOperationId), "demosaic-1",
                      {{"mode", ParameterValue{clamped.demosaic_mode}}}, 1, std::nullopt, true);
    }
    if (!clamped.temperature.is_identity())
    {
        add_operation(recipe, "ravo.color.temperature", "temperature-1",
                      temperature_to_parameters(clamped.temperature), 1, std::nullopt,
                      clamped.white_balance_effect_enabled);
    }
    if (clamped.profile_gamma_enabled)
    {
        auto profile_gamma = profile_gamma_to_parameters(clamped.profile_gamma);
        if (!profile_gamma)
        {
            return profile_gamma.error();
        }
        add_operation(recipe, std::string(kProfileGammaOperationId), "profilegamma-1",
                      std::move(profile_gamma).value(), kProfileGammaOperationSchemaVersion,
                      std::nullopt, true);
    }
    add_operation(recipe, "ravo.color.input", "color-input-1",
                  input_color_to_parameters(clamped.input_color), 1, std::nullopt,
                  clamped.input_profile_effect_enabled);
    if (!clamped.primaries.is_identity())
    {
        add_operation(recipe, std::string(kPrimariesOperationId), "primaries-1",
                      primaries_to_parameters(clamped.primaries), 1, std::nullopt,
                      clamped.primaries_effect_enabled);
    }
    if (!clamped.channel_mixer.is_identity())
    {
        add_operation(recipe, "ravo.color.channelmixerrgb", "channelmixerrgb-1",
                      channel_mixer_to_parameters(clamped.channel_mixer), 1, std::nullopt,
                      clamped.calibration_effect_enabled);
    }
    if (!near(clamped.hot_pixels_strength, 0.0))
    {
        add_operation(recipe, "ravo.raw.hotpixels", "hotpixels-1",
                      {{"strength", ParameterValue{clamped.hot_pixels_strength}},
                       {"threshold", ParameterValue{clamped.hot_pixels_threshold}},
                       {"permissive", ParameterValue{clamped.hot_pixels_permissive}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (!near(clamped.raw_highlights, 0.0))
    {
        add_operation(recipe, "ravo.raw.highlights", "raw-highlights-1",
                      {{"mode", ParameterValue{clamped.raw_highlights_mode}},
                       {"amount", ParameterValue{clamped.raw_highlights}},
                       {"clip", ParameterValue{clamped.raw_highlights_clip}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (clamped.raw_ca_iterations > 0)
    {
        add_operation(recipe, "ravo.raw.cacorrect", "cacorrect-1",
                      {{"iterations", ParameterValue{clamped.raw_ca_iterations}},
                       {"avoid_color_shift", ParameterValue{clamped.raw_ca_avoid_shift}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (!near(clamped.raw_denoise_threshold, 0.0))
    {
        add_operation(
            recipe, "ravo.raw.denoise", "rawdenoise-1",
            raw_denoise_to_parameters(clamped.raw_denoise_threshold, clamped.raw_denoise_bands), 1,
            std::nullopt, clamped.raw_effect_enabled);
    }
    if (!near(clamped.denoise, 0.0))
    {
        add_operation(recipe, "ravo.detail.denoiseprofile", "denoiseprofile-1",
                      {{"strength", ParameterValue{clamped.denoise}},
                       {"chroma", ParameterValue{clamped.denoise_chroma}},
                       {"radius", ParameterValue{clamped.denoise_radius}}},
                      1, std::nullopt, clamped.detail_effect_enabled);
    }
    if (clamped.lens_mode == kLensModeLookup || !near(clamped.lens_k1, 0.0) ||
        !near(clamped.lens_k2, 0.0) || !near(clamped.lens_tca_r, 1.0) ||
        !near(clamped.lens_tca_b, 1.0) || !near(clamped.lens_vignetting, 0.0))
    {
        add_operation(recipe, "ravo.geometry.lens", "lens-1",
                      {{"mode", ParameterValue{clamped.lens_mode}},
                       {"k1", ParameterValue{clamped.lens_k1}},
                       {"k2", ParameterValue{clamped.lens_k2}},
                       {"tca_r", ParameterValue{clamped.lens_tca_r}},
                       {"tca_b", ParameterValue{clamped.lens_tca_b}},
                       {"vignetting", ParameterValue{clamped.lens_vignetting}},
                       {"camera_make", ParameterValue{clamped.lens_make}},
                       {"camera_model", ParameterValue{clamped.lens_model}},
                       {"lens", ParameterValue{clamped.lens_name}},
                       {"focal_mm", ParameterValue{clamped.lens_focal_mm}}},
                      1, std::nullopt, clamped.raw_effect_enabled);
    }
    if (clamped.canvas_present || clamped.canvas_enabled)
    {
        auto canvas = canvas_to_parameters(clamped.canvas);
        if (!canvas)
            return canvas.error();
        add_operation(recipe, std::string(kCanvasOperationId), "canvas-1",
                      std::move(canvas).value(), kCanvasOperationSchemaVersion, std::nullopt,
                      clamped.geometry_effect_enabled && clamped.canvas_enabled);
    }
    if (!clamped.exposure_instances.empty())
    {
        for (const auto &instance : clamped.exposure_instances)
        {
            const ExposureParams exposure{instance.mode,
                                          instance.black,
                                          instance.exposure_ev,
                                          instance.deflicker_percentile,
                                          instance.deflicker_target_ev,
                                          instance.compensate_exposure_bias,
                                          instance.compensate_highlight_preservation};
            const bool emit = !exposure.is_identity() || instance.mask_id.has_value() ||
                              !instance.name.empty() || instance.bypass ||
                              clamped.exposure_instances.size() > 1U;
            if (!emit)
            {
                continue;
            }
            OperationInstance operation{std::string(kExposureOperationId),
                                        kExposureOperationSchemaVersion,
                                        instance.instance_id.empty() ? "exposure-1" :
                                                                       instance.instance_id,
                                        instance.enabled && clamped.light_effect_enabled,
                                        exposure_to_parameters(exposure),
                                        instance.mask_id};
            if (!instance.name.empty())
            {
                operation.name = instance.name;
            }
            operation.bypass = instance.bypass;
            recipe.operations.push_back(std::move(operation));
        }
    }
    else
    {
        const ExposureParams exposure{clamped.exposure_mode,
                                      clamped.exposure_black,
                                      clamped.exposure_ev,
                                      clamped.exposure_deflicker_percentile,
                                      clamped.exposure_deflicker_target_ev,
                                      clamped.exposure_compensate_exposure_bias,
                                      clamped.exposure_compensate_highlight_preservation};
        if (!exposure.is_identity() || clamped.exposure_mask_id.has_value())
        {
            add_operation(recipe, std::string(kExposureOperationId), "exposure-1",
                          exposure_to_parameters(exposure), kExposureOperationSchemaVersion,
                          clamped.exposure_mask_id, clamped.light_effect_enabled);
        }
    }
    if (!near(clamped.tone_eq_blacks, 0.0) || !near(clamped.tone_eq_shadows, 0.0) ||
        !near(clamped.tone_eq_midtones, 0.0) || !near(clamped.tone_eq_highlights, 0.0) ||
        !near(clamped.tone_eq_whites, 0.0))
    {
        add_operation(recipe, "ravo.core.toneequal", "toneequal-1",
                      {{"blacks", ParameterValue{clamped.tone_eq_blacks}},
                       {"shadows", ParameterValue{clamped.tone_eq_shadows}},
                       {"midtones", ParameterValue{clamped.tone_eq_midtones}},
                       {"highlights", ParameterValue{clamped.tone_eq_highlights}},
                       {"whites", ParameterValue{clamped.tone_eq_whites}}},
                      1, std::nullopt, clamped.tone_equal_effect_enabled);
    }
    if (clamped.graduated_present || !near(clamped.graduated_density, 0.0))
    {
        add_operation(recipe, "ravo.effect.graduatednd", "graduatednd-1",
                      {{"density_ev", ParameterValue{clamped.graduated_density}},
                       {"hardness", ParameterValue{clamped.graduated_hardness}},
                       {"rotation_deg", ParameterValue{clamped.graduated_rotation}},
                       {"offset", ParameterValue{clamped.graduated_offset}}},
                      1, clamped.graduated_mask_id,
                      clamped.graduated_effect_enabled &&
                          (clamped.graduated_present ? clamped.graduated_enabled : true));
    }
    if (clamped.color_checker_enabled)
    {
        auto color_checker = color_checker_to_parameters(clamped.color_checker);
        if (!color_checker)
        {
            return color_checker.error();
        }
        add_operation(recipe, std::string(kColorCheckerOperationId), "colorchecker-1",
                      std::move(color_checker).value(), kColorCheckerOperationSchemaVersion,
                      std::nullopt, clamped.color_effect_enabled);
    }
    if (clamped.color_harmonizer_present || clamped.color_harmonizer_enabled)
    {
        auto color_harmonizer = color_harmonizer_to_parameters(clamped.color_harmonizer);
        if (!color_harmonizer)
        {
            return color_harmonizer.error();
        }
        add_operation(recipe, std::string(kColorHarmonizerOperationId), "colorharmonizer-1",
                      std::move(color_harmonizer).value(), kColorHarmonizerOperationSchemaVersion,
                      clamped.color_harmonizer_mask_id,
                      clamped.color_effect_enabled && clamped.color_harmonizer_enabled);
    }
    if (!near(clamped.highlights, 0.0) || clamped.highlights_mask_id.has_value())
    {
        add_operation(recipe, "ravo.core.highlights", "highlights-1",
                      {{"amount", ParameterValue{clamped.highlights}}}, 1,
                      clamped.highlights_mask_id, clamped.light_effect_enabled);
    }
    if (!near(clamped.shadows, 0.0) || clamped.shadows_mask_id.has_value())
    {
        add_operation(recipe, "ravo.core.shadows", "shadows-1",
                      {{"amount", ParameterValue{clamped.shadows}}}, 1, clamped.shadows_mask_id,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.whites, 0.0) || clamped.whites_mask_id.has_value())
    {
        add_operation(recipe, "ravo.core.whites", "whites-1",
                      {{"amount", ParameterValue{clamped.whites}}}, 1, clamped.whites_mask_id,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.blacks, 0.0) || clamped.blacks_mask_id.has_value())
    {
        add_operation(recipe, "ravo.core.blacks", "blacks-1",
                      {{"amount", ParameterValue{clamped.blacks}}}, 1, clamped.blacks_mask_id,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.contrast, 0.0))
    {
        add_operation(recipe, "ravo.core.contrast", "contrast-1",
                      {{"amount", ParameterValue{clamped.contrast}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!near(clamped.gamma, kDevelopGammaDefault))
    {
        add_operation(recipe, "ravo.core.gamma", "gamma-1",
                      {{"gamma", ParameterValue{clamped.gamma}}}, 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    if (!clamped.rgb_levels.is_identity())
    {
        add_operation(recipe, "ravo.color.rgblevels", "rgblevels-1",
                      rgb_levels_to_parameters(clamped.rgb_levels), 1, std::nullopt,
                      clamped.light_effect_enabled);
    }
    const bool has_rgb_curve =
        !clamped.rgb_curve.is_identity() || clamped.rgb_curve_mask_id.has_value();
    const bool display_rgb_curve =
        !clamped.rgb_curve.is_identity() &&
        clamped.rgb_curve.application_space == kRgbCurveApplicationSpaceDisplaySrgb;
    if (has_rgb_curve && !display_rgb_curve)
    {
        add_operation(recipe, "ravo.color.rgbcurve", "rgbcurve-1",
                      rgb_curve_to_parameters(clamped.rgb_curve), 1, clamped.rgb_curve_mask_id,
                      clamped.curves_effect_enabled);
    }
    const bool tone_independent =
        clamped.tone_curve_channel_mode == kToneCurveChannelModeIndependent ||
        clamped.tone_curve_working_space == kToneCurveWorkingSpaceLabIndependent;
    if (!tone_curve_is_identity(clamped.tone_curve) || clamped.tone_curve_mask_id.has_value() ||
        (tone_independent && (!tone_curve_is_identity(clamped.tone_curve_a) ||
                              !tone_curve_is_identity(clamped.tone_curve_b))))
    {
        std::map<std::string, ParameterValue, std::less<>> curve_parameters{
            {"working_space", ParameterValue{clamped.tone_curve_working_space}},
            {"interpolation", ParameterValue{clamped.tone_curve_interpolation}},
            {"channel_mode", ParameterValue{clamped.tone_curve_channel_mode}},
            {"preserve_colors", ParameterValue{clamped.tone_curve_preserve_colors}},
            {"points", tone_curve_points_to_parameter(clamped.tone_curve)}};
        if (tone_independent)
        {
            curve_parameters.emplace("points_a",
                                     tone_curve_points_to_parameter(clamped.tone_curve_a));
            curve_parameters.emplace("points_b",
                                     tone_curve_points_to_parameter(clamped.tone_curve_b));
        }
        add_operation(recipe, "ravo.core.tonecurve", "tonecurve-1", std::move(curve_parameters), 1,
                      clamped.tone_curve_mask_id, clamped.curves_effect_enabled);
    }
    if (clamped.color_balance_enabled)
    {
        add_operation(recipe, std::string(kColorBalanceOperationId), "colorbalance-1",
                      color_balance_to_parameters(clamped.color_balance),
                      kColorBalanceOperationSchemaVersion, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (!clamped.color_balance_rgb_instances.empty())
    {
        for (const auto &instance : clamped.color_balance_rgb_instances)
        {
            const bool emit = !instance.params.is_identity() || instance.mask_id.has_value() ||
                              !instance.name.empty() || instance.bypass ||
                              clamped.color_balance_rgb_instances.size() > 1U;
            if (!emit)
            {
                continue;
            }
            OperationInstance operation{"ravo.color.colorbalancergb",
                                        1,
                                        instance.instance_id.empty() ? "colorbalancergb-1" :
                                                                       instance.instance_id,
                                        instance.enabled && clamped.color_effect_enabled,
                                        color_balance_rgb_to_parameters(instance.params),
                                        instance.mask_id};
            if (!instance.name.empty())
            {
                operation.name = instance.name;
            }
            operation.bypass = instance.bypass;
            recipe.operations.push_back(std::move(operation));
        }
    }
    else if (!clamped.color_balance_rgb.is_identity() ||
             clamped.color_balance_rgb_mask_id.has_value())
    {
        add_operation(recipe, "ravo.color.colorbalancergb", "colorbalancergb-1",
                      color_balance_rgb_to_parameters(clamped.color_balance_rgb), 1,
                      clamped.color_balance_rgb_mask_id, clamped.color_effect_enabled);
    }
    if (clamped.color_correction_enabled)
    {
        auto color_correction = color_correction_to_parameters(clamped.color_correction);
        if (!color_correction)
        {
            return color_correction.error();
        }
        add_operation(recipe, std::string(kColorCorrectionOperationId), "colorcorrection-1",
                      std::move(color_correction).value(), kColorCorrectionOperationSchemaVersion,
                      std::nullopt, clamped.color_effect_enabled);
    }
    if (clamped.color_contrast_enabled)
    {
        auto color_contrast = color_contrast_to_parameters(clamped.color_contrast);
        if (!color_contrast)
        {
            return color_contrast.error();
        }
        add_operation(recipe, std::string(kColorContrastOperationId), "colorcontrast-1",
                      std::move(color_contrast).value(), kColorContrastOperationSchemaVersion,
                      std::nullopt, clamped.color_effect_enabled);
    }
    if (clamped.velvia_present || clamped.velvia_enabled || clamped.velvia_mask_id.has_value())
    {
        auto velvia = velvia_to_parameters(clamped.velvia);
        if (!velvia)
            return velvia.error();
        add_operation(recipe, std::string(kVelviaOperationId), "velvia-1",
                      std::move(velvia).value(), kVelviaOperationSchemaVersion,
                      clamped.velvia_mask_id,
                      clamped.color_effect_enabled && clamped.velvia_enabled);
    }
    if (clamped.lut3d_present || clamped.lut3d_enabled)
    {
        auto lut = lut3d_to_parameters(clamped.lut3d);
        if (!lut)
            return lut.error();
        add_operation(recipe, std::string(kLut3dOperationId), "lut3d-1", std::move(lut).value(),
                      kLut3dOperationSchemaVersion, std::nullopt,
                      clamped.color_effect_enabled && clamped.lut3d_enabled);
    }
    if (!near(clamped.vibrance, 0.0))
    {
        add_operation(recipe, "ravo.color.vibrance", "vibrance-1",
                      {{"amount", ParameterValue{clamped.vibrance}}}, 1, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (!near(clamped.saturation, 0.0))
    {
        add_operation(recipe, "ravo.color.saturation", "saturation-1",
                      {{"amount", ParameterValue{clamped.saturation}}}, 1, std::nullopt,
                      clamped.color_effect_enabled);
    }
    if (!bands_near_zero(clamped.color_eq_hue) || !bands_near_zero(clamped.color_eq_sat) ||
        !bands_near_zero(clamped.color_eq_light))
    {
        add_operation(recipe, "ravo.color.colorequal", "colorequal-1",
                      {{"hue_shift", band_array_parameter(clamped.color_eq_hue)},
                       {"saturation", band_array_parameter(clamped.color_eq_sat)},
                       {"lightness", band_array_parameter(clamped.color_eq_light)}},
                      1, std::nullopt, clamped.color_eq_effect_enabled);
    }
    if (clamped.color_zones_present || clamped.color_zones_enabled ||
        clamped.color_zones_mask_id.has_value())
    {
        auto zones = color_zones_to_parameters(clamped.color_zones);
        if (!zones)
            return zones.error();
        add_operation(recipe, std::string(kColorZonesOperationId), "colorzones-1",
                      std::move(zones).value(), kColorZonesOperationSchemaVersion,
                      clamped.color_zones_mask_id,
                      clamped.color_effect_enabled && clamped.color_zones_enabled);
    }
    if (clamped.monochrome_present || clamped.monochrome_enabled ||
        clamped.monochrome_mask_id.has_value())
    {
        auto monochrome = monochrome_to_parameters(clamped.monochrome);
        if (!monochrome)
            return monochrome.error();
        add_operation(recipe, std::string(kMonochromeOperationId), "monochrome-1",
                      std::move(monochrome).value(), kMonochromeOperationSchemaVersion,
                      clamped.monochrome_mask_id,
                      clamped.color_effect_enabled && clamped.monochrome_enabled);
    }
    if (clamped.split_toning_present || clamped.split_toning_enabled ||
        clamped.split_toning_mask_id.has_value())
    {
        auto split = split_toning_to_parameters(clamped.split_toning);
        if (!split)
            return split.error();
        add_operation(recipe, std::string(kSplitToningOperationId), "splittoning-1",
                      std::move(split).value(), kSplitToningOperationSchemaVersion,
                      clamped.split_toning_mask_id,
                      clamped.color_effect_enabled && clamped.split_toning_enabled);
    }
    if (!near(clamped.texture.strength, 0.0))
    {
        auto texture = texture_to_parameters(clamped.texture);
        if (!texture)
        {
            return texture.error();
        }
        add_operation(recipe, std::string(kTextureOperationId), "texture-1",
                      std::move(texture).value(), kTextureOperationSchemaVersion, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (!near(clamped.sharpen, 0.0))
    {
        auto sharpen = sharpen_to_parameters(
            {clamped.sharpen_radius, clamped.sharpen, clamped.sharpen_threshold});
        if (!sharpen)
        {
            return sharpen.error();
        }
        add_operation(recipe, std::string(kSharpenOperationId), "sharpen-1",
                      std::move(sharpen).value(), kSharpenOperationSchemaVersion, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (!clamped.retouch.is_identity())
    {
        add_operation(recipe, std::string(kRetouchOperationId), "retouch-1",
                      retouch_to_parameters(clamped.retouch), kRetouchOperationSchemaVersion,
                      std::nullopt, clamped.detail_effect_enabled);
    }
    if (!near(clamped.clarity, 0.0))
    {
        add_operation(recipe, "ravo.detail.clarity", "clarity-1",
                      {{"amount", ParameterValue{clamped.clarity}}}, 1, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (!near(clamped.bloom, 0.0))
    {
        add_operation(recipe, "ravo.effect.bloom", "bloom-1",
                      {{"amount", ParameterValue{clamped.bloom}}}, 1, std::nullopt,
                      clamped.effects_effect_enabled);
    }
    if (!near(clamped.soften, 0.0))
    {
        add_operation(recipe, "ravo.effect.soften", "soften-1",
                      {{"amount", ParameterValue{clamped.soften}}}, 1, std::nullopt,
                      clamped.effects_effect_enabled);
    }
    if (!near(clamped.dehaze, 0.0))
    {
        auto dehaze = dehaze_to_parameters(
            {clamped.dehaze, clamped.dehaze_distance, clamped.dehaze_adaptive});
        if (!dehaze)
        {
            return dehaze.error();
        }
        add_operation(recipe, std::string(kDehazeOperationId), "dehaze-1",
                      std::move(dehaze).value(), kDehazeOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled);
    }
    if (!near(clamped.vignette, 0.0))
    {
        add_operation(recipe, "ravo.effect.vignette", "vignette-1",
                      {{"amount", ParameterValue{clamped.vignette}},
                       {"midpoint", ParameterValue{clamped.vignette_midpoint}},
                       {"falloff", ParameterValue{clamped.vignette_falloff}},
                       {"shape", ParameterValue{clamped.vignette_shape}},
                       {"center_x", ParameterValue{clamped.vignette_center_x}},
                       {"center_y", ParameterValue{clamped.vignette_center_y}}},
                      1, std::nullopt, clamped.effects_effect_enabled);
    }
    if (!near(clamped.grain, 0.0))
    {
        add_operation(recipe, "ravo.effect.grain", "grain-1",
                      {{"amount", ParameterValue{clamped.grain}}}, 1, std::nullopt,
                      clamped.detail_effect_enabled);
    }
    if (clamped.rotate_quarters % 4 != 0)
    {
        add_operation(recipe, "ravo.geometry.rotate", "rotate-1",
                      {{"quarters", ParameterValue{clamped.rotate_quarters % 4}}}, 1, std::nullopt,
                      clamped.geometry_effect_enabled);
    }
    if (clamped.flip_horizontal != 0 || clamped.flip_vertical != 0)
    {
        add_operation(recipe, "ravo.geometry.flip", "flip-1",
                      {{"horizontal", ParameterValue{clamped.flip_horizontal}},
                       {"vertical", ParameterValue{clamped.flip_vertical}}},
                      1, std::nullopt, clamped.geometry_effect_enabled);
    }
    if (!near(clamped.straighten_degrees, 0.0) || !near(clamped.perspective_vertical, 0.0) ||
        !near(clamped.perspective_horizontal, 0.0) || !near(clamped.perspective_shear, 0.0))
    {
        static constexpr std::array<std::string_view, 3> kInterpolations{
            kPerspectiveInterpolationBilinear, kPerspectiveInterpolationLanczos2,
            kPerspectiveInterpolationLanczos3};
        PerspectiveParams perspective;
        perspective.rotation_degrees = clamped.straighten_degrees;
        perspective.vertical_shift = clamped.perspective_vertical;
        perspective.horizontal_shift = clamped.perspective_horizontal;
        perspective.shear = clamped.perspective_shear;
        perspective.constrain_crop = clamped.perspective_constrain_crop;
        perspective.interpolation =
            kInterpolations[static_cast<std::size_t>(clamped.perspective_interpolation_index)];
        auto parameters = perspective_to_parameters(perspective);
        if (!parameters)
            return parameters.error();
        add_operation(recipe, std::string(kPerspectiveOperationId), "perspective-1",
                      std::move(parameters).value(), kPerspectiveOperationSchemaVersion,
                      std::nullopt, clamped.geometry_effect_enabled);
    }
    if (!near(clamped.crop_x, 0.0) || !near(clamped.crop_y, 0.0) ||
        !near(clamped.crop_width, 1.0) || !near(clamped.crop_height, 1.0))
    {
        add_operation(recipe, "ravo.geometry.crop", "crop-1",
                      {{"x", ParameterValue{clamped.crop_x}},
                       {"y", ParameterValue{clamped.crop_y}},
                       {"width", ParameterValue{clamped.crop_width}},
                       {"height", ParameterValue{clamped.crop_height}}},
                      1, std::nullopt, clamped.geometry_effect_enabled);
    }
    if (clamped.sigmoid_enabled)
    {
        add_operation(
            recipe, "ravo.display.sigmoid", "sigmoid-1",
            {{"working_space", ParameterValue{std::string(kSigmoidWorkingSpaceLinearSrgb)}},
             {"color_processing", ParameterValue{std::string(kSigmoidColorProcessingPerChannel)}},
             {"middle_grey_contrast", ParameterValue{clamped.sigmoid_contrast}},
             {"contrast_skewness", ParameterValue{clamped.sigmoid_skew}},
             {"display_white_target", ParameterValue{clamped.sigmoid_display_white}},
             {"display_black_target", ParameterValue{clamped.sigmoid_display_black}},
             {"hue_preservation", ParameterValue{clamped.sigmoid_hue_preservation}}},
            1, std::nullopt, clamped.light_effect_enabled);
    }
    if (display_rgb_curve)
    {
        add_operation(recipe, "ravo.color.rgbcurve", "rgbcurve-1",
                      rgb_curve_to_parameters(clamped.rgb_curve), 1, clamped.rgb_curve_mask_id,
                      clamped.curves_effect_enabled);
    }
    if (clamped.color_reconstruction_enabled)
    {
        auto color_reconstruction =
            color_reconstruction_to_parameters(clamped.color_reconstruction);
        if (!color_reconstruction)
        {
            return color_reconstruction.error();
        }
        add_operation(recipe, std::string(kColorReconstructionOperationId), "colorreconstruct-1",
                      std::move(color_reconstruction).value(),
                      kColorReconstructionOperationSchemaVersion, std::nullopt,
                      clamped.color_effect_enabled);
    }
    add_operation(recipe, "ravo.color.output", "color-output-1",
                  output_color_to_parameters(clamped.output_color), 1, std::nullopt,
                  clamped.output_profile_effect_enabled);
    if (clamped.output_dither_present || clamped.output_dither_enabled)
    {
        auto dither = output_dither_to_parameters(clamped.output_dither);
        if (!dither)
            return dither.error();
        add_operation(recipe, std::string(kOutputDitherOperationId), "output-dither-1",
                      std::move(dither).value(), kOutputDitherOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled && clamped.output_dither_enabled);
    }
    if (clamped.frame_present || clamped.frame_enabled)
    {
        auto frame = frame_to_parameters(clamped.frame);
        if (!frame)
            return frame.error();
        add_operation(recipe, std::string(kFrameOperationId), "frame-1", std::move(frame).value(),
                      kFrameOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled && clamped.frame_enabled);
    }
    if (clamped.watermark_present || clamped.watermark_enabled)
    {
        auto watermark = watermark_to_parameters(clamped.watermark);
        if (!watermark)
            return watermark.error();
        add_operation(recipe, std::string(kWatermarkOperationId), "watermark-1",
                      std::move(watermark).value(), kWatermarkOperationSchemaVersion, std::nullopt,
                      clamped.effects_effect_enabled && clamped.watermark_enabled);
    }
    return recipe;
}

} // namespace ravo
