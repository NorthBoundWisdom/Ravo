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
using develop_internal::add_operation;
using develop_internal::append_color_balance_develop_names;
using develop_internal::append_legacy_color_balance_develop_names;
using develop_internal::append_develop_numeric_field_names;
using develop_internal::apply_color_checker_field;
using develop_internal::apply_color_contrast_field;
using develop_internal::apply_color_correction_field;
using develop_internal::apply_color_harmonizer_field;
using develop_internal::apply_color_reconstruction_field;
using develop_internal::apply_color_balance_field;
using develop_internal::apply_legacy_color_balance_field;
using develop_internal::apply_temperature_field;
using develop_internal::assign_color_harmonizer_hue_turns;
using develop_internal::assign_develop_field;
using develop_internal::apply_rgb_curve_field;
using develop_internal::apply_rgb_levels_field;
using develop_internal::as_integer;
using develop_internal::as_number;
using develop_internal::as_string_if;
using develop_internal::band_array_parameter;
using develop_internal::bands_near_zero;
using develop_internal::clamp_value;
using develop_internal::clamp_color_balance;
using develop_internal::clamp_color_contrast;
using develop_internal::clamp_color_correction;
using develop_internal::clamp_color_harmonizer;
using develop_internal::clamp_color_reconstruction;
using develop_internal::clamp_legacy_color_balance;
using develop_internal::clamp_temperature;
using develop_internal::clamp_rgb_levels;
using develop_internal::exact_develop_integer;
using develop_internal::candidate_develop_set_names;
using develop_internal::develop_set_field_extreme;
using develop_internal::develop_set_field_accepts;
using develop_internal::first_accepted_develop_set_value;
using develop_internal::flag01;
using develop_internal::kEpsilon;
using develop_internal::make_studio_color_zones_curves;
using develop_internal::near;
using develop_internal::parse_band_array;
using develop_internal::parse_band_field;
using develop_internal::reset_color_balance_field;
using develop_internal::reset_color_checker_field;
using develop_internal::reset_color_contrast_field;
using develop_internal::reset_color_correction_field;
using develop_internal::reset_color_harmonizer_field;
using develop_internal::reset_color_reconstruction_field;
using develop_internal::reset_legacy_color_balance_field;
using develop_internal::reset_temperature_field;
using develop_internal::reset_rgb_curve_field;
using develop_internal::reset_rgb_levels_field;
using develop_internal::rgb_levels_preserve_names;
using develop_internal::studio_color_zones_curves;



bool apply_develop_field(DevelopParams &params, const std::string_view name, const double value)
{
    if (is_develop_mask_field(name))
    {
        return static_cast<bool>(apply_develop_mask_field_strict(params, name, value));
    }
    if (!assign_develop_field(params, name, value))
    {
        return false;
    }
    clamp_develop(params);
    return true;
}

Result<void> apply_develop_field_strict(DevelopParams &params, const std::string_view name,
                                        const double value)
{
    if (is_develop_mask_field(name))
    {
        return apply_develop_mask_field_strict(params, name, value);
    }
    DevelopParams candidate = params;
    if (!assign_develop_field(candidate, name, value))
    {
        return make_error(ErrorCode::kInvalidArgument, "Develop field or value is unsupported",
                          {{"name", std::string(name)}, {"value", std::to_string(value)}});
    }
    DevelopParams clamped = candidate;
    clamp_develop(clamped);
    if (clamped != candidate)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Develop field value is outside the supported range",
                          {{"name", std::string(name)}, {"value", std::to_string(value)}});
    }
    params = std::move(candidate);
    return {};
}

Result<void> apply_develop_text_field_strict(DevelopParams &params, const std::string_view name,
                                             const std::string_view value)
{
    if (name == "lut3dFile")
    {
        DevelopParams candidate = params;
        if (value.empty())
        {
            const DevelopParams identity;
            candidate.lut3d_present = identity.lut3d_present;
            candidate.lut3d_enabled = identity.lut3d_enabled;
            candidate.lut3d = identity.lut3d;
        }
        else
        {
            candidate.lut3d_present = true;
            candidate.lut3d_enabled = true;
            candidate.color_effect_enabled = true;
            candidate.lut3d.file_path = std::string(value);
            auto valid = lut3d_to_parameters(candidate.lut3d);
            if (!valid)
                return valid.error();
        }
        params = std::move(candidate);
        return {};
    }
    if (name != "watermarkText")
        return make_error(ErrorCode::kInvalidArgument, "Develop text field is unsupported",
                          {{"name", std::string(name)}});
    DevelopParams candidate = params;
    candidate.watermark_present = true;
    candidate.watermark_enabled = true;
    candidate.effects_effect_enabled = true;
    candidate.watermark.text = std::string(value);
    auto valid = watermark_to_parameters(candidate.watermark);
    if (!valid)
        return valid.error();
    params = std::move(candidate);
    return {};
}

std::string_view develop_set_field_kind_name(const DevelopSetFieldKind kind) noexcept
{
    switch (kind)
    {
    case DevelopSetFieldKind::Number:
        return "number";
    case DevelopSetFieldKind::Integer:
        return "integer";
    case DevelopSetFieldKind::Toggle:
        return "toggle";
    case DevelopSetFieldKind::Text:
        return "text";
    }
    return "number";
}

std::vector<std::string_view> develop_set_field_prefixes() noexcept
{
    return {kColorHarmonizerMaskFieldPrefix, kGraduatedMaskFieldPrefix};
}

