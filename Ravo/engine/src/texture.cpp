#include "texture.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "guided_filter.h"
#include "parallel_rows.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

constexpr float kGuideEpsilon = 0.001F;
constexpr float kMinimumGuideLuminance = 1.0e-5F;
constexpr float kMaximumGuideLuminance = 32.0F;
constexpr float kScaleLuminanceFloor = 1.0e-8F;

void checkpoint(const detail::TextureControl &control,
                const detail::TextureCheckpoint checkpoint_value,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
    {
        control.checkpoint_callback(control.context, checkpoint_value, progress);
    }
}

[[nodiscard]] float luminance(const float red, const float green, const float blue) noexcept
{
    return 0.2126F * red + 0.7152F * green + 0.0722F * blue;
}

[[nodiscard]] Result<void> validate_working_input(const WorkingImage &input)
{
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Texture input dimensions must be non-zero",
                          {{"reason", "invalid_texture_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Texture input buffer does not match its dimensions",
                          {{"reason", "invalid_texture_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Texture requires declared linear Rec.709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_texture_working_space"}});
    }
    if (!input.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kValidation, "Texture requires canonical ROI scale",
                          {{"reason", "invalid_texture_roi_scale"}});
    }
    return {};
}

[[nodiscard]] int scaled_radius(const WorkingImage &input, const TextureParams &params) noexcept
{
    const auto maximum_dimension = std::max(input.width, input.height);
    if (maximum_dimension <= 1U)
    {
        return 0;
    }
    const int maximum_radius = static_cast<int>(std::min<std::uint32_t>(
        maximum_dimension - 1U, static_cast<std::uint32_t>(std::numeric_limits<int>::max())));
    const double radius = 3.5 * params.detail_threshold * input.canonical_roi_scale.value();
    if (!std::isfinite(radius) || radius >= static_cast<double>(maximum_radius))
    {
        return maximum_radius;
    }
    return std::max(1, static_cast<int>(std::lround(radius)));
}

[[nodiscard]] int coarse_radius(const int fine_radius, const WorkingImage &input) noexcept
{
    const auto maximum_dimension = std::max(input.width, input.height);
    if (maximum_dimension <= 1U)
    {
        return 0;
    }
    const std::uint64_t maximum_radius = std::min<std::uint64_t>(
        maximum_dimension - 1U, static_cast<std::uint64_t>(std::numeric_limits<int>::max()));
    return static_cast<int>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(fine_radius) * 4U, maximum_radius));
}

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    return left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

} // namespace

