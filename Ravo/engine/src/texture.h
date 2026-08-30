#pragma once

#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/texture.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_texture(const WorkingImage &input,
                                                 const TextureParams &params,
                                                 const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_texture(const WorkingImage &input,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);

namespace detail
{

enum class TextureCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kInputRow,
    kBeforeFineFilter,
    kBeforeCoarseFilter,
    kOutputRow,
    kBeforePublication,
};

using TextureCheckpointCallback = void (*)(void *context, TextureCheckpoint checkpoint,
                                           std::uint32_t progress) noexcept;

struct TextureControl
{
    void *context = nullptr;
    TextureCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage> apply_texture_controlled(const WorkingImage &input,
                                                            const TextureParams &params,
                                                            const CancellationToken &cancellation,
                                                            TextureControl control);
[[nodiscard]] std::uint64_t texture_working_bytes(std::uint32_t width, std::uint32_t height,
                                                  const TextureParams &params) noexcept;

} // namespace detail
} // namespace ravo
