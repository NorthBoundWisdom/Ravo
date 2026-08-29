#include "ravo/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ravo
{
namespace
{

[[nodiscard]] Result<void> validate_raster(const RasterBuffer &raster)
{
    if (raster.width == 0 || raster.height == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Scope raster dimensions must be non-zero");
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(raster.width) * raster.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U)
    {
        return make_error(ErrorCode::kValidation, "Scope raster dimensions overflow",
                          {{"reason", "scope_dimensions_overflow"}});
    }
    const std::size_t expected = static_cast<std::size_t>(pixels) * 3U;
    if (raster.srgb.size() != expected)
    {
        return make_error(ErrorCode::kValidation, "Scope raster is undersized");
    }
    return {};
}

struct WaveformCounts
{
    std::uint32_t bins = 0U;
    std::uint32_t tones = kWaveformTones;
    std::size_t samples_per_bin = 0U;
    std::vector<std::uint32_t> values;
};

[[nodiscard]] WaveformCounts waveform_counts(const RasterBuffer &raster)
{
    WaveformCounts result;
    result.samples_per_bin = static_cast<std::size_t>(
        std::ceil(static_cast<float>(raster.width) / static_cast<float>(kWaveformMaxBins)));
    result.bins = static_cast<std::uint32_t>(
        std::ceil(static_cast<float>(raster.width) /
                  static_cast<float>(result.samples_per_bin)));
    result.values.assign(static_cast<std::size_t>(3U) * result.bins * result.tones, 0U);
    for (std::uint32_t y = 0U; y < raster.height; ++y)
    {
        for (std::uint32_t x = 0U; x < raster.width; ++x)
        {
            const std::size_t pixel = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
            const std::size_t bin = static_cast<std::size_t>(x) / result.samples_per_bin;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float value = (8.0F / 9.0F) *
                                    (static_cast<float>(raster.srgb[pixel + channel]) / 255.0F);
                const std::uint32_t tone = static_cast<std::uint32_t>(
                    std::ceil(std::clamp(value, 0.0F, 1.0F) *
                              static_cast<float>(result.tones - 1U)));
                ++result.values[result.tones * (channel * result.bins + bin) + tone];
            }
        }
    }
    return result;
}

[[nodiscard]] float waveform_scale(const RasterBuffer &raster,
                                   const WaveformCounts &counts) noexcept
{
    const float brightness = static_cast<float>(counts.tones) / 40.0F;
    return brightness /
           (static_cast<float>(raster.height) * static_cast<float>(counts.samples_per_bin));
}

[[nodiscard]] float srgb_to_linear(const float encoded) noexcept
{
    return encoded <= 0.04045F ? encoded / 12.92F :
                                 std::pow((encoded + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] std::array<float, 2> rgb_to_d50_uv(const std::array<float, 3> &encoded) noexcept
{
    const float red = srgb_to_linear(encoded[0]);
    const float green = srgb_to_linear(encoded[1]);
    const float blue = srgb_to_linear(encoded[2]);
    const float x = 0.4360747F * red + 0.3850649F * green + 0.1430804F * blue;
    const float y = 0.2225045F * red + 0.7168786F * green + 0.0606169F * blue;
    const float z = 0.0139322F * red + 0.0971045F * green + 0.7141733F * blue;
    const float denominator = x + 15.0F * y + 3.0F * z;
    if (denominator <= 0.0F)
        return {0.0F, 0.0F};
    constexpr float white_x = 0.9642F;
    constexpr float white_y = 1.0F;
    constexpr float white_z = 0.8249F;
    constexpr float white_denominator = white_x + 15.0F * white_y + 3.0F * white_z;
    constexpr float white_u = 4.0F * white_x / white_denominator;
    constexpr float white_v = 9.0F * white_y / white_denominator;
    constexpr float epsilon = 216.0F / 24389.0F;
    constexpr float kappa = 24389.0F / 27.0F;
    const float lightness = y > epsilon ? 116.0F * std::cbrt(y) - 16.0F : kappa * y;
    const float u = 4.0F * x / denominator;
    const float v = 9.0F * y / denominator;
    return {13.0F * lightness * (u - white_u), 13.0F * lightness * (v - white_v)};
}

// Rec.2100 HLG OETF. Frozen waveform display uses an HLG Rec.2020 LUT as a
// linear-to-display shortcut; this is the same transfer without LCMS.
[[nodiscard]] float hlg_oetf(const float linear) noexcept
{
    const float encoded = std::clamp(linear, 0.0F, 1.0F);
    constexpr float kA = 0.17883277F;
    constexpr float kB = 0.28466892F;
    constexpr float kC = 0.55991073F;
    if (encoded <= 1.0F / 12.0F)
    {
        return std::sqrt(3.0F * encoded);
    }
    return kA * std::log(12.0F * encoded - kB) + kC;
}

} // namespace

Result<RgbHistogram> collect_rgb_histogram(const RasterBuffer &raster)
{
    auto valid = validate_raster(raster);
    if (!valid)
    {
        return valid.error();
    }

    RgbHistogram histogram;
    const std::size_t pixels =
        static_cast<std::size_t>(raster.width) * raster.height;
    for (std::size_t pixel = 0; pixel < pixels; ++pixel)
    {
        const std::size_t index = pixel * 3U;
        // Frozen _bin_rgb: bin = CLAMP((bins-1) * channel, 0, bins-1) on 0-1
        // float. Display-referred 8-bit preview is already that 0-255 code.
        ++histogram.red[raster.srgb[index]];
        ++histogram.green[raster.srgb[index + 1U]];
        ++histogram.blue[raster.srgb[index + 2U]];
    }
    // Frozen helper skips bin 0 when computing channel maxima so crushed
    // blacks do not flatten the plot.
    for (std::uint32_t bin = 1; bin < kRgbHistogramBins; ++bin)
    {
        histogram.max_count =
            std::max(histogram.max_count,
                     std::max(histogram.red[bin], std::max(histogram.green[bin], histogram.blue[bin])));
    }
    return histogram;
}

Result<RgbParade> collect_rgb_parade(const RasterBuffer &raster)
{
    auto valid = validate_raster(raster);
    if (!valid)
    {
        return valid.error();
    }

    const WaveformCounts counts = waveform_counts(raster);
    const std::uint32_t num_bins = counts.bins;
    const std::uint32_t num_tones = counts.tones;
    const float scale = waveform_scale(raster, counts);

    RgbParade parade;
    parade.bins = num_bins;
    parade.tones = num_tones;
    const std::uint32_t out_width = num_bins * 3U;
    parade.rgb.assign(static_cast<std::size_t>(out_width) * num_tones * 3U, 0);
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        for (std::uint32_t bin = 0; bin < num_bins; ++bin)
        {
            for (std::uint32_t tone = 0; tone < num_tones; ++tone)
            {
                const float linear = std::min(
                    1.0F, scale * static_cast<float>(
                                      counts.values[num_tones * (channel * num_bins + bin) + tone]));
                const auto display =
                    static_cast<std::uint8_t>(std::lround(hlg_oetf(linear) * 255.0F));
                const std::uint32_t x = static_cast<std::uint32_t>(channel) * num_bins + bin;
                const std::uint32_t y = num_tones - 1U - tone;
                parade.rgb[(static_cast<std::size_t>(y) * out_width + x) * 3U + channel] = display;
            }
        }
    }
    return parade;
}

Result<RgbScopeImage> collect_rgb_waveform(const RasterBuffer &raster)
{
    auto valid = validate_raster(raster);
    if (!valid)
        return valid.error();
    const WaveformCounts counts = waveform_counts(raster);
    const float scale = waveform_scale(raster, counts);
    RgbScopeImage waveform;
    waveform.width = counts.bins;
    waveform.height = counts.tones;
    waveform.rgb.assign(static_cast<std::size_t>(waveform.width) * waveform.height * 3U, 0U);
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        for (std::uint32_t bin = 0U; bin < counts.bins; ++bin)
        {
            for (std::uint32_t tone = 0U; tone < counts.tones; ++tone)
            {
                const float linear = std::min(
                    1.0F, scale * static_cast<float>(counts.values[
                                      counts.tones * (channel * counts.bins + bin) + tone]));
                const auto display =
                    static_cast<std::uint8_t>(std::lround(hlg_oetf(linear) * 255.0F));
                const std::uint32_t y = counts.tones - 1U - tone;
                waveform.rgb[(static_cast<std::size_t>(y) * waveform.width + bin) * 3U + channel] =
                    display;
            }
        }
    }
    return waveform;
}

