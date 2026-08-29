#pragma once

#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/color_zones.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_color_zones(WorkingImage input,
                                                     const ColorZonesParams &params,
                                                     const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_color_zones(WorkingImage input,
                                                     const OperationInstance &operation,
                                                     const CancellationToken &cancellation);

namespace detail
{

inline constexpr std::uint64_t kColorZonesLutBytes = 3U * 0x10000U * sizeof(float);

enum class ColorZonesCheckpoint : std::uint8_t
{
    kBuildLut,
    kProcessRow,
    kBeforePublication,
};

using ColorZonesCheckpointCallback = void (*)(void *context, ColorZonesCheckpoint checkpoint,
                                              std::uint32_t progress) noexcept;

struct ColorZonesControl
{
    void *context = nullptr;
    ColorZonesCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<WorkingImage>
apply_color_zones_controlled(WorkingImage input, const ColorZonesParams &params,
                             const CancellationToken &cancellation, ColorZonesControl control = {});

} // namespace detail
} // namespace ravo
