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

namespace ravo::develop_internal
{

[[nodiscard]] std::optional<std::size_t>
selected_color_checker_patch(const DevelopParams &params) noexcept
{
    if (params.color_checker_patch < 0 ||
        params.color_checker_patch >=
            static_cast<std::int64_t>(params.color_checker.patches.size()))
    {
        return std::nullopt;
    }
    return static_cast<std::size_t>(params.color_checker_patch);
}

[[nodiscard]] bool apply_color_checker_field(DevelopParams &params, const std::string_view name,
                                             const double value)
{
    if (!std::isfinite(value))
    {
        return false;
    }
    if (name == "colorCheckerEnabled")
    {
        params.color_checker_enabled = value >= 0.5;
        return true;
    }
    if (name == "colorCheckerPreset")
    {
        const auto index = static_cast<std::int64_t>(std::llround(value));
        const auto presets = color_checker_presets();
        if (value != static_cast<double>(index) || index < 0 ||
            index >= static_cast<std::int64_t>(presets.size()))
        {
            return false;
        }
        auto preset = color_checker_params_for_preset(presets[static_cast<std::size_t>(index)].id);
        if (!preset)
        {
            return false;
        }
        params.color_checker = std::move(preset).value();
        params.color_checker_enabled = true;
        params.color_checker_patch = 0;
        return true;
    }
    if (name == "colorCheckerPatch")
    {
        const auto index = static_cast<std::int64_t>(std::llround(value));
        if (value != static_cast<double>(index) || index < 0 ||
            index >= static_cast<std::int64_t>(params.color_checker.patches.size()))
        {
            return false;
        }
        params.color_checker_patch = index;
        return true;
    }
    const auto patch = selected_color_checker_patch(params);
    if (!patch || !std::isfinite(static_cast<float>(value)))
    {
        return false;
    }
    auto &selected = params.color_checker.patches[*patch];
    double *component = nullptr;
    if (name == "colorCheckerSourceL")
    {
        component = &selected.source_lab[0];
    }
    else if (name == "colorCheckerSourceA")
    {
        component = &selected.source_lab[1];
    }
    else if (name == "colorCheckerSourceB")
    {
        component = &selected.source_lab[2];
    }
    else if (name == "colorCheckerTargetL")
    {
        component = &selected.target_lab[0];
    }
    else if (name == "colorCheckerTargetA")
    {
        component = &selected.target_lab[1];
    }
    else if (name == "colorCheckerTargetB")
    {
        component = &selected.target_lab[2];
    }
    else
    {
        return false;
    }
    *component = value;
    params.color_checker_enabled = true;
    return true;
}

[[nodiscard]] bool reset_color_checker_field(DevelopParams &params, const std::string_view name)
{
    if (name == "colorChecker")
    {
        params.color_checker_enabled = false;
        params.color_checker = ColorCheckerParams{};
        params.color_checker_patch = 0;
        return true;
    }
    if (name == "colorCheckerEnabled")
    {
        params.color_checker_enabled = false;
        return true;
    }
    if (name == "colorCheckerPatch")
    {
        params.color_checker_patch = 0;
        return true;
    }
    const auto patch = selected_color_checker_patch(params);
    if (!patch)
    {
        return false;
    }
    auto &selected = params.color_checker.patches[*patch];
    if (name == "colorCheckerSourceL")
    {
        selected.source_lab[0] = selected.target_lab[0];
    }
    else if (name == "colorCheckerSourceA")
    {
        selected.source_lab[1] = selected.target_lab[1];
    }
    else if (name == "colorCheckerSourceB")
    {
        selected.source_lab[2] = selected.target_lab[2];
    }
    else if (name == "colorCheckerTargetL")
    {
        selected.target_lab[0] = selected.source_lab[0];
    }
    else if (name == "colorCheckerTargetA")
    {
        selected.target_lab[1] = selected.source_lab[1];
    }
    else if (name == "colorCheckerTargetB")
    {
        selected.target_lab[2] = selected.source_lab[2];
    }
    else
    {
        return false;
    }
    return true;
}

struct ColorCorrectionNumericField
{
    std::string_view develop_name;
    double ColorCorrectionParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorCorrectionNumericField, 5> &
color_correction_numeric_fields() noexcept
{
    static const std::array<ColorCorrectionNumericField, 5> fields{{
        {"colorCorrectionHighlightA", &ColorCorrectionParams::highlight_a,
         kColorCorrectionEndpointMin, kColorCorrectionEndpointMax},
        {"colorCorrectionHighlightB", &ColorCorrectionParams::highlight_b,
         kColorCorrectionEndpointMin, kColorCorrectionEndpointMax},
        {"colorCorrectionShadowA", &ColorCorrectionParams::shadow_a, kColorCorrectionEndpointMin,
         kColorCorrectionEndpointMax},
        {"colorCorrectionShadowB", &ColorCorrectionParams::shadow_b, kColorCorrectionEndpointMin,
         kColorCorrectionEndpointMax},
        {"colorCorrectionSaturation", &ColorCorrectionParams::saturation,
         kColorCorrectionSaturationMin, kColorCorrectionSaturationMax},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_correction_field(DevelopParams &params, const std::string_view name,
                                                const double value) noexcept
{
    if (name == "colorCorrectionEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_correction_enabled = value == 1.0;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : color_correction_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_correction.*(field.member) = value;
            params.color_correction_enabled = true;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset_color_correction_field(DevelopParams &params,
                                                const std::string_view name) noexcept
{
    const ColorCorrectionParams defaults;
    if (name == "colorCorrection")
    {
        params.color_correction_enabled = false;
        params.color_correction = defaults;
        return true;
    }
    if (name == "colorCorrectionEnabled")
    {
        params.color_correction_enabled = false;
        return true;
    }
    for (const auto &field : color_correction_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_correction.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_correction(ColorCorrectionParams &params) noexcept
{
    const ColorCorrectionParams defaults;
    for (const auto &field : color_correction_numeric_fields())
    {
        double &value = params.*(field.member);
        value = std::isfinite(value) ? clamp_value(value, field.minimum, field.maximum) :
                                       defaults.*(field.member);
    }
}

struct ColorContrastNumericField
{
    std::string_view develop_name;
    double ColorContrastParams::*member;
    bool is_steepness;
};

[[nodiscard]] const std::array<ColorContrastNumericField, 4> &
color_contrast_numeric_fields() noexcept
{
    static const std::array<ColorContrastNumericField, 4> fields{{
        {"colorContrastASteepness", &ColorContrastParams::a_steepness, true},
        {"colorContrastAOffset", &ColorContrastParams::a_offset, false},
        {"colorContrastBSteepness", &ColorContrastParams::b_steepness, true},
        {"colorContrastBOffset", &ColorContrastParams::b_offset, false},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_contrast_field(DevelopParams &params, const std::string_view name,
                                              const double value) noexcept
{
    if (name == "colorContrastEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_contrast_enabled = value == 1.0;
        return true;
    }
    if (name == "colorContrastUnbound")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_contrast.unbound = value == 1.0;
        params.color_contrast_enabled = true;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : color_contrast_numeric_fields())
    {
        if (name != field.develop_name)
        {
            continue;
        }
        if (!field.is_steepness && !std::isfinite(static_cast<float>(value)))
        {
            return false;
        }
        params.color_contrast.*(field.member) = value;
        params.color_contrast_enabled = true;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_color_contrast_field(DevelopParams &params,
                                              const std::string_view name) noexcept
{
    const ColorContrastParams defaults;
    if (name == "colorContrast")
    {
        params.color_contrast_enabled = false;
        params.color_contrast = defaults;
        return true;
    }
    if (name == "colorContrastEnabled")
    {
        params.color_contrast_enabled = false;
        return true;
    }
    if (name == "colorContrastUnbound")
    {
        params.color_contrast.unbound = defaults.unbound;
        return true;
    }
    for (const auto &field : color_contrast_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_contrast.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_contrast(ColorContrastParams &params) noexcept
{
    const ColorContrastParams defaults;
    for (const auto &field : color_contrast_numeric_fields())
    {
        double &value = params.*(field.member);
        if (!std::isfinite(value))
        {
            value = defaults.*(field.member);
        }
        else if (field.is_steepness)
        {
            value = clamp_value(value, kColorContrastSteepnessMin, kColorContrastSteepnessMax);
        }
        else if (!std::isfinite(static_cast<float>(value)))
        {
            value = defaults.*(field.member);
        }
    }
}

struct ColorReconstructionNumericField
{
    std::string_view develop_name;
    double ColorReconstructionParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorReconstructionNumericField, 3> &
color_reconstruction_numeric_fields() noexcept
{
    static const std::array<ColorReconstructionNumericField, 3> fields{{
        {"colorReconstructionThreshold", &ColorReconstructionParams::threshold,
         kColorReconstructionThresholdMin, kColorReconstructionThresholdMax},
        {"colorReconstructionSpatial", &ColorReconstructionParams::spatial,
         kColorReconstructionSpatialMin, kColorReconstructionSpatialMax},
        {"colorReconstructionRange", &ColorReconstructionParams::range,
         kColorReconstructionRangeMin, kColorReconstructionRangeMax},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_reconstruction_field(DevelopParams &params,
                                                    const std::string_view name,
                                                    const double value) noexcept
{
    if (name == "colorReconstructionEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_reconstruction_enabled = value == 1.0;
        return true;
    }
    if (name == "colorReconstructionPrecedenceIndex")
    {
        if (!std::isfinite(value) || std::trunc(value) != value || value < 0.0 || value > 2.0)
        {
            return false;
        }
        params.color_reconstruction.precedence =
            static_cast<ColorReconstructionPrecedence>(static_cast<std::uint8_t>(value));
        params.color_reconstruction_enabled = true;
        return true;
    }
    if (name == "colorReconstructionHueDegrees")
    {
        if (!std::isfinite(value) || value < 0.0 || value > 360.0)
        {
            return false;
        }
        params.color_reconstruction.hue = value / 360.0;
        params.color_reconstruction_enabled = true;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        if (name != field.develop_name)
        {
            continue;
        }
        params.color_reconstruction.*(field.member) = value;
        params.color_reconstruction_enabled = true;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_color_reconstruction_field(DevelopParams &params,
                                                    const std::string_view name) noexcept
{
    const ColorReconstructionParams defaults;
    if (name == "colorReconstruction")
    {
        params.color_reconstruction_enabled = false;
        params.color_reconstruction = defaults;
        return true;
    }
    if (name == "colorReconstructionEnabled")
    {
        params.color_reconstruction_enabled = false;
        return true;
    }
    if (name == "colorReconstructionPrecedenceIndex")
    {
        params.color_reconstruction.precedence = defaults.precedence;
        return true;
    }
    if (name == "colorReconstructionHueDegrees")
    {
        params.color_reconstruction.hue = defaults.hue;
        return true;
    }
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        if (name == field.develop_name)
        {
            params.color_reconstruction.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    return false;
}

void clamp_color_reconstruction(ColorReconstructionParams &params) noexcept
{
    const ColorReconstructionParams defaults;
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        double &value = params.*(field.member);
        value = std::isfinite(value) ? clamp_value(value, field.minimum, field.maximum) :
                                       defaults.*(field.member);
    }
    params.hue = std::isfinite(params.hue) ? clamp_value(params.hue, kColorReconstructionHueMin,
                                                         kColorReconstructionHueMax) :
                                             defaults.hue;
    switch (params.precedence)
    {
    case ColorReconstructionPrecedence::kNone:
    case ColorReconstructionPrecedence::kChroma:
    case ColorReconstructionPrecedence::kHue:
        break;
    default:
        params.precedence = defaults.precedence;
        break;
    }
}

struct RgbLevelsStopField
{
    std::string_view develop_name;
    std::size_t channel;
    std::size_t stop;
};

[[nodiscard]] const std::array<RgbLevelsStopField, 9> &rgb_levels_stop_fields() noexcept
{
    static const std::array<RgbLevelsStopField, 9> fields{{
        {"rgbLevelsBlack", 0, 0},
        {"rgbLevelsGrey", 0, 1},
        {"rgbLevelsWhite", 0, 2},
        {"rgbLevelsBlackG", 1, 0},
        {"rgbLevelsGreyG", 1, 1},
        {"rgbLevelsWhiteG", 1, 2},
        {"rgbLevelsBlackB", 2, 0},
        {"rgbLevelsGreyB", 2, 1},
        {"rgbLevelsWhiteB", 2, 2},
    }};
    return fields;
}

[[nodiscard]] bool exact_develop_integer(const double value, const std::int64_t minimum,
                                         const std::int64_t maximum, std::int64_t &out) noexcept;

void clamp_rgb_levels(RgbLevelsParams &params) noexcept
{
    if (params.mode != kRgbLevelsModeIndependent)
    {
        params.mode = std::string(kRgbLevelsModeLinked);
    }
    bool preserve_known = false;
    for (const auto name : rgb_levels_preserve_names())
    {
        if (params.preserve_colors == name)
        {
            preserve_known = true;
            break;
        }
    }
    if (!preserve_known)
    {
        params.preserve_colors = std::string(kToneCurvePreserveColorsLuminance);
    }
    for (auto &channel : params.levels)
    {
        for (auto &stop : channel)
        {
            if (!std::isfinite(stop))
            {
                stop = 0.0;
            }
            stop = clamp_value(stop, 0.0, 1.0);
        }
        if (!(channel[2] > channel[0]))
        {
            channel[2] = std::min(1.0, channel[0] + 1.0e-3);
        }
    }
}

[[nodiscard]] bool apply_rgb_levels_field(DevelopParams &params, const std::string_view name,
                                          const double value) noexcept
{
    if (name == "rgbLevelsMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.rgb_levels.mode = value == 1.0 ? std::string(kRgbLevelsModeIndependent) :
                                                std::string(kRgbLevelsModeLinked);
        return true;
    }
    if (name == "rgbLevelsPreserve")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 6, index))
        {
            return false;
        }
        params.rgb_levels.preserve_colors =
            std::string(rgb_levels_preserve_names()[static_cast<std::size_t>(index)]);
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    for (const auto &field : rgb_levels_stop_fields())
    {
        if (name != field.develop_name)
        {
            continue;
        }
        params.rgb_levels.levels[field.channel][field.stop] = value;
        return true;
    }
    return false;
}

[[nodiscard]] bool apply_rgb_curve_field(DevelopParams &params, const std::string_view name,
                                         const double value) noexcept
{
    if (name == "rgbCurveMode")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.rgb_curve.mode = value == 1.0 ? std::string(kRgbLevelsModeIndependent) :
                                               std::string(kRgbLevelsModeLinked);
        return true;
    }
    if (name == "rgbCurvePreserve")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 6, index))
        {
            return false;
        }
        params.rgb_curve.preserve_colors =
            std::string(rgb_levels_preserve_names()[static_cast<std::size_t>(index)]);
        return true;
    }
    if (name == "rgbCurveInterpolation")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(value, 0, 2, index))
        {
            return false;
        }
        params.rgb_curve.interpolation =
            std::string(curve_interpolation_from_index(static_cast<int>(index)));
        return true;
    }
    if (name == "rgbCurveCompensate")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.rgb_curve.compensate_middle_grey = value == 1.0;
        return true;
    }
    if (!std::isfinite(value))
    {
        return false;
    }
    if (name == "rgbCurveShadows")
    {
        params.rgb_curve.parametric_shadows = value;
        return true;
    }
    if (name == "rgbCurveDarks")
    {
        params.rgb_curve.parametric_darks = value;
        return true;
    }
    if (name == "rgbCurveLights")
    {
        params.rgb_curve.parametric_lights = value;
        return true;
    }
    if (name == "rgbCurveHighlights")
    {
        params.rgb_curve.parametric_highlights = value;
        return true;
    }
    if (name == "rgbCurveSplit0")
    {
        params.rgb_curve.parametric_split_shadows = value;
        return true;
    }
    if (name == "rgbCurveSplit1")
    {
        params.rgb_curve.parametric_split_mid = value;
        return true;
    }
    if (name == "rgbCurveSplit2")
    {
        params.rgb_curve.parametric_split_highlights = value;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_rgb_curve_field(DevelopParams &params,
                                         const std::string_view name) noexcept
{
    const RgbCurveParams defaults;
    if (name == "rgbCurve")
    {
        params.rgb_curve = defaults;
        return true;
    }
    if (name == "rgbCurveMode")
    {
        params.rgb_curve.mode = defaults.mode;
        return true;
    }
    if (name == "rgbCurvePreserve")
    {
        params.rgb_curve.preserve_colors = defaults.preserve_colors;
        return true;
    }
    if (name == "rgbCurveInterpolation")
    {
        params.rgb_curve.interpolation = defaults.interpolation;
        return true;
    }
    if (name == "rgbCurveCompensate")
    {
        params.rgb_curve.compensate_middle_grey = defaults.compensate_middle_grey;
        return true;
    }
    if (name == "rgbCurveShadows")
    {
        params.rgb_curve.parametric_shadows = defaults.parametric_shadows;
        return true;
    }
    if (name == "rgbCurveDarks")
    {
        params.rgb_curve.parametric_darks = defaults.parametric_darks;
        return true;
    }
    if (name == "rgbCurveLights")
    {
        params.rgb_curve.parametric_lights = defaults.parametric_lights;
        return true;
    }
    if (name == "rgbCurveHighlights")
    {
        params.rgb_curve.parametric_highlights = defaults.parametric_highlights;
        return true;
    }
    if (name == "rgbCurveSplit0")
    {
        params.rgb_curve.parametric_split_shadows = defaults.parametric_split_shadows;
        return true;
    }
    if (name == "rgbCurveSplit1")
    {
        params.rgb_curve.parametric_split_mid = defaults.parametric_split_mid;
        return true;
    }
    if (name == "rgbCurveSplit2")
    {
        params.rgb_curve.parametric_split_highlights = defaults.parametric_split_highlights;
        return true;
    }
    return false;
}

[[nodiscard]] bool reset_rgb_levels_field(DevelopParams &params,
                                          const std::string_view name) noexcept
{
    const RgbLevelsParams defaults;
    if (name == "rgbLevels")
    {
        params.rgb_levels = defaults;
        return true;
    }
    if (name == "rgbLevelsMode")
    {
        params.rgb_levels.mode = defaults.mode;
        return true;
    }
    if (name == "rgbLevelsPreserve")
    {
        params.rgb_levels.preserve_colors = defaults.preserve_colors;
        return true;
    }
    for (const auto &field : rgb_levels_stop_fields())
    {
        if (name == field.develop_name)
        {
            params.rgb_levels.levels[field.channel][field.stop] =
                defaults.levels[field.channel][field.stop];
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool exact_develop_integer(const double value, const std::int64_t minimum,
                                         const std::int64_t maximum, std::int64_t &out) noexcept
{
    if (!std::isfinite(value) || value < static_cast<double>(minimum) ||
        value > static_cast<double>(maximum) || value != std::trunc(value))
    {
        return false;
    }
    out = static_cast<std::int64_t>(value);
    return true;
}

[[nodiscard]] bool assign_color_harmonizer_hue_turns(double &target, const double degrees) noexcept
{
    const auto turns = color_harmonizer_hue_degrees_to_turns(degrees);
    if (!turns)
    {
        return false;
    }
    target = turns.value();
    return true;
}

struct ColorHarmonizerNumericField
{
    std::string_view develop_name;
    double ColorHarmonizerParams::*member;
    double minimum;
    double maximum;
};

[[nodiscard]] const std::array<ColorHarmonizerNumericField, 4> &
color_harmonizer_linear_fields() noexcept
{
    static const std::array<ColorHarmonizerNumericField, 4> fields{{
        {"colorHarmonizerPullStrength", &ColorHarmonizerParams::pull_strength,
         kColorHarmonizerPullStrengthMin, kColorHarmonizerPullStrengthMax},
        {"colorHarmonizerNeutralProtection", &ColorHarmonizerParams::neutral_protection,
         kColorHarmonizerNeutralProtectionMin, kColorHarmonizerNeutralProtectionMax},
        {"colorHarmonizerPullWidth", &ColorHarmonizerParams::pull_width,
         kColorHarmonizerPullWidthMin, kColorHarmonizerPullWidthMax},
        {"colorHarmonizerSmoothing", &ColorHarmonizerParams::smoothing,
         kColorHarmonizerSmoothingMin, kColorHarmonizerSmoothingMax},
    }};
    return fields;
}

[[nodiscard]] const std::array<std::pair<std::string_view, std::size_t>, 4> &
color_harmonizer_custom_hue_fields() noexcept
{
    static const std::array<std::pair<std::string_view, std::size_t>, 4> fields{{
        {"colorHarmonizerCustomHue0Degrees", 0U},
        {"colorHarmonizerCustomHue1Degrees", 1U},
        {"colorHarmonizerCustomHue2Degrees", 2U},
        {"colorHarmonizerCustomHue3Degrees", 3U},
    }};
    return fields;
}

[[nodiscard]] const std::array<std::pair<std::string_view, std::size_t>, 4> &
color_harmonizer_node_saturation_fields() noexcept
{
    static const std::array<std::pair<std::string_view, std::size_t>, 4> fields{{
        {"colorHarmonizerNodeSaturation0", 0U},
        {"colorHarmonizerNodeSaturation1", 1U},
        {"colorHarmonizerNodeSaturation2", 2U},
        {"colorHarmonizerNodeSaturation3", 3U},
    }};
    return fields;
}

[[nodiscard]] bool apply_color_harmonizer_field(DevelopParams &params, const std::string_view name,
                                                const double value) noexcept
{
    if (name == "colorHarmonizerEnabled")
    {
        if (value != 0.0 && value != 1.0)
        {
            return false;
        }
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = value == 1.0;
        return true;
    }
    if (name == "colorHarmonizerRuleIndex")
    {
        std::int64_t index = 0;
        if (!exact_develop_integer(
                value, 0, static_cast<std::int64_t>(kColorHarmonizerRuleCount - 1U), index))
        {
            return false;
        }
        auto rule = color_harmonizer_rule_from_index(index);
        if (!rule)
        {
            return false;
        }
        params.color_harmonizer.rule = rule.value();
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return true;
    }
    if (name == "colorHarmonizerCustomNodeCount")
    {
        std::int64_t count = 0;
        if (!exact_develop_integer(value, kColorHarmonizerCustomNodesMin,
                                   kColorHarmonizerCustomNodesMax, count))
        {
            return false;
        }
        params.color_harmonizer.num_custom_nodes = count;
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return true;
    }
    if (name == "colorHarmonizerAnchorHueDegrees")
    {
        if (!assign_color_harmonizer_hue_turns(params.color_harmonizer.anchor_hue, value))
        {
            return false;
        }
        params.color_harmonizer_present = true;
        params.color_harmonizer_enabled = true;
        return true;
    }
    for (const auto &[field, index] : color_harmonizer_custom_hue_fields())
    {
        if (name == field)
        {
            if (!assign_color_harmonizer_hue_turns(params.color_harmonizer.custom_hue[index],
                                                   value))
            {
                return false;
            }
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = true;
            return true;
        }
    }
    if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
    {
        return false;
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        if (name == field.develop_name)
        {
            params.color_harmonizer.*(field.member) = value;
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = true;
            return true;
        }
    }
    for (const auto &[field, index] : color_harmonizer_node_saturation_fields())
    {
        if (name == field)
        {
            params.color_harmonizer.node_saturation[index] = value;
            params.color_harmonizer_present = true;
            params.color_harmonizer_enabled = true;
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool reset_color_harmonizer_field(DevelopParams &params,
                                                const std::string_view name) noexcept
{
    const ColorHarmonizerParams defaults;
    if (name == "colorHarmonizer")
    {
        params.color_harmonizer_enabled = false;
        params.color_harmonizer = defaults;
        return true;
    }
    if (name == "colorHarmonizerEnabled")
    {
        params.color_harmonizer_enabled = false;
        return true;
    }
    if (name == "colorHarmonizerRuleIndex")
    {
        params.color_harmonizer.rule = defaults.rule;
        return true;
    }
    if (name == "colorHarmonizerCustomNodeCount")
    {
        params.color_harmonizer.num_custom_nodes = defaults.num_custom_nodes;
        return true;
    }
    if (name == "colorHarmonizerAnchorHueDegrees")
    {
        params.color_harmonizer.anchor_hue = defaults.anchor_hue;
        return true;
    }
    for (const auto &[field, index] : color_harmonizer_custom_hue_fields())
    {
        if (name == field)
        {
            params.color_harmonizer.custom_hue[index] = defaults.custom_hue[index];
            return true;
        }
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        if (name == field.develop_name)
        {
            params.color_harmonizer.*(field.member) = defaults.*(field.member);
            return true;
        }
    }
    for (const auto &[field, index] : color_harmonizer_node_saturation_fields())
    {
        if (name == field)
        {
            params.color_harmonizer.node_saturation[index] = defaults.node_saturation[index];
            return true;
        }
    }
    return false;
}

void clamp_color_harmonizer(ColorHarmonizerParams &params) noexcept
{
    const ColorHarmonizerParams defaults;
    auto rule = color_harmonizer_rule_from_index(color_harmonizer_rule_index(params.rule));
    if (!rule)
    {
        params.rule = defaults.rule;
    }
    const auto clamp_hue = [&](double &value, const double fallback)
    {
        if (!std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
        {
            value = fallback;
            return;
        }
        value = clamp_value(value, kColorHarmonizerHueMin, kColorHarmonizerHueMax);
    };
    clamp_hue(params.anchor_hue, defaults.anchor_hue);
    for (std::size_t index = 0U; index < params.custom_hue.size(); ++index)
    {
        clamp_hue(params.custom_hue[index], defaults.custom_hue[index]);
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        double &value = params.*(field.member);
        value = std::isfinite(value) && std::isfinite(static_cast<float>(value)) ?
                    clamp_value(value, field.minimum, field.maximum) :
                    defaults.*(field.member);
    }
    if (params.num_custom_nodes < kColorHarmonizerCustomNodesMin ||
        params.num_custom_nodes > kColorHarmonizerCustomNodesMax)
    {
        params.num_custom_nodes = defaults.num_custom_nodes;
    }
    for (std::size_t index = 0U; index < params.node_saturation.size(); ++index)
    {
        double &value = params.node_saturation[index];
        value = std::isfinite(value) && std::isfinite(static_cast<float>(value)) ?
                    clamp_value(value, kColorHarmonizerNodeSaturationMin,
                                kColorHarmonizerNodeSaturationMax) :
                    defaults.node_saturation[index];
    }
}

void append_develop_numeric_field_names(std::vector<std::string> &names)
{
    for (const auto &field : rgb_levels_stop_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_correction_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_contrast_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_reconstruction_numeric_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &field : color_harmonizer_linear_fields())
    {
        names.emplace_back(field.develop_name);
    }
    for (const auto &[field, index] : color_harmonizer_custom_hue_fields())
    {
        static_cast<void>(index);
        names.emplace_back(field);
    }
    for (const auto &[field, index] : color_harmonizer_node_saturation_fields())
    {
        static_cast<void>(index);
        names.emplace_back(field);
    }
}

} // namespace ravo::develop_internal
