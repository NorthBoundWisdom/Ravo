#include "dehaze.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

constexpr std::uint32_t kGuidedTileSize = 512U;
constexpr float kDarkChannelQuantile = 0.95F;
constexpr float kBrightQuantile = 0.95F;

struct FrozenDehazeData
{
    float strength = 0.2F;
    float distance = 0.2F;
    bool adaptive = true;
};

struct AmbientLight
{
    std::array<float, 3> rgb{};
    float distance_max = 0.0F;
};

struct Tile
{
    std::uint32_t left = 0U;
    std::uint32_t right = 0U;
    std::uint32_t top = 0U;
    std::uint32_t bottom = 0U;
};

[[nodiscard]] FrozenDehazeData commit_dehaze(const DehazeParams &params) noexcept
{
    return {static_cast<float>(params.strength), static_cast<float>(params.distance),
            params.adaptive};
}

void checkpoint(const detail::DehazeControl &control, const detail::DehazeCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
    {
        control.checkpoint_callback(control.context, stage, progress);
    }
}

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    return left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left ?
               std::numeric_limits<std::uint64_t>::max() :
               left + right;
}

[[nodiscard]] std::uint32_t round_up_16(const std::uint32_t value) noexcept
{
    return (value + 15U) & ~15U;
}

void kahan_add(float value, float &sum, float &compensation) noexcept
{
    const float adjusted = value - compensation;
    const float next = sum + adjusted;
    compensation = (next - sum) - adjusted;
    sum = next;
}

void box_mean_line(const float *source, const std::size_t source_stride, float *destination,
                   const std::size_t destination_stride, const std::uint32_t length,
                   const std::uint32_t radius)
{
    float sum = 0.0F;
    float compensation = 0.0F;
    std::uint32_t hits = 0U;
    for (std::uint32_t position = 0U; position < std::min(radius, length); ++position)
    {
        ++hits;
        kahan_add(source[static_cast<std::size_t>(position) * source_stride], sum, compensation);
    }
    std::uint32_t position = 0U;
    for (; position <= radius && position + radius < length; ++position)
    {
        const std::uint32_t added = position + radius;
        ++hits;
        kahan_add(source[static_cast<std::size_t>(added) * source_stride], sum, compensation);
        destination[static_cast<std::size_t>(position) * destination_stride] =
            sum / static_cast<float>(hits);
    }
    for (; position <= radius && position < length; ++position)
    {
        destination[static_cast<std::size_t>(position) * destination_stride] =
            sum / static_cast<float>(hits);
    }
    for (; position + radius < length; ++position)
    {
        const std::uint32_t removed = position - radius - 1U;
        const std::uint32_t added = position + radius;
        kahan_add(-source[static_cast<std::size_t>(removed) * source_stride], sum, compensation);
        kahan_add(source[static_cast<std::size_t>(added) * source_stride], sum, compensation);
        destination[static_cast<std::size_t>(position) * destination_stride] =
            sum / static_cast<float>(hits);
    }
    for (; position < length; ++position)
    {
        const std::uint32_t removed = position - radius - 1U;
        --hits;
        kahan_add(-source[static_cast<std::size_t>(removed) * source_stride], sum, compensation);
        destination[static_cast<std::size_t>(position) * destination_stride] =
            sum / static_cast<float>(hits);
    }
}

[[nodiscard]] Result<void> box_mean_interleaved(std::vector<float> &data, const std::uint32_t width,
                                                const std::uint32_t height,
                                                const std::uint32_t channels,
                                                const std::uint32_t radius,
                                                const CancellationToken &cancellation,
                                                const detail::DehazeControl &control)
{
    std::vector<float> horizontal(data.size());
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        checkpoint(control, detail::DehazeCheckpoint::kGuidedStatisticsRow, row);
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t channel = 0U; channel < channels; ++channel)
        {
            const std::size_t base = static_cast<std::size_t>(row) * width * channels + channel;
            box_mean_line(data.data() + base, channels, horizontal.data() + base, channels, width,
                          radius);
        }
    }
    for (std::uint32_t column = 0U; column < width; ++column)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t channel = 0U; channel < channels; ++channel)
        {
            const std::size_t base = static_cast<std::size_t>(column) * channels + channel;
            box_mean_line(horizontal.data() + base, static_cast<std::size_t>(width) * channels,
                          data.data() + base, static_cast<std::size_t>(width) * channels, height,
                          radius);
        }
    }
    return {};
}

