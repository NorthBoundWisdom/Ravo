#include "photo_merge_crop.h"
#include <algorithm>

namespace ravo::photo_merge_internal
{
Result<CroppedMergeImage> crop_valid_rectangle(const LinearWorkingBuffer &image,
                                               const std::span<const std::uint8_t> coverage,
                                               const CancellationToken &cancellation)
{
    // Largest covered axis-aligned rectangle, O(width*height), also handles
    // projective wedges and holes rather than merely cropping a bounding box.
    std::vector<std::uint32_t> heights(image.width + 1, 0), stack;
    std::uint64_t best = 0;
    std::uint32_t bx = 0, by = 0, bw = 0, bh = 0;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        for (std::uint32_t x = 0; x < image.width; ++x)
            heights[x] = coverage[std::size_t(y) * image.width + x] ? heights[x] + 1 : 0;
        stack.clear();
        for (std::uint32_t x = 0; x <= image.width; ++x)
        {
            while (!stack.empty() && heights[stack.back()] > heights[x])
            {
                const auto h = heights[stack.back()];
                stack.pop_back();
                const auto left = stack.empty() ? 0 : stack.back() + 1;
                const auto area = std::uint64_t(x - left) * h;
                if (area > best)
                {
                    best = area;
                    bx = left;
                    by = y + 1 - h;
                    bw = x - left;
                    bh = h;
                }
            }
            stack.push_back(x);
        }
    }
    if (!best)
        return make_error(ErrorCode::kValidation, "Merge has no fully covered rectangle",
                          {{"reason", "merge_no_coverage"}});
    LinearWorkingBuffer output;
    output.width = bw;
    output.height = bh;
    output.color_profile = image.color_profile;
    output.rgb.resize(std::size_t(bw) * bh * 3);
    for (std::uint32_t y = 0; y < bh; ++y)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        const auto begin = image.rgb.data() + (std::size_t(y + by) * image.width + bx) * 3;
        std::copy_n(begin, std::size_t(bw) * 3, output.rgb.data() + std::size_t(y) * bw * 3);
    }
    return CroppedMergeImage{std::move(output), bx, by};
}
} // namespace ravo::photo_merge_internal
