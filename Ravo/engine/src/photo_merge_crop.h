#pragma once
#include "ravo/engine/photo_merge.h"
namespace ravo::photo_merge_internal
{
struct CroppedMergeImage
{
    LinearWorkingBuffer image;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
};
[[nodiscard]] Result<CroppedMergeImage> crop_valid_rectangle(const LinearWorkingBuffer &image,
                                                             std::span<const std::uint8_t> coverage,
                                                             const CancellationToken &cancellation);
} // namespace ravo::photo_merge_internal
