#pragma once

#include <array>

#include "image_ops.h"
#include "ravo/recipe/primaries.h"

namespace ravo
{

// Row-major, column-vector RGB adjustment from the buffer's declared working
// RGB space back into that same working RGB space.
[[nodiscard]] Result<std::array<double, 9>>
primaries_adjustment_matrix(const ColorProfileState &working_profile,
                            const PrimariesParams &params);

// The input is borrowed and never mutated. A successful result owns its pixels
// and retains the exact declared working profile state.
[[nodiscard]] Result<WorkingImage> apply_primaries(const WorkingImage &input,
                                                   const PrimariesParams &params,
                                                   const CancellationToken &cancellation);

[[nodiscard]] Result<WorkingImage> apply_primaries(const WorkingImage &input,
                                                   const OperationInstance &operation,
                                                   const CancellationToken &cancellation);

} // namespace ravo
