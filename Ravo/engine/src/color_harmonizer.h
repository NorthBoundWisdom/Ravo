#pragma once

#include "image_ops.h"
#include "ravo/recipe/color_harmonizer.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_color_harmonizer(const WorkingImage &input,
                                                          const ColorHarmonizerParams &params,
                                                          const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_harmonizer(const WorkingImage &input,
                                                          const OperationInstance &operation,
                                                          const CancellationToken &cancellation);

} // namespace ravo
