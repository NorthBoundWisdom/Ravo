#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "image_ops.h"
#include "ravo/recipe/sharpen.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_sharpen(const WorkingImage &input,
                                                 const SharpenParams &params,
                                                 const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_sharpen(const WorkingImage &input,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);

namespace detail
{

using SharpenLabPixel = std::array<float, 3>;

enum class SharpenCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kConvertInputRow,
    kVerticalRow,
    kHorizontalRow,
    kConvertOutputRow,
    kBeforePublication,
};

using SharpenCheckpointCallback = void (*)(void *context, SharpenCheckpoint checkpoint,
                                           std::uint32_t progress) noexcept;

struct SharpenControl
{
    void *context = nullptr;
    SharpenCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<std::vector<SharpenLabPixel>>
apply_sharpen_lab(std::span<const SharpenLabPixel> input, std::uint32_t width, std::uint32_t height,
                  float canonical_scale, const SharpenParams &params,
                  const CancellationToken &cancellation, SharpenControl control = {});
[[nodiscard]] Result<WorkingImage> apply_sharpen_controlled(const WorkingImage &input,
                                                            const SharpenParams &params,
                                                            const CancellationToken &cancellation,
                                                            SharpenControl control);

[[nodiscard]] std::uint64_t sharpen_working_bytes(std::uint32_t width, std::uint32_t height,
                                                  const SharpenParams &params) noexcept;

} // namespace detail

} // namespace ravo
