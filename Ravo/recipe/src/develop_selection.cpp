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

namespace
{

constexpr auto kDevelopSelectableFields = std::to_array<std::string_view>({
    "demosaic",
    "whiteBalance",
    "profileGamma",
    "inputProfile",
    "outputProfile",
    "primaries",
    "calibration",
    "exposure",
    "contrast",
    "highlights",
    "shadows",
    "whites",
    "blacks",
    "gamma",
    "rgbLevels",
    "sigmoid",
    "toneEqual",
    "rgbCurve",
    "toneCurve",
    "vibrance",
    "saturation",
    "velvia",
    "lut3d",
    "colorBalance",
    "colorChecker",
    "colorBalanceRgb",
    "colorCorrection",
    "colorContrast",
    "colorReconstruction",
    "colorZones",
    "colorHarmonizer",
    "monochrome",
    "splitToning",
    "colorEqualizer",
    "sharpen",
    "texture",
    "retouch",
    "clarity",
    "denoise",
    "grain",
    "rawHighlights",
    "hotPixels",
    "rawChromaticAberration",
    "rawDenoise",
    "rotate",
    "flip",
    "straighten",
    "perspective",
    "crop",
    "canvas",
    "lens",
    "vignette",
    "bloom",
    "soften",
    "dehaze",
    "outputDither",
    "graduated",
    "outputFrame",
    "watermark",
    "masks",
    "geometrySectionState",
    "inputProfileSectionState",
    "outputProfileSectionState",
    "whiteBalanceSectionState",
    "calibrationSectionState",
    "primariesSectionState",
    "lightSectionState",
    "colorSectionState",
    "detailSectionState",
    "effectsSectionState",
    "rawSectionState",
    "toneEqualSectionState",
    "graduatedSectionState",
    "colorEqualizerSectionState",
    "curvesSectionState",
});

[[nodiscard]] bool copy_develop_selected_field(DevelopParams &destination,
                                               const DevelopParams &source,
                                               const std::string_view field)
{
    if (field == "demosaic")
    {
        destination.demosaic_mode = source.demosaic_mode;
    }
    else if (field == "whiteBalance")
    {
        destination.temperature = source.temperature;
    }
    else if (field == "profileGamma")
    {
        destination.profile_gamma_enabled = source.profile_gamma_enabled;
        destination.profile_gamma = source.profile_gamma;
    }
    else if (field == "inputProfile")
    {
        destination.input_color = source.input_color;
    }
    else if (field == "outputProfile")
    {
        destination.output_color = source.output_color;
    }
    else if (field == "primaries")
    {
        destination.primaries = source.primaries;
    }
    else if (field == "calibration")
    {
        destination.channel_mixer = source.channel_mixer;
    }
    else if (field == "exposure")
    {
        destination.exposure_mode = source.exposure_mode;
        destination.exposure_black = source.exposure_black;
        destination.exposure_ev = source.exposure_ev;
        destination.exposure_deflicker_percentile = source.exposure_deflicker_percentile;
        destination.exposure_deflicker_target_ev = source.exposure_deflicker_target_ev;
        destination.exposure_compensate_exposure_bias = source.exposure_compensate_exposure_bias;
        destination.exposure_compensate_highlight_preservation =
            source.exposure_compensate_highlight_preservation;
        destination.exposure_mask_id = source.exposure_mask_id;
    }
    else if (field == "contrast")
    {
        destination.contrast = source.contrast;
    }
    else if (field == "highlights")
    {
        destination.highlights = source.highlights;
    }
    else if (field == "shadows")
    {
        destination.shadows = source.shadows;
    }
    else if (field == "whites")
    {
        destination.whites = source.whites;
    }
    else if (field == "blacks")
    {
        destination.blacks = source.blacks;
    }
    else if (field == "gamma")
    {
        destination.gamma = source.gamma;
    }
    else if (field == "rgbLevels")
    {
        destination.rgb_levels = source.rgb_levels;
    }
    else if (field == "sigmoid")
    {
        destination.sigmoid_enabled = source.sigmoid_enabled;
        destination.sigmoid_contrast = source.sigmoid_contrast;
        destination.sigmoid_skew = source.sigmoid_skew;
        destination.sigmoid_display_white = source.sigmoid_display_white;
        destination.sigmoid_display_black = source.sigmoid_display_black;
        destination.sigmoid_hue_preservation = source.sigmoid_hue_preservation;
    }
    else if (field == "toneEqual")
    {
        destination.tone_eq_blacks = source.tone_eq_blacks;
        destination.tone_eq_shadows = source.tone_eq_shadows;
        destination.tone_eq_midtones = source.tone_eq_midtones;
        destination.tone_eq_highlights = source.tone_eq_highlights;
        destination.tone_eq_whites = source.tone_eq_whites;
    }
    else if (field == "rgbCurve")
    {
        destination.rgb_curve = source.rgb_curve;
        destination.rgb_curve_mask_id = source.rgb_curve_mask_id;
    }
    else if (field == "toneCurve")
    {
        destination.tone_curve = source.tone_curve;
        destination.tone_curve_a = source.tone_curve_a;
        destination.tone_curve_b = source.tone_curve_b;
        destination.tone_curve_working_space = source.tone_curve_working_space;
        destination.tone_curve_interpolation = source.tone_curve_interpolation;
        destination.tone_curve_channel_mode = source.tone_curve_channel_mode;
        destination.tone_curve_preserve_colors = source.tone_curve_preserve_colors;
    }
    else if (field == "vibrance")
    {
        destination.vibrance = source.vibrance;
    }
    else if (field == "saturation")
    {
        destination.saturation = source.saturation;
    }
    else if (field == "velvia")
    {
        destination.velvia_present = source.velvia_present;
        destination.velvia_enabled = source.velvia_enabled;
        destination.velvia = source.velvia;
        destination.velvia_mask_id = source.velvia_mask_id;
    }
    else if (field == "lut3d")
    {
        destination.lut3d_present = source.lut3d_present;
        destination.lut3d_enabled = source.lut3d_enabled;
        destination.lut3d = source.lut3d;
    }
    else if (field == "colorBalance")
    {
        destination.color_balance_enabled = source.color_balance_enabled;
        destination.color_balance = source.color_balance;
    }
    else if (field == "colorChecker")
    {
        destination.color_checker_enabled = source.color_checker_enabled;
        destination.color_checker = source.color_checker;
    }
    else if (field == "colorBalanceRgb")
    {
        destination.color_balance_rgb = source.color_balance_rgb;
        destination.color_balance_rgb_mask_id = source.color_balance_rgb_mask_id;
    }
    else if (field == "colorCorrection")
    {
        destination.color_correction_enabled = source.color_correction_enabled;
        destination.color_correction = source.color_correction;
    }
    else if (field == "colorContrast")
    {
        destination.color_contrast_enabled = source.color_contrast_enabled;
        destination.color_contrast = source.color_contrast;
    }
    else if (field == "colorReconstruction")
    {
        destination.color_reconstruction_enabled = source.color_reconstruction_enabled;
        destination.color_reconstruction = source.color_reconstruction;
    }
    else if (field == "colorZones")
    {
        destination.color_zones_present = source.color_zones_present;
        destination.color_zones_enabled = source.color_zones_enabled;
        destination.color_zones = source.color_zones;
        destination.color_zones_mask_id = source.color_zones_mask_id;
    }
    else if (field == "colorHarmonizer")
    {
        destination.color_harmonizer_present = source.color_harmonizer_present;
        destination.color_harmonizer_enabled = source.color_harmonizer_enabled;
        destination.color_harmonizer = source.color_harmonizer;
        destination.color_harmonizer_mask_id = source.color_harmonizer_mask_id;
    }
    else if (field == "monochrome")
    {
        destination.monochrome_present = source.monochrome_present;
        destination.monochrome_enabled = source.monochrome_enabled;
        destination.monochrome = source.monochrome;
        destination.monochrome_mask_id = source.monochrome_mask_id;
    }
    else if (field == "splitToning")
    {
        destination.split_toning_present = source.split_toning_present;
        destination.split_toning_enabled = source.split_toning_enabled;
        destination.split_toning = source.split_toning;
        destination.split_toning_mask_id = source.split_toning_mask_id;
    }
    else if (field == "colorEqualizer")
    {
        destination.color_eq_hue = source.color_eq_hue;
        destination.color_eq_sat = source.color_eq_sat;
        destination.color_eq_light = source.color_eq_light;
    }
    else if (field == "sharpen")
    {
        destination.sharpen = source.sharpen;
        destination.sharpen_radius = source.sharpen_radius;
        destination.sharpen_threshold = source.sharpen_threshold;
    }
    else if (field == "texture")
    {
        destination.texture = source.texture;
    }
    else if (field == "retouch")
    {
        destination.retouch = source.retouch;
    }
    else if (field == "clarity")
    {
        destination.clarity = source.clarity;
    }
    else if (field == "denoise")
    {
        destination.denoise = source.denoise;
        destination.denoise_chroma = source.denoise_chroma;
        destination.denoise_radius = source.denoise_radius;
    }
    else if (field == "grain")
    {
        destination.grain = source.grain;
    }
    else if (field == "rawHighlights")
    {
        destination.raw_highlights = source.raw_highlights;
        destination.raw_highlights_clip = source.raw_highlights_clip;
        destination.raw_highlights_mode = source.raw_highlights_mode;
    }
    else if (field == "hotPixels")
    {
        destination.hot_pixels_strength = source.hot_pixels_strength;
        destination.hot_pixels_threshold = source.hot_pixels_threshold;
        destination.hot_pixels_permissive = source.hot_pixels_permissive;
    }
    else if (field == "rawChromaticAberration")
    {
        destination.raw_ca_iterations = source.raw_ca_iterations;
        destination.raw_ca_avoid_shift = source.raw_ca_avoid_shift;
    }
    else if (field == "rawDenoise")
    {
        destination.raw_denoise_threshold = source.raw_denoise_threshold;
        destination.raw_denoise_bands = source.raw_denoise_bands;
    }
    else if (field == "rotate")
    {
        destination.rotate_quarters = source.rotate_quarters;
    }
    else if (field == "flip")
    {
        destination.flip_horizontal = source.flip_horizontal;
        destination.flip_vertical = source.flip_vertical;
    }
    else if (field == "straighten")
    {
        destination.straighten_degrees = source.straighten_degrees;
    }
    else if (field == "perspective")
    {
        destination.perspective_vertical = source.perspective_vertical;
        destination.perspective_horizontal = source.perspective_horizontal;
        destination.perspective_shear = source.perspective_shear;
        destination.perspective_constrain_crop = source.perspective_constrain_crop;
        destination.perspective_interpolation_index = source.perspective_interpolation_index;
    }
    else if (field == "crop")
    {
        destination.crop_x = source.crop_x;
        destination.crop_y = source.crop_y;
        destination.crop_width = source.crop_width;
        destination.crop_height = source.crop_height;
    }
    else if (field == "canvas")
    {
        destination.canvas_present = source.canvas_present;
        destination.canvas_enabled = source.canvas_enabled;
        destination.canvas = source.canvas;
    }
    else if (field == "lens")
    {
        destination.lens_k1 = source.lens_k1;
        destination.lens_k2 = source.lens_k2;
        destination.lens_tca_r = source.lens_tca_r;
        destination.lens_tca_b = source.lens_tca_b;
        destination.lens_vignetting = source.lens_vignetting;
        destination.lens_mode = source.lens_mode;
        destination.lens_make = source.lens_make;
        destination.lens_model = source.lens_model;
        destination.lens_name = source.lens_name;
        destination.lens_focal_mm = source.lens_focal_mm;
    }
    else if (field == "vignette")
    {
        destination.vignette = source.vignette;
        destination.vignette_midpoint = source.vignette_midpoint;
        destination.vignette_falloff = source.vignette_falloff;
        destination.vignette_shape = source.vignette_shape;
        destination.vignette_center_x = source.vignette_center_x;
        destination.vignette_center_y = source.vignette_center_y;
    }
    else if (field == "bloom")
    {
        destination.bloom = source.bloom;
    }
    else if (field == "soften")
    {
        destination.soften = source.soften;
    }
    else if (field == "dehaze")
    {
        destination.dehaze = source.dehaze;
        destination.dehaze_distance = source.dehaze_distance;
        destination.dehaze_adaptive = source.dehaze_adaptive;
    }
    else if (field == "outputDither")
    {
        destination.output_dither_present = source.output_dither_present;
        destination.output_dither_enabled = source.output_dither_enabled;
        destination.output_dither = source.output_dither;
    }
    else if (field == "graduated")
    {
        destination.graduated_present = source.graduated_present;
        destination.graduated_enabled = source.graduated_enabled;
        destination.graduated_density = source.graduated_density;
        destination.graduated_hardness = source.graduated_hardness;
        destination.graduated_rotation = source.graduated_rotation;
        destination.graduated_offset = source.graduated_offset;
        destination.graduated_mask_id = source.graduated_mask_id;
    }
    else if (field == "outputFrame")
    {
        destination.frame_present = source.frame_present;
        destination.frame_enabled = source.frame_enabled;
        destination.frame = source.frame;
    }
    else if (field == "watermark")
    {
        destination.watermark_present = source.watermark_present;
        destination.watermark_enabled = source.watermark_enabled;
        destination.watermark = source.watermark;
    }
    else if (field == "masks")
    {
        destination.velvia_mask_id = source.velvia_mask_id;
        destination.color_zones_mask_id = source.color_zones_mask_id;
        destination.color_harmonizer_mask_id = source.color_harmonizer_mask_id;
        destination.color_balance_rgb_mask_id = source.color_balance_rgb_mask_id;
        destination.exposure_mask_id = source.exposure_mask_id;
        destination.rgb_curve_mask_id = source.rgb_curve_mask_id;
        destination.monochrome_mask_id = source.monochrome_mask_id;
        destination.split_toning_mask_id = source.split_toning_mask_id;
        destination.graduated_mask_id = source.graduated_mask_id;
    }
    else if (field == "geometrySectionState")
        destination.geometry_effect_enabled = source.geometry_effect_enabled;
    else if (field == "inputProfileSectionState")
        destination.input_profile_effect_enabled = source.input_profile_effect_enabled;
    else if (field == "outputProfileSectionState")
        destination.output_profile_effect_enabled = source.output_profile_effect_enabled;
    else if (field == "whiteBalanceSectionState")
        destination.white_balance_effect_enabled = source.white_balance_effect_enabled;
    else if (field == "calibrationSectionState")
        destination.calibration_effect_enabled = source.calibration_effect_enabled;
    else if (field == "primariesSectionState")
        destination.primaries_effect_enabled = source.primaries_effect_enabled;
    else if (field == "lightSectionState")
        destination.light_effect_enabled = source.light_effect_enabled;
    else if (field == "colorSectionState")
        destination.color_effect_enabled = source.color_effect_enabled;
    else if (field == "detailSectionState")
        destination.detail_effect_enabled = source.detail_effect_enabled;
    else if (field == "effectsSectionState")
        destination.effects_effect_enabled = source.effects_effect_enabled;
    else if (field == "rawSectionState")
        destination.raw_effect_enabled = source.raw_effect_enabled;
    else if (field == "toneEqualSectionState")
        destination.tone_equal_effect_enabled = source.tone_equal_effect_enabled;
    else if (field == "graduatedSectionState")
        destination.graduated_effect_enabled = source.graduated_effect_enabled;
    else if (field == "colorEqualizerSectionState")
        destination.color_eq_effect_enabled = source.color_eq_effect_enabled;
    else if (field == "curvesSectionState")
        destination.curves_effect_enabled = source.curves_effect_enabled;
    else
    {
        return false;
    }
    return true;
}

[[nodiscard]] bool selected_field_carries_masks(const std::string_view field) noexcept
{
    return field == "masks" || field == "retouch" || field == "velvia" || field == "colorZones" ||
           field == "colorHarmonizer" || field == "colorBalanceRgb" || field == "monochrome" ||
           field == "splitToning" || field == "graduated";
}

void merge_develop_masks(DevelopParams &destination, const DevelopParams &source)
{
    for (const auto &source_mask : source.masks)
    {
        const auto existing = std::find_if(destination.masks.begin(), destination.masks.end(),
                                           [&source_mask](const Mask &candidate)
                                           { return candidate.id == source_mask.id; });
        if (existing == destination.masks.end())
        {
            destination.masks.push_back(source_mask);
        }
        else
        {
            *existing = source_mask;
        }
    }
}

} // namespace