template <typename Comparator>
[[nodiscard]] Result<void> box_extreme(std::vector<float> &plane, const std::uint32_t width,
                                       const std::uint32_t height, const std::uint32_t radius,
                                       Comparator compare, const CancellationToken &cancellation,
                                       const detail::DehazeControl &control, const bool report_rows)
{
    std::vector<float> horizontal(plane.size());
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        if (report_rows)
        {
            checkpoint(control, detail::DehazeCheckpoint::kDarkChannelRow, row);
        }
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::uint32_t first = column > radius ? column - radius : 0U;
            const std::uint32_t last = std::min(width - 1U, column + radius);
            float value = plane[static_cast<std::size_t>(row) * width + first];
            for (std::uint32_t source = first + 1U; source <= last; ++source)
            {
                value = compare(value, plane[static_cast<std::size_t>(row) * width + source]);
            }
            horizontal[static_cast<std::size_t>(row) * width + column] = value;
        }
    }
    for (std::uint32_t column = 0U; column < width; ++column)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t row = 0U; row < height; ++row)
        {
            const std::uint32_t first = row > radius ? row - radius : 0U;
            const std::uint32_t last = std::min(height - 1U, row + radius);
            float value = horizontal[static_cast<std::size_t>(first) * width + column];
            for (std::uint32_t source = first + 1U; source <= last; ++source)
            {
                value =
                    compare(value, horizontal[static_cast<std::size_t>(source) * width + column]);
            }
            plane[static_cast<std::size_t>(row) * width + column] = value;
        }
    }
    return {};
}

[[nodiscard]] Result<AmbientLight> ambient_light(const WorkingImage &input,
                                                 const std::uint32_t radius,
                                                 const CancellationToken &cancellation,
                                                 const detail::DehazeControl &control)
{
    const std::size_t count = static_cast<std::size_t>(input.width) * input.height;
    std::vector<float> dark(count);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, detail::DehazeCheckpoint::kDarkChannelRow, row);
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * input.width + column;
            const std::size_t rgb = pixel * 3U;
            dark[pixel] =
                std::fmin(input.rgb[rgb], std::fmin(input.rgb[rgb + 1U], input.rgb[rgb + 2U]));
        }
    }
    auto filtered = box_extreme(
        dark, input.width, input.height, radius, [](const float left, const float right)
        { return std::fmin(left, right); }, cancellation, control, false);
    if (!filtered)
    {
        return filtered.error();
    }
    checkpoint(control, detail::DehazeCheckpoint::kAmbientSelection, 0U);
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    std::vector<float> ordered = dark;
    const std::size_t dark_index =
        static_cast<std::size_t>(static_cast<float>(count) * kDarkChannelQuantile);
    std::nth_element(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(dark_index),
                     ordered.end());
    const float critical_haze = ordered[dark_index];
    std::vector<float> hazy_brightness;
    hazy_brightness.reserve(count - dark_index + 1U);
    for (std::size_t pixel = 0U; pixel < count; ++pixel)
    {
        if (dark[pixel] >= critical_haze)
        {
            const std::size_t rgb = pixel * 3U;
            hazy_brightness.push_back(input.rgb[rgb] + input.rgb[rgb + 1U] + input.rgb[rgb + 2U]);
        }
    }
    if (hazy_brightness.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Dehaze could not select a hazy ambient-light population",
                          {{"reason", "invalid_dehaze_ambient_population"}});
    }
    const std::size_t bright_index =
        static_cast<std::size_t>(static_cast<float>(hazy_brightness.size()) * kBrightQuantile);
    std::nth_element(hazy_brightness.begin(),
                     hazy_brightness.begin() + static_cast<std::ptrdiff_t>(bright_index),
                     hazy_brightness.end());
    const float critical_brightness = hazy_brightness[bright_index];
    AmbientLight result;
    std::size_t selected = 0U;
    for (std::size_t pixel = 0U; pixel < count; ++pixel)
    {
        const std::size_t rgb = pixel * 3U;
        const float brightness = input.rgb[rgb] + input.rgb[rgb + 1U] + input.rgb[rgb + 2U];
        if (dark[pixel] >= critical_haze && brightness >= critical_brightness)
        {
            result.rgb[0] += input.rgb[rgb];
            result.rgb[1] += input.rgb[rgb + 1U];
            result.rgb[2] += input.rgb[rgb + 2U];
            ++selected;
        }
    }
    if (selected == 0U)
    {
        return make_error(ErrorCode::kValidation, "Dehaze ambient-light selection is empty",
                          {{"reason", "invalid_dehaze_ambient_population"}});
    }
    for (float &channel : result.rgb)
    {
        channel /= static_cast<float>(selected);
        if (!std::isfinite(channel) || channel == 0.0F)
        {
            return make_error(ErrorCode::kValidation, "Dehaze ambient light is invalid",
                              {{"reason", "invalid_dehaze_ambient_light"}});
        }
    }
    result.distance_max = critical_haze > 0.0F ? -1.125F * std::log(critical_haze) :
                                                 std::log(std::numeric_limits<float>::max()) / 2.0F;
    if (!std::isfinite(result.distance_max))
    {
        return make_error(ErrorCode::kValidation, "Dehaze distance estimate is non-finite",
                          {{"reason", "invalid_dehaze_distance_estimate"}});
    }
    return result;
}

