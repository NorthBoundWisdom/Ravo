#include "ravo/engine/video_color.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <new>
namespace ravo
{
namespace
{
double pq_decode(double signal)
{
    const double p = std::pow(std::max(0., signal), 32. / 2523.);
    return 10000. * std::pow(std::max(p - 107. / 128., 0.) / (2413. / 128. - 2392. / 128. * p),
                             8192. / 1305.);
}
double pq_encode(double nits)
{
    const double p = std::pow(std::max(0., nits) / 10000., 1305. / 8192.);
    return std::pow((107. / 128. + 2413. / 128. * p) / (1. + 2392. / 128. * p), 2523. / 32.);
}
double tone_scale(double luminance, double peak)
{
    if (luminance <= 0.)
        return 1.;
    const double mastering = pq_encode(peak);
    double p = pq_encode(std::min(luminance * 100., peak)) / mastering;
    const double limit = pq_encode(100.) / mastering;
    const double knee = 1.5 * limit - 0.5;
    if (p <= knee)
        return 1.;
    const double t = std::clamp((p - knee) / (1. - knee), 0., 1.);
    p = (2 * t * t * t - 3 * t * t + 1) * knee + (t * t * t - 2 * t * t + t) * (1. - knee) +
        (-2 * t * t * t + 3 * t * t) * limit;
    return pq_decode(p * mastering) / (100. * luminance);
}
} // namespace
Result<std::vector<std::uint8_t>> video_rgb_to_sdr(std::span<const float> rgb,
                                                   std::string_view transfer,
                                                   std::string_view primaries, double peak_nits,
                                                   const CancellationToken &cancellation)
try
{
    if (rgb.empty() || rgb.size() % 3 != 0 || rgb.size() > 4096U * 4096U * 3U ||
        (transfer != "pq" && transfer != "hlg" && transfer != "bt709" && transfer != "srgb") ||
        (primaries != "bt709" && primaries != "bt2020" && primaries != "p3_d65") ||
        !std::isfinite(peak_nits) || peak_nits <= 100 || peak_nits > 10000)
        return make_error(ErrorCode::kUnsupported, "Video colour contract is unsupported",
                          {{"reason", "video_colour_unsupported"}});
    std::vector<std::uint8_t> output(rgb.size());
    const bool hdr = transfer == "pq" || transfer == "hlg";
    for (std::size_t i = 0; i < rgb.size(); i += 3)
    {
        if (i % 12288 == 0)
            if (auto active = cancellation.check(); !active)
                return active.error();
        std::array<double, 3> linear{};
        for (std::size_t c = 0; c < 3; ++c)
        {
            const double v = rgb[i + c];
            if (!std::isfinite(v))
                return make_error(ErrorCode::kValidation, "Video contains non-finite pixels",
                                  {{"reason", "video_nonfinite_pixels"}});
            if (transfer == "pq")
                linear[c] = pq_decode(std::clamp(v, 0., 1.)) / 100.;
            else if (transfer == "hlg")
                linear[c] = v <= .5 ? v * v / 3. :
                                      (std::exp((v - .55991073) / .17883277) + .28466892) / 12.;
            else if (transfer == "srgb")
                linear[c] = v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
            else
                linear[c] = v < .081 ? v / 4.5 : std::pow((v + .099) / 1.099, 1. / .45);
        }
        if (transfer == "hlg")
        {
            const double luminance =
                primaries == "p3_d65" ?
                    std::max(0., .2289745641 * linear[0] + .6917385218 * linear[1] +
                                     .0792869141 * linear[2]) :
                    std::max(0., .2627 * linear[0] + .6780 * linear[1] + .0593 * linear[2]);
            const double ootf = std::pow(luminance, .2) * peak_nits / 100.;
            for (auto &v : linear)
                v *= ootf;
        }
        if (primaries == "bt2020")
        {
            const auto source = linear;
            linear = {1.660491 * source[0] - .587641 * source[1] - .072850 * source[2],
                      -.124550 * source[0] + 1.132900 * source[1] - .008349 * source[2],
                      -.018151 * source[0] - .100579 * source[1] + 1.118730 * source[2]};
        }
        else if (primaries == "p3_d65")
        {
            // SMPTE 432-1 D65 primaries -> linear Rec.709, independent of the
            // tagged transfer function. Derived from the CSS Color 4 D65 XYZ matrices.
            const auto source = linear;
            linear = {1.224940176281 * source[0] - .224940176281 * source[1],
                      -.042056954710 * source[0] + 1.042056954710 * source[1],
                      -.019637554590 * source[0] - .078636045551 * source[1] +
                          1.098273600141 * source[2]};
        }
        const double scale =
            hdr ?
                tone_scale(std::max(0., .2126 * linear[0] + .7152 * linear[1] + .0722 * linear[2]),
                           peak_nits) :
                1.;
        for (std::size_t c = 0; c < 3; ++c)
        {
            const double v = std::clamp(linear[c] * scale, 0., 1.);
            const double encoded = v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1. / 2.4) - .055;
            output[i + c] =
                static_cast<std::uint8_t>(std::clamp(std::lround(encoded * 255.), 0L, 255L));
        }
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kInternal, "Video colour conversion exhausted memory",
                      {{"reason", "video_allocation_failed"}});
}
} // namespace ravo