std::span<const std::string_view> develop_selectable_field_names() noexcept
{
    return kDevelopSelectableFields;
}

bool is_develop_selectable_field(const std::string_view field) noexcept
{
    return std::find(kDevelopSelectableFields.begin(), kDevelopSelectableFields.end(), field) !=
           kDevelopSelectableFields.end();
}

std::vector<DevelopChange> develop_modified_fields(const DevelopParams &before,
                                                   const DevelopParams &after)
{
    std::vector<DevelopChange> changes;
    changes.reserve(kDevelopSelectableFields.size());
    DevelopParams candidate = before;
    for (const auto field : kDevelopSelectableFields)
    {
        if (field == "masks")
        {
            if (before.masks != after.masks)
                changes.push_back({std::string(field), {}});
            continue;
        }
        if (copy_develop_selected_field(candidate, after, field) && candidate != before)
            changes.push_back({std::string(field), {}});
        static_cast<void>(copy_develop_selected_field(candidate, before, field));
    }
    return changes;
}

Result<void> apply_develop_selected_fields(DevelopParams &destination, const DevelopParams &source,
                                           const std::vector<std::string> &fields)
{
    if (fields.empty())
    {
        return make_error(ErrorCode::kValidation, "Develop field selection is empty",
                          {{"reason", "empty_develop_field_selection"}});
    }
    if (fields.size() > kDevelopSelectableFields.size())
    {
        return make_error(ErrorCode::kValidation, "Develop field selection is too large",
                          {{"reason", "develop_field_selection_too_large"}});
    }

    DevelopParams candidate = destination;
    std::set<std::string, std::less<>> seen;
    bool carries_masks = false;
    for (const auto &field : fields)
    {
        if (!seen.insert(field).second)
        {
            return make_error(ErrorCode::kValidation, "Develop field is duplicated",
                              {{"field", field}, {"reason", "duplicate_develop_field"}});
        }
        if (!copy_develop_selected_field(candidate, source, field))
        {
            return make_error(ErrorCode::kUnsupported, "Develop field is unsupported",
                              {{"field", field}, {"reason", "unsupported_develop_field"}});
        }
        carries_masks = carries_masks || selected_field_carries_masks(field);
    }
    if (carries_masks)
        merge_develop_masks(candidate, source);
    destination = std::move(candidate);
    return {};
}

} // namespace ravo