[[nodiscard]] Result<void>
guided_filter_tile(const WorkingImage &guide, const std::vector<float> &input,
                   std::vector<float> &output, const Tile &target, const std::uint32_t radius,
                   const CancellationToken &cancellation, const detail::DehazeControl &control)
{
    const std::uint32_t overlap = round_up_16(3U * radius);
    const Tile source{target.left > overlap ? target.left - overlap : 0U,
                      std::min(guide.width, target.right + overlap),
                      target.top > overlap ? target.top - overlap : 0U,
                      std::min(guide.height, target.bottom + overlap)};
    const std::uint32_t width = source.right - source.left;
    const std::uint32_t height = source.bottom - source.top;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    std::vector<float> mean(pixels * 4U);
    std::vector<float> variance(pixels * 9U);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        checkpoint(control, detail::DehazeCheckpoint::kGuidedStatisticsRow, row);
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::uint32_t source_row = source.top + row;
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::uint32_t source_column = source.left + column;
            const std::size_t global =
                static_cast<std::size_t>(source_row) * guide.width + source_column;
            const std::size_t local = static_cast<std::size_t>(row) * width + column;
            const float red = guide.rgb[global * 3U];
            const float green = guide.rgb[global * 3U + 1U];
            const float blue = guide.rgb[global * 3U + 2U];
            const float value = input[global];
            mean[local * 4U] = value;
            mean[local * 4U + 1U] = red;
            mean[local * 4U + 2U] = green;
            mean[local * 4U + 3U] = blue;
            variance[local * 9U] = red * value;
            variance[local * 9U + 1U] = green * value;
            variance[local * 9U + 2U] = blue * value;
            variance[local * 9U + 3U] = red * red;
            variance[local * 9U + 4U] = red * green;
            variance[local * 9U + 5U] = red * blue;
            variance[local * 9U + 6U] = green * green;
            variance[local * 9U + 7U] = green * blue;
            variance[local * 9U + 8U] = blue * blue;
        }
    }
    auto averaged = box_mean_interleaved(mean, width, height, 4U, radius, cancellation, control);
    if (!averaged)
    {
        return averaged.error();
    }
    const float sqrt_epsilon = std::sqrt(0.025F);
    const float epsilon = sqrt_epsilon * sqrt_epsilon;
    averaged = box_mean_interleaved(variance, width, height, 9U, radius, cancellation, control);
    if (!averaged)
    {
        return averaged.error();
    }
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        checkpoint(control, detail::DehazeCheckpoint::kGuidedSolveRow, row);
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * width + column;
            const float input_mean = mean[pixel * 4U];
            const float red = mean[pixel * 4U + 1U];
            const float green = mean[pixel * 4U + 2U];
            const float blue = mean[pixel * 4U + 3U];
            const float s00 = variance[pixel * 9U + 3U] - red * red + epsilon;
            const float s01 = variance[pixel * 9U + 4U] - red * green;
            const float s02 = variance[pixel * 9U + 5U] - red * blue;
            const float s11 = variance[pixel * 9U + 6U] - green * green + epsilon;
            const float s12 = variance[pixel * 9U + 7U] - green * blue;
            const float s22 = variance[pixel * 9U + 8U] - blue * blue + epsilon;
            const float det0 = s00 * (s11 * s22 - s12 * s12) - s01 * (s01 * s22 - s02 * s12) +
                               s02 * (s01 * s12 - s02 * s11);
            float ar = 0.0F;
            float ag = 0.0F;
            float ab = 0.0F;
            float b = input_mean;
            if (std::fabs(det0) > 4.0F * std::numeric_limits<float>::epsilon())
            {
                const float cr = variance[pixel * 9U] - red * input_mean;
                const float cg = variance[pixel * 9U + 1U] - green * input_mean;
                const float cb = variance[pixel * 9U + 2U] - blue * input_mean;
                const float det1 = cr * (s11 * s22 - s12 * s12) - s01 * (cg * s22 - cb * s12) +
                                   s02 * (cg * s12 - cb * s11);
                const float det2 = s00 * (cg * s22 - cb * s12) - cr * (s01 * s22 - s02 * s12) +
                                   s02 * (s01 * cb - s02 * cg);
                const float det3 = s00 * (s11 * cb - s12 * cg) - s01 * (s01 * cb - s02 * cg) +
                                   cr * (s01 * s12 - s02 * s11);
                ar = det1 / det0;
                ag = det2 / det0;
                ab = det3 / det0;
                b = input_mean - ar * red - ag * green - ab * blue;
            }
            mean[pixel * 4U] = ar;
            mean[pixel * 4U + 1U] = ag;
            mean[pixel * 4U + 2U] = ab;
            mean[pixel * 4U + 3U] = b;
        }
    }
    averaged = box_mean_interleaved(mean, width, height, 4U, radius, cancellation, control);
    if (!averaged)
    {
        return averaged.error();
    }
    for (std::uint32_t row = target.top; row < target.bottom; ++row)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = target.left; column < target.right; ++column)
        {
            const std::size_t global = static_cast<std::size_t>(row) * guide.width + column;
            const std::size_t local =
                static_cast<std::size_t>(row - source.top) * width + (column - source.left);
            const float result = mean[local * 4U] * guide.rgb[global * 3U] +
                                 mean[local * 4U + 1U] * guide.rgb[global * 3U + 1U] +
                                 mean[local * 4U + 2U] * guide.rgb[global * 3U + 2U] +
                                 mean[local * 4U + 3U];
            if (!std::isfinite(result))
            {
                return make_error(ErrorCode::kValidation,
                                  "Dehaze guided filter produced a non-finite value",
                                  {{"pixel", std::to_string(global)},
                                   {"reason", "nonfinite_dehaze_guided_output"}});
            }
            output[global] = result;
        }
    }
    return {};
}

