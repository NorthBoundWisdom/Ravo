#pragma once

#include <array>

#include "image_ops.h"
#include "ravo/recipe/color_correction.h"

namespace ravo
{

// Engine-private frozen boundary for legacy colorcorrection.c v1. The operation
// itself consumes D50 Lab; the WorkingImage overload owns the surrounding S1.1
// linear-Rec709 bridge and publishes only a complete owned result.
[[nodiscard]] Result<std::array<float, 3>>
apply_color_correction_lab(const ColorCorrectionParams &params, const std::array<float, 3> &lab,
                           const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_correction(const WorkingImage &input,
                                                          const ColorCorrectionParams &params,
                                                          const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_correction(const WorkingImage &input,
                                                          const OperationInstance &operation,
                                                          const CancellationToken &cancellation);

} // namespace ravo