std::uint64_t detail::texture_working_bytes(const std::uint32_t width, const std::uint32_t height,
                                            const TextureParams &params) noexcept
{
    if (width == 0U || height == 0U || !std::isfinite(params.strength) ||
        params.strength < kTextureStrengthMin || params.strength > kTextureStrengthMax ||
        !std::isfinite(params.detail_threshold) ||
        params.detail_threshold < kTextureDetailThresholdMin ||
        params.detail_threshold > kTextureDetailThresholdMax ||
        params.iterations < kTextureIterationsMin || params.iterations > kTextureIterationsMax)
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    if (std::abs(params.strength) <= std::numeric_limits<double>::epsilon())
    {
        return 0U;
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    // Middle and base are caller-owned planes. The shared self-guided filter
    // adds four live planes at its peak; the normal render estimate already
    // owns both borrowed and published RGB buffers.
    return saturating_multiply(pixels, 6U * sizeof(float));
}

Result<WorkingImage> detail::apply_texture_controlled(const WorkingImage &input,
                                                      const TextureParams &params,
                                                      const CancellationToken &cancellation,
                                                      const TextureControl control)
try
{
    checkpoint(control, TextureCheckpoint::kBeforeValidation, 0U);
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto canonical = texture_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    auto valid = validate_working_input(input);
    if (!valid)
    {
        return valid.error();
    }
    if (std::abs(params.strength) <= std::numeric_limits<double>::epsilon())
    {
        return input;
    }

    const std::size_t pixels = static_cast<std::size_t>(input.width) * input.height;
    std::vector<float> middle(pixels);
    std::atomic<std::size_t> invalid_input{std::numeric_limits<std::size_t>::max()};
    auto measured = detail::for_each_row(
        input.height, cancellation,
        [&](const std::uint32_t row)
        {
            checkpoint(control, TextureCheckpoint::kInputRow, row);
            const std::size_t begin = static_cast<std::size_t>(row) * input.width;
            const std::size_t end = begin + input.width;
            for (std::size_t pixel = begin; pixel < end; ++pixel)
            {
                const std::size_t rgb = pixel * 3U;
                const float red = input.rgb[rgb];
                const float green = input.rgb[rgb + 1U];
                const float blue = input.rgb[rgb + 2U];
                if (!std::isfinite(red) || !std::isfinite(green) || !std::isfinite(blue))
                {
                    std::size_t previous = invalid_input.load(std::memory_order_relaxed);
                    while (rgb < previous && !invalid_input.compare_exchange_weak(
                                                 previous, rgb, std::memory_order_relaxed))
                    {
                    }
                    continue;
                }
                const float measured_luminance = luminance(red, green, blue);
                if (!std::isfinite(measured_luminance))
                {
                    std::size_t previous = invalid_input.load(std::memory_order_relaxed);
                    while (rgb < previous && !invalid_input.compare_exchange_weak(
                                                 previous, rgb, std::memory_order_relaxed))
                    {
                    }
                    continue;
                }
                middle[pixel] =
                    std::clamp(measured_luminance, kMinimumGuideLuminance, kMaximumGuideLuminance);
            }
        });
    if (!measured)
    {
        return measured.error();
    }
    const std::size_t bad_input = invalid_input.load(std::memory_order_relaxed);
    if (bad_input != std::numeric_limits<std::size_t>::max())
    {
        return make_error(
            ErrorCode::kValidation, "Texture input contains a non-finite sample",
            {{"sample_index", std::to_string(bad_input)}, {"reason", "nonfinite_texture_input"}});
    }

    WorkingImage output = input;
    std::vector<float> base;
    const int fine_radius = scaled_radius(input, params);
    const int broad_radius = coarse_radius(fine_radius, input);
    const float authored_strength = static_cast<float>(params.strength);
    const float shaped_strength = authored_strength >= 0.0F ?
                                      std::pow(authored_strength / 2.0F, 0.3F) * 2.0F :
                                      authored_strength;
    const float fine_gain =
        shaped_strength >= 0.0F ? 1.0F + shaped_strength : 1.0F / (1.0F - shaped_strength);
    const float broad_gain = shaped_strength >= 0.0F ? 1.0F + shaped_strength / 4.0F :
                                                       1.0F / (1.0F - shaped_strength / 2.0F);

    for (std::int64_t iteration = 0; iteration < params.iterations; ++iteration)
    {
        checkpoint(control, TextureCheckpoint::kBeforeFineFilter,
                   static_cast<std::uint32_t>(iteration));
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        if (auto filtered = detail::self_guided_filter_plane(
                middle, input.width, input.height, fine_radius, kGuideEpsilon, cancellation);
            !filtered)
        {
            return filtered.error();
        }
        base = middle;
        checkpoint(control, TextureCheckpoint::kBeforeCoarseFilter,
                   static_cast<std::uint32_t>(iteration));
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        if (auto filtered = detail::self_guided_filter_plane(
                base, input.width, input.height, broad_radius, kGuideEpsilon / 10.0F, cancellation);
            !filtered)
        {
            return filtered.error();
        }

        const float blend = std::exp2(-static_cast<float>(iteration));
        std::atomic<std::size_t> invalid_output{std::numeric_limits<std::size_t>::max()};
        auto adjusted = detail::for_each_row(
            input.height, cancellation,
            [&](const std::uint32_t row)
            {
                checkpoint(control, TextureCheckpoint::kOutputRow, row);
                const std::size_t begin = static_cast<std::size_t>(row) * input.width;
                const std::size_t end = begin + input.width;
                for (std::size_t pixel = begin; pixel < end; ++pixel)
                {
                    const std::size_t rgb = pixel * 3U;
                    const float current =
                        luminance(output.rgb[rgb], output.rgb[rgb + 1U], output.rgb[rgb + 2U]);
                    if (!(current > kScaleLuminanceFloor))
                    {
                        continue;
                    }
                    const double candidate = std::max(
                        0.0, static_cast<double>(base[pixel]) +
                                 static_cast<double>(current - middle[pixel]) * fine_gain +
                                 static_cast<double>(middle[pixel] - base[pixel]) * broad_gain);
                    const double adjusted_luminance =
                        static_cast<double>(current) +
                        static_cast<double>(blend) * (candidate - static_cast<double>(current));
                    const double scale = adjusted_luminance / static_cast<double>(current);
                    bool finite = std::isfinite(scale) && scale >= 0.0;
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                    {
                        const double value = static_cast<double>(output.rgb[rgb + channel]) * scale;
                        finite = finite && std::isfinite(value) &&
                                 std::abs(value) <= std::numeric_limits<float>::max();
                        if (finite)
                        {
                            output.rgb[rgb + channel] = static_cast<float>(value);
                        }
                    }
                    if (!finite)
                    {
                        std::size_t previous = invalid_output.load(std::memory_order_relaxed);
                        while (rgb < previous && !invalid_output.compare_exchange_weak(
                                                     previous, rgb, std::memory_order_relaxed))
                        {
                        }
                    }
                }
            });
        if (!adjusted)
        {
            return adjusted.error();
        }
        const std::size_t bad_output = invalid_output.load(std::memory_order_relaxed);
        if (bad_output != std::numeric_limits<std::size_t>::max())
        {
            return make_error(ErrorCode::kValidation,
                              "Texture produced a non-finite or unrepresentable sample",
                              {{"sample_index", std::to_string(bad_output)},
                               {"reason", "nonfinite_texture_output"}});
        }
    }

    checkpoint(control, TextureCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Texture allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_texture(const WorkingImage &input, const TextureParams &params,
                                   const CancellationToken &cancellation)
{
    return detail::apply_texture_controlled(input, params, cancellation, {});
}

Result<WorkingImage> apply_texture(const WorkingImage &input, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kTextureOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Texture",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kTextureOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Texture operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Texture masks are unavailable",
            {{"operation_id", operation.id}, {"reason", "texture_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = texture_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_texture(input, params.value(), cancellation);
}

} // namespace ravo