std::vector<DevelopSetField> list_develop_set_fields()
{
    std::vector<DevelopSetField> fields;
    for (const auto &name : candidate_develop_set_names())
    {
        const auto seed = first_accepted_develop_set_value(name);
        if (!seed)
        {
            continue;
        }
        const bool half = develop_set_field_accepts(name, *seed + 0.5) ||
                          develop_set_field_accepts(name, *seed - 0.5);
        const bool integer = !half;
        const bool toggle =
            develop_set_field_accepts(name, 0.0) && develop_set_field_accepts(name, 1.0) &&
            !develop_set_field_accepts(name, 0.5) && !develop_set_field_accepts(name, 2.0) &&
            !develop_set_field_accepts(name, -1.0);
        const bool indexed_choice = name.ends_with("Index");
        DevelopSetField field;
        field.name = name;
        field.kind = toggle && !indexed_choice ? DevelopSetFieldKind::Toggle :
                     integer                   ? DevelopSetFieldKind::Integer :
                                                 DevelopSetFieldKind::Number;
        field.minimum = develop_set_field_extreme(name, *seed, -1.0, integer || toggle);
        field.maximum = develop_set_field_extreme(name, *seed, 1.0, integer || toggle);
        if (toggle)
        {
            field.minimum = 0.0;
            field.maximum = 1.0;
        }
        fields.push_back(std::move(field));
    }
    DevelopSetField text;
    text.name = "watermarkText";
    text.kind = DevelopSetFieldKind::Text;
    fields.push_back(std::move(text));
    DevelopSetField lut_path;
    lut_path.name = "lut3dFile";
    lut_path.kind = DevelopSetFieldKind::Text;
    fields.push_back(std::move(lut_path));
    std::sort(fields.begin(), fields.end(),
              [](const DevelopSetField &left, const DevelopSetField &right)
              { return left.name < right.name; });
    return fields;
}

