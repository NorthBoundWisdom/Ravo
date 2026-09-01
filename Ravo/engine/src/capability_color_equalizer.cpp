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

Result<void> apply_color_equalizer(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    auto hue_shifts = parameter_band_array(operation, "hue_shift");
    if (!hue_shifts)
    {
        return hue_shifts.error();
    }
    auto sat_shifts = parameter_band_array(operation, "saturation");
    if (!sat_shifts)
    {
        return sat_shifts.error();
    }
    auto light_shifts = parameter_band_array(operation, "lightness");
    if (!light_shifts)
    {
        return light_shifts.error();
    }
    bool identity = true;
    std::array<float, kColorNodes> sat_nodes{};
    std::array<float, kColorNodes> hue_nodes{};
    std::array<float, kColorNodes> bright_nodes{};
    for (std::size_t index = 0; index < kColorEqualizerBandCount; ++index)
    {
        if (std::abs(hue_shifts.value()[index]) > 1.0e-8 ||
            std::abs(sat_shifts.value()[index]) > 1.0e-8 ||
            std::abs(light_shifts.value()[index]) > 1.0e-8)
        {
            identity = false;
        }
        hue_nodes[index] = static_cast<float>(hue_shifts.value()[index]) * kTwoPi;
        sat_nodes[index] = 1.0F + static_cast<float>(sat_shifts.value()[index]);
        bright_nodes[index] = 1.0F + static_cast<float>(light_shifts.value()[index]);
    }
    if (identity)
    {
        return {};
    }

    const float hue_shift = static_cast<float>(parameter(operation, "node_placement", 0.0));
    const float smoothing_hue =
        static_cast<float>(std::clamp(parameter(operation, "smoothing_hue", 1.0), 0.05, 2.0));
    std::array<float, kUcsLutSize> lut_sat{};
    std::array<float, kUcsLutSize> lut_hue{};
    std::array<float, kUcsLutSize> lut_bright{};
    periodic_rbf_interpolate(sat_nodes, kPi, lut_sat, hue_shift, true);
    periodic_rbf_interpolate(hue_nodes, kPi / smoothing_hue, lut_hue, hue_shift, false);
    periodic_rbf_interpolate(bright_nodes, kPi, lut_bright, hue_shift, true);
    const float white = y_to_ucs_l_star(1.0F);
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> lstar(count);
    std::vector<float> u_plane(count);
    std::vector<float> v_plane(count);
    auto converted = for_each_row(image.height, cancellation,
                                  [&](const std::uint32_t row)
                                  {
                                      const std::size_t begin =
                                          static_cast<std::size_t>(row) * image.width;
                                      const std::size_t end = begin + image.width;
                                      for (std::size_t pixel = begin; pixel < end; ++pixel)
                                      {
                                          const float r = image.rgb[pixel * 3U];
                                          const float g = image.rgb[pixel * 3U + 1U];
                                          const float b = image.rgb[pixel * 3U + 2U];
                                          float x = 0.0F;
                                          float y = 0.0F;
                                          float z = 0.0F;
                                          rgb_to_xyz_d65(r, g, b, x, y, z);
                                          float xyx = 0.0F;
                                          float xyy = 0.0F;
                                          float xy_y = 0.0F;
                                          xyz_to_xyy(x, y, z, xyx, xyy, xy_y);
                                          float uv[2]{};
                                          xyy_to_ucs_uv(xyx, xyy, uv);
                                          u_plane[pixel] = uv[0];
                                          v_plane[pixel] = uv[1];
                                          lstar[pixel] = y_to_ucs_l_star(xy_y);
                                      }
                                  });
    if (!converted)
    {
        return converted.error();
    }
    const float hue_sigma =
        0.5F * static_cast<float>(std::clamp(parameter(operation, "chroma_size", 1.5), 1.0, 10.0));
    if (auto blurred = blur_plane(u_plane, image.width, image.height, hue_sigma, cancellation);
        !blurred)
    {
        return blurred.error();
    }
    if (auto blurred = blur_plane(v_plane, image.width, image.height, hue_sigma, cancellation);
        !blurred)
    {
        return blurred.error();
    }
    const int guide_radius = std::max(1, static_cast<int>(std::lround(hue_sigma)));
    if (auto filtered = self_guided_filter_plane(u_plane, image.width, image.height, guide_radius,
                                                 1.0e-5F, cancellation);
        !filtered)
    {
        return filtered.error();
    }
    if (auto filtered = self_guided_filter_plane(v_plane, image.width, image.height, guide_radius,
                                                 1.0e-5F, cancellation);
        !filtered)
    {
        return filtered.error();
    }
    return for_each_row(
        image.height, cancellation,
        [&](const std::uint32_t y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
                float jch[3]{};
                const float uv[2]{u_plane[pixel], v_plane[pixel]};
                ucs_luv_to_jch(lstar[pixel], white, uv, jch);
                float hsb[3]{};
                ucs_jch_to_hsb(jch, hsb);
                if (jch[1] > 1.0e-6F)
                {
                    const float hue = hsb[0];
                    const float sat = hsb[1];
                    hsb[0] += lookup_lut_periodic(lut_hue, hue);
                    hsb[1] = std::max(
                        0.0F,
                        sat * (1.0F + kSatEffect * (lookup_lut_periodic(lut_sat, hue) - 1.0F)));
                    const float bright_corr = sat * (lookup_lut_periodic(lut_bright, hue) - 1.0F);
                    hsb[2] = std::max(0.0F, hsb[2] * (1.0F + kBrightEffect * bright_corr));
                }
                float r = 0.0F;
                float g = 0.0F;
                float b = 0.0F;
                ucs_hsb_to_rgb(hsb, white, r, g, b);
                image.rgb[pixel * 3U] = r;
                image.rgb[pixel * 3U + 1U] = g;
                image.rgb[pixel * 3U + 2U] = b;
            }
        });
}

