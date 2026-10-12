#pragma once

#include "image_ops.h"

namespace ravo
{
[[nodiscard]] Result<WorkingImage>
working_from_linear_raw(const DecodedRaw &raw, std::uint32_t width, std::uint32_t height,
                        const std::array<float, 4> &white_balance,
                        const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage>
working_from_linear_raw_window(const DecodedRaw &raw, std::uint32_t x, std::uint32_t y,
                               std::uint32_t width, std::uint32_t height,
                               const std::array<float, 4> &white_balance,
                               const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_linear_raw_highlights(DecodedRaw &raw,
                                                       const OperationInstance &operation,
                                                       const std::array<float, 4> &white_balance,
                                                       const CancellationToken &cancellation);
} // namespace ravo
