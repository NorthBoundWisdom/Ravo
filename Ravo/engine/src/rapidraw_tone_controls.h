#pragma once

#include "image_ops.h"
#include "ravo/recipe/operation.h"

namespace ravo
{

[[nodiscard]] std::uint32_t rapidraw_tonal_blur_radius(std::uint32_t width,
                                                       std::uint32_t height) noexcept;
[[nodiscard]] std::uint32_t rapidraw_tonal_blur_radius(float reference_short_edge) noexcept;

[[nodiscard]] Result<void> apply_rapidraw_tone_controls(WorkingImage &image,
                                                        const OperationInstance &operation,
                                                        const CancellationToken &cancellation);

} // namespace ravo
