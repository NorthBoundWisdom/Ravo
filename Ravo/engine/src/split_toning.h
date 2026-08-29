#pragma once

#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/split_toning.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_split_toning(WorkingImage input,
                                                      const SplitToningParams &params,
                                                      const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_split_toning(WorkingImage input,
                                                      const OperationInstance &operation,
                                                      const CancellationToken &cancellation);

namespace detail
{

enum class SplitToningCheckpoint : std::uint8_t
{
    kProcessRow,
    kBeforePublication,
};

using SplitToningCheckpointCallback = void (*)(void *context, SplitToningCheckpoint checkpoint,
                                               std::uint32_t progress) noexcept;

struct SplitToningControl
{
    void *context = nullptr;
    SplitToningCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage>
apply_split_toning_controlled(WorkingImage input, const SplitToningParams &params,
                              const CancellationToken &cancellation,
                              SplitToningControl control = {});

} // namespace detail
} // namespace ravo
