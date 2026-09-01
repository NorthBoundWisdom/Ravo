#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/develop.h"

#include "image_ops.h"

namespace ravo::engine_color_test_support
{

using FrozenD50Triplet = std::array<float, 3>;
inline constexpr float kPlatformLibmReferenceTolerance = 1.0e-5F;

[[nodiscard]] FrozenD50Triplet
frozen_linear_rec709_to_xyz_d50(const FrozenD50Triplet &rgb) noexcept;
[[nodiscard]] FrozenD50Triplet
frozen_xyz_d50_to_linear_rec709(const FrozenD50Triplet &xyz) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_xyz_d50_to_lab(const FrozenD50Triplet &xyz) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_lab_to_xyz_d50(const FrozenD50Triplet &lab) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_color_contrast_lab(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &lab) noexcept;
[[nodiscard]] FrozenD50Triplet frozen_color_contrast_rgb(const ColorContrastParams &params,
                                                         const FrozenD50Triplet &rgb) noexcept;
[[nodiscard]] std::array<std::uint32_t, 3>
d50_triplet_bits(const FrozenD50Triplet &triplet) noexcept;
void expect_frozen_d50_bits(const FrozenD50Triplet &actual, const FrozenD50Triplet &oracle,
                            const std::array<std::uint32_t, 3> &golden);
void expect_frozen_d50_cbrt_reference(const FrozenD50Triplet &actual,
                                      const FrozenD50Triplet &oracle,
                                      const std::array<std::uint32_t, 3> &reference);
[[nodiscard]] std::vector<float>
frozen_legacy_color_balance_reference(const WorkingImage &input, const ColorBalanceParams &params);
[[nodiscard]] std::array<float, 3>
frozen_color_checker_lab_reference(const ColorCheckerParams &params,
                                   const std::array<float, 3> &lab, bool use_libm = false,
                                   bool promote_n3_sum = false);

} // namespace ravo::engine_color_test_support
