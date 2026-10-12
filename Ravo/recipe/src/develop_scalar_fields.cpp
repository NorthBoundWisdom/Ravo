#include "develop_internal.h"

#include <algorithm>
#include <array>

namespace ravo::develop_internal
{
namespace
{
struct ScalarField
{
    std::string_view name;
    double DevelopParams::*member;
    double minimum;
    double maximum;
    bool individual_reset;
};
constexpr auto fields = std::to_array<ScalarField>({
    {"contrast", &DevelopParams::contrast, -1.0, 1.0, true},
    {"highlights", &DevelopParams::highlights, -1.0, 1.0, true},
    {"shadows", &DevelopParams::shadows, -1.0, 1.0, true},
    {"whites", &DevelopParams::whites, -1.0, 1.0, true},
    {"blacks", &DevelopParams::blacks, -1.0, 1.0, true},
    {"vibrance", &DevelopParams::vibrance, -1.0, 1.0, true},
    {"saturation", &DevelopParams::saturation, -1.0, 1.0, true},
    {"straighten", &DevelopParams::straighten_degrees, kDevelopStraightenMin, kDevelopStraightenMax,
     true},
    {"sharpen", &DevelopParams::sharpen, 0.0, 2.0, false},
    {"sharpenRadius", &DevelopParams::sharpen_radius, kSharpenRadiusMin, kSharpenRadiusMax, false},
    {"sharpenThreshold", &DevelopParams::sharpen_threshold, kSharpenThresholdMin,
     kSharpenThresholdMax, false},
    {"clarity", &DevelopParams::clarity, -1.0, 1.0, true},
    {"vignette", &DevelopParams::vignette, -1.0, 1.0, false},
    {"vignetteMidpoint", &DevelopParams::vignette_midpoint, 0.0, 1.0, false},
    {"vignetteFalloff", &DevelopParams::vignette_falloff, 0.05, 1.0, false},
    {"vignetteShape", &DevelopParams::vignette_shape, 0.5, 5.0, false},
    {"vignetteCenterX", &DevelopParams::vignette_center_x, -1.0, 1.0, false},
    {"vignetteCenterY", &DevelopParams::vignette_center_y, -1.0, 1.0, false},
    {"grain", &DevelopParams::grain, 0.0, 1.0, true},
    {"bloom", &DevelopParams::bloom, 0.0, 1.0, true},
    {"soften", &DevelopParams::soften, 0.0, 1.0, true},
    {"dehaze", &DevelopParams::dehaze, -1.0, 1.0, true},
    {"dehazeDistance", &DevelopParams::dehaze_distance, kDehazeDistanceMin, kDehazeDistanceMax,
     true},
    {"gamma", &DevelopParams::gamma, 0.2, 3.0, true},
    {"rawHighlights", &DevelopParams::raw_highlights, 0.0, 1.0, false},
    {"rawHighlightsClip", &DevelopParams::raw_highlights_clip, 0.5, 1.0, false},
    {"rawDenoiseThreshold", &DevelopParams::raw_denoise_threshold, 0.0, 1.0, true},
    {"hotPixelsStrength", &DevelopParams::hot_pixels_strength, 0.0, 1.0, false},
    {"hotPixelsThreshold", &DevelopParams::hot_pixels_threshold, 0.0, 1.0, false},
    {"denoise", &DevelopParams::denoise, 0.0, 1.0, true},
    {"denoiseChroma", &DevelopParams::denoise_chroma, 0.0, 1.0, true},
    {"denoiseRadius", &DevelopParams::denoise_radius, 0.5, 8.0, true},
    {"lensK1", &DevelopParams::lens_k1, -2.0, 2.0, false},
    {"lensK2", &DevelopParams::lens_k2, -2.0, 2.0, false},
    {"lensTcaR", &DevelopParams::lens_tca_r, 0.9, 1.1, false},
    {"lensTcaB", &DevelopParams::lens_tca_b, 0.9, 1.1, false},
    {"lensVignetting", &DevelopParams::lens_vignetting, 0.0, 1.0, false},
    {"lensFocal", &DevelopParams::lens_focal_mm, 1.0, 2000.0, false},
    {"toneEqBlacks", &DevelopParams::tone_eq_blacks, -4.0, 4.0, true},
    {"toneEqShadows", &DevelopParams::tone_eq_shadows, -4.0, 4.0, true},
    {"toneEqMidtones", &DevelopParams::tone_eq_midtones, -4.0, 4.0, true},
    {"toneEqHighlights", &DevelopParams::tone_eq_highlights, -4.0, 4.0, true},
    {"toneEqWhites", &DevelopParams::tone_eq_whites, -4.0, 4.0, true},
});
const ScalarField *find_field(const std::string_view name) noexcept
{
    const auto found = std::find_if(fields.begin(), fields.end(),
                                    [name](const auto &field) { return field.name == name; });
    return found == fields.end() ? nullptr : &*found;
}
} // namespace

bool assign_scalar_develop_field(DevelopParams &params, const std::string_view name,
                                 const double value) noexcept
{
    const auto *field = find_field(name);
    if (!field)
        return false;
    params.*field->member = value;
    return true;
}

bool reset_scalar_develop_field(DevelopParams &params, const std::string_view name)
{
    const auto *field = find_field(name);
    if (!field || !field->individual_reset)
        return false;
    const DevelopParams identity;
    params.*field->member = identity.*field->member;
    return true;
}

void clamp_scalar_develop_fields(DevelopParams &params) noexcept
{
    for (const auto &field : fields)
        params.*field.member = clamp_value(params.*field.member, field.minimum, field.maximum);
}

void append_scalar_develop_field_names(std::vector<std::string> &names)
{
    for (const auto &field : fields)
        names.emplace_back(field.name);
}

std::optional<DevelopSetField> scalar_develop_field_schema(const std::string_view name)
{
    const auto *field = find_field(name);
    if (!field)
        return std::nullopt;
    return DevelopSetField{std::string(name), DevelopSetFieldKind::Number, field->minimum,
                           field->maximum};
}
} // namespace ravo::develop_internal
