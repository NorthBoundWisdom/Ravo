#pragma once

#include <array>

#include "image_ops.h"
#include "ravo/recipe/color_checker.h"

namespace ravo
{

// Engine-private frozen boundary for legacy colorchecker.c v2. The surrounding
// working image remains RGB; the fit and point evaluation operate in D50 Lab.
[[nodiscard]] float color_checker_thin_plate_kernel(const std::array<float, 3> &left,
                                                    const std::array<float, 3> &right) noexcept;
[[nodiscard]] Result<std::array<float, 3>>
apply_color_checker_lab(const ColorCheckerParams &params, const std::array<float, 3> &lab,
                        const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_checker(const WorkingImage &input,
                                                       const ColorCheckerParams &params,
                                                       const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_checker(const WorkingImage &input,
                                                       const OperationInstance &operation,
                                                       const CancellationToken &cancellation);

} // namespace ravo