bool reset_develop_field(DevelopParams &params, const std::string_view name)
{
    if (is_develop_mask_field(name))
    {
        return static_cast<bool>(reset_develop_mask_field(params, name));
    }
    DevelopParams identity;
    if (name == "demosaicModeIndex")
    {
        params.demosaic_mode = identity.demosaic_mode;
    }
    else if (reset_temperature_field(params.temperature, name))
    {
    }
    else if (name == "profileGammaEnabled")
    {
        params.profile_gamma_enabled = identity.profile_gamma_enabled;
    }
    else if (name == "profileGammaModeIndex")
    {
        params.profile_gamma.mode = identity.profile_gamma.mode;
    }
    else if (name == "profileGammaLinear")
    {
        params.profile_gamma.linear = identity.profile_gamma.linear;
    }
    else if (name == "profileGammaGamma")
    {
        params.profile_gamma.gamma = identity.profile_gamma.gamma;
    }
    else if (name == "profileGammaDynamicRange")
    {
        params.profile_gamma.dynamic_range = identity.profile_gamma.dynamic_range;
    }
    else if (name == "profileGammaGreyPoint")
    {
        params.profile_gamma.grey_point = identity.profile_gamma.grey_point;
    }
    else if (name == "profileGammaShadowsRange")
    {
        params.profile_gamma.shadows_range = identity.profile_gamma.shadows_range;
    }
    else if (name == "profileGammaSecurityFactor")
    {
        params.profile_gamma.security_factor = identity.profile_gamma.security_factor;
    }
    else if (name == "inputProfile" || name == "workingProfile" || name == "renderingIntent" ||
             name == "gamutNormalize" || name == "blueMapping")
    {
        params.input_color = identity.input_color;
    }
    else if (name == "outputProfile" || name == "outputRenderingIntent" || name == "proofMode" ||
             name == "proofProfile" || name == "proofIntent" ||
             name == "outputBlackPointCompensation")
    {
        params.output_color = identity.output_color;
    }
    else if (name == "primariesAchromaticHueDegrees")
    {
        params.primaries.achromatic_tint_hue = identity.primaries.achromatic_tint_hue;
    }
    else if (name == "primariesAchromaticPurity")
    {
        params.primaries.achromatic_tint_purity = identity.primaries.achromatic_tint_purity;
    }
    else if (name == "primariesRedHueDegrees")
    {
        params.primaries.red_hue = identity.primaries.red_hue;
    }
    else if (name == "primariesRedPurity")
    {
        params.primaries.red_purity = identity.primaries.red_purity;
    }
    else if (name == "primariesGreenHueDegrees")
    {
        params.primaries.green_hue = identity.primaries.green_hue;
    }
    else if (name == "primariesGreenPurity")
    {
        params.primaries.green_purity = identity.primaries.green_purity;
    }
    else if (name == "primariesBlueHueDegrees")
    {
        params.primaries.blue_hue = identity.primaries.blue_hue;
    }
    else if (name == "primariesBluePurity")
    {
        params.primaries.blue_purity = identity.primaries.blue_purity;
    }
    else if (name == "channelMixerRR" || name == "channelMixerRG" || name == "channelMixerRB" ||
             name == "channelMixerGR" || name == "channelMixerGG" || name == "channelMixerGB" ||
             name == "channelMixerBR" || name == "channelMixerBG" || name == "channelMixerBB")
    {
        params.channel_mixer = identity.channel_mixer;
    }
    else if (name == "exposureMode")
    {
        params.exposure_mode = identity.exposure_mode;
    }
    else if (name == "exposureBlack")
    {
        params.exposure_black = identity.exposure_black;
    }
    else if (name == "exposure")
    {
        params.exposure_ev = identity.exposure_ev;
    }
    else if (name == "exposureDeflickerPercentile")
    {
        params.exposure_deflicker_percentile = identity.exposure_deflicker_percentile;
    }
    else if (name == "exposureDeflickerTarget")
    {
        params.exposure_deflicker_target_ev = identity.exposure_deflicker_target_ev;
    }
    else if (name == "exposureCompensateBias")
    {
        params.exposure_compensate_exposure_bias = identity.exposure_compensate_exposure_bias;
    }
    else if (name == "exposureCompensateHighlight")
    {
        params.exposure_compensate_highlight_preservation =
            identity.exposure_compensate_highlight_preservation;
    }
    else if (name == "contrast")
    {
        params.contrast = identity.contrast;
    }
    else if (name == "highlights")
    {
        params.highlights = identity.highlights;
    }
    else if (name == "shadows")
    {
        params.shadows = identity.shadows;
    }
    else if (name == "whites")
    {
        params.whites = identity.whites;
    }
    else if (name == "blacks")
    {
        params.blacks = identity.blacks;
    }
    else if (name == "vibrance")
    {
        params.vibrance = identity.vibrance;
    }
    else if (name == "saturation")
    {
        params.saturation = identity.saturation;
    }
    else if (name == "rotate")
    {
        params.rotate_quarters = 0;
    }
    else if (name == "flip")
    {
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
    }
    else if (name == "straighten")
    {
        params.straighten_degrees = identity.straighten_degrees;
    }
    else if (name == "perspectiveVertical")
    {
        params.perspective_vertical = identity.perspective_vertical;
    }
    else if (name == "perspectiveHorizontal")
    {
        params.perspective_horizontal = identity.perspective_horizontal;
    }
    else if (name == "perspectiveShear")
    {
        params.perspective_shear = identity.perspective_shear;
    }
    else if (name == "perspectiveConstrainCrop")
    {
        params.perspective_constrain_crop = identity.perspective_constrain_crop;
    }
    else if (name == "perspectiveInterpolationIndex")
    {
        params.perspective_interpolation_index = identity.perspective_interpolation_index;
    }
    else if (name == "crop" || name == "cropX" || name == "cropY" || name == "cropWidth" ||
             name == "cropHeight")
    {
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
        params.canvas_present = identity.canvas_present;
        params.canvas_enabled = identity.canvas_enabled;
        params.canvas = identity.canvas;
    }
    else if (name == "canvas")
    {
        params.canvas_present = identity.canvas_present;
        params.canvas_enabled = identity.canvas_enabled;
        params.canvas = identity.canvas;
    }
    else if (name == "sharpen" || name == "sharpenRadius" || name == "sharpenThreshold")
    {
        params.sharpen = identity.sharpen;
        if (name == "sharpenRadius")
        {
            params.sharpen_radius = identity.sharpen_radius;
        }
        else if (name == "sharpenThreshold")
        {
            params.sharpen_threshold = identity.sharpen_threshold;
        }
    }
    else if (name == "texture" || name == "textureDetailThreshold" || name == "textureIterations")
    {
        if (name == "textureDetailThreshold")
        {
            params.texture.detail_threshold = identity.texture.detail_threshold;
        }
        else if (name == "textureIterations")
        {
            params.texture.iterations = identity.texture.iterations;
        }
        else
        {
            params.texture = identity.texture;
        }
    }
    else if (name == "retouch")
    {
        params.retouch = identity.retouch;
    }
    else if (name == "clarity")
    {
        params.clarity = identity.clarity;
    }
    else if (name == "vignette" || name == "vignetteMidpoint" || name == "vignetteFalloff" ||
             name == "vignetteShape" || name == "vignetteCenterX" || name == "vignetteCenterY")
    {
        if (name == "vignetteMidpoint")
            params.vignette_midpoint = identity.vignette_midpoint;
        else if (name == "vignetteFalloff")
            params.vignette_falloff = identity.vignette_falloff;
        else if (name == "vignetteShape")
            params.vignette_shape = identity.vignette_shape;
        else if (name == "vignetteCenterX")
            params.vignette_center_x = identity.vignette_center_x;
        else if (name == "vignetteCenterY")
            params.vignette_center_y = identity.vignette_center_y;
        else
        {
            params.vignette = identity.vignette;
            params.vignette_midpoint = identity.vignette_midpoint;
            params.vignette_falloff = identity.vignette_falloff;
            params.vignette_shape = identity.vignette_shape;
            params.vignette_center_x = identity.vignette_center_x;
            params.vignette_center_y = identity.vignette_center_y;
        }
    }
    else if (name == "grain")
    {
        params.grain = identity.grain;
    }
    else if (name == "bloom")
    {
        params.bloom = identity.bloom;
    }
    else if (name == "soften")
    {
        params.soften = identity.soften;
    }
    else if (name == "dehaze")
    {
        params.dehaze = identity.dehaze;
    }
    else if (name == "dehazeDistance")
    {
        params.dehaze_distance = identity.dehaze_distance;
    }
    else if (name == "dehazeAdaptive")
    {
        params.dehaze_adaptive = identity.dehaze_adaptive;
    }
    else if (name == "outputDither")
    {
        params.output_dither_present = identity.output_dither_present;
        params.output_dither_enabled = identity.output_dither_enabled;
        params.output_dither = identity.output_dither;
        params.frame_present = identity.frame_present;
        params.frame_enabled = identity.frame_enabled;
        params.frame = identity.frame;
        params.watermark_present = identity.watermark_present;
        params.watermark_enabled = identity.watermark_enabled;
        params.watermark = identity.watermark;
    }
    else if (name == "outputDitherMethodIndex")
    {
        params.output_dither.method = identity.output_dither.method;
    }
    else if (name == "outputDitherDamping")
    {
        params.output_dither.random_damping_db = identity.output_dither.random_damping_db;
    }
    else if (name == "outputFrame")
    {
        params.frame_present = identity.frame_present;
        params.frame_enabled = identity.frame_enabled;
        params.frame = identity.frame;
    }
    else if (name == "watermark")
    {
        params.watermark_present = identity.watermark_present;
        params.watermark_enabled = identity.watermark_enabled;
        params.watermark = identity.watermark;
    }
    else if (name == "velvia")
    {
        params.velvia_present = identity.velvia_present;
        params.velvia_enabled = identity.velvia_enabled;
        params.velvia = identity.velvia;
        params.velvia_mask_id = identity.velvia_mask_id;
    }
    else if (name == "velviaEnabled")
    {
        params.velvia_enabled = identity.velvia_enabled;
    }
    else if (name == "velviaStrength")
    {
        params.velvia.strength = identity.velvia.strength;
    }
    else if (name == "velviaBias")
    {
        params.velvia.bias = identity.velvia.bias;
    }
    else if (name == "lut3d" || name == "lut3dFile")
    {
        params.lut3d_present = identity.lut3d_present;
        params.lut3d_enabled = identity.lut3d_enabled;
        params.lut3d = identity.lut3d;
    }
    else if (name == "lut3dEnabled")
    {
        params.lut3d_enabled = identity.lut3d_enabled;
    }
    else if (name == "lut3dInputSpaceIndex")
    {
        params.lut3d.input_space = identity.lut3d.input_space;
    }
    else if (name == "lut3dOutputSpaceIndex")
    {
        params.lut3d.output_space = identity.lut3d.output_space;
    }
    else if (name == "lut3dInterpolationIndex")
    {
        params.lut3d.interpolation = identity.lut3d.interpolation;
    }
    else if (name == "lut3dStrength")
    {
        params.lut3d.strength = identity.lut3d.strength;
    }
    else if (reset_legacy_color_balance_field(params.color_balance, name))
    {
        if (name == "legacyColorBalance")
        {
            params.color_balance_enabled = false;
        }
    }
    else if (reset_color_checker_field(params, name))
    {
    }
    else if (reset_color_balance_field(params.color_balance_rgb, name))
    {
    }
    else if (reset_color_correction_field(params, name))
    {
    }
    else if (reset_color_contrast_field(params, name))
    {
    }
    else if (reset_color_reconstruction_field(params, name))
    {
    }
    else if (reset_color_harmonizer_field(params, name))
    {
    }
    else if (name == "monochrome")
    {
        params.monochrome_present = identity.monochrome_present;
        params.monochrome_enabled = identity.monochrome_enabled;
        params.monochrome = identity.monochrome;
        params.monochrome_mask_id = identity.monochrome_mask_id;
    }
    else if (name == "splitShadowsHue")
    {
        params.split_toning.shadow_hue = identity.split_toning.shadow_hue;
    }
    else if (name == "splitHighlightsHue")
    {
        params.split_toning.highlight_hue = identity.split_toning.highlight_hue;
    }
    else if (name == "splitBalance")
    {
        params.split_toning.balance = identity.split_toning.balance;
    }
    else if (name == "splitAmount")
    {
        params.split_toning_present = identity.split_toning_present;
        params.split_toning_enabled = identity.split_toning_enabled;
        params.split_toning = identity.split_toning;
        params.split_toning_mask_id = identity.split_toning_mask_id;
    }
    else if (name == "splitToning")
    {
        params.split_toning_present = identity.split_toning_present;
        params.split_toning_enabled = identity.split_toning_enabled;
        params.split_toning = identity.split_toning;
        params.split_toning_mask_id = identity.split_toning_mask_id;
    }
    else if (name == "gamma")
    {
        params.gamma = identity.gamma;
    }
    else if (reset_rgb_levels_field(params, name))
    {
    }
    else if (reset_rgb_curve_field(params, name))
    {
    }
    else if (name == "toneCurve" || name == "toneCurveInterpolation" ||
             name == "toneCurveChannelMode" || name == "toneCurvePreserve" ||
             name == "toneCurveWorkingSpace")
    {
        if (name == "toneCurveInterpolation")
        {
            params.tone_curve_interpolation = identity.tone_curve_interpolation;
        }
        else if (name == "toneCurveChannelMode")
        {
            params.tone_curve_channel_mode = identity.tone_curve_channel_mode;
        }
        else if (name == "toneCurvePreserve")
        {
            params.tone_curve_preserve_colors = identity.tone_curve_preserve_colors;
        }
        else if (name == "toneCurveWorkingSpace")
        {
            params.tone_curve_working_space = identity.tone_curve_working_space;
        }
        else
        {
            params.tone_curve.clear();
            params.tone_curve_a.clear();
            params.tone_curve_b.clear();
            params.tone_curve_working_space = identity.tone_curve_working_space;
            params.tone_curve_interpolation = identity.tone_curve_interpolation;
            params.tone_curve_channel_mode = identity.tone_curve_channel_mode;
            params.tone_curve_preserve_colors = identity.tone_curve_preserve_colors;
        }
    }
    else if (name == "sigmoidContrast")
    {
        params.sigmoid_contrast = identity.sigmoid_contrast;
    }
    else if (name == "sigmoidSkew")
    {
        params.sigmoid_skew = identity.sigmoid_skew;
    }
    else if (name == "sigmoidHuePreservation")
    {
        params.sigmoid_hue_preservation = identity.sigmoid_hue_preservation;
    }
    else if (name == "rawHighlights" || name == "rawHighlightsClip" || name == "rawHighlightsMode")
    {
        params.raw_highlights = identity.raw_highlights;
        params.raw_highlights_clip = identity.raw_highlights_clip;
        params.raw_highlights_mode = identity.raw_highlights_mode;
    }
    else if (name == "rawDenoiseThreshold")
    {
        params.raw_denoise_threshold = identity.raw_denoise_threshold;
    }
    else if (name == "hotPixelsStrength" || name == "hotPixelsThreshold" ||
             name == "hotPixelsPermissive")
    {
        params.hot_pixels_strength = identity.hot_pixels_strength;
        params.hot_pixels_threshold = identity.hot_pixels_threshold;
        params.hot_pixels_permissive = identity.hot_pixels_permissive;
    }
    else if (name == "rawCaIterations" || name == "rawCaAvoidShift")
    {
        params.raw_ca_iterations = identity.raw_ca_iterations;
        params.raw_ca_avoid_shift = identity.raw_ca_avoid_shift;
    }
    else if (name == "denoise")
    {
        params.denoise = identity.denoise;
    }
    else if (name == "denoiseChroma")
    {
        params.denoise_chroma = identity.denoise_chroma;
    }
    else if (name == "denoiseRadius")
    {
        params.denoise_radius = identity.denoise_radius;
    }
    else if (name == "lensK1" || name == "lensK2" || name == "lensTcaR" || name == "lensTcaB" ||
             name == "lensVignetting" || name == "lensMode" || name == "lensFocal")
    {
        params.lens_k1 = identity.lens_k1;
        params.lens_k2 = identity.lens_k2;
        params.lens_tca_r = identity.lens_tca_r;
        params.lens_tca_b = identity.lens_tca_b;
        params.lens_vignetting = identity.lens_vignetting;
        params.lens_mode = identity.lens_mode;
        params.lens_focal_mm = identity.lens_focal_mm;
    }
    else if (name == "colorZones")
    {
        params.color_zones_present = identity.color_zones_present;
        params.color_zones_enabled = identity.color_zones_enabled;
        params.color_zones = identity.color_zones;
        params.color_zones_mask_id = identity.color_zones_mask_id;
        params.color_zones_band = identity.color_zones_band;
    }
    else if (name == "colorEqHue" || name == "colorEqSat" || name == "colorEqLight" ||
             name == "colorEqBand")
    {
        const auto band = static_cast<std::size_t>(
            std::clamp(params.color_eq_band, std::int64_t{0}, std::int64_t{7}));
        if (name == "colorEqHue")
        {
            params.color_eq_hue[band] = 0.0;
        }
        else if (name == "colorEqSat")
        {
            params.color_eq_sat[band] = 0.0;
        }
        else if (name == "colorEqLight")
        {
            params.color_eq_light[band] = 0.0;
        }
        else
        {
            params.color_eq_band = 0;
        }
    }
    else if (name == "graduatedDensity" || name == "graduatedHardness" ||
             name == "graduatedRotation" || name == "graduatedOffset")
    {
        params.graduated_density = identity.graduated_density;
        params.graduated_hardness = identity.graduated_hardness;
        params.graduated_rotation = identity.graduated_rotation;
        params.graduated_offset = identity.graduated_offset;
    }
    else if (name == "toneEqBlacks")
    {
        params.tone_eq_blacks = identity.tone_eq_blacks;
    }
    else if (name == "toneEqShadows")
    {
        params.tone_eq_shadows = identity.tone_eq_shadows;
    }
    else if (name == "toneEqMidtones")
    {
        params.tone_eq_midtones = identity.tone_eq_midtones;
    }
    else if (name == "toneEqHighlights")
    {
        params.tone_eq_highlights = identity.tone_eq_highlights;
    }
    else if (name == "toneEqWhites")
    {
        params.tone_eq_whites = identity.tone_eq_whites;
    }
    else
    {
        std::size_t band = 0;
        if (parse_band_field(name, "colorEqHue", band))
        {
            params.color_eq_hue[band] = 0.0;
        }
        else if (parse_band_field(name, "colorEqSat", band))
        {
            params.color_eq_sat[band] = 0.0;
        }
        else if (parse_band_field(name, "colorEqLight", band))
        {
            params.color_eq_light[band] = 0.0;
        }
        else
        {
            return false;
        }
    }
    clamp_develop(params);
    return true;
}

