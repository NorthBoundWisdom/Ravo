#pragma once

#include <array>

#include "image_ops.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

struct ColorBalanceRgbMasks
{
    std::array<float, 3> opacity{};
    std::array<float, 3> complement{};
};

struct ColorBalanceRgbJzClip
{
    float chroma = 0.0F;
    bool clipped = false;
};

[[nodiscard]] ColorBalanceRgbMasks
color_balance_rgb_opacity_masks(float luminance, const ColorBalanceRgbParams &params) noexcept;

[[nodiscard]] std::array<float, 4>
color_balance_rgb_working_to_ych(const std::array<float, 3> &working_rgb) noexcept;

[[nodiscard]] std::array<float, 3>
color_balance_rgb_ych_to_grading_rgb(const std::array<float, 4> &ych) noexcept;

[[nodiscard]] Result<ColorBalanceRgbJzClip>
color_balance_rgb_jzazbz_negative_lms_clip(float lightness, float chroma, float hue_radians);

[[nodiscard]] Result<void> apply_color_balance_rgb(WorkingImage &image,
                                                   const OperationInstance &operation,
                                                   const CancellationToken &cancellation);

} // namespace ravo
