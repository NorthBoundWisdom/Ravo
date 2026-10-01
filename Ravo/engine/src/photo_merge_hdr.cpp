// Exposure envelope adapted from darktable src/control/jobs/control_jobs.c,
// Copyright (C) darktable developers, GPL-3.0-or-later. The RGB merge,
// registration, deghosting and delivery contract are Ravo-owned.
#include "photo_registration.h"
#include "photo_merge_crop.h"

#include <algorithm>
#include <cmath>

namespace ravo::photo_merge_internal
{
namespace
{
float envelope(const float value)
{
    const float x = std::clamp(value, 0.F, 1.F);
    if (x < .5F)
    {
        const float t = 2 * x - 1;
        return 1 - t * t;
    }
    const float t = 2 * (1 - x);
    return 3 * t * t - 2 * t * t * t;
}
} // namespace
Result<PhotoMergeImage> merge_hdr(const std::span<const LinearWorkingBuffer> frames,
                                  const PhotoMergeOptions &options,
                                  std::vector<PhotoMergeAlignment> alignments,
                                  const CancellationToken &cancellation)
{
    const auto &reference = frames.front();
    const std::size_t pixels = std::size_t(reference.width) * reference.height;
    std::uint64_t input_bytes = 0;
    for (const auto &f : frames)
        input_bytes += f.rgb.size() * sizeof(float);
    if (input_bytes + pixels * 40 + 64ULL * 1024 * 1024 > options.memory_budget_bytes)
        return make_error(ErrorCode::kValidation, "HDR result exceeds memory budget",
                          {{"reason", "merge_memory_budget_exceeded"}});
    PhotoMergeImage result;
    result.alignments = std::move(alignments);
    result.image.width = reference.width;
    result.image.height = reference.height;
    result.image.color_profile = reference.color_profile;
    result.image.rgb.resize(pixels * 3);
    std::vector<std::uint8_t> coverage(pixels, 1);
    std::vector<Matrix> inverse_transforms;
    std::vector<float> gains;
    auto exposures = options.exposure_ev;
    std::sort(exposures.begin(), exposures.end());
    const auto middle = exposures.size() / 2;
    result.exposure_normalization_ev =
        exposures.size() % 2 ? exposures[middle] : (exposures[middle - 1] + exposures[middle]) / 2;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        auto inv = inverse(result.alignments[i].source_to_reference);
        if (!inv)
            return inv.error();
        inverse_transforms.push_back(inv.value());
        gains.push_back(static_cast<float>(
            std::exp2(options.exposure_ev[i] - result.exposure_normalization_ev)));
    }
    for (std::uint32_t y = 0; y < reference.height; ++y)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        for (std::uint32_t x = 0; x < reference.width; ++x)
        {
            std::array<std::array<float, 3>, 16> colors{};
            std::array<float, 16> weights{};
            std::array<bool, 16> valid{};
            float best_quality = -1;
            std::size_t anchor = 0;
            const auto p = std::size_t(y) * reference.width + x;
            for (std::size_t i = 0; i < frames.size(); ++i)
            {
                valid[i] = sample(frames[i], project(inverse_transforms[i], {double(x), double(y)}),
                                  colors[i]);
                if (!valid[i])
                {
                    coverage[p] = 0;
                    continue;
                }
                const float max = *std::max_element(colors[i].begin(), colors[i].end());
                weights[i] = envelope(max);
                // If everything is clipped, explicitly retain the shortest
                // exposure; no merge can recreate detail clipped in every frame.
                const float quality = max < .98F ? weights[i] + 1.F : -gains[i];
                if (quality > best_quality || !valid[anchor])
                {
                    best_quality = quality;
                    anchor = i;
                }
            }
            // Frame 0 always covers its own canvas. Choose the best exposed
            // anchor at each pixel so a clipped reference does not cause ghosts.
            float sum = 0;
            std::array<float, 3> accum{};
            bool ghost = false;
            for (std::size_t i = 0; i < frames.size(); ++i)
                if (valid[i])
                {
                    float difference = 0, level = 0;
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const float base = colors[anchor][c] / gains[anchor];
                        difference = std::max(difference, std::abs(colors[i][c] / gains[i] - base));
                        level = std::max(level, std::abs(base));
                    }
                    if (options.deghost_threshold > 0 && i != anchor && weights[i] > .05F &&
                        difference > options.deghost_threshold * (.05F + level))
                    {
                        ghost = true;
                        continue;
                    }
                    sum += weights[i];
                    for (unsigned c = 0; c < 3; ++c)
                        accum[c] += weights[i] * colors[i][c] / gains[i];
                }
            if (ghost)
                ++result.deghosted_pixels;
            for (unsigned c = 0; c < 3; ++c)
                result.image.rgb[p * 3 + c] =
                    sum > 1e-6F ? accum[c] / sum : colors[anchor][c] / gains[anchor];
        }
    }
    if (options.auto_crop)
    {
        auto cropped = crop_valid_rectangle(result.image, coverage, cancellation);
        if (!cropped)
            return cropped.error();
        result.origin_x = cropped.value().x;
        result.origin_y = cropped.value().y;
        result.image = std::move(cropped.value().image);
    }
    return result;
}
} // namespace ravo::photo_merge_internal
