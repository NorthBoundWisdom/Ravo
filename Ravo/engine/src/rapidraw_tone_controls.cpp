#include "rapidraw_tone_controls.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "parallel_rows.h"
#include "ravo/recipe/rapidraw_tone_controls.h"

namespace ravo
{
namespace
{

constexpr float kF16Max = 65504.0F;

[[nodiscard]] float luma(const float r, const float g, const float b) noexcept
{
    return 0.2126F * r + 0.7152F * g + 0.0722F * b;
}

[[nodiscard]] float smoothstep(const float edge0, const float edge1, const float value) noexcept
{
    const float t = std::clamp((value - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float mix(const float left, const float right, const float amount) noexcept
{
    return left + amount * (right - left);
}

void filmic_exposure(float rgb[3], const float brightness) noexcept
{
    if (brightness == 0.0F)
        return;
    constexpr float kRationalMix = 0.95F;
    constexpr float kMidtoneStrength = 1.2F;
    constexpr float kTopAnchor = 1.06F;
    const float original_luma = luma(rgb[0], rgb[1], rgb[2]);
    if (std::abs(original_luma) < 0.00001F)
        return;
    const float scale = std::exp2(brightness * (1.0F - kRationalMix));
    const float k = std::exp2(-brightness * kRationalMix * kMidtoneStrength);
    const float absolute_luma = std::abs(original_luma);
    const float floor_luma = std::floor(absolute_luma / kTopAnchor) * kTopAnchor;
    const float normalized = (absolute_luma - floor_luma) / kTopAnchor;
    const float denominator = normalized + (1.0F - normalized) * k;
    const float shaped = denominator == 0.0F ? normalized : normalized / denominator;
    const float new_luma = std::copysign(floor_luma + shaped * kTopAnchor, original_luma) * scale;
    const float total_scale = new_luma / original_luma;
    const float exponent = mix(0.95F, 0.65F, std::clamp(new_luma, 0.0F, 2.0F) * 0.5F);
    const float chroma_scale =
        std::pow(total_scale, exponent) / (1.0F + std::max(0.0F, new_luma - 0.9F) * 2.0F);
    for (int channel = 0; channel < 3; ++channel)
        rgb[channel] = new_luma + (rgb[channel] - original_luma) * chroma_scale;
}

void tonal_adjustments(float rgb[3], float blurred[3], const float contrast, const float shadows,
                       const float whites, const float blacks) noexcept
{
    if (whites != 0.0F)
    {
        const float multiplier = 1.0F / std::max(1.0F - whites * 0.25F, 0.01F);
        for (int channel = 0; channel < 3; ++channel)
        {
            rgb[channel] *= multiplier;
            blurred[channel] *= multiplier;
        }
    }
    const float pixel_luma =
        luma(std::max(rgb[0], 0.0F), std::max(rgb[1], 0.0F), std::max(rgb[2], 0.0F));
    const float blurred_luma =
        luma(std::max(blurred[0], 0.0F), std::max(blurred[1], 0.0F), std::max(blurred[2], 0.0F));
    const float safe_pixel_luma = std::max(pixel_luma, 0.0001F);
    const float safe_blurred_luma = std::max(blurred_luma, 0.0001F);
    if (shadows != 0.0F || blacks != 0.0F)
    {
        const float pixel = std::pow(safe_pixel_luma, 0.4545F);
        const float blurred_value = std::pow(safe_blurred_luma, 0.4545F);
        const float shadow_lift = shadows * pixel * std::pow(std::max(1.0F - pixel, 0.0F), 4.5F);
        const float black_lift = blacks * pixel * std::pow(std::max(1.0F - pixel, 0.0F), 12.0F);
        const float lift = std::max(shadow_lift + black_lift, 0.0F);
        const float curved = std::max(pixel + shadow_lift + black_lift, 0.0F);
        const float contrasted = 0.2F + (curved - 0.2F) * (1.0F + lift * 1.3F);
        const float final_value = std::max(mix(curved, contrasted, 0.85F), 0.0F);
        const float ratio = std::pow(final_value, 2.2F) / safe_pixel_luma;
        for (int channel = 0; channel < 3; ++channel)
            rgb[channel] *= ratio;
        const float safe_detail = std::clamp(pixel / std::max(blurred_value, 0.0001F), 0.8F, 1.25F);
        const float exponent = 1.0F + lift * 1.2F * smoothstep(0.0F, 0.1F, blurred_value);
        const float correction = std::pow(std::pow(safe_detail, exponent) / safe_detail, 2.2F);
        for (int channel = 0; channel < 3; ++channel)
            rgb[channel] *= correction;
        if (ratio > 1.0F)
        {
            const float recovered = luma(rgb[0], rgb[1], rgb[2]);
            const float amount = std::clamp((ratio - 1.0F) * 0.15F, 0.0F, 0.4F);
            for (int channel = 0; channel < 3; ++channel)
                rgb[channel] = mix(rgb[channel], recovered, amount);
        }
    }
    if (contrast != 0.0F)
    {
        const float strength = std::exp2(contrast * 1.25F);
        for (int channel = 0; channel < 3; ++channel)
        {
            const float safe = std::max(rgb[channel], 0.0F);
            const float perceptual = std::clamp(std::pow(safe, 1.0F / 2.2F), 0.0F, 1.0F);
            const float curved = perceptual < 0.5F ?
                                     0.5F * std::pow(2.0F * perceptual, strength) :
                                     1.0F - 0.5F * std::pow(2.0F * (1.0F - perceptual), strength);
            rgb[channel] = mix(std::pow(curved, 2.2F), rgb[channel], smoothstep(1.0F, 1.01F, safe));
        }
    }
}

void highlights_adjustment(float rgb[3], const float highlights) noexcept
{
    if (highlights == 0.0F)
        return;
    const float pixel_luma =
        luma(std::max(rgb[0], 0.0F), std::max(rgb[1], 0.0F), std::max(rgb[2], 0.0F));
    const float mask = smoothstep(0.3F, 0.95F, std::tanh(std::max(pixel_luma, 0.0001F) * 1.5F));
    if (mask < 0.001F)
        return;
    float adjusted[3] = {rgb[0], rgb[1], rgb[2]};
    if (highlights < 0.0F)
    {
        float new_luma = 0.0F;
        if (pixel_luma <= 1.0F)
            new_luma = std::pow(pixel_luma, 1.0F - highlights * 1.75F);
        else
        {
            const float excess = pixel_luma - 1.0F;
            new_luma = 1.0F + excess / (1.0F + excess * (-highlights * 6.0F));
        }
        const float ratio = new_luma / std::max(pixel_luma, 0.0001F);
        const float desaturation = smoothstep(1.0F, 10.0F, pixel_luma);
        for (float &channel : adjusted)
            channel = mix(channel * ratio, new_luma, desaturation);
    }
    else
    {
        const float factor = std::exp2(highlights * 1.75F);
        for (float &channel : adjusted)
            channel *= factor;
    }
    for (int channel = 0; channel < 3; ++channel)
        rgb[channel] = mix(rgb[channel], adjusted[channel], mask);
}

[[nodiscard]] Result<void> make_tonal_blur(const WorkingImage &image, std::vector<float> &blurred,
                                           const CancellationToken &cancellation)
{
    if (!image.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kValidation,
                          "RapidRAW shadows and blacks require canonical ROI scale",
                          {{"reason", "invalid_rapidraw_tone_roi_scale"}});
    }
    std::vector<float> horizontal;
    const int radius = static_cast<int>(
        rapidraw_tonal_blur_radius(image.canonical_roi_scale.reference_short_edge()));
    std::vector<float> weights;
    try
    {
        horizontal.resize(image.rgb.size());
        blurred.resize(image.rgb.size());
        weights.resize(static_cast<std::size_t>(radius * 2 + 1));
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "RapidRAW tone blur allocation failed",
                          {{"reason", "allocation_failed"}});
    }
    const float sigma = static_cast<float>(radius) / 2.0F;
    float weight_sum = 0.0F;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const float weight =
            std::exp(-static_cast<float>(offset * offset) / (2.0F * sigma * sigma));
        weights[static_cast<std::size_t>(offset + radius)] = weight;
        weight_sum += weight;
    }
    auto rows = detail::for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            for (std::uint32_t column = 0U; column < image.width; ++column)
                for (std::uint32_t channel = 0U; channel < 3U; ++channel)
                {
                    float sum = 0.0F;
                    for (int offset = -radius; offset <= radius; ++offset)
                    {
                        const auto sample = static_cast<std::uint32_t>(
                            std::clamp(static_cast<int>(column) + offset, 0,
                                       static_cast<int>(image.width) - 1));
                        const std::size_t index =
                            (static_cast<std::size_t>(row) * image.width + sample) * 3U + channel;
                        sum += std::clamp(image.rgb[index], 0.0F, kF16Max) *
                               weights[static_cast<std::size_t>(offset + radius)];
                    }
                    horizontal[(static_cast<std::size_t>(row) * image.width + column) * 3U +
                               channel] = sum / weight_sum;
                }
        });
    if (!rows)
        return rows.error();
    return detail::for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            for (std::uint32_t column = 0U; column < image.width; ++column)
                for (std::uint32_t channel = 0U; channel < 3U; ++channel)
                {
                    float sum = 0.0F;
                    for (int offset = -radius; offset <= radius; ++offset)
                    {
                        const auto sample = static_cast<std::uint32_t>(std::clamp(
                            static_cast<int>(row) + offset, 0, static_cast<int>(image.height) - 1));
                        const std::size_t index =
                            (static_cast<std::size_t>(sample) * image.width + column) * 3U +
                            channel;
                        sum += std::clamp(horizontal[index], 0.0F, kF16Max) *
                               weights[static_cast<std::size_t>(offset + radius)];
                    }
                    blurred[(static_cast<std::size_t>(row) * image.width + column) * 3U + channel] =
                        sum / weight_sum;
                }
        });
}

} // namespace

