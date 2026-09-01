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

std::string format_signed_amount(const double value)
{
    if (!std::isfinite(value))
    {
        return {};
    }
    const double rounded = std::round(value * 10.0) / 10.0;
    if (std::abs(rounded - std::round(rounded)) < 1e-6)
    {
        const int whole = static_cast<int>(std::round(rounded));
        return (whole > 0 ? "+" : "") + std::to_string(whole);
    }
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << std::showpos << rounded;
    return out.str();
}

void add_scaled_change(std::vector<DevelopChange> &changes, std::string field, const double before,
                       const double after, const double scale)
{
    if (near(before, after))
    {
        return;
    }
    changes.push_back({std::move(field), format_signed_amount((after - before) * scale)});
}

void add_toggle_change(std::vector<DevelopChange> &changes, std::string field, const bool before,
                       const bool after)
{
    if (before == after)
    {
        return;
    }
    changes.push_back({std::move(field), after ? std::string("on") : std::string("off")});
}

void add_named_change(std::vector<DevelopChange> &changes, std::string field, const bool changed)
{
    if (!changed)
    {
        return;
    }
    changes.push_back({std::move(field), {}});
}

} // namespace

std::vector<DevelopChange> develop_change_summary(const DevelopParams &before,
                                                  const DevelopParams &after)
{
    if (after.is_identity() && !before.is_identity())
    {
        return {{"reset", {}}};
    }
    std::vector<DevelopChange> changes;
    add_scaled_change(changes, "exposure", before.exposure_ev, after.exposure_ev, 1.0);
    add_named_change(changes, "exposureMask", before.exposure_mask_id != after.exposure_mask_id);
    add_scaled_change(changes, "black", before.exposure_black, after.exposure_black, 10.0);
    add_scaled_change(changes, "contrast", before.contrast, after.contrast, 10.0);
    add_scaled_change(changes, "highlights", before.highlights, after.highlights, 10.0);
    add_scaled_change(changes, "shadows", before.shadows, after.shadows, 10.0);
    add_scaled_change(changes, "whites", before.whites, after.whites, 10.0);
    add_scaled_change(changes, "blacks", before.blacks, after.blacks, 10.0);
    add_scaled_change(changes, "vibrance", before.vibrance, after.vibrance, 10.0);
    add_scaled_change(changes, "saturation", before.saturation, after.saturation, 10.0);
    add_named_change(changes, "velvia",
                     before.velvia_present != after.velvia_present ||
                         before.velvia_enabled != after.velvia_enabled ||
                         before.velvia != after.velvia ||
                         before.velvia_mask_id != after.velvia_mask_id);
    add_named_change(changes, "lut3d",
                     before.lut3d_present != after.lut3d_present ||
                         before.lut3d_enabled != after.lut3d_enabled ||
                         before.lut3d != after.lut3d);
    add_scaled_change(changes, "gamma", before.gamma, after.gamma, 10.0);
    add_named_change(changes, "rgbLevels", before.rgb_levels != after.rgb_levels);
    add_named_change(changes, "rgbCurve", before.rgb_curve != after.rgb_curve);
    add_scaled_change(changes, "sharpen", before.sharpen, after.sharpen, 10.0);
    add_scaled_change(changes, "sharpenRadius", before.sharpen_radius, after.sharpen_radius, 1.0);
    add_scaled_change(changes, "sharpenThreshold", before.sharpen_threshold,
                      after.sharpen_threshold, 1.0);
    add_scaled_change(changes, "texture", before.texture.strength, after.texture.strength, 10.0);
    add_scaled_change(changes, "textureDetailThreshold", before.texture.detail_threshold,
                      after.texture.detail_threshold, 1.0);
    add_scaled_change(changes, "textureIterations", static_cast<double>(before.texture.iterations),
                      static_cast<double>(after.texture.iterations), 1.0);
    add_named_change(changes, "retouch", before.retouch != after.retouch);
    add_scaled_change(changes, "clarity", before.clarity, after.clarity, 10.0);
    add_scaled_change(changes, "vignette", before.vignette, after.vignette, 10.0);
    add_scaled_change(changes, "grain", before.grain, after.grain, 10.0);
    add_scaled_change(changes, "bloom", before.bloom, after.bloom, 10.0);
    add_scaled_change(changes, "soften", before.soften, after.soften, 10.0);
    add_scaled_change(changes, "dehaze", before.dehaze, after.dehaze, 10.0);
    add_scaled_change(changes, "dehazeDistance", before.dehaze_distance, after.dehaze_distance,
                      10.0);
    add_toggle_change(changes, "dehazeAdaptive", before.dehaze_adaptive, after.dehaze_adaptive);
    add_named_change(changes, "outputDither",
                     before.output_dither_present != after.output_dither_present ||
                         before.output_dither_enabled != after.output_dither_enabled ||
                         before.output_dither != after.output_dither);
    add_named_change(changes, "outputFrame",
                     before.frame_present != after.frame_present ||
                         before.frame_enabled != after.frame_enabled ||
                         before.frame != after.frame);
    add_named_change(changes, "colorZones",
                     before.color_zones_present != after.color_zones_present ||
                         before.color_zones_enabled != after.color_zones_enabled ||
                         before.color_zones != after.color_zones ||
                         before.color_zones_mask_id != after.color_zones_mask_id);
    add_named_change(changes, "watermark",
                     before.watermark_present != after.watermark_present ||
                         before.watermark_enabled != after.watermark_enabled ||
                         before.watermark != after.watermark);
    add_named_change(changes, "monochrome",
                     before.monochrome_present != after.monochrome_present ||
                         before.monochrome_enabled != after.monochrome_enabled ||
                         before.monochrome != after.monochrome ||
                         before.monochrome_mask_id != after.monochrome_mask_id);
    add_named_change(changes, "splitToning",
                     before.split_toning_present != after.split_toning_present ||
                         before.split_toning_enabled != after.split_toning_enabled ||
                         before.split_toning != after.split_toning ||
                         before.split_toning_mask_id != after.split_toning_mask_id);
    add_scaled_change(changes, "denoise", before.denoise, after.denoise, 10.0);
    add_scaled_change(changes, "straighten", before.straighten_degrees, after.straighten_degrees,
                      1.0);
    add_scaled_change(changes, "perspectiveVertical", before.perspective_vertical,
                      after.perspective_vertical, 10.0);
    add_scaled_change(changes, "perspectiveHorizontal", before.perspective_horizontal,
                      after.perspective_horizontal, 10.0);
    add_scaled_change(changes, "perspectiveShear", before.perspective_shear,
                      after.perspective_shear, 10.0);
    add_toggle_change(changes, "perspectiveConstrainCrop", before.perspective_constrain_crop,
                      after.perspective_constrain_crop);
    add_named_change(changes, "perspectiveInterpolation",
                     before.perspective_interpolation_index !=
                         after.perspective_interpolation_index);
    add_scaled_change(changes, "toneEqBlacks", before.tone_eq_blacks, after.tone_eq_blacks, 1.0);
    add_scaled_change(changes, "toneEqShadows", before.tone_eq_shadows, after.tone_eq_shadows, 1.0);
    add_scaled_change(changes, "toneEqMidtones", before.tone_eq_midtones, after.tone_eq_midtones,
                      1.0);
    add_scaled_change(changes, "toneEqHighlights", before.tone_eq_highlights,
                      after.tone_eq_highlights, 1.0);
    add_scaled_change(changes, "toneEqWhites", before.tone_eq_whites, after.tone_eq_whites, 1.0);
    add_scaled_change(changes, "graduated", before.graduated_density, after.graduated_density, 1.0);
    if (before.rotate_quarters % 4 != after.rotate_quarters % 4)
    {
        const auto delta = ((after.rotate_quarters - before.rotate_quarters) % 4 + 4) % 4;
        const int degrees = delta == 3 ? -90 : static_cast<int>(delta) * 90;
        changes.push_back({"rotate", format_signed_amount(static_cast<double>(degrees))});
    }
    add_named_change(changes, "flip",
                     before.flip_horizontal != after.flip_horizontal ||
                         before.flip_vertical != after.flip_vertical);
    add_named_change(changes, "canvas",
                     before.canvas_present != after.canvas_present ||
                         before.canvas_enabled != after.canvas_enabled ||
                         before.canvas != after.canvas);
    add_named_change(changes, "crop",
                     !near(before.crop_x, after.crop_x) || !near(before.crop_y, after.crop_y) ||
                         !near(before.crop_width, after.crop_width) ||
                         !near(before.crop_height, after.crop_height));
    add_named_change(changes, "toneCurve",
                     before.tone_curve != after.tone_curve ||
                         before.tone_curve_a != after.tone_curve_a ||
                         before.tone_curve_b != after.tone_curve_b ||
                         before.tone_curve_interpolation != after.tone_curve_interpolation ||
                         before.tone_curve_channel_mode != after.tone_curve_channel_mode ||
                         before.tone_curve_preserve_colors != after.tone_curve_preserve_colors ||
                         before.tone_curve_working_space != after.tone_curve_working_space);
    add_toggle_change(changes, "curves", before.curves_effect_enabled, after.curves_effect_enabled);
    add_named_change(changes, "demosaic", before.demosaic_mode != after.demosaic_mode);
    add_named_change(changes, "whiteBalance", before.temperature != after.temperature);
    add_named_change(changes, "inputProfile", before.input_color != after.input_color);
    add_named_change(changes, "outputProfile", before.output_color != after.output_color);
    add_named_change(changes, "primaries", before.primaries != after.primaries);
    add_named_change(changes, "mixer", before.channel_mixer != after.channel_mixer);
    add_named_change(changes, "colorBalance",
                     before.color_balance != after.color_balance ||
                         before.color_balance_enabled != after.color_balance_enabled);
    add_named_change(changes, "colorBalanceRgb",
                     before.color_balance_rgb != after.color_balance_rgb ||
                         before.color_balance_rgb_mask_id != after.color_balance_rgb_mask_id);
    add_named_change(changes, "colorReconstruction",
                     before.color_reconstruction != after.color_reconstruction ||
                         before.color_reconstruction_enabled != after.color_reconstruction_enabled);
    add_toggle_change(changes, "profileGamma", before.profile_gamma_enabled,
                      after.profile_gamma_enabled);
    add_toggle_change(changes, "sigmoid", before.sigmoid_enabled, after.sigmoid_enabled);
    add_toggle_change(changes, "light", before.light_effect_enabled, after.light_effect_enabled);
    add_toggle_change(changes, "color", before.color_effect_enabled, after.color_effect_enabled);
    add_toggle_change(changes, "detail", before.detail_effect_enabled, after.detail_effect_enabled);
    add_toggle_change(changes, "effects", before.effects_effect_enabled,
                      after.effects_effect_enabled);
    add_toggle_change(changes, "geometry", before.geometry_effect_enabled,
                      after.geometry_effect_enabled);
    add_toggle_change(changes, "colorEqualizer", before.color_eq_effect_enabled,
                      after.color_eq_effect_enabled);
    return changes;
}

} // namespace ravo
