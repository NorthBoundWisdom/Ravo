#pragma once

#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/color_reconstruction.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage>
apply_color_reconstruction(const WorkingImage &input, const ColorReconstructionParams &params,
                           const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage>
apply_color_reconstruction(const WorkingImage &input, const OperationInstance &operation,
                           const CancellationToken &cancellation);

namespace detail
{

enum class ColorReconstructionCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kSplatRow,
    kBlurLine,
    kSliceRow,
    kBeforePublication,
};

using ColorReconstructionCheckpointCallback = void (*)(void *context,
                                                       ColorReconstructionCheckpoint checkpoint,
                                                       std::uint32_t progress) noexcept;

struct ColorReconstructionControl
{
    void *context = nullptr;
    ColorReconstructionCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage> apply_color_reconstruction_controlled(
    const WorkingImage &input, const ColorReconstructionParams &params,
    const CancellationToken &cancellation, ColorReconstructionControl control);

// One deterministic CPU grid is live next to the borrowed input and owned
// output. Invalid dimensions/scale/parameters saturate so callers cannot
// under-account an unsupported request.
[[nodiscard]] std::uint64_t
color_reconstruction_grid_bytes(std::uint32_t width, std::uint32_t height, float canonical_scale,
                                const ColorReconstructionParams &params) noexcept;

} // namespace detail

} // namespace ravo
