#pragma once

#include <array>
#include <cstdint>

#include "image_ops.h"
#include "ravo/recipe/dehaze.h"

namespace ravo
{

[[nodiscard]] Result<WorkingImage> apply_dehaze(const WorkingImage &input,
                                                const DehazeParams &params,
                                                const CancellationToken &cancellation);
[[nodiscard]] Result<WorkingImage> apply_dehaze(const WorkingImage &input,
                                                const OperationInstance &operation,
                                                const CancellationToken &cancellation);

namespace detail
{

enum class DehazeCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kDarkChannelRow,
    kAmbientSelection,
    kTransitionRow,
    kGuidedTile,
    kGuidedStatisticsRow,
    kGuidedSolveRow,
    kOutputRow,
    kBeforePublication,
};

using DehazeCheckpointCallback = void (*)(void *context, DehazeCheckpoint checkpoint,
                                          std::uint32_t progress) noexcept;

struct DehazeControl
{
    void *context = nullptr;
    DehazeCheckpointCallback checkpoint_callback = nullptr;
};

struct DehazeAnalysis
{
    std::array<float, 3> ambient{};
    float distance_max = 0.0F;
    int dark_channel_radius = 0;
    int guided_filter_radius = 0;

    [[nodiscard]] bool operator==(const DehazeAnalysis &) const noexcept = default;
};

[[nodiscard]] Result<WorkingImage> apply_dehaze_controlled(const WorkingImage &input,
                                                           const DehazeParams &params,
                                                           const CancellationToken &cancellation,
                                                           DehazeControl control,
                                                           DehazeAnalysis *analysis = nullptr);
[[nodiscard]] std::uint64_t dehaze_working_bytes(std::uint32_t width, std::uint32_t height,
                                                 float canonical_scale,
                                                 const DehazeParams &params) noexcept;

} // namespace detail

} // namespace ravo
