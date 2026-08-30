#pragma once

#include <cstdint>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

// The borrowed input and the published output may be the same logical plane, but
// publication happens only after all coefficient planes have been computed.
[[nodiscard]] Result<void> box_blur_plane(const std::vector<float> &input,
                                          std::vector<float> &output, std::uint32_t width,
                                          std::uint32_t height, int radius,
                                          const CancellationToken &cancellation);
[[nodiscard]] Result<void> self_guided_filter_plane(std::vector<float> &plane, std::uint32_t width,
                                                    std::uint32_t height, int radius, float epsilon,
                                                    const CancellationToken &cancellation);

// Additional peak owned by self_guided_filter_plane, excluding its caller-owned
// input/output plane: mean, correlation/coefficient, auxiliary, and box scratch.
[[nodiscard]] std::uint64_t guided_filter_additional_bytes(std::uint32_t width,
                                                           std::uint32_t height) noexcept;

} // namespace ravo::detail
