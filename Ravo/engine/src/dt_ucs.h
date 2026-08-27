#pragma once

#include <array>

namespace ravo::dt_ucs
{

using Triplet = std::array<float, 3>;

// Engine-private, value-only colour-science boundary. These helpers own no
// profile state or parser handles; callers own working-space conversion and
// publication policy.
[[nodiscard]] float y_to_lightness(float luminance) noexcept;
[[nodiscard]] float lightness_to_y(float lightness) noexcept;
[[nodiscard]] Triplet xyz_d50_to_d65(Triplet xyz) noexcept;
[[nodiscard]] Triplet xyz_d65_to_d50(Triplet xyz) noexcept;
[[nodiscard]] Triplet xyz_d65_to_xyy(Triplet xyz) noexcept;
[[nodiscard]] Triplet xyy_to_xyz_d65(Triplet xyy) noexcept;
[[nodiscard]] Triplet xyy_to_jch(Triplet xyy, float white_lightness) noexcept;
[[nodiscard]] Triplet jch_to_xyy(Triplet jch, float white_lightness) noexcept;
[[nodiscard]] Triplet xyz_d50_to_jch(Triplet xyz, float white_lightness) noexcept;
[[nodiscard]] Triplet jch_to_xyz_d50(Triplet jch, float white_lightness) noexcept;

} // namespace ravo::dt_ucs
