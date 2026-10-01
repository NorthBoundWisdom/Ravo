#include "photo_registration.h"
#include "photo_merge_crop.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ravo::photo_merge_internal
{
namespace
{
Result<std::vector<int>> seam(const LinearWorkingBuffer &canvas, const std::vector<float> &added,
                              const std::vector<std::uint8_t> &old_mask,
                              const std::vector<std::uint8_t> &new_mask, const bool vertical,
                              const CancellationToken &cancellation)
{
    const int rows = int(vertical ? canvas.height : canvas.width);
    const int columns = int(vertical ? canvas.width : canvas.height);
    const auto pixel = [&](int row, int col)
    {
        return vertical ? std::size_t(row) * canvas.width + col :
                          std::size_t(col) * canvas.width + row;
    };
    int first = rows, last = -1, left = columns, right = -1;
    for (int row = 0; row < rows; ++row)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        for (int col = 0; col < columns; ++col)
        {
            const auto p = pixel(row, col);
            if (old_mask[p] && new_mask[p])
            {
                first = std::min(first, row);
                last = row;
                left = std::min(left, col);
                right = std::max(right, col);
            }
        }
    }
    if (last < first)
        return make_error(ErrorCode::kValidation, "Panorama frames do not overlap",
                          {{"reason", "panorama_no_overlap"}});
    const int width = right - left + 1;
    const float infinity = std::numeric_limits<float>::infinity();
    std::vector<float> previous(width, infinity), current(width, infinity);
    std::vector<std::int8_t> parents(std::size_t(last - first + 1) * width, 0);
    for (int row = first; row <= last; ++row)
    {
        if (auto active = cancellation.check(); !active)
            return active.error();
        std::fill(current.begin(), current.end(), infinity);
        for (int col = left; col <= right; ++col)
        {
            const auto p = pixel(row, col);
            if (!old_mask[p] || !new_mask[p])
                continue;
            float cost = .001F;
            for (unsigned c = 0; c < 3; ++c)
                cost += std::abs(canvas.rgb[p * 3 + c] - added[p * 3 + c]);
            // Encourage a seam away from the overlapping edge to leave room
            // for feathering. The dynamic path avoids high-disagreement pixels.
            cost += .01F / (1 + std::min(col - left, right - col));
            const int index = col - left;
            if (row == first)
            {
                current[index] = cost;
                continue;
            }
            int best = index;
            for (int k = std::max(0, index - 1); k <= std::min(width - 1, index + 1); ++k)
                if (previous[k] < previous[best])
                    best = k;
            current[index] = cost + previous[best];
            parents[std::size_t(row - first) * width + index] =
                static_cast<std::int8_t>(best - index);
        }
        previous.swap(current);
    }
    const auto best = std::min_element(previous.begin(), previous.end());
    if (!std::isfinite(*best))
        return make_error(ErrorCode::kValidation, "No continuous panorama seam",
                          {{"reason", "panorama_seam_failed"}});
    int col = int(best - previous.begin());
    std::vector<int> path(rows, -1);
    for (int row = last; row >= first; --row)
    {
        path[row] = col + left;
        col += parents[std::size_t(row - first) * width + col];
    }
    return path;
}
} // namespace