[[nodiscard]] Result<void> guided_filter(const WorkingImage &guide, const std::vector<float> &input,
                                         std::vector<float> &output, const std::uint32_t radius,
                                         const CancellationToken &cancellation,
                                         const detail::DehazeControl &control)
{
    for (std::uint32_t top = 0U; top < guide.height; top += kGuidedTileSize)
    {
        for (std::uint32_t left = 0U; left < guide.width; left += kGuidedTileSize)
        {
            const std::uint32_t tile_index =
                (top / kGuidedTileSize) * ((guide.width + kGuidedTileSize - 1U) / kGuidedTileSize) +
                left / kGuidedTileSize;
            checkpoint(control, detail::DehazeCheckpoint::kGuidedTile, tile_index);
            auto active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
            const Tile target{left, std::min(guide.width, left + kGuidedTileSize), top,
                              std::min(guide.height, top + kGuidedTileSize)};
            auto filtered =
                guided_filter_tile(guide, input, output, target, radius, cancellation, control);
            if (!filtered)
            {
                return filtered.error();
            }
        }
    }
    return {};
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input)
{
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Dehaze input dimensions must be non-zero",
                          {{"reason", "invalid_dehaze_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Dehaze input buffer does not match its dimensions",
                          {{"reason", "invalid_dehaze_buffer"}});
    }
    const bool source_linear = input.color_profile.identifier == kInputProfileEmbeddedMatrix ||
                               input.color_profile.identifier == kInputProfileStandardMatrix ||
                               input.color_profile.identifier == kInputProfileEnhancedMatrix ||
                               input.color_profile.identifier == kInputProfileVendorMatrix ||
                               input.color_profile.identifier == kInputProfileAlternateMatrix ||
                               input.color_profile.identifier == kInputProfileLinearRec709 ||
                               input.color_profile.identifier == kInputProfileLinearRec2020;
    if (input.color_profile.model != ColorModel::kRgb || !source_linear)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Dehaze requires declared source-linear RGB pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_dehaze_working_space"}});
    }
    return {};
}

} // namespace

