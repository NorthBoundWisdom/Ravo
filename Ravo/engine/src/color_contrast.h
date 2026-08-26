#pragma once

#include <array>

#include "image_ops.h"
#include "ravo/recipe/color_contrast.h"

namespace ravo
{

// Engine-private frozen boundary for legacy colorcontrast.c v2. The operation
// consumes D50 Lab; the WorkingImage overload owns the surrounding S1.1
// linear-Rec709 bridge and publishes only a complete owned result.
[[nodiscard]] Result<std::array<float, 3>>
apply_color_contrast_lab(const ColorContrastParams &params, const std::array<float, 3> &lab,
                         const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_contrast(const WorkingImage &input,
                                                        const ColorContrastParams &params,
                                                        const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_contrast(const WorkingImage &input,
                                                        const OperationInstance &operation,
                                                        const CancellationToken &cancellation);

} // namespace ravo
