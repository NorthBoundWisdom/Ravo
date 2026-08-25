#include "ravo/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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
    const std::size_t expected =
        static_cast<std::size_t>(raster.width) * raster.height * 3U;
    if (raster.srgb.size() < expected)
    {
        return make_error(ErrorCode::kValidation, "Scope raster is undersized");
    }
    return {};
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

    const std::uint32_t sample_width = raster.width;
    const std::uint32_t sample_height = raster.height;
    const std::size_t samples_per_bin = static_cast<std::size_t>(
        std::ceil(static_cast<float>(sample_width) / static_cast<float>(kWaveformMaxBins)));
    const std::uint32_t num_bins = static_cast<std::uint32_t>(
        std::ceil(static_cast<float>(sample_width) / static_cast<float>(samples_per_bin)));
    const std::uint32_t num_tones = kWaveformTones;

    std::vector<std::uint32_t> counts(static_cast<std::size_t>(3U) * num_bins * num_tones, 0U);
    for (std::uint32_t y = 0; y < sample_height; ++y)
    {
        for (std::uint32_t x = 0; x < sample_width; ++x)
        {
            const std::size_t pixel =
                (static_cast<std::size_t>(y) * sample_width + x) * 3U;
            const std::size_t bin = static_cast<std::size_t>(x) / samples_per_bin;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float px = static_cast<float>(raster.srgb[pixel + channel]) / 255.0F;
                // Frozen waveform: 1.0 sits at 8/9 of the tone axis.
                const float v = (8.0F / 9.0F) * px;
                const std::uint32_t tone = static_cast<std::uint32_t>(
                    std::ceil(std::clamp(v, 0.0F, 1.0F) * static_cast<float>(num_tones - 1U)));
                ++counts[num_tones * (channel * num_bins + bin) + tone];
            }
        }
    }

    const float brightness = static_cast<float>(num_tones) / 40.0F;
    const float scale =
        brightness / (static_cast<float>(sample_height) * static_cast<float>(samples_per_bin));

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
                                      counts[num_tones * (channel * num_bins + bin) + tone]));
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

} // namespace ravo