std::uint64_t detail::dehaze_working_bytes(const std::uint32_t width, const std::uint32_t height,
                                           const float canonical_scale,
                                           const DehazeParams &params) noexcept
{
    const auto bounded = [](const double value, const double minimum, const double maximum) noexcept
    {
        return std::isfinite(value) && std::isfinite(static_cast<float>(value)) &&
               value >= minimum && value <= maximum;
    };
    if (width == 0U || height == 0U ||
        !bounded(params.strength, kDehazeStrengthMin, kDehazeStrengthMax) ||
        !bounded(params.distance, kDehazeDistanceMin, kDehazeDistanceMax) ||
        (params.adaptive && (!std::isfinite(canonical_scale) || canonical_scale <= 0.0F)))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const float scale = params.adaptive ? std::clamp(canonical_scale, 0.0F, 1.0F) : 1.0F;
    const std::uint32_t guided_radius = 3U + static_cast<std::uint32_t>(std::ceil(6.0F * scale));
    const std::uint32_t overlap = round_up_16(3U * guided_radius);
    const std::uint32_t tile_width = std::min(width, kGuidedTileSize + 2U * overlap);
    const std::uint32_t tile_height = std::min(height, kGuidedTileSize + 2U * overlap);
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    const std::uint64_t tile_pixels = static_cast<std::uint64_t>(tile_width) * tile_height;
    const std::uint64_t full_planes = saturating_multiply(pixels, 2U * sizeof(float));
    const std::uint64_t tile_planes = saturating_multiply(tile_pixels, 22U * sizeof(float));
    return saturating_add(full_planes, tile_planes);
}

Result<WorkingImage> detail::apply_dehaze_controlled(const WorkingImage &input,
                                                     const DehazeParams &params,
                                                     const CancellationToken &cancellation,
                                                     const DehazeControl control,
                                                     DehazeAnalysis *const analysis)