Result<RgbScopeImage> collect_uv_vectorscope(const RasterBuffer &raster)
{
    auto valid = validate_raster(raster);
    if (!valid)
        return valid.error();
    constexpr std::uint32_t diameter = kVectorscopeDiameter;
    std::vector<std::uint32_t> counts(static_cast<std::size_t>(diameter) * diameter, 0U);
    const std::uint32_t sample_width = raster.width - raster.width % 2U;
    const std::uint32_t sample_height = raster.height - raster.height % 2U;
    for (std::uint32_t y = 0U; y < sample_height; y += 2U)
    {
        for (std::uint32_t x = 0U; x < sample_width; x += 2U)
        {
            std::array<float, 3> average{};
            for (std::uint32_t yy = 0U; yy < 2U; ++yy)
            {
                for (std::uint32_t xx = 0U; xx < 2U; ++xx)
                {
                    const std::size_t pixel =
                        (static_cast<std::size_t>(y + yy) * raster.width + x + xx) * 3U;
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                        average[channel] +=
                            static_cast<float>(raster.srgb[pixel + channel]) / (255.0F * 4.0F);
                }
            }
            const auto uv = rgb_to_d50_uv(average);
            const float mapped_x = uv[0] / (2.0F * kVectorscopeUvRadius) + 0.5F;
            const float mapped_y = uv[1] / (2.0F * kVectorscopeUvRadius) + 0.5F;
            const int out_x = static_cast<int>((diameter - 1U) * mapped_x);
            const int out_y = static_cast<int>((diameter - 1U) * mapped_y);
            if (out_x >= 0 && out_x < static_cast<int>(diameter) && out_y >= 0 &&
                out_y < static_cast<int>(diameter))
            {
                const std::uint32_t image_y = diameter - 1U - static_cast<std::uint32_t>(out_y);
                ++counts[static_cast<std::size_t>(image_y) * diameter +
                         static_cast<std::uint32_t>(out_x)];
            }
        }
    }
    const float scale = (1.0F / 30.0F) * static_cast<float>(diameter * diameter) /
                        static_cast<float>(static_cast<std::uint64_t>(raster.width) * raster.height);
    RgbScopeImage result;
    result.width = diameter;
    result.height = diameter;
    result.rgb.resize(static_cast<std::size_t>(diameter) * diameter * 3U);
    for (std::size_t pixel = 0U; pixel < counts.size(); ++pixel)
    {
        const float linear = std::min(1.0F, scale * static_cast<float>(counts[pixel]));
        const auto display = static_cast<std::uint8_t>(std::lround(hlg_oetf(linear) * 255.0F));
        result.rgb[pixel * 3U] = display;
        result.rgb[pixel * 3U + 1U] = display;
        result.rgb[pixel * 3U + 2U] = display;
    }
    return result;
}

