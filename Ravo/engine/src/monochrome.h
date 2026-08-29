#pragma once

#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/monochrome.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_monochrome(WorkingImage input,
                                                    const MonochromeParams &params,
                                                    const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_monochrome(WorkingImage input,
                                                    const OperationInstance &operation,
                                                    const CancellationToken &cancellation);

namespace detail
{

enum class MonochromeCheckpoint : std::uint8_t
{
    kConvertRow,
    kFilterRow,
    kBeforeBilateral,
    kOutputRow,
    kBeforePublication,
};

using MonochromeCheckpointCallback = void (*)(void *context, MonochromeCheckpoint checkpoint,
                                              std::uint32_t progress) noexcept;

struct MonochromeControl
{
    void *context = nullptr;
    MonochromeCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage>
apply_monochrome_controlled(WorkingImage input, const MonochromeParams &params,
                            const CancellationToken &cancellation, MonochromeControl control = {});
[[nodiscard]] std::uint64_t monochrome_working_bytes(std::uint32_t width, std::uint32_t height,
                                                     float canonical_scale) noexcept;

} // namespace detail
} // namespace ravo