try
{
    checkpoint(control, DehazeCheckpoint::kBeforeValidation, 0U);
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto canonical = dehaze_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    auto valid = validate_input(input);
    if (!valid)
    {
        return valid.error();
    }
    const FrozenDehazeData data = commit_dehaze(params);
    if (data.strength == 0.0F)
    {
        if (analysis != nullptr)
        {
            *analysis = {};
        }
        return input;
    }
    if (data.adaptive && !input.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kValidation,
                          "Adaptive Dehaze requires a canonical full-frame ROI scale",
                          {{"reason", "invalid_dehaze_roi_scale"}});
    }
    for (std::size_t index = 0U; index < input.rgb.size(); ++index)
    {
        if (!std::isfinite(input.rgb[index]))
        {
            return make_error(
                ErrorCode::kValidation, "Dehaze input contains a non-finite RGB sample",
                {{"sample_index", std::to_string(index)}, {"reason", "nonfinite_dehaze_input"}});
        }
    }
    const float window_scale =
        data.adaptive ? std::clamp(input.canonical_roi_scale.value(), 0.0F, 1.0F) : 1.0F;
    const std::uint32_t dark_radius =
        2U + static_cast<std::uint32_t>(std::ceil(4.0F * window_scale));
    const std::uint32_t guided_radius =
        3U + static_cast<std::uint32_t>(std::ceil(6.0F * window_scale));
    auto ambient = ambient_light(input, dark_radius, cancellation, control);
    if (!ambient)
    {
        return ambient.error();
    }
    if (analysis != nullptr)
    {
        *analysis = {ambient.value().rgb, ambient.value().distance_max,
                     static_cast<int>(dark_radius), static_cast<int>(guided_radius)};
    }
    const std::size_t pixels = static_cast<std::size_t>(input.width) * input.height;
    std::vector<float> transition(pixels);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, DehazeCheckpoint::kTransitionRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * input.width + column;
            const std::size_t rgb = pixel * 3U;
            const float minimum =
                std::fmin(input.rgb[rgb] / ambient.value().rgb[0],
                          std::fmin(input.rgb[rgb + 1U] / ambient.value().rgb[1],
                                    input.rgb[rgb + 2U] / ambient.value().rgb[2]));
            transition[pixel] = 1.0F - minimum * data.strength;
        }
    }
    auto extreme = box_extreme(
        transition, input.width, input.height, dark_radius, [](const float left, const float right)
        { return std::fmax(left, right); }, cancellation, control, false);
    if (!extreme)
    {
        return extreme.error();
    }
    extreme = box_extreme(
        transition, input.width, input.height, dark_radius, [](const float left, const float right)
        { return std::fmin(left, right); }, cancellation, control, false);
    if (!extreme)
    {
        return extreme.error();
    }
    std::vector<float> filtered(pixels);
    auto guided = guided_filter(input, transition, filtered, guided_radius, cancellation, control);
    if (!guided)
    {
        return guided.error();
    }
    const float minimum_transition =
        std::clamp(std::exp(-data.distance * ambient.value().distance_max), 1.0F / 1024.0F, 1.0F);
    if (!std::isfinite(minimum_transition))
    {
        return make_error(ErrorCode::kValidation, "Dehaze minimum transition is non-finite",
                          {{"reason", "nonfinite_dehaze_transition"}});
    }
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, DehazeCheckpoint::kOutputRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * input.width + column;
            const std::size_t rgb = pixel * 3U;
            const float transmission = std::fmax(filtered[pixel], minimum_transition);
            if (!std::isfinite(transmission) || transmission == 0.0F)
            {
                return make_error(
                    ErrorCode::kValidation, "Dehaze transition map contains an invalid value",
                    {{"pixel", std::to_string(pixel)}, {"reason", "nonfinite_dehaze_transition"}});
            }
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float value =
                    (input.rgb[rgb + channel] - ambient.value().rgb[channel]) / transmission +
                    ambient.value().rgb[channel];
                if (!std::isfinite(value))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Dehaze produced a non-finite RGB sample",
                                      {{"sample_index", std::to_string(rgb + channel)},
                                       {"reason", "nonfinite_dehaze_output"}});
                }
                output.rgb[rgb + channel] = value;
            }
        }
    }
    checkpoint(control, DehazeCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Dehaze allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_dehaze(const WorkingImage &input, const DehazeParams &params,
                                  const CancellationToken &cancellation)
{
    return detail::apply_dehaze_controlled(input, params, cancellation, {}, nullptr);
}

Result<WorkingImage> apply_dehaze(const WorkingImage &input, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kDehazeOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Dehaze",
                          {{"operation_id", operation.id}});
    }
    OperationInstance canonical = operation;
    auto upgraded = upgrade_dehaze_operation(canonical);
    if (!upgraded)
    {
        return upgraded.error();
    }
    if (canonical.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Dehaze mask evaluation is unavailable",
            {{"operation_id", canonical.id}, {"reason", "dehaze_mask_graph_unavailable"}});
    }
    if (!canonical.enabled)
    {
        return input;
    }
    auto params = dehaze_from_parameters(canonical.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_dehaze(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Dehaze operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
