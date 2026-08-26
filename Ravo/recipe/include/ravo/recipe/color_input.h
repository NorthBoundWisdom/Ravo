#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kInputProfileSource = "source";
inline constexpr std::string_view kInputProfileFileIcc = "file_icc";
inline constexpr std::string_view kInputProfileEmbeddedIcc = "embedded_icc";
inline constexpr std::string_view kInputProfileEmbeddedMatrix = "embedded_matrix";
inline constexpr std::string_view kInputProfileStandardMatrix = "standard_matrix";
inline constexpr std::string_view kInputProfileEnhancedMatrix = "enhanced_matrix";
inline constexpr std::string_view kInputProfileVendorMatrix = "vendor_matrix";
inline constexpr std::string_view kInputProfileAlternateMatrix = "alternate_matrix";
inline constexpr std::string_view kInputProfileSrgb = "srgb";
inline constexpr std::string_view kInputProfileAdobeRgb = "adobe_rgb";
inline constexpr std::string_view kInputProfileLinearRec709 = "linear_rec709";
inline constexpr std::string_view kInputProfileLinearRec2020 = "linear_rec2020";
inline constexpr std::string_view kInputProfileRec709 = "rec709";
inline constexpr std::string_view kInputProfileProPhotoRgb = "prophoto_rgb";
inline constexpr std::string_view kInputProfilePqRec2020 = "pq_rec2020";
inline constexpr std::string_view kInputProfileHlgRec2020 = "hlg_rec2020";
inline constexpr std::string_view kInputProfilePqP3 = "pq_p3";
inline constexpr std::string_view kInputProfileHlgP3 = "hlg_p3";
inline constexpr std::string_view kInputProfileDisplayP3 = "display_p3";
inline constexpr std::string_view kInputProfileXyz = "xyz";
inline constexpr std::string_view kInputProfileLab = "lab";

inline constexpr std::string_view kColorIntentPerceptual = "perceptual";
inline constexpr std::string_view kColorIntentRelative = "relative_colorimetric";
inline constexpr std::string_view kColorIntentSaturation = "saturation";
inline constexpr std::string_view kColorIntentAbsolute = "absolute_colorimetric";

inline constexpr std::string_view kColorNormalizeOff = "off";
inline constexpr std::string_view kColorNormalizeSrgb = "srgb";
inline constexpr std::string_view kColorNormalizeAdobeRgb = "adobe_rgb";
inline constexpr std::string_view kColorNormalizeLinearRec709 = "linear_rec709";
inline constexpr std::string_view kColorNormalizeLinearRec2020 = "linear_rec2020";

inline constexpr std::array<std::string_view, 9> kSelectableInputProfiles{
    kInputProfileSource,       kInputProfileSrgb,          kInputProfileAdobeRgb,
    kInputProfileLinearRec709, kInputProfileLinearRec2020, kInputProfileRec709,
    kInputProfileProPhotoRgb,  kInputProfileDisplayP3,     kInputProfileHlgP3};
inline constexpr std::array<std::string_view, 5> kSelectableWorkingProfiles{
    kInputProfileLinearRec709, kInputProfileLinearRec2020, kInputProfileProPhotoRgb,
    kInputProfileDisplayP3, kInputProfileAdobeRgb};
inline constexpr std::array<std::string_view, 4> kSelectableColorIntents{
    kColorIntentPerceptual, kColorIntentRelative, kColorIntentSaturation, kColorIntentAbsolute};
inline constexpr std::array<std::string_view, 5> kSelectableColorNormalizations{
    kColorNormalizeOff, kColorNormalizeSrgb, kColorNormalizeAdobeRgb, kColorNormalizeLinearRec709,
    kColorNormalizeLinearRec2020};

struct InputColorParams
{
    std::string input_profile{std::string(kInputProfileSource)};
    std::string input_profile_filename;
    std::string rendering_intent{std::string(kColorIntentPerceptual)};
    std::string gamut_normalize{std::string(kColorNormalizeOff)};
    bool blue_mapping = false;
    std::string working_profile{std::string(kInputProfileLinearRec709)};
    std::string working_profile_filename;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const InputColorParams &) const noexcept = default;
};

[[nodiscard]] Result<void> validate_input_color_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<InputColorParams>
input_color_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
input_color_to_parameters(const InputColorParams &params);

} // namespace ravo
