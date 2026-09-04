#include "capability_ops.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <new>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "parallel_rows.h"
#include "guided_filter.h"
#include "ravo/recipe/develop.h"

#include "capability_ops_internal.h"

namespace ravo
{
using namespace capability_internal;
using detail::for_each_row;

Result<void> apply_raw_hotpixels(DecodedRaw &raw, const OperationInstance &operation,
                                 const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const double strength = parameter(operation, "strength", 0.25);
    const double threshold = parameter(operation, "threshold", 0.05);
    const bool permissive = parameter_bool(operation, "permissive", false);
    if (!std::isfinite(strength) || !std::isfinite(threshold) || strength < 0.0 || strength > 1.0 ||
        threshold < 0.0 || threshold > 1.0)
    {
        return make_error(ErrorCode::kValidation,
                          "Hot pixel parameters must be finite and within [0, 1]");
    }
    if (strength == 0.0)
    {
        return {};
    }
    if (raw.cfa_width != 2U || raw.cfa_height != 2U || raw.cfa_channels.size() != 4U)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Hot pixel correction currently requires a Bayer 2x2 CFA");
    }
    const bool has_red =
        std::find(raw.cfa_channels.begin(), raw.cfa_channels.end(), 0U) != raw.cfa_channels.end();
    const bool has_green =
        std::find(raw.cfa_channels.begin(), raw.cfa_channels.end(), 1U) != raw.cfa_channels.end();
    const bool has_blue =
        std::find(raw.cfa_channels.begin(), raw.cfa_channels.end(), 2U) != raw.cfa_channels.end();
    if (!has_red || !has_green || !has_blue)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Hot pixel correction does not support monochrome CFA data");
    }
    if (raw.width < 5U || raw.height < 5U ||
        raw.pixels.size() != static_cast<std::size_t>(raw.width) * raw.height)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Hot pixel correction requires a complete CFA frame of at least 5x5");
    }
    const std::int64_t black = raw.black_level;
    const std::int64_t white = raw.white_level;
    if (white <= black)
    {
        return make_error(ErrorCode::kValidation,
                          "Hot pixel correction requires white level above black level");
    }
    const float range = static_cast<float>(white - black);
    const auto normalized = [black, range](const std::uint16_t sample)
    {
        return std::max(static_cast<float>(static_cast<std::int64_t>(sample) - black), 0.0F) /
               range;
    };
    const std::vector<std::uint16_t> input = raw.pixels;
    const float multiplier = static_cast<float>(strength * 0.5);
    const float threshold_value = static_cast<float>(threshold);
    const int minimum_neighbours = permissive ? 3 : 4;
    const std::ptrdiff_t width = static_cast<std::ptrdiff_t>(raw.width);
    const std::array<std::ptrdiff_t, 4> offsets{-2, -2 * width, 2, 2 * width};
    for (std::uint32_t row = 2; row + 2U < raw.height; ++row)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t column = 2; column + 2U < raw.width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * raw.width + column;
            const float value = normalized(input[index]);
            if (value <= threshold_value)
            {
                continue;
            }
            const float midpoint = value * multiplier;
            int count = 0;
            float maximum = 0.0F;
            for (const std::ptrdiff_t offset : offsets)
            {
                const auto neighbour_index =
                    static_cast<std::size_t>(static_cast<std::ptrdiff_t>(index) + offset);
                const float neighbour = normalized(input[neighbour_index]);
                if (midpoint > neighbour)
                {
                    ++count;
                    maximum = std::max(maximum, neighbour);
                }
            }
            if (count >= minimum_neighbours)
            {
                const auto replacement = static_cast<std::int64_t>(
                    std::lround(static_cast<double>(black) + static_cast<double>(maximum * range)));
                raw.pixels[index] = static_cast<std::uint16_t>(std::clamp<std::int64_t>(
                    replacement, 0, std::numeric_limits<std::uint16_t>::max()));
            }
        }
    }
    return {};
}