Result<void> apply_graduated_nd(WorkingImage &image, const OperationInstance &operation,
                                const CancellationToken &cancellation)
{
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Graduated ND masks require canonical recipe dispatch",
            {{"operation_id", operation.id}, {"reason", "graduatednd_mask_dispatch_required"}});
    }
    const double density = std::clamp(parameter(operation, "density_ev", 0.0), -8.0, 8.0);
    if (std::abs(density) <= 1.0e-8)
    {
        return {};
    }
    const double hardness = std::clamp(parameter(operation, "hardness", 0.5), 0.0, 1.0) * 100.0;
    const double rotation_deg =
        std::clamp(parameter(operation, "rotation_deg", 0.0), -180.0, 180.0);
    const double offset_norm = std::clamp(parameter(operation, "offset", 0.0), -1.0, 1.0);
    const double offset = (offset_norm + 1.0) * 50.0;
    const float hue = static_cast<float>(std::clamp(parameter(operation, "hue", 0.0), 0.0, 1.0));
    const float saturation =
        static_cast<float>(std::clamp(parameter(operation, "saturation", 0.0), 0.0, 1.0));

    std::array<float, 4> color{};
    hsl_to_rgb(hue, saturation, 0.5F, color[0], color[1], color[2]);
    if (density < 0.0)
    {
        for (float &channel : color)
        {
            channel = 1.0F - channel;
        }
    }
    std::array<float, 4> color1{};
    for (int c = 0; c < 4; ++c)
    {
        color1[static_cast<std::size_t>(c)] = 1.0F - color[static_cast<std::size_t>(c)];
    }

    const float iw = static_cast<float>(image.width);
    const float ih = static_cast<float>(image.height);
    const float hw = iw / 2.0F;
    const float hh = ih / 2.0F;
    const float hw_inv = 1.0F / hw;
    const float hh_inv = 1.0F / hh;
    const float v = static_cast<float>(-rotation_deg) * kPi / 180.0F;
    const float sinv = std::sin(v);
    const float cosv = std::cos(v);
    const float cosv_hh_inv = cosv * hh_inv;
    const float filter_radie = std::hypot(hh, hw) / hh;
    const float offset_c = static_cast<float>(offset / 100.0 * 2.0);
    const float filter_hardness =
        (1.0F / filter_radie) /
        (1.0F - (0.5F + (static_cast<float>(hardness) / 100.0F) * 0.9F / 2.0F)) * 0.5F;
    const float length_base = sinv * (-1.0F) + cosv - 1.0F + offset_c;
    const float length_inc = sinv * hw_inv * filter_hardness;
    const float dens = static_cast<float>(density);

    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        float length = (length_base - static_cast<float>(y) * cosv_hh_inv) * filter_hardness;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            if (dens > 0.0F)
            {
                const float curr = compute_density(dens, length);
                for (int c = 0; c < 3; ++c)
                {
                    image.rgb[index + static_cast<std::size_t>(c)] /=
                        color[static_cast<std::size_t>(c)] +
                        color1[static_cast<std::size_t>(c)] * curr;
                }
            }
            else
            {
                const float curr = compute_density(-dens, -length);
                for (int c = 0; c < 3; ++c)
                {
                    image.rgb[index + static_cast<std::size_t>(c)] *=
                        color[static_cast<std::size_t>(c)] +
                        color1[static_cast<std::size_t>(c)] * curr;
                }
            }
            length += length_inc;
        }
    }
    return {};
}

} // namespace ravo
