#pragma once

#include <cstdint>

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

namespace detail
{

// Engine-source test seam.  It is a per-call observer, never global state,
// and is not exported through Ravo's installed engine headers.
enum class ColorHarmonizerCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kMapRow,
    kMapChunk,
    kGaussian,
    kApplyRow,
    kApplyChunk,
    kBeforePublication,
};

using ColorHarmonizerCheckpointCallback = void (*)(void *context,
                                                   ColorHarmonizerCheckpoint checkpoint,
                                                   std::uint32_t progress) noexcept;

struct ColorHarmonizerControl
{
    void *context = nullptr;
    ColorHarmonizerCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage>
apply_color_harmonizer_controlled(const WorkingImage &input, const ColorHarmonizerParams &params,
                                  const CancellationToken &cancellation,
                                  ColorHarmonizerControl control);

} // namespace detail

} // namespace ravo
