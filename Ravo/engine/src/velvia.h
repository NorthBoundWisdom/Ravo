#pragma once

#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/velvia.h"

namespace ravo
{
[[nodiscard]] Result<WorkingImage> apply_velvia(WorkingImage input, const VelviaParams &params,
                                                const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_velvia(WorkingImage input,
                                                const OperationInstance &operation,
                                                const CancellationToken &cancellation);
namespace detail
{
enum class VelviaCheckpoint : std::uint8_t
{
    kProcessRow,
    kBeforePublication,
};
using VelviaCheckpointCallback = void (*)(void *, VelviaCheckpoint, std::uint32_t) noexcept;
struct VelviaControl
{
    void *context = nullptr;
    VelviaCheckpointCallback checkpoint_callback = nullptr;
};
[[nodiscard]] Result<WorkingImage>
apply_velvia_controlled(WorkingImage input, const VelviaParams &params,
                        const CancellationToken &cancellation, VelviaControl control = {});
} // namespace detail
} // namespace ravo
