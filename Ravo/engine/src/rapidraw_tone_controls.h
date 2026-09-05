#pragma once

#include "image_ops.h"
#include "ravo/recipe/operation.h"

namespace ravo
{

[[nodiscard]] Result<void> apply_rapidraw_tone_controls(WorkingImage &image,
                                                        const OperationInstance &operation,
                                                        const CancellationToken &cancellation);

} // namespace ravo