bool reset_develop_section(DevelopParams &params, const std::string_view section)
{
    DevelopParams identity;
    if (section == "geometry")
    {
        params.rotate_quarters = 0;
        params.flip_horizontal = 0;
        params.flip_vertical = 0;
        params.straighten_degrees = 0.0;
        params.perspective_vertical = identity.perspective_vertical;
        params.perspective_horizontal = identity.perspective_horizontal;
        params.perspective_shear = identity.perspective_shear;
        params.perspective_constrain_crop = identity.perspective_constrain_crop;
        params.perspective_interpolation_index = identity.perspective_interpolation_index;
        params.crop_x = 0.0;
        params.crop_y = 0.0;
        params.crop_width = 1.0;
        params.crop_height = 1.0;
        params.lens_k1 = identity.lens_k1;
        params.lens_k2 = identity.lens_k2;
        params.lens_tca_r = identity.lens_tca_r;
        params.lens_tca_b = identity.lens_tca_b;
        params.lens_vignetting = identity.lens_vignetting;
        params.lens_mode = identity.lens_mode;
        params.lens_focal_mm = identity.lens_focal_mm;
    }
    else if (section == "whiteBalance")
    {
        params.temperature = identity.temperature;
    }
    else if (section == "profileGamma")
    {
        params.profile_gamma_enabled = identity.profile_gamma_enabled;
        params.profile_gamma = identity.profile_gamma;
    }
    else if (section == "inputProfile")
    {
        params.input_color = identity.input_color;
    }
    else if (section == "outputProfile")
    {
        params.output_color = identity.output_color;
    }
    else if (section == "calibration")
    {
        params.channel_mixer = identity.channel_mixer;
    }
    else if (section == "primaries")
    {
        params.primaries = identity.primaries;
    }
    else if (section == "light")
    {
        params.exposure_mode = identity.exposure_mode;
        params.exposure_black = identity.exposure_black;
        params.exposure_ev = identity.exposure_ev;
        params.exposure_deflicker_percentile = identity.exposure_deflicker_percentile;
        params.exposure_deflicker_target_ev = identity.exposure_deflicker_target_ev;
        params.exposure_compensate_exposure_bias = identity.exposure_compensate_exposure_bias;
        params.exposure_compensate_highlight_preservation =
            identity.exposure_compensate_highlight_preservation;
        params.contrast = identity.contrast;
        params.highlights = identity.highlights;
        params.shadows = identity.shadows;
        params.whites = identity.whites;
        params.blacks = identity.blacks;
        params.gamma = identity.gamma;
        params.rgb_levels = identity.rgb_levels;
        params.sigmoid_contrast = identity.sigmoid_contrast;
        params.sigmoid_skew = identity.sigmoid_skew;
        params.sigmoid_display_white = identity.sigmoid_display_white;
        params.sigmoid_display_black = identity.sigmoid_display_black;
        params.sigmoid_hue_preservation = identity.sigmoid_hue_preservation;
        params.tone_eq_blacks = identity.tone_eq_blacks;
        params.tone_eq_shadows = identity.tone_eq_shadows;
        params.tone_eq_midtones = identity.tone_eq_midtones;
        params.tone_eq_highlights = identity.tone_eq_highlights;
        params.tone_eq_whites = identity.tone_eq_whites;
    }
    else if (section == "curves")
    {
        params.rgb_curve = identity.rgb_curve;
        params.tone_curve.clear();
        params.tone_curve_a.clear();
        params.tone_curve_b.clear();
        params.tone_curve_working_space = identity.tone_curve_working_space;
        params.tone_curve_interpolation = identity.tone_curve_interpolation;
        params.tone_curve_channel_mode = identity.tone_curve_channel_mode;
        params.tone_curve_preserve_colors = identity.tone_curve_preserve_colors;
        params.curves_effect_enabled = identity.curves_effect_enabled;
    }
    else if (section == "color")
    {
        params.vibrance = identity.vibrance;
        params.saturation = identity.saturation;
        params.velvia_present = identity.velvia_present;
        params.velvia_enabled = identity.velvia_enabled;
        params.velvia = identity.velvia;
        params.velvia_mask_id = identity.velvia_mask_id;
        params.lut3d_present = identity.lut3d_present;
        params.lut3d_enabled = identity.lut3d_enabled;
        params.lut3d = identity.lut3d;
        params.color_balance_enabled = identity.color_balance_enabled;
        params.color_balance = identity.color_balance;
        params.color_checker_enabled = identity.color_checker_enabled;
        params.color_checker = identity.color_checker;
        params.color_checker_patch = identity.color_checker_patch;
        params.color_balance_rgb = identity.color_balance_rgb;
        params.color_correction_enabled = identity.color_correction_enabled;
        params.color_correction = identity.color_correction;
        params.color_contrast_enabled = identity.color_contrast_enabled;
        params.color_contrast = identity.color_contrast;
        params.color_reconstruction_enabled = identity.color_reconstruction_enabled;
        params.color_reconstruction = identity.color_reconstruction;
        params.color_harmonizer_enabled = identity.color_harmonizer_enabled;
        params.color_harmonizer = identity.color_harmonizer;
        params.monochrome_present = identity.monochrome_present;
        params.monochrome_enabled = identity.monochrome_enabled;
        params.monochrome = identity.monochrome;
        params.monochrome_mask_id = identity.monochrome_mask_id;
        params.split_toning_present = identity.split_toning_present;
        params.split_toning_enabled = identity.split_toning_enabled;
        params.split_toning = identity.split_toning;
        params.split_toning_mask_id = identity.split_toning_mask_id;
        params.color_zones_present = identity.color_zones_present;
        params.color_zones_enabled = identity.color_zones_enabled;
        params.color_zones = identity.color_zones;
        params.color_zones_mask_id = identity.color_zones_mask_id;
        params.color_zones_band = identity.color_zones_band;
    }
    else if (section == "colorEqualizer")
    {
        params.color_eq_hue = {};
        params.color_eq_sat = {};
        params.color_eq_light = {};
        params.color_eq_band = 0;
    }
    else if (section == "colorHarmonizer")
    {
        params.color_harmonizer_enabled = identity.color_harmonizer_enabled;
        params.color_harmonizer = identity.color_harmonizer;
    }
    else if (section == "detail")
    {
        params.sharpen = identity.sharpen;
        params.sharpen_radius = identity.sharpen_radius;
        params.sharpen_threshold = identity.sharpen_threshold;
        params.texture = identity.texture;
        params.retouch = identity.retouch;
        params.clarity = identity.clarity;
        params.grain = identity.grain;
        params.denoise = identity.denoise;
        params.denoise_chroma = identity.denoise_chroma;
        params.denoise_radius = identity.denoise_radius;
    }
    else if (section == "effects")
    {
        params.vignette = identity.vignette;
        params.vignette_midpoint = identity.vignette_midpoint;
        params.vignette_falloff = identity.vignette_falloff;
        params.vignette_shape = identity.vignette_shape;
        params.vignette_center_x = identity.vignette_center_x;
        params.vignette_center_y = identity.vignette_center_y;
        params.bloom = identity.bloom;
        params.soften = identity.soften;
        params.dehaze = identity.dehaze;
        params.dehaze_distance = identity.dehaze_distance;
        params.dehaze_adaptive = identity.dehaze_adaptive;
        params.output_dither_present = identity.output_dither_present;
        params.output_dither_enabled = identity.output_dither_enabled;
        params.output_dither = identity.output_dither;
        params.graduated_density = identity.graduated_density;
        params.graduated_hardness = identity.graduated_hardness;
        params.graduated_rotation = identity.graduated_rotation;
        params.graduated_offset = identity.graduated_offset;
    }
    else if (section == "raw")
    {
        params.demosaic_mode = identity.demosaic_mode;
        params.raw_highlights = identity.raw_highlights;
        params.raw_highlights_clip = identity.raw_highlights_clip;
        params.raw_highlights_mode = identity.raw_highlights_mode;
        params.hot_pixels_strength = identity.hot_pixels_strength;
        params.hot_pixels_threshold = identity.hot_pixels_threshold;
        params.hot_pixels_permissive = identity.hot_pixels_permissive;
        params.raw_ca_iterations = identity.raw_ca_iterations;
        params.raw_ca_avoid_shift = identity.raw_ca_avoid_shift;
        params.raw_denoise_threshold = identity.raw_denoise_threshold;
        params.raw_denoise_bands = identity.raw_denoise_bands;
        params.lens_k1 = identity.lens_k1;
        params.lens_vignetting = identity.lens_vignetting;
    }
    else if (section == "toneEqual")
    {
        params.tone_eq_blacks = identity.tone_eq_blacks;
        params.tone_eq_shadows = identity.tone_eq_shadows;
        params.tone_eq_midtones = identity.tone_eq_midtones;
        params.tone_eq_highlights = identity.tone_eq_highlights;
        params.tone_eq_whites = identity.tone_eq_whites;
    }
    else if (section == "graduated")
    {
        params.graduated_density = identity.graduated_density;
        params.graduated_hardness = identity.graduated_hardness;
        params.graduated_rotation = identity.graduated_rotation;
        params.graduated_offset = identity.graduated_offset;
    }
    else
    {
        return false;
    }
    if (section != "profileGamma")
    {
        static_cast<void>(set_develop_section_effect_enabled(params, section, true));
    }
    clamp_develop(params);
    return true;
}