std::uint32_t rapidraw_tonal_blur_radius(const std::uint32_t width,
                                         const std::uint32_t height) noexcept
{
    return rapidraw_tonal_blur_radius(static_cast<float>(std::min(width, height)));
}

std::uint32_t rapidraw_tonal_blur_radius(const float reference_short_edge) noexcept
{
    const float reference_scale = reference_short_edge / 1080.0F;
    return static_cast<std::uint32_t>(
        std::max(1, static_cast<int>(std::ceil(3.5F * reference_scale))));
}

// Adapted from RapidRAW commit d6d8daa999f81198fb49e99b7e8ff43b47a6ffcd,
// src-tauri/src/shaders/{blur,shader}.wgsl and image_processing.rs, AGPL-3.0.
Result<void> apply_rapidraw_tone_controls(WorkingImage &image, const OperationInstance &operation,
                                          const CancellationToken &cancellation)
{
    auto parsed = rapidraw_tone_controls_from_parameters(operation.parameters);
    if (!parsed)
        return parsed.error();
    if (parsed.value().is_identity())
        return {};
    const std::uint64_t expected = static_cast<std::uint64_t>(image.width) * image.height * 3U;
    if (image.width == 0U || image.height == 0U || expected != image.rgb.size())
        return make_error(ErrorCode::kInvalidArgument, "RapidRAW tone image shape is invalid",
                          {{"reason", "rapidraw_tone_size_mismatch"}});
    auto active = cancellation.check();
    if (!active)
        return active.error();

    const float shadows = static_cast<float>(parsed.value().shadows / 120.0);
    const float blacks = static_cast<float>(parsed.value().blacks / 40.0);
    std::vector<float> blurred;
    if (shadows != 0.0F || blacks != 0.0F)
    {
        auto made = make_tonal_blur(image, blurred, cancellation);
        if (!made)
            return made.error();
    }
    const float ev_shift = static_cast<float>(parsed.value().ev_shift / 0.8);
    const float exposure = static_cast<float>(parsed.value().exposure / 0.8);
    const float contrast = static_cast<float>(parsed.value().contrast / 100.0);
    const float highlights = static_cast<float>(parsed.value().highlights / 120.0);
    const float whites = static_cast<float>(parsed.value().whites / 30.0);
    std::atomic<bool> invalid{false};
    std::atomic<std::size_t> invalid_index{0U};
    auto rows = detail::for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            for (std::uint32_t column = 0U; column < image.width; ++column)
            {
                const std::size_t base =
                    (static_cast<std::size_t>(row) * image.width + column) * 3U;
                float rgb[3] = {image.rgb[base], image.rgb[base + 1U], image.rgb[base + 2U]};
                if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]))
                {
                    invalid_index.store(base, std::memory_order_relaxed);
                    invalid.store(true, std::memory_order_release);
                    continue;
                }
                const float linear_scale = std::exp2(ev_shift);
                for (float &channel : rgb)
                    channel *= linear_scale;
                filmic_exposure(rgb, exposure);
                float blurred_rgb[3] = {rgb[0], rgb[1], rgb[2]};
                if (!blurred.empty())
                    for (int channel = 0; channel < 3; ++channel)
                        blurred_rgb[channel] = blurred[base + static_cast<std::size_t>(channel)];
                tonal_adjustments(rgb, blurred_rgb, contrast, shadows, whites, blacks);
                highlights_adjustment(rgb, highlights);
                if (!std::isfinite(rgb[0]) || !std::isfinite(rgb[1]) || !std::isfinite(rgb[2]))
                {
                    invalid_index.store(base, std::memory_order_relaxed);
                    invalid.store(true, std::memory_order_release);
                    continue;
                }
                for (int channel = 0; channel < 3; ++channel)
                    image.rgb[base + static_cast<std::size_t>(channel)] = rgb[channel];
            }
        });
    if (!rows)
        return rows.error();
    if (invalid.load(std::memory_order_acquire))
        return make_error(
            ErrorCode::kValidation, "RapidRAW tone input or output is non-finite",
            {{"sample_index", std::to_string(invalid_index.load(std::memory_order_relaxed))},
             {"reason", "nonfinite_rapidraw_tone_controls"}});
    return {};
}

} // namespace ravo
