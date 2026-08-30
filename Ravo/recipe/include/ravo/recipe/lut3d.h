#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

inline constexpr std::string_view kLut3dOperationId = "ravo.color.lut3d";
inline constexpr std::int64_t kLut3dOperationSchemaVersion = 1;
inline constexpr std::string_view kLut3dInterpolationTetrahedral = "tetrahedral";
inline constexpr std::string_view kLut3dInterpolationTrilinear = "trilinear";
inline constexpr std::string_view kLut3dSpaceSrgb = "srgb";
inline constexpr std::string_view kLut3dSpaceAdobeRgb = "adobe_rgb";
inline constexpr std::string_view kLut3dSpaceRec709 = "rec709";
inline constexpr std::string_view kLut3dSpaceLinearRec709 = "linear_rec709";
inline constexpr std::string_view kLut3dSpaceLinearRec2020 = "linear_rec2020";
inline constexpr std::string_view kLut3dSpaceLinearProPhoto = "linear_prophoto";
inline constexpr std::size_t kLut3dPathMaximumBytes = 4096U;

inline constexpr std::array<std::string_view, 6> kLut3dSelectableSpaces{
    kLut3dSpaceSrgb,         kLut3dSpaceAdobeRgb,      kLut3dSpaceRec709,
    kLut3dSpaceLinearRec709, kLut3dSpaceLinearRec2020, kLut3dSpaceLinearProPhoto};
inline constexpr std::array<std::string_view, 2> kLut3dSelectableInterpolations{
    kLut3dInterpolationTetrahedral, kLut3dInterpolationTrilinear};

struct Lut3dParams
{
    std::string file_path;
    std::string input_space{std::string(kLut3dSpaceSrgb)};
    std::string output_space{std::string(kLut3dSpaceSrgb)};
    std::string interpolation{std::string(kLut3dInterpolationTetrahedral)};
    double strength = 1.0;

    [[nodiscard]] bool operator==(const Lut3dParams &) const noexcept = default;
};

[[nodiscard]] bool lut3d_space_supported(std::string_view space) noexcept;
[[nodiscard]] bool lut3d_interpolation_supported(std::string_view interpolation) noexcept;
[[nodiscard]] Result<Lut3dParams>
lut3d_from_parameters(const std::map<std::string, ParameterValue, std::less<>> &parameters);
[[nodiscard]] Result<std::map<std::string, ParameterValue, std::less<>>>
lut3d_to_parameters(const Lut3dParams &params);

} // namespace ravo