bool develop_section_modified(const DevelopParams &params, const std::string_view section)
{
    const DevelopParams identity;
    if (section == "geometry")
    {
        return params.rotate_quarters % 4 != 0 || params.flip_horizontal != 0 ||
               params.flip_vertical != 0 || !near(params.straighten_degrees, 0.0) ||
               !near(params.perspective_vertical, 0.0) ||
               !near(params.perspective_horizontal, 0.0) || !near(params.perspective_shear, 0.0) ||
               !near(params.crop_x, 0.0) || !near(params.crop_y, 0.0) ||
               !near(params.crop_width, 1.0) || !near(params.crop_height, 1.0) ||
               params.canvas_present || params.canvas_enabled;
    }
    if (section == "whiteBalance")
    {
        return !params.temperature.is_identity();
    }
    if (section == "profileGamma")
    {
        return params.profile_gamma_enabled || !params.profile_gamma.is_default();
    }
    if (section == "inputProfile")
    {
        return !params.input_color.is_identity();
    }
    if (section == "outputProfile")
    {
        return !params.output_color.is_identity();
    }
    if (section == "calibration")
    {
        return !params.channel_mixer.is_identity();
    }
    if (section == "primaries")
    {
        return !params.primaries.is_identity();
    }
    if (section == "light")
    {
        const ExposureParams exposure{params.exposure_mode,
                                      params.exposure_black,
                                      params.exposure_ev,
                                      params.exposure_deflicker_percentile,
                                      params.exposure_deflicker_target_ev,
                                      params.exposure_compensate_exposure_bias,
                                      params.exposure_compensate_highlight_preservation};
        return !exposure.is_identity() || params.exposure_mask_id.has_value() ||
               !near(params.contrast, 0.0) ||
               !near(params.highlights, 0.0) || !near(params.shadows, 0.0) ||
               !near(params.whites, 0.0) || !near(params.blacks, 0.0) ||
               !near(params.gamma, kDevelopGammaDefault) || !params.rgb_levels.is_identity() ||
               params.sigmoid_enabled ||
               !near(params.sigmoid_contrast, identity.sigmoid_contrast) ||
               !near(params.sigmoid_skew, identity.sigmoid_skew) ||
               !near(params.sigmoid_display_white, identity.sigmoid_display_white) ||
               !near(params.sigmoid_display_black, identity.sigmoid_display_black) ||
               !near(params.sigmoid_hue_preservation, identity.sigmoid_hue_preservation);
    }
    if (section == "curves")
    {
        return !params.rgb_curve.is_identity() || !tone_curve_is_identity(params.tone_curve) ||
               !tone_curve_is_identity(params.tone_curve_a) ||
               !tone_curve_is_identity(params.tone_curve_b);
    }
    if (section == "color")
    {
        return !near(params.vibrance, 0.0) || !near(params.saturation, 0.0) ||
               params.velvia_present || params.velvia_enabled ||
               params.velvia_mask_id.has_value() || params.lut3d_present || params.lut3d_enabled ||
               params.color_balance_enabled || !params.color_balance.is_identity() ||
               params.color_checker_enabled || !params.color_balance_rgb.is_identity() ||
               params.color_balance_rgb_mask_id.has_value() ||
               params.color_correction_enabled || params.color_contrast_enabled ||
               params.color_reconstruction_enabled || params.color_zones_present ||
               params.color_zones_enabled || params.color_zones_mask_id.has_value() ||
               params.color_harmonizer_enabled || params.color_harmonizer_present ||
               params.monochrome_present || params.monochrome_enabled ||
               params.monochrome_mask_id.has_value() || params.split_toning_present ||
               params.split_toning_enabled || params.split_toning_mask_id.has_value();
    }
    if (section == "colorHarmonizer")
    {
        return params.color_harmonizer_enabled || params.color_harmonizer_present;
    }
    if (section == "detail")
    {
        return !near(params.sharpen, 0.0) ||
               !near(params.sharpen_radius, identity.sharpen_radius) ||
               !near(params.sharpen_threshold, identity.sharpen_threshold) ||
               !near(params.texture.strength, identity.texture.strength) ||
               !near(params.texture.detail_threshold, identity.texture.detail_threshold) ||
               params.texture.iterations != identity.texture.iterations ||
               !params.retouch.is_identity() || !near(params.clarity, 0.0) ||
               !near(params.grain, 0.0) || !near(params.denoise, 0.0);
    }
    if (section == "effects")
    {
        return !near(params.vignette, 0.0) || !near(params.bloom, 0.0) ||
               !near(params.soften, 0.0) || !near(params.dehaze, 0.0) ||
               !near(params.dehaze_distance, identity.dehaze_distance) ||
               params.dehaze_adaptive != identity.dehaze_adaptive || params.output_dither_present ||
               params.output_dither_enabled || params.frame_present || params.frame_enabled ||
               params.watermark_present || params.watermark_enabled;
    }
    if (section == "raw")
    {
        return params.demosaic_mode != kDemosaicModeRcd || !near(params.raw_highlights, 0.0) ||
               !near(params.hot_pixels_strength, 0.0) || params.raw_ca_iterations > 0 ||
               !near(params.raw_denoise_threshold, 0.0) || !near(params.lens_k1, 0.0) ||
               !near(params.lens_vignetting, 0.0);
    }
    if (section == "toneEqual")
    {
        return !near(params.tone_eq_blacks, 0.0) || !near(params.tone_eq_shadows, 0.0) ||
               !near(params.tone_eq_midtones, 0.0) || !near(params.tone_eq_highlights, 0.0) ||
               !near(params.tone_eq_whites, 0.0);
    }
    if (section == "graduated")
    {
        return params.graduated_present || params.graduated_enabled ||
               !near(params.graduated_density, 0.0);
    }
    if (section == "colorEqualizer")
    {
        return !bands_near_zero(params.color_eq_hue) || !bands_near_zero(params.color_eq_sat) ||
               !bands_near_zero(params.color_eq_light);
    }
    return false;
}

