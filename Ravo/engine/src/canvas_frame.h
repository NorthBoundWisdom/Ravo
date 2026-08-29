#pragma once

#include <cstdint>

#include "image_ops.h"
#include "output_color.h"
#include "ravo/recipe/canvas_frame.h"

namespace ravo
{

struct CanvasLayout
{
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t image_x = 0;
    std::uint32_t image_y = 0;

    [[nodiscard]] bool operator==(const CanvasLayout &) const noexcept = default;
};

struct FrameLayout
{
    std::uint32_t output_width = 0;
    std::uint32_t output_height = 0;
    std::uint32_t image_x = 0;
    std::uint32_t image_y = 0;

    [[nodiscard]] bool operator==(const FrameLayout &) const noexcept = default;
};

[[nodiscard]] Result<CanvasLayout> compute_canvas_layout(std::uint32_t width, std::uint32_t height,
                                                         const CanvasParams &params);
[[nodiscard]] Result<WorkingImage> apply_canvas(WorkingImage input, const CanvasParams &params,
                                                const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_canvas(WorkingImage input,
                                                const OperationInstance &operation,
                                                const CancellationToken &cancellation);

[[nodiscard]] Result<FrameLayout> compute_frame_layout(std::uint32_t width, std::uint32_t height,
                                                       const FrameParams &params);
[[nodiscard]] Result<ProfiledOutputBuffer> apply_frame(ProfiledOutputBuffer input,
                                                       const FrameParams &params,
                                                       const CancellationToken &cancellation);
[[nodiscard]] Result<ProfiledOutputBuffer> apply_frame(ProfiledOutputBuffer input,
                                                       const OperationInstance &operation,
                                                       const CancellationToken &cancellation);

namespace detail
{

enum class CanvasFrameCheckpoint : std::uint8_t
{
    kCanvasRow,
    kFrameRow,
    kBeforePublication,
};

using CanvasFrameCheckpointCallback = void (*)(void *context, CanvasFrameCheckpoint checkpoint,
                                               std::uint32_t progress) noexcept;

struct CanvasFrameControl
{
    void *context = nullptr;
    CanvasFrameCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage> apply_canvas_controlled(WorkingImage input,
                                                           const CanvasParams &params,
                                                           const CancellationToken &cancellation,
                                                           CanvasFrameControl control = {});
[[nodiscard]] Result<ProfiledOutputBuffer>
apply_frame_controlled(ProfiledOutputBuffer input, const FrameParams &params,
                       const CancellationToken &cancellation, CanvasFrameControl control = {});

} // namespace detail

} // namespace ravo
