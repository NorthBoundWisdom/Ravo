#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kProofModeOff = "off";
inline constexpr std::string_view kProofModeSoftproof = "softproof";
inline constexpr std::string_view kProofModeGamutCheck = "gamut_check";

inline constexpr std::array<std::string_view, 11> kSelectableOutputProfiles{
    kInputProfileSrgb,          kInputProfileAdobeRgb,   kInputProfileLinearRec709,
    kInputProfileLinearRec2020, kInputProfileRec709,     kInputProfileProPhotoRgb,
    kInputProfilePqRec2020,     kInputProfileHlgRec2020, kInputProfilePqP3,
    kInputProfileHlgP3,         kInputProfileDisplayP3};
inline constexpr auto kSelectableProofProfiles = kSelectableOutputProfiles;
inline constexpr std::array<std::string_view, 3> kSelectableProofModes{
    kProofModeOff, kProofModeSoftproof, kProofModeGamutCheck};

struct OutputColorParams
{
    std::string output_profile{std::string(kInputProfileSrgb)};
    std::string output_profile_filename;
    std::string rendering_intent{std::string(kColorIntentPerceptual)};
    std::string proof_mode{std::string(kProofModeOff)};
    std::string proof_profile{std::string(kInputProfileSrgb)};
    std::string proof_profile_filename;
    std::string proof_intent{std::string(kColorIntentRelative)};
    bool black_point_compensation = true;

    [[nodiscard]] bool is_identity() const noexcept;
    [[nodiscard]] bool operator==(const OutputColorParams &) const noexcept = default;
};

[[nodiscard]] Result<void> validate_output_color_parameters(
    const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<OutputColorParams>
output_color_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] std::map<std::string, ParameterValue, std::less<>>
output_color_to_parameters(const OutputColorParams &params);

} // namespace ravo