bool develop_section_effect_enabled(const DevelopParams &params, const std::string_view section)
{
    if (section == "geometry")
    {
        return params.geometry_effect_enabled;
    }
    if (section == "whiteBalance")
    {
        return params.white_balance_effect_enabled;
    }
    if (section == "profileGamma")
    {
        return params.profile_gamma_enabled;
    }
    if (section == "inputProfile")
    {
        return params.input_profile_effect_enabled;
    }
    if (section == "outputProfile")
    {
        return params.output_profile_effect_enabled;
    }
    if (section == "calibration")
    {
        return params.calibration_effect_enabled;
    }
    if (section == "primaries")
    {
        return params.primaries_effect_enabled;
    }
    if (section == "light")
    {
        return params.light_effect_enabled;
    }
    if (section == "color" || section == "colorHarmonizer")
    {
        return params.color_effect_enabled;
    }
    if (section == "detail")
    {
        return params.detail_effect_enabled;
    }
    if (section == "effects")
    {
        return params.effects_effect_enabled;
    }
    if (section == "raw")
    {
        return params.raw_effect_enabled;
    }
    if (section == "toneEqual")
    {
        return params.tone_equal_effect_enabled;
    }
    if (section == "graduated")
    {
        return params.graduated_effect_enabled;
    }
    if (section == "colorEqualizer")
    {
        return params.color_eq_effect_enabled;
    }
    if (section == "curves")
    {
        return params.curves_effect_enabled;
    }
    return false;
}

