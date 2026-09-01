#include "capability_ops.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
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
using detail::self_guided_filter_plane;

Result<void> apply_tone_equalizer(WorkingImage &image, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const std::array<float, 5> band_ev{
        static_cast<float>(parameter(operation, "blacks", 0.0)),
        static_cast<float>(parameter(operation, "shadows", 0.0)),
        static_cast<float>(parameter(operation, "midtones", 0.0)),
        static_cast<float>(parameter(operation, "highlights", 0.0)),
        static_cast<float>(parameter(operation, "whites", 0.0)),
    };
    bool identity = true;
    for (const float gain : band_ev)
    {
        if (!std::isfinite(gain) || std::abs(gain) > 4.0F)
        {
            return make_error(ErrorCode::kValidation,
                              "Tone equalizer band must be a finite EV in [-4, 4]",
                              {{"reason", "invalid_tone_equalizer_band"}});
        }
        if (std::abs(gain) > 1.0e-8F)
        {
            identity = false;
        }
    }
    if (identity || image.width == 0 || image.height == 0)
    {
        return {};
    }
    const std::uint64_t pixel_count = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 3U ||
        image.rgb.size() != static_cast<std::size_t>(pixel_count * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Tone equalizer input does not match its dimensions",
                          {{"reason", "invalid_tone_equalizer_buffer"}});
    }
    if (!image.canonical_roi_scale.valid())
    {
        return make_error(ErrorCode::kValidation, "Tone equalizer requires canonical ROI scale",
                          {{"reason", "invalid_tone_equalizer_roi_scale"}});
    }

    // Expand the five authored photographic groups into the accepted nine one-stop bands. The
    // intermediate bands interpolate authored EV, not linear gain. Normalizing the gaussian sum
    // makes identity exact and avoids an under-determined inverse oscillating between controls.
    // The correction bound retains the Studio slider's +/-2 EV range for recipes authored through
    // the wider machine-visible validation interval.
    constexpr std::array<float, kToneAnchorCount> kAnchorEv{-8.0F, -7.0F, -6.0F, -5.0F, -4.0F,
                                                            -3.0F, -2.0F, -1.0F, 0.0F};
    const std::array<float, kToneAnchorCount> kAnchorCorrectionEv{
        band_ev[0], 0.5F * (band_ev[0] + band_ev[1]), band_ev[1], 0.5F * (band_ev[1] + band_ev[2]),
        band_ev[2], 0.5F * (band_ev[2] + band_ev[3]), band_ev[3], 0.5F * (band_ev[3] + band_ev[4]),
        band_ev[4],
    };
    const std::array<float, kToneAnchorCount> kAnchorGain{
        std::exp2(kAnchorCorrectionEv[0]), std::exp2(kAnchorCorrectionEv[1]),
        std::exp2(kAnchorCorrectionEv[2]), std::exp2(kAnchorCorrectionEv[3]),
        std::exp2(kAnchorCorrectionEv[4]), std::exp2(kAnchorCorrectionEv[5]),
        std::exp2(kAnchorCorrectionEv[6]), std::exp2(kAnchorCorrectionEv[7]),
        std::exp2(kAnchorCorrectionEv[8]),
    };
    constexpr float kAnchorSigmaEv = std::numbers::sqrt2_v<float>;
    const float denominator = gaussian_denom(kAnchorSigmaEv);
    constexpr int kToneLutSteps =
        static_cast<int>((kToneLutMaxEv - kToneLutMinEv) * kToneLutResolution);
    std::vector<float> lut(static_cast<std::size_t>(kToneLutSteps + 1));
    for (int step = 0; step <= kToneLutSteps; ++step)
    {
        if ((step & 4095) == 0)
        {
            active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
        }
        const float exposure =
            kToneLutMinEv + static_cast<float>(step) / static_cast<float>(kToneLutResolution);
        float weighted_gain = 0.0F;
        float weight_sum = 0.0F;
        for (std::size_t anchor = 0; anchor < kAnchorEv.size(); ++anchor)
        {
            const float weight = gaussian_func(exposure - kAnchorEv[anchor], denominator);
            weighted_gain += weight * kAnchorGain[anchor];
            weight_sum += weight;
        }
        lut[static_cast<std::size_t>(step)] =
            std::clamp(weighted_gain / std::max(weight_sum, 1.0e-12F), 0.25F, 4.0F);
    }

    const std::size_t count = static_cast<std::size_t>(pixel_count);
    std::vector<float> mask_ev(count);
    auto measured = for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t row)
        {
            const std::size_t begin = static_cast<std::size_t>(row) * image.width;
            const std::size_t end = begin + image.width;
            for (std::size_t pixel = begin; pixel < end; ++pixel)
            {
                const float r = image.rgb[pixel * 3U];
                const float g = image.rgb[pixel * 3U + 1U];
                const float b = image.rgb[pixel * 3U + 2U];
                if (!std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b))
                {
                    mask_ev[pixel] = std::numeric_limits<float>::quiet_NaN();
                    continue;
                }
                const double energy = std::hypot(static_cast<double>(r), static_cast<double>(g),
                                                 static_cast<double>(b));
                const double exposure =
                    std::log2(std::max(static_cast<double>(std::exp2(-16.0F)), energy));
                mask_ev[pixel] =
                    static_cast<float>(std::clamp(exposure, static_cast<double>(kToneLutMinEv),
                                                  static_cast<double>(kToneLutMaxEv)));
            }
        });
    if (!measured)
    {
        return measured.error();
    }
    const auto invalid = std::find_if(mask_ev.cbegin(), mask_ev.cend(),
                                      [](const float value) { return !std::isfinite(value); });
    if (invalid != mask_ev.cend())
    {
        return make_error(ErrorCode::kValidation, "Tone equalizer input must be finite",
                          {{"reason", "non_finite_tone_equalizer_input"},
                           {"pixel_index", std::to_string(static_cast<std::size_t>(
                                               std::distance(mask_ev.cbegin(), invalid)))}});
    }
    const std::uint32_t maximum_dimension = std::max(image.width, image.height);
    if (maximum_dimension > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
    {
        return make_error(ErrorCode::kUnsupported, "Tone equalizer dimensions are too large",
                          {{"reason", "unsupported_tone_equalizer_dimensions"}});
    }
    const int maximum_radius = std::max(1, static_cast<int>(maximum_dimension) - 1);
    const double scaled_radius =
        static_cast<double>(kToneMaskRadiusOriginalPixels) * image.canonical_roi_scale.value();
    const int radius = scaled_radius >= static_cast<double>(maximum_radius) ?
                           maximum_radius :
                           std::max(1, static_cast<int>(std::lround(scaled_radius)));
    auto filtered = self_guided_filter_plane(mask_ev, image.width, image.height, radius,
                                             kToneMaskEpsilonEv, cancellation);
    if (!filtered)
    {
        return filtered.error();
    }

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
            const float exposure = std::clamp(mask_ev[pixel], kToneLutMinEv, kToneLutMaxEv);
            const auto lut_index = static_cast<std::size_t>(
                std::lround((exposure - kToneLutMinEv) * kToneLutResolution));
            mask_ev[pixel] = lut[std::min(lut_index, lut.size() - 1U)];
            for (std::size_t channel = 0; channel < 3U; ++channel)
            {
                const double adjusted = static_cast<double>(image.rgb[pixel * 3U + channel]) *
                                        static_cast<double>(mask_ev[pixel]);
                if (!std::isfinite(adjusted) ||
                    std::abs(adjusted) > std::numeric_limits<float>::max())
                {
                    return make_error(ErrorCode::kValidation,
                                      "Tone equalizer output must be finite",
                                      {{"reason", "non_finite_tone_equalizer_output"},
                                       {"pixel_index", std::to_string(pixel)},
                                       {"channel_index", std::to_string(channel)}});
                }
            }
        }
    }
    return for_each_row(image.height, cancellation,
                        [&](const std::uint32_t row)
                        {
                            const std::size_t begin = static_cast<std::size_t>(row) * image.width;
                            const std::size_t end = begin + image.width;
                            for (std::size_t pixel = begin; pixel < end; ++pixel)
                            {
                                const float correction = mask_ev[pixel];
                                image.rgb[pixel * 3U] *= correction;
                                image.rgb[pixel * 3U + 1U] *= correction;
                                image.rgb[pixel * 3U + 2U] *= correction;
                            }
                        });
}

} // namespace ravo
