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

Result<DevelopParams> develop_from_recipe(const Recipe &recipe)
{
    DevelopParams params;
    params.masks = recipe.masks;
    bool demosaic_present = false;
    bool perspective_geometry_present = false;
    std::map<std::string, std::pair<bool, bool>, std::less<>> section_flags;
    const auto note_section = [&](const std::string_view section, const bool enabled)
    {
        auto &entry = section_flags[std::string(section)];
        entry.first = true;
        entry.second = entry.second || enabled;
    };
    for (const auto &operation : recipe.operations)
    {
        const auto number = [&](const std::string_view name, const double fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_number(found->second, fallback);
        };
        const auto integer = [&](const std::string_view name, const std::int64_t fallback)
        {
            const auto found = operation.parameters.find(std::string(name));
            if (found == operation.parameters.end())
            {
                return fallback;
            }
            return as_integer(found->second, fallback);
        };
        if (operation.id == kDemosaicOperationId)
        {
            if (demosaic_present)
            {
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate RAW demosaic operations",
                                  {{"reason", "duplicate_demosaic_operation"}});
            }
            if (!operation.enabled)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop cannot disable the RAW demosaic owner",
                                  {{"reason", "disabled_demosaic_operation"}});
            }
            auto mode = demosaic_mode_from_parameters(operation.parameters);
            if (!mode)
            {
                return mode.error();
            }
            params.demosaic_mode = std::move(mode).value();
            demosaic_present = true;
        }
        else if (operation.id == "ravo.color.temperature")
        {
            auto temperature = temperature_from_parameters(operation.parameters);
            if (!temperature)
            {
                return temperature.error();
            }
            params.temperature = std::move(temperature).value();
            note_section("whiteBalance", operation.enabled);
        }
        else if (operation.id == kProfileGammaOperationId)
        {
            auto profile_gamma = profile_gamma_from_parameters(operation.parameters);
            if (!profile_gamma)
            {
                return profile_gamma.error();
            }
            if (operation.enabled)
            {
                params.profile_gamma_enabled = true;
                params.profile_gamma = std::move(profile_gamma).value();
            }
        }
        else if (operation.id == "ravo.color.input")
        {
            auto input_color = input_color_from_parameters(operation.parameters);
            if (!input_color)
            {
                return input_color.error();
            }
            params.input_color = std::move(input_color).value();
            note_section("inputProfile", operation.enabled);
        }
        else if (operation.id == kPrimariesOperationId)
        {
            auto primaries = primaries_from_parameters(operation.parameters);
            if (!primaries)
            {
                return primaries.error();
            }
            params.primaries = std::move(primaries).value();
            note_section("primaries", operation.enabled);
        }
        else if (operation.id == "ravo.color.output")
        {
            auto output_color = output_color_from_parameters(operation.parameters);
            if (!output_color)
            {
                return output_color.error();
            }
            params.output_color = std::move(output_color).value();
            note_section("outputProfile", operation.enabled);
        }
        else if (operation.id == kOutputDitherOperationId)
        {
            if (params.output_dither_present)
            {
                return make_error(
                    ErrorCode::kValidation, "Develop contains duplicate Output Dither operations",
                    {{"operation_id", operation.id}, {"reason", "duplicate_output_dither"}});
            }
            if (operation.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Output Dither masks are unsupported",
                    {{"operation_id", operation.id}, {"reason", "unsupported_output_dither_mask"}});
            }
            auto dither = output_dither_from_parameters(operation.parameters);
            if (!dither)
                return dither.error();
            params.output_dither_present = true;
            params.output_dither_enabled = operation.enabled;
            params.output_dither = dither.value();
            note_section("effects", operation.enabled);
        }
        else if (operation.id == kFrameOperationId)
        {
            if (params.frame_present)
                return make_error(ErrorCode::kValidation, "Develop contains duplicate Frames",
                                  {{"reason", "duplicate_output_frame"}});
            if (operation.mask_id.has_value())
                return make_error(ErrorCode::kUnsupported, "Develop Frame masks are unsupported",
                                  {{"reason", "unsupported_frame_mask"}});
            auto frame = frame_from_parameters(operation.parameters);
            if (!frame)
                return frame.error();
            params.frame_present = true;
            params.frame_enabled = operation.enabled;
            params.frame = frame.value();
            note_section("effects", operation.enabled);
        }
        else if (operation.id == kWatermarkOperationId)
        {
            if (params.watermark_present)
                return make_error(ErrorCode::kValidation, "Develop contains duplicate Watermarks",
                                  {{"reason", "duplicate_watermark"}});
            if (operation.mask_id.has_value())
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Watermark masks are unsupported",
                                  {{"reason", "unsupported_watermark_mask"}});
            auto watermark = watermark_from_parameters(operation.parameters);
            if (!watermark)
                return watermark.error();
            params.watermark_present = true;
            params.watermark_enabled = operation.enabled;
            params.watermark = watermark.value();
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.color.channelmixerrgb")
        {
            auto mixer = channel_mixer_from_parameters(operation.parameters);
            if (!mixer)
            {
                return mixer.error();
            }
            params.channel_mixer = std::move(mixer).value();
            note_section("calibration", operation.enabled);
        }
        else if (operation.id == kExposureOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_exposure_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            auto exposure = exposure_from_parameters(canonical.parameters);
            if (!exposure)
            {
                return exposure.error();
            }
            params.exposure_mode = exposure.value().mode;
            params.exposure_black = exposure.value().black;
            params.exposure_ev = exposure.value().exposure_ev;
            params.exposure_deflicker_percentile = exposure.value().deflicker_percentile;
            params.exposure_deflicker_target_ev = exposure.value().deflicker_target_ev;
            params.exposure_compensate_exposure_bias = exposure.value().compensate_exposure_bias;
            params.exposure_compensate_highlight_preservation =
                exposure.value().compensate_highlight_preservation;
            params.exposure_mask_id = operation.mask_id;
            note_section("light", operation.enabled);
        }
        else if (operation.id == kColorCheckerOperationId)
        {
            auto color_checker = color_checker_from_parameters(operation.parameters);
            if (!color_checker)
            {
                return color_checker.error();
            }
            params.color_checker_enabled = true;
            params.color_checker = std::move(color_checker).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorHarmonizerOperationId)
        {
            if (params.color_harmonizer_present)
            {
                return make_error(ErrorCode::kConflict,
                                  "Develop Color Harmonizer does not allow duplicate operations",
                                  {{"operation_id", operation.id},
                                   {"reason", "duplicate_colorharmonizer_operation"}});
            }
            if (operation.schema_version != kColorHarmonizerOperationSchemaVersion)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Color Harmonizer schema version is unsupported",
                                  {{"operation_id", operation.id},
                                   {"schema_version", std::to_string(operation.schema_version)},
                                   {"reason", "unsupported_colorharmonizer_schema"}});
            }
            auto color_harmonizer = color_harmonizer_from_parameters(operation.parameters);
            if (!color_harmonizer)
            {
                return color_harmonizer.error();
            }
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = operation.enabled;
            params.color_harmonizer = std::move(color_harmonizer).value();
            params.color_harmonizer_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.core.contrast")
        {
            params.contrast = number("amount", params.contrast);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.highlights")
        {
            params.highlights = number("amount", params.highlights);
            params.highlights_mask_id = operation.mask_id;
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.shadows")
        {
            params.shadows = number("amount", params.shadows);
            params.shadows_mask_id = operation.mask_id;
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.whites")
        {
            params.whites = number("amount", params.whites);
            params.whites_mask_id = operation.mask_id;
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.blacks")
        {
            params.blacks = number("amount", params.blacks);
            params.blacks_mask_id = operation.mask_id;
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.core.gamma")
        {
            params.gamma = number("gamma", params.gamma);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.color.rgblevels")
        {
            const auto take_text = [&](const char *name, std::string &target)
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    if (const auto *text = as_string_if(found->second); text != nullptr)
                    {
                        target = *text;
                    }
                }
            };
            take_text("mode", params.rgb_levels.mode);
            take_text("preserve_colors", params.rgb_levels.preserve_colors);
            if (params.rgb_levels.mode != kRgbLevelsModeLinked &&
                params.rgb_levels.mode != kRgbLevelsModeIndependent)
            {
                return make_error(ErrorCode::kValidation, "RGB levels mode is unsupported",
                                  {{"mode", params.rgb_levels.mode}});
            }
            const auto preserve_names = rgb_levels_preserve_names();
            if (std::find(preserve_names.begin(), preserve_names.end(),
                          params.rgb_levels.preserve_colors) == preserve_names.end())
            {
                return make_error(ErrorCode::kValidation,
                                  "RGB levels preserve-colors is unsupported",
                                  {{"preserve_colors", params.rgb_levels.preserve_colors}});
            }
            params.rgb_levels.levels[0][0] = number("black", params.rgb_levels.levels[0][0]);
            params.rgb_levels.levels[0][1] = number("grey", params.rgb_levels.levels[0][1]);
            params.rgb_levels.levels[0][2] = number("white", params.rgb_levels.levels[0][2]);
            params.rgb_levels.levels[1][0] = number("black_g", params.rgb_levels.levels[1][0]);
            params.rgb_levels.levels[1][1] = number("grey_g", params.rgb_levels.levels[1][1]);
            params.rgb_levels.levels[1][2] = number("white_g", params.rgb_levels.levels[1][2]);
            params.rgb_levels.levels[2][0] = number("black_b", params.rgb_levels.levels[2][0]);
            params.rgb_levels.levels[2][1] = number("grey_b", params.rgb_levels.levels[2][1]);
            params.rgb_levels.levels[2][2] = number("white_b", params.rgb_levels.levels[2][2]);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.color.rgbcurve")
        {
            const auto take_text = [&](const char *name, std::string &target)
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    if (const auto *text = as_string_if(found->second); text != nullptr)
                    {
                        target = *text;
                    }
                }
            };
            take_text("mode", params.rgb_curve.mode);
            take_text("preserve_colors", params.rgb_curve.preserve_colors);
            take_text("interpolation", params.rgb_curve.interpolation);
            if (const auto found = operation.parameters.find("compensate_middle_grey");
                found != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
                {
                    params.rgb_curve.compensate_middle_grey = *flag;
                }
                else
                {
                    params.rgb_curve.compensate_middle_grey =
                        number("compensate_middle_grey", 0.0) != 0.0;
                }
            }
            if (const auto found = operation.parameters.find("application_space");
                found != operation.parameters.end())
            {
                if (const auto *text = std::get_if<std::string>(&found->second.value);
                    text != nullptr)
                {
                    params.rgb_curve.application_space = *text;
                }
            }
            const auto take_points = [&](const char *name,
                                         std::vector<ToneCurvePoint> &target) -> Result<void>
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    auto points = parse_rgb_curve_points(found->second);
                    if (!points)
                    {
                        return points.error();
                    }
                    target = std::move(points).value();
                }
                return {};
            };
            if (auto red = take_points("points", params.rgb_curve.channels[0]); !red)
            {
                return red.error();
            }
            if (auto green = take_points("points_g", params.rgb_curve.channels[1]); !green)
            {
                return green.error();
            }
            if (auto blue = take_points("points_b", params.rgb_curve.channels[2]); !blue)
            {
                return blue.error();
            }
            params.rgb_curve.parametric_shadows =
                number("parametric_shadows", params.rgb_curve.parametric_shadows);
            params.rgb_curve.parametric_darks =
                number("parametric_darks", params.rgb_curve.parametric_darks);
            params.rgb_curve.parametric_lights =
                number("parametric_lights", params.rgb_curve.parametric_lights);
            params.rgb_curve.parametric_highlights =
                number("parametric_highlights", params.rgb_curve.parametric_highlights);
            params.rgb_curve.parametric_split_shadows =
                number("parametric_split_shadows", params.rgb_curve.parametric_split_shadows);
            params.rgb_curve.parametric_split_mid =
                number("parametric_split_mid", params.rgb_curve.parametric_split_mid);
            params.rgb_curve.parametric_split_highlights =
                number("parametric_split_highlights", params.rgb_curve.parametric_split_highlights);
            params.rgb_curve_mask_id = operation.mask_id;
            note_section("curves", operation.enabled);
        }
        else if (operation.id == "ravo.core.tonecurve")
        {
            if (const auto found = operation.parameters.find("working_space");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.tone_curve_working_space = *text;
                }
            }
            if (const auto found = operation.parameters.find("interpolation");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.tone_curve_interpolation = *text;
                }
            }
            if (const auto found = operation.parameters.find("channel_mode");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.tone_curve_channel_mode = *text;
                }
            }
            if (const auto found = operation.parameters.find("preserve_colors");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.tone_curve_preserve_colors = *text;
                }
            }
            const auto take_tone_points = [&](const char *name,
                                              std::vector<ToneCurvePoint> &target) -> Result<void>
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    auto points = parse_tone_curve_points(found->second);
                    if (!points)
                    {
                        return points.error();
                    }
                    target = std::move(points).value();
                }
                return {};
            };
            if (auto points = take_tone_points("points", params.tone_curve); !points)
            {
                return points.error();
            }
            if (auto points = take_tone_points("points_a", params.tone_curve_a); !points)
            {
                return points.error();
            }
            if (auto points = take_tone_points("points_b", params.tone_curve_b); !points)
            {
                return points.error();
            }
            params.tone_curve_mask_id = operation.mask_id;
            note_section("curves", operation.enabled);
        }
        else if (operation.id == "ravo.color.vibrance")
        {
            params.vibrance = number("amount", params.vibrance);
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.saturation")
        {
            params.saturation = number("amount", params.saturation);
            note_section("color", operation.enabled);
        }
        else if (operation.id == kVelviaOperationId)
        {
            if (params.velvia_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Velvia operations",
                                  {{"reason", "duplicate_velvia"}});
            OperationInstance canonical = operation;
            auto upgraded = upgrade_velvia_operation(canonical);
            if (!upgraded)
                return upgraded.error();
            auto velvia = velvia_from_parameters(canonical.parameters);
            if (!velvia)
                return velvia.error();
            params.velvia_present = true;
            params.velvia_enabled = operation.enabled;
            params.velvia = velvia.value();
            params.velvia_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kLut3dOperationId)
        {
            if (params.lut3d_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate 3D LUT operations",
                                  {{"reason", "duplicate_lut3d"}});
            auto lut = lut3d_from_parameters(operation.parameters);
            if (!lut)
                return lut.error();
            params.lut3d_present = true;
            params.lut3d_enabled = operation.enabled;
            params.lut3d = std::move(lut).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.colorbalancergb")
        {
            auto color_balance = color_balance_rgb_from_parameters(operation.parameters);
            if (!color_balance)
            {
                return color_balance.error();
            }
            params.color_balance_rgb = std::move(color_balance).value();
            params.color_balance_rgb_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorCorrectionOperationId)
        {
            auto color_correction = color_correction_from_parameters(operation.parameters);
            if (!color_correction)
            {
                return color_correction.error();
            }
            params.color_correction_enabled = true;
            params.color_correction = std::move(color_correction).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorBalanceOperationId)
        {
            auto color_balance = color_balance_from_parameters(operation.parameters);
            if (!color_balance)
            {
                return color_balance.error();
            }
            params.color_balance = std::move(color_balance).value();
            params.color_balance_enabled = true;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorContrastOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_color_contrast_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            if (!canonical.enabled)
            {
                note_section("color", false);
                continue;
            }
            if (canonical.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Color Contrast masks are unsupported",
                    {{"operation_id", canonical.id}, {"reason", "unsupported_colorcontrast_mask"}});
            }
            auto color_contrast = color_contrast_from_parameters(canonical.parameters);
            if (!color_contrast)
            {
                return color_contrast.error();
            }
            params.color_contrast_enabled = true;
            params.color_contrast = std::move(color_contrast).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kColorReconstructionOperationId)
        {
            if (operation.mask_id.has_value())
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Color Reconstruction masks are unsupported",
                                  {{"operation_id", operation.id},
                                   {"reason", "unsupported_colorreconstruct_mask"}});
            }
            auto color_reconstruction = color_reconstruction_from_parameters(operation.parameters);
            if (!color_reconstruction)
            {
                return color_reconstruction.error();
            }
            params.color_reconstruction_enabled = true;
            params.color_reconstruction = std::move(color_reconstruction).value();
            note_section("color", operation.enabled);
        }
        else if (operation.id == kMonochromeOperationId)
        {
            if (params.monochrome_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Monochrome operations",
                                  {{"reason", "duplicate_monochrome"}});
            OperationInstance canonical = operation;
            auto upgraded = upgrade_monochrome_operation(canonical);
            if (!upgraded)
                return upgraded.error();
            auto monochrome = monochrome_from_parameters(canonical.parameters);
            if (!monochrome)
                return monochrome.error();
            params.monochrome_present = true;
            params.monochrome_enabled = operation.enabled;
            params.monochrome = monochrome.value();
            params.monochrome_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kSplitToningOperationId)
        {
            if (params.split_toning_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Split Toning operations",
                                  {{"reason", "duplicate_split_toning"}});
            OperationInstance canonical = operation;
            auto upgraded = upgrade_split_toning_operation(canonical);
            if (!upgraded)
                return upgraded.error();
            auto split = split_toning_from_parameters(canonical.parameters);
            if (!split)
                return split.error();
            params.split_toning_present = true;
            params.split_toning_enabled = operation.enabled;
            params.split_toning = split.value();
            params.split_toning_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == kSharpenOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_sharpen_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            if (canonical.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Sharpen masks are unsupported",
                    {{"operation_id", canonical.id}, {"reason", "unsupported_sharpen_mask"}});
            }
            auto sharpen = sharpen_from_parameters(canonical.parameters);
            if (!sharpen)
            {
                return sharpen.error();
            }
            params.sharpen = sharpen.value().amount;
            params.sharpen_radius = sharpen.value().radius;
            params.sharpen_threshold = sharpen.value().threshold;
            note_section("detail", operation.enabled);
        }
        else if (operation.id == kTextureOperationId)
        {
            if (operation.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Texture masks are unsupported",
                    {{"operation_id", operation.id}, {"reason", "unsupported_texture_mask"}});
            }
            auto texture = texture_from_parameters(operation.parameters);
            if (!texture)
            {
                return texture.error();
            }
            params.texture = texture.value();
            note_section("detail", operation.enabled);
        }
        else if (operation.id == kRetouchOperationId)
        {
            auto retouch = retouch_from_parameters(operation.parameters);
            if (!retouch)
            {
                return retouch.error();
            }
            params.retouch = std::move(retouch).value();
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.detail.clarity")
        {
            params.clarity = number("amount", params.clarity);
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.effect.vignette")
        {
            params.vignette = number("amount", params.vignette);
            params.vignette_midpoint = number("midpoint", params.vignette_midpoint);
            params.vignette_falloff = number("falloff", params.vignette_falloff);
            params.vignette_shape = number("shape", params.vignette_shape);
            params.vignette_center_x = number("center_x", params.vignette_center_x);
            params.vignette_center_y = number("center_y", params.vignette_center_y);
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.effect.grain")
        {
            params.grain = number("amount", params.grain);
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.effect.bloom")
        {
            params.bloom = number("amount", params.bloom);
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.effect.soften")
        {
            params.soften = number("amount", params.soften);
            note_section("effects", operation.enabled);
        }
        else if (operation.id == kDehazeOperationId)
        {
            OperationInstance canonical = operation;
            auto upgraded = upgrade_dehaze_operation(canonical);
            if (!upgraded)
            {
                return upgraded.error();
            }
            if (canonical.mask_id.has_value())
            {
                return make_error(
                    ErrorCode::kUnsupported, "Develop Dehaze masks are unsupported",
                    {{"operation_id", canonical.id}, {"reason", "unsupported_dehaze_mask"}});
            }
            auto dehaze = dehaze_from_parameters(canonical.parameters);
            if (!dehaze)
            {
                return dehaze.error();
            }
            params.dehaze = dehaze.value().strength;
            params.dehaze_distance = dehaze.value().distance;
            params.dehaze_adaptive = dehaze.value().adaptive;
            note_section("effects", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.rotate")
        {
            params.rotate_quarters = integer("quarters", 0) % 4;
            if (params.rotate_quarters < 0)
            {
                params.rotate_quarters += 4;
            }
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.flip")
        {
            params.flip_horizontal = flag01(integer("horizontal", 0));
            params.flip_vertical = flag01(integer("vertical", 0));
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.straighten")
        {
            if (perspective_geometry_present)
            {
                return make_error(ErrorCode::kConflict,
                                  "Develop does not allow duplicate perspective geometry",
                                  {{"reason", "duplicate_perspective_operation"}});
            }
            if (operation.mask_id.has_value())
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Perspective masks are unsupported",
                                  {{"reason", "unsupported_perspective_mask"}});
            }
            params.straighten_degrees = number("degrees", params.straighten_degrees);
            perspective_geometry_present = true;
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == kPerspectiveOperationId)
        {
            if (perspective_geometry_present)
            {
                return make_error(ErrorCode::kConflict,
                                  "Develop does not allow duplicate perspective geometry",
                                  {{"reason", "duplicate_perspective_operation"}});
            }
            if (operation.schema_version != kPerspectiveOperationSchemaVersion)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Perspective schema version is unsupported",
                                  {{"schema_version", std::to_string(operation.schema_version)},
                                   {"reason", "unsupported_perspective_schema"}});
            }
            if (operation.mask_id.has_value())
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Develop Perspective masks are unsupported",
                                  {{"reason", "unsupported_perspective_mask"}});
            }
            auto perspective = perspective_from_parameters(operation.parameters);
            if (!perspective)
                return perspective.error();
            params.straighten_degrees = perspective.value().rotation_degrees;
            params.perspective_vertical = perspective.value().vertical_shift;
            params.perspective_horizontal = perspective.value().horizontal_shift;
            params.perspective_shear = perspective.value().shear;
            params.perspective_constrain_crop = perspective.value().constrain_crop;
            params.perspective_interpolation_index =
                perspective.value().interpolation == kPerspectiveInterpolationBilinear ? 0 :
                perspective.value().interpolation == kPerspectiveInterpolationLanczos2 ? 1 :
                                                                                         2;
            perspective_geometry_present = true;
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.crop")
        {
            params.crop_x = number("x", params.crop_x);
            params.crop_y = number("y", params.crop_y);
            params.crop_width = number("width", params.crop_width);
            params.crop_height = number("height", params.crop_height);
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == "ravo.display.sigmoid")
        {
            auto validated = validate_sigmoid_parameters(operation.parameters);
            if (!validated)
            {
                return validated.error();
            }
            params.sigmoid_enabled = true;
            params.sigmoid_contrast = number("middle_grey_contrast", params.sigmoid_contrast);
            params.sigmoid_skew = number("contrast_skewness", params.sigmoid_skew);
            params.sigmoid_display_white =
                number("display_white_target", params.sigmoid_display_white);
            params.sigmoid_display_black =
                number("display_black_target", params.sigmoid_display_black);
            params.sigmoid_hue_preservation =
                number("hue_preservation", params.sigmoid_hue_preservation);
            note_section("light", operation.enabled);
        }
        else if (operation.id == "ravo.raw.highlights")
        {
            params.raw_highlights = number("amount", params.raw_highlights);
            params.raw_highlights_clip = number("clip", params.raw_highlights_clip);
            if (const auto found = operation.parameters.find("mode");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.raw_highlights_mode = *text;
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.raw.hotpixels")
        {
            params.hot_pixels_strength = number("strength", 0.25);
            params.hot_pixels_threshold = number("threshold", 0.05);
            if (const auto found = operation.parameters.find("permissive");
                found != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
                {
                    params.hot_pixels_permissive = *flag;
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.raw.denoise")
        {
            params.raw_denoise_threshold = number("threshold", params.raw_denoise_threshold);
            const char *names[4] = {"all", "red", "green", "blue"};
            for (int channel = 0; channel < 4; ++channel)
            {
                for (int band = 0; band < 5; ++band)
                {
                    const std::string key =
                        std::string("y_") + names[channel] + std::to_string(band);
                    params.raw_denoise_bands[static_cast<std::size_t>(channel)]
                                            [static_cast<std::size_t>(band)] =
                        number(key, params.raw_denoise_bands[static_cast<std::size_t>(channel)]
                                                            [static_cast<std::size_t>(band)]);
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.raw.cacorrect")
        {
            params.raw_ca_iterations = integer("iterations", 2);
            if (const auto found = operation.parameters.find("avoid_color_shift");
                found != operation.parameters.end())
            {
                if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
                {
                    params.raw_ca_avoid_shift = *flag;
                }
            }
            note_section("raw", operation.enabled);
        }
        else if (operation.id == "ravo.detail.denoiseprofile")
        {
            params.denoise = number("strength", params.denoise);
            params.denoise_chroma = number("chroma", params.denoise_chroma);
            params.denoise_radius = number("radius", params.denoise_radius);
            note_section("detail", operation.enabled);
        }
        else if (operation.id == "ravo.geometry.lens")
        {
            params.lens_k1 = number("k1", params.lens_k1);
            params.lens_k2 = number("k2", params.lens_k2);
            params.lens_tca_r = number("tca_r", params.lens_tca_r);
            params.lens_tca_b = number("tca_b", params.lens_tca_b);
            params.lens_vignetting = number("vignetting", params.lens_vignetting);
            params.lens_focal_mm = number("focal_mm", params.lens_focal_mm);
            if (const auto found = operation.parameters.find("mode");
                found != operation.parameters.end())
            {
                if (const auto *text = as_string_if(found->second); text != nullptr)
                {
                    params.lens_mode = *text;
                }
            }
            const auto take_text = [&](const char *name, std::string &target)
            {
                if (const auto found = operation.parameters.find(name);
                    found != operation.parameters.end())
                {
                    if (const auto *text = as_string_if(found->second); text != nullptr)
                    {
                        target = *text;
                    }
                }
            };
            take_text("camera_make", params.lens_make);
            take_text("camera_model", params.lens_model);
            take_text("lens", params.lens_name);
            note_section("raw", operation.enabled);
        }
        else if (operation.id == kCanvasOperationId)
        {
            if (params.canvas_present)
                return make_error(ErrorCode::kValidation, "Develop contains duplicate Canvases",
                                  {{"reason", "duplicate_canvas"}});
            if (operation.mask_id.has_value())
                return make_error(ErrorCode::kUnsupported, "Develop Canvas masks are unsupported",
                                  {{"reason", "unsupported_canvas_mask"}});
            auto canvas = canvas_from_parameters(operation.parameters);
            if (!canvas)
                return canvas.error();
            params.canvas_present = true;
            params.canvas_enabled = operation.enabled;
            params.canvas = canvas.value();
            note_section("geometry", operation.enabled);
        }
        else if (operation.id == kColorZonesOperationId)
        {
            if (params.color_zones_present)
                return make_error(ErrorCode::kValidation,
                                  "Develop contains duplicate Color Zones operations",
                                  {{"reason", "duplicate_color_zones"}});
            auto zones = color_zones_from_parameters(operation.parameters);
            if (!zones)
                return zones.error();
            params.color_zones_present = true;
            params.color_zones_enabled = operation.enabled;
            params.color_zones = std::move(zones).value();
            params.color_zones_mask_id = operation.mask_id;
            note_section("color", operation.enabled);
        }
        else if (operation.id == "ravo.color.colorequal")
        {
            if (const auto found = operation.parameters.find("hue_shift");
                found != operation.parameters.end())
            {
                auto parsed = parse_band_array(found->second, "hue_shift");
                if (!parsed)
                {
                    return parsed.error();
                }
                params.color_eq_hue = parsed.value();
            }
            if (const auto found = operation.parameters.find("saturation");
                found != operation.parameters.end())
            {
                auto parsed = parse_band_array(found->second, "saturation");
                if (!parsed)
                {
                    return parsed.error();
                }
                params.color_eq_sat = parsed.value();
            }
            if (const auto found = operation.parameters.find("lightness");
                found != operation.parameters.end())
            {
                auto parsed = parse_band_array(found->second, "lightness");
                if (!parsed)
                {
                    return parsed.error();
                }
                params.color_eq_light = parsed.value();
            }
            note_section("colorEqualizer", operation.enabled);
        }
        else if (operation.id == "ravo.effect.graduatednd")
        {
            params.graduated_present = true;
            params.graduated_enabled = operation.enabled;
            params.graduated_density = number("density_ev", params.graduated_density);
            params.graduated_hardness = number("hardness", params.graduated_hardness);
            params.graduated_rotation = number("rotation_deg", params.graduated_rotation);
            params.graduated_offset = number("offset", params.graduated_offset);
            params.graduated_mask_id = operation.mask_id;
            note_section("graduated", operation.enabled);
        }
        else if (operation.id == "ravo.core.toneequal")
        {
            params.tone_eq_blacks = number("blacks", params.tone_eq_blacks);
            params.tone_eq_shadows = number("shadows", params.tone_eq_shadows);
            params.tone_eq_midtones = number("midtones", params.tone_eq_midtones);
            params.tone_eq_highlights = number("highlights", params.tone_eq_highlights);
            params.tone_eq_whites = number("whites", params.tone_eq_whites);
            note_section("toneEqual", operation.enabled);
        }
    }
    for (const auto &[section, flags] : section_flags)
    {
        if (flags.first && !flags.second)
        {
            static_cast<void>(set_develop_section_effect_enabled(params, section, false));
        }
    }
    clamp_develop(params);
    return params;
}

} // namespace ravo