bool set_develop_section_effect_enabled(DevelopParams &params, const std::string_view section,
                                        const bool enabled)
{
    if (section == "geometry")
    {
        params.geometry_effect_enabled = enabled;
    }
    else if (section == "whiteBalance")
    {
        params.white_balance_effect_enabled = enabled;
    }
    else if (section == "profileGamma")
    {
        params.profile_gamma_enabled = enabled;
    }
    else if (section == "inputProfile")
    {
        params.input_profile_effect_enabled = enabled;
    }
    else if (section == "outputProfile")
    {
        params.output_profile_effect_enabled = enabled;
    }
    else if (section == "calibration")
    {
        params.calibration_effect_enabled = enabled;
    }
    else if (section == "primaries")
    {
        params.primaries_effect_enabled = enabled;
    }
    else if (section == "light")
    {
        params.light_effect_enabled = enabled;
    }
    else if (section == "color" || section == "colorHarmonizer")
    {
        params.color_effect_enabled = enabled;
    }
    else if (section == "detail")
    {
        params.detail_effect_enabled = enabled;
    }
    else if (section == "effects")
    {
        params.effects_effect_enabled = enabled;
    }
    else if (section == "raw")
    {
        params.raw_effect_enabled = enabled;
    }
    else if (section == "toneEqual")
    {
        params.tone_equal_effect_enabled = enabled;
    }
    else if (section == "graduated")
    {
        params.graduated_effect_enabled = enabled;
    }
    else if (section == "colorEqualizer")
    {
        params.color_eq_effect_enabled = enabled;
    }
    else if (section == "curves")
    {
        params.curves_effect_enabled = enabled;
    }
    else
    {
        return false;
    }
    return true;
}


} // namespace ravo