Result<RgbScopeImage> collect_split_scope(const RasterBuffer &raster)
{
    auto waveform = collect_rgb_waveform(raster);
    if (!waveform)
        return waveform.error();
    auto vectorscope = collect_uv_vectorscope(raster);
    if (!vectorscope)
        return vectorscope.error();
    const std::uint32_t vector_side = waveform.value().height;
    RgbScopeImage result;
    result.width = waveform.value().width + vector_side;
    result.height = waveform.value().height;
    result.rgb.assign(static_cast<std::size_t>(result.width) * result.height * 3U, 0U);
    for (std::uint32_t y = 0U; y < result.height; ++y)
    {
        const std::size_t destination = static_cast<std::size_t>(y) * result.width * 3U;
        const std::size_t source = static_cast<std::size_t>(y) * waveform.value().width * 3U;
        std::copy_n(waveform.value().rgb.begin() + static_cast<std::ptrdiff_t>(source),
                    static_cast<std::size_t>(waveform.value().width) * 3U,
                    result.rgb.begin() + static_cast<std::ptrdiff_t>(destination));
        for (std::uint32_t x = 0U; x < vector_side; ++x)
        {
            const std::size_t vector_destination =
                (static_cast<std::size_t>(y) * result.width + waveform.value().width + x) * 3U;
            const std::uint32_t source_x0 = x * vectorscope.value().width / vector_side;
            const std::uint32_t source_x1 = std::max(
                source_x0 + 1U, (x + 1U) * vectorscope.value().width / vector_side);
            const std::uint32_t source_y0 = y * vectorscope.value().height / result.height;
            const std::uint32_t source_y1 = std::max(
                source_y0 + 1U, (y + 1U) * vectorscope.value().height / result.height);
            for (std::uint32_t source_y = source_y0;
                 source_y < std::min(source_y1, vectorscope.value().height); ++source_y)
            {
                for (std::uint32_t source_x = source_x0;
                     source_x < std::min(source_x1, vectorscope.value().width); ++source_x)
                {
                    const std::size_t vector_source =
                        (static_cast<std::size_t>(source_y) * vectorscope.value().width + source_x) *
                        3U;
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                        result.rgb[vector_destination + channel] =
                            std::max(result.rgb[vector_destination + channel],
                                     vectorscope.value().rgb[vector_source + channel]);
                }
            }
        }
    }
    return result;
}

} // namespace ravo
