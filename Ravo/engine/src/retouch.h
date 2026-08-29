#pragma once

#include <cstdint>
#include <span>

#include "image_ops.h"
#include "ravo/recipe/retouch.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_retouch(WorkingImage image, const Recipe &recipe,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);

namespace detail
{

[[nodiscard]] std::uint64_t retouch_working_bytes(std::uint32_t width, std::uint32_t height,
                                                  const RetouchParams &params) noexcept;
[[nodiscard]] std::uint64_t bilateral_filter_working_bytes(std::uint32_t width,
                                                           std::uint32_t height, float sigma_s,
                                                           float sigma_r) noexcept;
[[nodiscard]] Result<void> bilateral_filter_lightness(std::span<float> lightness,
                                                      std::uint32_t width, std::uint32_t height,
                                                      float sigma_s, float sigma_r,
                                                      const CancellationToken &cancellation);

} // namespace detail

} // namespace ravo