Result<PhotoMergeImage> stitch_panorama(const std::span<const LinearWorkingBuffer> frames,
                                        const PhotoMergeOptions &options,
                                        std::vector<PhotoMergeAlignment> alignments,
                                        const CancellationToken &cancellation)
{
    double minx = 0, miny = 0, maxx = frames.front().width - 1, maxy = frames.front().height - 1;
    std::uint64_t input_bytes = 0;
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        const auto &f = frames[i];
        const auto &m = alignments[i].source_to_reference;
        input_bytes += f.rgb.size() * sizeof(float);
        const std::array<Point, 4> corners{{{0, 0},
                                            {double(f.width - 1), 0},
                                            {double(f.width - 1), double(f.height - 1)},
                                            {0, double(f.height - 1)}}};
        std::array<Point, 4> warped{};
        double sign = 0, area = 0;
        for (std::size_t k = 0; k < 4; ++k)
        {
            const double z = m[6] * corners[k].x + m[7] * corners[k].y + m[8];
            if (std::abs(z) < 1e-6 || (k && z * sign <= 0))
                return make_error(ErrorCode::kValidation,
                                  "Panorama transform crosses its projection horizon",
                                  {{"reason", "panorama_unreliable_transform"}});
            sign = z;
            warped[k] = project(m, corners[k]);
            const auto p = warped[k];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 100000 ||
                std::abs(p.y) > 100000)
                return make_error(ErrorCode::kValidation,
                                  "Panorama transform exceeds bounded canvas",
                                  {{"reason", "panorama_unreliable_transform"}});
            minx = std::min(minx, p.x);
            miny = std::min(miny, p.y);
            maxx = std::max(maxx, p.x);
            maxy = std::max(maxy, p.y);
        }
        for (std::size_t k = 0; k < 4; ++k)
            area += warped[k].x * warped[(k + 1) % 4].y - warped[k].y * warped[(k + 1) % 4].x;
        const double ratio = area / (2.0 * f.width * f.height);
        if (ratio < .2 || ratio > 5)
            return make_error(ErrorCode::kValidation,
                              "Panorama transform has implausible scale or orientation",
                              {{"reason", "panorama_unreliable_transform"}});
    }
    const double ox = std::floor(minx), oy = std::floor(miny);
    const auto width = static_cast<std::uint32_t>(std::ceil(maxx) - ox + 1);
    const auto height = static_cast<std::uint32_t>(std::ceil(maxy) - oy + 1);
    const std::uint64_t pixels = std::uint64_t(width) * height;
    if (width > 50000 || height > 50000 || pixels > 80000000 ||
        input_bytes + pixels * 48 + 64ULL * 1024 * 1024 > options.memory_budget_bytes)
        return make_error(ErrorCode::kValidation, "Panorama canvas exceeds memory budget",
                          {{"reason", "merge_memory_budget_exceeded"}});
    PhotoMergeImage result;
    result.alignments = std::move(alignments);
    result.origin_x = ox;
    result.origin_y = oy;
    auto &canvas = result.image;
    canvas.width = width;
    canvas.height = height;
    canvas.color_profile = frames.front().color_profile;
    canvas.rgb.resize(pixels * 3, 0);
    std::vector<std::uint8_t> mask(pixels, 0), new_mask(pixels, 0);
    std::vector<float> added(pixels * 3, 0);
    std::vector<bool> rendered(frames.size(), false);
    for (std::size_t count = 0; count < frames.size(); ++count)
    {
        std::size_t index = 0;
        if (count)
        {
            index = 1;
            while (index < frames.size() &&
                   (rendered[index] || !rendered[result.alignments[index].reference_frame]))
                ++index;
            if (index == frames.size())
                return make_error(ErrorCode::kInternal, "Invalid panorama overlap tree");
        }
        const auto &f = frames[index];
        const auto &m = result.alignments[index].source_to_reference;
        auto inv = inverse(m);
        if (!inv)
            return inv.error();
        std::fill(new_mask.begin(), new_mask.end(), 0);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            if (auto active = cancellation.check(); !active)
                return active.error();
            for (std::uint32_t x = 0; x < width; ++x)
            {
                std::array<float, 3> color{};
                if (!sample(f, project(inv.value(), {x + ox, y + oy}), color))
                    continue;
                const auto p = std::size_t(y) * width + x;
                new_mask[p] = 1;
                std::copy(color.begin(), color.end(), added.begin() + p * 3);
            }
        }
        const Point center = project(m, {f.width / 2.0, f.height / 2.0});
        if (!count)
        {
            canvas.rgb = added;
            mask = new_mask;
            rendered[index] = true;
            continue;
        }
        // Estimate one scalar exposure compensation on safe overlapping luma;
        // never change channel ratios or independently balance colour channels.
        std::vector<float> ratios;
        for (std::size_t p = 0; p < pixels; p += 16)
            if (mask[p] && new_mask[p])
            {
                const float a = .2126F * canvas.rgb[p * 3] + .7152F * canvas.rgb[p * 3 + 1] +
                                .0722F * canvas.rgb[p * 3 + 2];
                const float b =
                    .2126F * added[p * 3] + .7152F * added[p * 3 + 1] + .0722F * added[p * 3 + 2];
                if (a > .02F && b > .02F && a < .9F && b < .9F)
                    ratios.push_back(a / b);
            }
        float gain = 1;
        if (ratios.size() >= 16)
        {
            auto mid = ratios.begin() + ratios.size() / 2;
            std::nth_element(ratios.begin(), mid, ratios.end());
            gain = *mid;
            if (gain < .25F || gain > 4.F)
                return make_error(ErrorCode::kValidation,
                                  "Panorama exposure difference is too large",
                                  {{"reason", "panorama_exposure_mismatch"}});
        }
        for (auto &v : added)
            v *= gain;
        const auto parent = result.alignments[index].reference_frame;
        const auto parent_center =
            project(result.alignments[parent].source_to_reference,
                    {frames[parent].width / 2.0, frames[parent].height / 2.0});
        const bool vertical =
            std::abs(center.x - parent_center.x) >= std::abs(center.y - parent_center.y);
        const bool dominant = vertical ? center.x > parent_center.x : center.y > parent_center.y;
        auto path = seam(canvas, added, mask, new_mask, vertical, cancellation);
        if (!path)
            return path.error();
        for (std::uint32_t y = 0; y < height; ++y)
        {
            if (auto active = cancellation.check(); !active)
                return active.error();
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const auto p = std::size_t(y) * width + x;
                if (!new_mask[p])
                    continue;
                float alpha = 1;
                if (mask[p])
                {
                    const auto row = vertical ? y : x;
                    const int cut = path.value()[row];
                    if (cut < 0)
                        return make_error(ErrorCode::kValidation,
                                          "Panorama overlap is outside its seam",
                                          {{"reason", "panorama_seam_failed"}});
                    const float distance = float(vertical ? x : y) - cut;
                    alpha = std::clamp(.5F + (dominant ? distance : -distance) / 32.F, 0.F, 1.F);
                }
                for (unsigned c = 0; c < 3; ++c)
                    canvas.rgb[p * 3 + c] =
                        (1 - alpha) * canvas.rgb[p * 3 + c] + alpha * added[p * 3 + c];
                mask[p] = 1;
            }
        }
        rendered[index] = true;
    }
    if (options.auto_crop)
    {
        auto cropped = crop_valid_rectangle(canvas, mask, cancellation);
        if (!cropped)
            return cropped.error();
        result.origin_x += cropped.value().x;
        result.origin_y += cropped.value().y;
        canvas = std::move(cropped.value().image);
    }
    return result;
}
} // namespace ravo::photo_merge_internal
