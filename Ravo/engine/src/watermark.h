#pragma once

#include <cstdint>

#include "output_color.h"
#include "ravo/recipe/watermark.h"

namespace ravo
{

[[nodiscard]] Result<ProfiledOutputBuffer> apply_watermark(ProfiledOutputBuffer input,
                                                           const WatermarkParams &params,
                                                           const AssetDescriptor &asset,
                                                           const CancellationToken &cancellation);
[[nodiscard]] Result<ProfiledOutputBuffer> apply_watermark(ProfiledOutputBuffer input,
                                                           const OperationInstance &operation,
                                                           const AssetDescriptor &asset,
                                                           const CancellationToken &cancellation);

namespace detail
{

enum class WatermarkCheckpoint : std::uint8_t
{
    kProcessRow,
    kBeforePublication,
};

using WatermarkCheckpointCallback = void (*)(void *context, WatermarkCheckpoint checkpoint,
                                             std::uint32_t progress) noexcept;

struct WatermarkControl
{
    void *context = nullptr;
    WatermarkCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<ProfiledOutputBuffer>
apply_watermark_controlled(ProfiledOutputBuffer input, const WatermarkParams &params,
                           const AssetDescriptor &asset, const CancellationToken &cancellation,
                           WatermarkControl control = {});

} // namespace detail
} // namespace ravo