Result<void> apply_raw_highlights(DecodedRaw &raw, const OperationInstance &operation,
                                  const std::array<float, 4> &white_balance,
                                  const CancellationToken &cancellation)
try
{
    const bool bayer = raw.cfa_width == 2U && raw.cfa_height == 2U && raw.cfa_channels.size() == 4U;
    const bool xtrans =
        raw.cfa_width == 6U && raw.cfa_height == 6U && raw.cfa_channels.size() == 36U;
    if ((!bayer && !xtrans) || raw.width == 0U || raw.height == 0U ||
        raw.width > std::numeric_limits<std::size_t>::max() / raw.height ||
        std::any_of(raw.cfa_channels.begin(), raw.cfa_channels.end(),
                    [](const std::uint8_t channel) { return channel > 2U; }) ||
        raw.pixels.size() != static_cast<std::size_t>(raw.width) * raw.height)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "RAW highlight reconstruction requires a complete RGB Bayer or X-Trans CFA",
            {{"cfa_width", std::to_string(raw.cfa_width)},
             {"cfa_height", std::to_string(raw.cfa_height)}});
    }
    const std::string mode =
        parameter_string(operation, "mode", std::string(kRawHighlightsModeOpposed));
    if (mode != kRawHighlightsModeClip && mode != kRawHighlightsModeInpaint &&
        mode != kRawHighlightsModeOpposed && mode != kRawHighlightsModeLch)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW highlight reconstruction mode is unsupported", {{"mode", mode}});
    }
    if (!bayer && mode != kRawHighlightsModeClip && mode != kRawHighlightsModeOpposed)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW highlight reconstruction mode requires a Bayer 2x2 CFA",
                          {{"mode", mode}, {"sensor", "xtrans"}});
    }
    if (std::any_of(white_balance.begin(), white_balance.end(), [](const float coefficient)
                    { return !std::isfinite(coefficient) || coefficient <= 0.0F; }))
    {
        return make_error(ErrorCode::kValidation,
                          "RAW highlight reconstruction requires finite positive white balance");
    }
    const std::int64_t black_code = std::max<std::int64_t>(raw.black_level, 0);
    if (raw.white_level <= static_cast<std::uint64_t>(black_code))
    {
        return make_error(ErrorCode::kValidation,
                          "RAW highlight reconstruction requires white above black");
    }
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        const std::uint32_t limit = raw.linear_response_limits[channel];
        if (limit != 0U &&
            (limit <= static_cast<std::uint64_t>(black_code) || limit > raw.white_level))
        {
            return make_error(ErrorCode::kValidation,
                              "RAW linear response limit is outside the sensor range",
                              {{"channel", std::to_string(channel)},
                               {"reason", "invalid_raw_linear_response_limit"}});
        }
    }
    const double amount = std::clamp(parameter(operation, "amount", 1.0), 0.0, 1.0);
    const double clip = std::clamp(parameter(operation, "clip", 1.0), 0.0, 2.0);
    if (amount <= 0.0 || raw.width < 3 || raw.height < 3)
    {
        return {};
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const std::array<float, 4> identity_balance{1.0F, 1.0F, 1.0F, 1.0F};
    const auto &processing_balance =
        mode == kRawHighlightsModeOpposed ? white_balance : identity_balance;
    auto converted = raw_to_float(raw, processing_balance, cancellation);
    if (!converted)
    {
        return converted.error();
    }
    auto &buffer = converted.value();
    float magic = 1.0F;
    if (mode == kRawHighlightsModeInpaint || mode == kRawHighlightsModeOpposed)
    {
        magic = 0.987F;
    }
    const float clipper = static_cast<float>(clip) * magic;
    std::array<float, 3> clips{};
    const float black = static_cast<float>(black_code);
    const float range = std::max(1.0F, static_cast<float>(raw.white_level) - black);
    for (std::size_t channel = 0U; channel < clips.size(); ++channel)
    {
        float sensor_clip = clipper;
        const std::uint32_t linear_limit = raw.linear_response_limits[channel];
        if (mode == kRawHighlightsModeOpposed && linear_limit != 0U)
        {
            sensor_clip = std::min(sensor_clip, (static_cast<float>(linear_limit) - black) / range);
        }
        clips[channel] = sensor_clip * processing_balance[channel];
    }
    if (mode == kRawHighlightsModeClip)
    {
        process_highlights_clip(buffer, raw, clips);
    }
    else if (mode == kRawHighlightsModeLch)
    {
        process_highlights_lch(buffer, raw, clipper);
    }
    else if (mode == kRawHighlightsModeInpaint)
    {
        process_highlights_inpaint(buffer, raw, clips);
    }
    else
    {
        process_highlights_opposed(buffer, raw, clips);
    }
    cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    return float_to_raw(raw, buffer, processing_balance, amount, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "RAW highlight reconstruction allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> apply_denoise_profile(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const double strength = parameter(operation, "strength", 0.0);
    const double chroma = parameter(operation, "chroma", 1.0);
    const double radius = parameter(operation, "radius", 1.0);
    const double shadows_value = parameter(operation, "shadows", 1.0);
    const double bias_value = parameter(operation, "bias", 0.0);
    const double noise_a_value = parameter(operation, "noise_a", kGenericNoiseA);
    const double noise_b_value = parameter(operation, "noise_b", kGenericNoiseB);
    if (!std::isfinite(strength) || strength < 0.0 || strength > 1.0 || !std::isfinite(chroma) ||
        chroma < 0.0 || chroma > 1.0 || !std::isfinite(radius) || radius < 0.5 || radius > 8.0 ||
        !std::isfinite(shadows_value) || shadows_value < 0.0 || shadows_value > 1.8 ||
        !std::isfinite(bias_value) || std::abs(bias_value) > std::numeric_limits<float>::max() ||
        !std::isfinite(noise_a_value) || noise_a_value <= 0.0 ||
        noise_a_value > std::numeric_limits<float>::max() || !std::isfinite(noise_b_value) ||
        noise_b_value < 0.0 || noise_b_value > std::numeric_limits<float>::max())
    {
        return make_error(ErrorCode::kValidation, "Profile denoise parameters are invalid",
                          {{"reason", "invalid_profile_denoise_parameters"}});
    }
    if (strength == 0.0)
    {
        return {};
    }
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 3U ||
        image.rgb.size() != static_cast<std::size_t>(pixel_count * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Profile denoise input does not match its dimensions",
                          {{"reason", "invalid_profile_denoise_buffer"}});
    }
    if (image.width < 8U || image.height < 8U)
    {
        return {};
    }
    if (!image.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kValidation, "Profile denoise requires canonical ROI scale",
                          {{"reason", "invalid_profile_denoise_roi_scale"}});
    }
    if (image.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        image.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return make_error(ErrorCode::kUnsupported, "Profile denoise dimensions are too large",
                          {{"reason", "unsupported_profile_denoise_dimensions"}});
    }
    const auto invalid_input =
        std::find_if(image.rgb.cbegin(), image.rgb.cend(),
                     [](const float value) { return !std::isfinite(value); });
    if (invalid_input != image.rgb.cend())
    {
        const std::size_t sample =
            static_cast<std::size_t>(std::distance(image.rgb.cbegin(), invalid_input));
        return make_error(ErrorCode::kValidation, "Profile denoise input must be finite",
                          {{"reason", "non_finite_profile_denoise_input"},
                           {"pixel_index", std::to_string(sample / 3U)},
                           {"channel_index", std::to_string(sample % 3U)}});
    }
    const float shadows = static_cast<float>(shadows_value);
    const float bias = static_cast<float>(bias_value);
    const float noise_a = static_cast<float>(noise_a_value);
    const float noise_b = static_cast<float>(noise_b_value);
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    const std::size_t npixels = static_cast<std::size_t>(pixel_count);

    int max_scale = 0;
    const float input_scale = std::min(image.canonical_roi_scale.value(), 1.0F);
    const float original_long_edge =
        static_cast<float>(std::max(image.width, image.height)) / input_scale;
    const float supp0 =
        std::min(2.0F * (2U << (kDenoiseBands - 1)) + 1.0F, original_long_edge * 0.2F);
    const float i0 = std::log2(std::max(supp0 - 1.0F, 1.0F) * 0.5F);
    if (!std::isfinite(i0) || i0 <= 0.0F)
    {
        return {};
    }
    for (; max_scale < kDenoiseBands; ++max_scale)
    {
        const float supp = 2.0F * static_cast<float>(2U << max_scale) + 1.0F;
        const float original_support = supp / input_scale;
        const float i_in = std::log2(std::max(original_support - 1.0F, 1.0F) * 0.5F) - 1.0F;
        const float t = 1.0F - (i_in + 0.5F) / i0;
        if (t < 0.0F)
        {
            break;
        }
    }
    max_scale = std::max(1, max_scale);
    const int max_mult =
        std::max(1, static_cast<int>(std::lround(static_cast<float>(1 << (max_scale - 1)) *
                                                 static_cast<float>(radius))));
    if (width < 2 * max_mult || height < 2 * max_mult)
    {
        return {};
    }

    std::array<float, 3> wb{1.0F, 1.0F, 1.0F};
    const float p_base = std::max(shadows, 0.0F);
    const std::array<float, 3> p{p_base, p_base, p_base};
    const float compensate_p = kDenoisePFulcrum / std::pow(kDenoisePFulcrum, shadows);
    constexpr float kYuvStrengthScale = 2.5F;

    float to_yuv[3][3] = {
        {1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}, {0.5F, 0.0F, -0.5F}, {0.25F, -0.5F, 0.25F}};
    float sum_invwb = (1.0F / wb[0] + 1.0F / wb[1] + 1.0F / wb[2]) * std::sqrt(3.0F);
    to_yuv[0][0] = sum_invwb / wb[0];
    to_yuv[0][1] = sum_invwb / wb[1];
    to_yuv[0][2] = sum_invwb / wb[2];
    const float stddev_u = std::sqrt(0.25F * wb[0] * wb[0] + 0.25F * wb[2] * wb[2]);
    const float stddev_v =
        std::sqrt(0.0625F * wb[0] * wb[0] + 0.25F * wb[1] * wb[1] + 0.0625F * wb[2] * wb[2]);
    to_yuv[1][0] /= stddev_u;
    to_yuv[1][1] /= stddev_u;
    to_yuv[1][2] /= stddev_u;
    to_yuv[2][0] /= stddev_v;
    to_yuv[2][1] /= stddev_v;
    to_yuv[2][2] /= stddev_v;
    float to_rgb[3][3]{};
    if (!invert_matrix3(to_yuv, to_rgb))
    {
        const float stddev_y = std::sqrt((wb[0] * wb[0] + wb[1] * wb[1] + wb[2] * wb[2]) / 9.0F);
        to_yuv[0][0] = to_yuv[0][1] = to_yuv[0][2] = 1.0F / (3.0F * stddev_y);
        if (!invert_matrix3(to_yuv, to_rgb))
        {
            return make_error(ErrorCode::kInternal, "Profile denoise color transform is singular",
                              {{"reason", "singular_profile_denoise_transform"}});
        }
    }
    for (int k = 0; k < 3; ++k)
    {
        for (int c = 0; c < 3; ++c)
        {
            to_yuv[k][c] /= kYuvStrengthScale;
            to_rgb[k][c] *= kYuvStrengthScale;
        }
    }

    const float a = noise_a * compensate_p;
    const std::array<float, 3> expon{-p[0] / 2.0F + 1.0F, -p[1] / 2.0F + 1.0F, -p[2] / 2.0F + 1.0F};
    const std::array<float, 3> scale{2.0F / ((-p[0] + 2.0F) * std::sqrt(a)),
                                     2.0F / ((-p[1] + 2.0F) * std::sqrt(a)),
                                     2.0F / ((-p[2] + 2.0F) * std::sqrt(a))};
    // The wavelet buffers carry YUV only. A padded fourth component would be
    // read across every 5x5 neighbourhood and every scale without contributing
    // to the result.
    std::vector<float> current(npixels * 3U, 0.0F);
    auto preconditioned = for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width;
            const std::size_t end = begin + image.width;
            for (std::size_t pixel = begin; pixel < end; ++pixel)
            {
                float tmp[3]{};
                for (int c = 0; c < 3; ++c)
                {
                    tmp[c] = std::pow(std::max(image.rgb[pixel * 3U + static_cast<std::size_t>(c)] +
                                                   noise_b,
                                               0.0F),
                                      expon[static_cast<std::size_t>(c)]) *
                             scale[static_cast<std::size_t>(c)];
                }
                float yuv[3]{};
                apply_matrix(to_yuv, tmp, yuv);
                current[pixel * 3U] = yuv[0];
                current[pixel * 3U + 1U] = yuv[1];
                current[pixel * 3U + 2U] = yuv[2];
            }
        });
    if (!preconditioned)
    {
        return preconditioned.error();
    }

    std::vector<float> out(current.size(), 0.0F);
    std::vector<float> coarse(current.size(), 0.0F);
    std::vector<float> detail(current.size(), 0.0F);
    const float varf = std::sqrt(2.0F + 2.0F * 16.0F + 36.0F) / 16.0F;
    std::array<float, 3> base_noise_sigma{1.0F, 1.0F, 1.0F};
    for (int scale_index = 0; scale_index < max_scale; ++scale_index)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const float sigma_at_scale = std::pow(varf, static_cast<float>(scale_index));
        const float guide_sigma =
            sigma_at_scale * std::sqrt((base_noise_sigma[0] * base_noise_sigma[0] +
                                        base_noise_sigma[1] * base_noise_sigma[1] +
                                        base_noise_sigma[2] * base_noise_sigma[2]) /
                                       3.0F);
        std::array<float, 3> sum_y2{};
        auto decomposed = eaw_dn_decompose(
            coarse, current, detail, sum_y2, scale_index, static_cast<float>(radius),
            1.0F / std::max(guide_sigma * guide_sigma, 1.0e-8F), width, height, cancellation);
        if (!decomposed)
        {
            return decomposed.error();
        }
        if (scale_index == 0)
        {
            auto estimated = estimate_wavelet_noise_sigma(detail, npixels, cancellation);
            if (!estimated)
            {
                return estimated.error();
            }
            base_noise_sigma = estimated.value();
            const float calibrated_guide = std::sqrt((base_noise_sigma[0] * base_noise_sigma[0] +
                                                      base_noise_sigma[1] * base_noise_sigma[1] +
                                                      base_noise_sigma[2] * base_noise_sigma[2]) /
                                                     3.0F);
            decomposed = eaw_dn_decompose(
                coarse, current, detail, sum_y2, scale_index, static_cast<float>(radius),
                1.0F / std::max(calibrated_guide * calibrated_guide, 1.0e-8F), width, height,
                cancellation);
            if (!decomposed)
            {
                return decomposed.error();
            }
        }
        const float scale_position =
            max_scale <= 1 ? 0.0F :
                             static_cast<float>(scale_index) / static_cast<float>(max_scale - 1);
        // Radius changes both sampling dilation and the coarse-band threshold. Radius 1 retains
        // the source-default à-trous response; larger values progressively reach wider texture.
        const float radius_threshold_gain = std::pow(static_cast<float>(radius), scale_position);
        std::array<float, 3> thresholds{};
        for (int c = 0; c < 3; ++c)
        {
            const float noise_sigma =
                base_noise_sigma[static_cast<std::size_t>(c)] * sigma_at_scale;
            const float noise_variance = noise_sigma * noise_sigma;
            const float signal_sigma =
                std::sqrt(std::max(1.0e-6F, sum_y2[static_cast<std::size_t>(c)] /
                                                    (static_cast<float>(npixels) - 1.0F) -
                                                noise_variance));
            thresholds[static_cast<std::size_t>(c)] =
                8.0F * radius_threshold_gain * noise_variance / signal_sigma;
        }
        auto synthesized = eaw_synthesize(out, detail, thresholds, width, height, cancellation);
        if (!synthesized)
        {
            return synthesized.error();
        }
        current.swap(coarse);
    }
    const std::array<float, 3> back_expon{1.0F / (1.0F - p[0] / 2.0F), 1.0F / (1.0F - p[1] / 2.0F),
                                          1.0F / (1.0F - p[2] / 2.0F)};
    const std::array<float, 3> back_scale{(std::sqrt(a) * (2.0F - p[0])) / 4.0F,
                                          (std::sqrt(a) * (2.0F - p[1])) / 4.0F,
                                          (std::sqrt(a) * (2.0F - p[2])) / 4.0F};
    const float applied_bias = bias;
    constexpr std::array<float, 3> kLumaWeights{0.2126F, 0.7152F, 0.0722F};
    const float luma_amount = static_cast<float>(strength);
    const float chroma_amount = static_cast<float>(strength * chroma);
    auto restored = for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width;
            const std::size_t end = begin + image.width;
            for (std::size_t pixel = begin; pixel < end; ++pixel)
            {
                float yuv[3]{out[pixel * 3U] + current[pixel * 3U],
                             out[pixel * 3U + 1U] + current[pixel * 3U + 1U],
                             out[pixel * 3U + 2U] + current[pixel * 3U + 2U]};
                float rgb[3]{};
                apply_matrix(to_rgb, yuv, rgb);
                std::array<float, 3> denoised{};
                for (int c = 0; c < 3; ++c)
                {
                    const float x = std::max(rgb[c], 0.0F);
                    const float delta = x * x + applied_bias * wb[static_cast<std::size_t>(c)];
                    const float z1 = (x + std::sqrt(std::max(delta, 0.0F))) *
                                     back_scale[static_cast<std::size_t>(c)];
                    denoised[static_cast<std::size_t>(c)] =
                        std::pow(std::max(z1, 0.0F), back_expon[static_cast<std::size_t>(c)]) -
                        noise_b;
                }
                const std::size_t offset = pixel * 3U;
                const std::array<float, 3> delta{denoised[0] - image.rgb[offset],
                                                 denoised[1] - image.rgb[offset + 1U],
                                                 denoised[2] - image.rgb[offset + 2U]};
                const float luma_delta = kLumaWeights[0] * delta[0] + kLumaWeights[1] * delta[1] +
                                         kLumaWeights[2] * delta[2];
                for (std::size_t c = 0U; c < 3U; ++c)
                {
                    const float value = image.rgb[offset + c] + luma_amount * luma_delta +
                                        chroma_amount * (delta[c] - luma_delta);
                    if (!std::isfinite(value))
                    {
                        detail[offset + c] = std::numeric_limits<float>::quiet_NaN();
                    }
                    else
                    {
                        detail[offset + c] = value;
                    }
                }
            }
        });
    if (!restored)
    {
        return restored.error();
    }
    const auto invalid_output = std::find_if(detail.cbegin(), detail.cend(), [](const float value)
                                             { return !std::isfinite(value); });
    if (invalid_output != detail.cend())
    {
        const std::size_t sample =
            static_cast<std::size_t>(std::distance(detail.cbegin(), invalid_output));
        return make_error(ErrorCode::kValidation, "Profile denoise output must be finite",
                          {{"reason", "non_finite_profile_denoise_output"},
                           {"pixel_index", std::to_string(sample / 3U)},
                           {"channel_index", std::to_string(sample % 3U)}});
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    image.rgb.swap(detail);
    return {};
}

} // namespace ravo
