#include "guided_filter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

#include "parallel_rows.h"

namespace ravo::detail
{
namespace
{

[[nodiscard]] bool valid_plane(const std::vector<float> &plane, const std::uint32_t width,
                               const std::uint32_t height) noexcept
{
    return width != 0U && height != 0U &&
           static_cast<std::uint64_t>(width) * height == plane.size();
}

[[nodiscard]] constexpr std::uint64_t saturating_multiply(const std::uint64_t left,
                                                          const std::uint64_t right) noexcept
{
    return left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

} // namespace

std::uint64_t guided_filter_additional_bytes(const std::uint32_t width,
                                             const std::uint32_t height) noexcept
{
    return saturating_multiply(static_cast<std::uint64_t>(width) * height, 4U * sizeof(float));
}

Result<void> box_blur_plane(const std::vector<float> &input, std::vector<float> &output,
                            const std::uint32_t width, const std::uint32_t height, const int radius,
                            const CancellationToken &cancellation)
try
{
    if (!valid_plane(input, width, height) || radius < 0 ||
        radius > (std::numeric_limits<int>::max() - 1) / 2)
    {
        return make_error(ErrorCode::kValidation, "Guided-filter box blur input is invalid",
                          {{"reason", "invalid_guided_filter_box_input"}});
    }
    output.assign(input.size(), 0.0F);
    if (radius == 0)
    {
        output = input;
        return {};
    }
    const int window = radius * 2 + 1;
    std::vector<float> temporary(input.size());
    auto horizontal = for_each_row(
        height, cancellation,
        [&](const std::uint32_t y)
        {
            double accumulator = 0.0;
            for (int x = -radius; x <= radius; ++x)
            {
                const int sample_x = std::clamp(x, 0, static_cast<int>(width) - 1);
                accumulator += input[static_cast<std::size_t>(y) * width +
                                     static_cast<std::uint32_t>(sample_x)];
            }
            for (std::uint32_t x = 0U; x < width; ++x)
            {
                temporary[static_cast<std::size_t>(y) * width + x] =
                    static_cast<float>(accumulator / window);
                const int drop =
                    std::clamp(static_cast<int>(x) - radius, 0, static_cast<int>(width) - 1);
                const int add =
                    std::clamp(static_cast<int>(x) + radius + 1, 0, static_cast<int>(width) - 1);
                accumulator +=
                    input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(add)] -
                    input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(drop)];
            }
        });
    if (!horizontal)
    {
        return horizontal.error();
    }

    auto vertical = for_each_row(
        width, cancellation,
        [&](const std::uint32_t x)
        {
            double accumulator = 0.0;
            for (int y = -radius; y <= radius; ++y)
            {
                const int sample_y = std::clamp(y, 0, static_cast<int>(height) - 1);
                accumulator += temporary[static_cast<std::size_t>(sample_y) * width + x];
            }
            for (std::uint32_t y = 0U; y < height; ++y)
            {
                output[static_cast<std::size_t>(y) * width + x] =
                    static_cast<float>(accumulator / window);
                const int drop =
                    std::clamp(static_cast<int>(y) - radius, 0, static_cast<int>(height) - 1);
                const int add =
                    std::clamp(static_cast<int>(y) + radius + 1, 0, static_cast<int>(height) - 1);
                accumulator += temporary[static_cast<std::size_t>(add) * width + x] -
                               temporary[static_cast<std::size_t>(drop) * width + x];
            }
        });
    return vertical ? Result<void>{} : vertical.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Guided-filter box blur allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> self_guided_filter_plane(std::vector<float> &plane, const std::uint32_t width,
                                      const std::uint32_t height, const int radius,
                                      const float epsilon, const CancellationToken &cancellation)
try
{
    if (!valid_plane(plane, width, height) || radius < 0 || !std::isfinite(epsilon) ||
        epsilon <= 0.0F)
    {
        return make_error(ErrorCode::kValidation, "Self-guided filter input is invalid",
                          {{"reason", "invalid_self_guided_filter_input"}});
    }
    if (radius == 0)
    {
        return {};
    }
    const std::size_t count = plane.size();
    std::vector<float> mean;
    std::vector<float> correlation;
    if (auto blurred = box_blur_plane(plane, mean, width, height, radius, cancellation); !blurred)
    {
        return blurred.error();
    }
    {
        std::vector<float> squared(count);
        auto products = for_each_row(height, cancellation,
                                     [&](const std::uint32_t row)
                                     {
                                         const std::size_t begin =
                                             static_cast<std::size_t>(row) * width;
                                         const std::size_t end = begin + width;
                                         for (std::size_t index = begin; index < end; ++index)
                                         {
                                             squared[index] = plane[index] * plane[index];
                                         }
                                     });
        if (!products)
        {
            return products.error();
        }
        if (auto blurred =
                box_blur_plane(squared, correlation, width, height, radius, cancellation);
            !blurred)
        {
            return blurred.error();
        }
    }
    auto coefficients = for_each_row(height, cancellation,
                                     [&](const std::uint32_t row)
                                     {
                                         const std::size_t begin =
                                             static_cast<std::size_t>(row) * width;
                                         const std::size_t end = begin + width;
                                         for (std::size_t index = begin; index < end; ++index)
                                         {
                                             const float covariance =
                                                 correlation[index] - mean[index] * mean[index];
                                             const float variance = std::max(0.0F, covariance);
                                             correlation[index] = variance / (variance + epsilon);
                                             mean[index] -= correlation[index] * mean[index];
                                         }
                                     });
    if (!coefficients)
    {
        return coefficients.error();
    }
    std::vector<float> mean_a;
    if (auto blurred = box_blur_plane(correlation, mean_a, width, height, radius, cancellation);
        !blurred)
    {
        return blurred.error();
    }
    if (auto blurred = box_blur_plane(mean, correlation, width, height, radius, cancellation);
        !blurred)
    {
        return blurred.error();
    }
    return for_each_row(height, cancellation,
                        [&](const std::uint32_t row)
                        {
                            const std::size_t begin = static_cast<std::size_t>(row) * width;
                            const std::size_t end = begin + width;
                            for (std::size_t index = begin; index < end; ++index)
                            {
                                plane[index] = mean_a[index] * plane[index] + correlation[index];
                            }
                        });
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Self-guided filter allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo::detail
