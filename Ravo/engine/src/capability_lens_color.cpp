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

Result<void> apply_lens_correction(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    const std::string mode = parameter_string(operation, "mode", "manual");
    double k1 = parameter(operation, "k1", 0.0);
    double k2 = parameter(operation, "k2", 0.0);
    double tca_r = parameter(operation, "tca_r", 1.0);
    double tca_b = parameter(operation, "tca_b", 1.0);
    double vignetting = parameter(operation, "vignetting", 0.0);
    double v_radius = std::clamp(parameter(operation, "v_radius", 0.5), 0.0, 1.0);
    double v_steepness = std::clamp(parameter(operation, "v_steepness", 0.5), 0.0, 1.0);
    if (mode == kLensModeLookup)
    {
        const auto *calibration = find_lens_calibration(
            parameter_string(operation, "camera_make", ""),
            parameter_string(operation, "camera_model", ""),
            parameter_string(operation, "lens", ""), parameter(operation, "focal_mm", 50.0));
        if (calibration == nullptr)
        {
            return make_error(ErrorCode::kNotFound,
                              "No lens calibration matches the lookup request",
                              {{"camera_make", parameter_string(operation, "camera_make", "")},
                               {"camera_model", parameter_string(operation, "camera_model", "")},
                               {"lens", parameter_string(operation, "lens", "")}});
        }
        k1 = calibration->k1;
        k2 = calibration->k2;
        tca_r = calibration->tca_r;
        tca_b = calibration->tca_b;
        vignetting = calibration->vignetting;
    }
    else if (mode != kLensModeManual)
    {
        return make_error(ErrorCode::kUnsupported, "Lens correction mode is unsupported",
                          {{"mode", mode}});
    }
    if (!std::isfinite(k1) || !std::isfinite(k2) || !std::isfinite(tca_r) ||
        !std::isfinite(tca_b) || !std::isfinite(vignetting))
    {
        return make_error(ErrorCode::kValidation, "Lens correction coefficients must be finite");
    }
    if (std::abs(k1) <= 1.0e-8 && std::abs(k2) <= 1.0e-8 && std::abs(tca_r - 1.0) <= 1.0e-8 &&
        std::abs(tca_b - 1.0) <= 1.0e-8 && std::abs(vignetting) <= 1.0e-8)
    {
        return {};
    }
    if (image.width < 2 || image.height < 2)
    {
        return make_error(ErrorCode::kValidation, "Lens correction requires a non-empty image");
    }

    std::array<float, kVignetteSplines> spline{};
    const double v = v_steepness;
    const double b = 1.0 + v_radius * 10.0;
    const double mul = -v / std::tanh(b);
    for (int i = 0; i < kVignetteSplines; ++i)
    {
        const double radius = static_cast<double>(i) / static_cast<double>(kVignetteSplines - 1);
        spline[static_cast<std::size_t>(i)] =
            static_cast<float>(v + mul * std::tanh(b * (1.0 - radius)));
    }
    const auto vignette_at = [&](const float radius)
    {
        if (radius >= 1.0F)
        {
            return spline[kVignetteSplines - 1];
        }
        const float r = radius * static_cast<float>(kVignetteSplines - 1);
        const float frac = r - std::trunc(r);
        const int i = static_cast<int>(r);
        return spline[static_cast<std::size_t>(i)] +
               (spline[static_cast<std::size_t>(i + 1)] - spline[static_cast<std::size_t>(i)]) *
                   frac;
    };

    WorkingImage source = image;
    const float cx = static_cast<float>(image.width) * 0.5F;
    const float cy = static_cast<float>(image.height) * 0.5F;
    const float inv_maxr = 1.0F / std::hypot(cx, cy);
    const float vig_strength = 2.0F * static_cast<float>(vignetting);
    const auto scales =
        std::array<float, 3>{static_cast<float>(tca_r), 1.0F, static_cast<float>(tca_b)};
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float dx = static_cast<float>(x) - cx;
            const float dy = static_cast<float>(y) - cy;
            const float ru = std::hypot(dx, dy) * inv_maxr;
            const float ru2 = ru * ru;
            // lensfun poly5 dest-to-source: Rd = Ru * (1 + k1 Ru^2 + k2 Ru^4)
            const float geometry =
                1.0F + static_cast<float>(k1) * ru2 + static_cast<float>(k2) * ru2 * ru2;
            const float vig = std::max(0.0F, vig_strength * vignette_at(ru));
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float scale = geometry * scales[channel];
                const float sx = cx + dx * scale;
                const float sy = cy + dy * scale;
                image.rgb[index + channel] = sample_channel(source, sx, sy, channel) * (1.0F + vig);
            }
        }
    }
    return {};
}

Result<void> apply_channel_mixer_rgb(WorkingImage &image, const OperationInstance &operation,
                                     const CancellationToken &cancellation)
{
    auto parsed = channel_mixer_from_parameters(operation.parameters);
    if (!parsed)
    {
        return parsed.error();
    }
    const ChannelMixerParams &params = parsed.value();
    if (params.is_identity())
    {
        return {};
    }
    if (image.width == 0 || image.height == 0 ||
        image.rgb.size() != static_cast<std::size_t>(image.width) * image.height * 3U)
    {
        return make_error(ErrorCode::kValidation,
                          "Color calibration input buffer is empty or undersized");
    }

    const auto normalize_row =
        [](const std::array<double, kChannelMixerChannelCount> &source, const bool normalize)
    {
        ChannelVector row{static_cast<float>(source[0]), static_cast<float>(source[1]),
                          static_cast<float>(source[2])};
        if (normalize)
        {
            const float sum = row[0] + row[1] + row[2];
            for (float &value : row)
            {
                value /= sum;
            }
        }
        return row;
    };
    ChannelMatrix mix{normalize_row(params.red, params.normalize_red),
                      normalize_row(params.green, params.normalize_green),
                      normalize_row(params.blue, params.normalize_blue)};
    ChannelVector saturation{};
    ChannelVector lightness{};
    ChannelVector grey{};
    const float saturation_norm =
        params.normalize_saturation ?
            static_cast<float>(
                (params.saturation[0] + params.saturation[1] + params.saturation[2]) / 3.0) :
            0.0F;
    const float lightness_norm =
        params.normalize_lightness ?
            static_cast<float>((params.lightness[0] + params.lightness[1] + params.lightness[2]) /
                               3.0) :
            0.0F;
    double grey_norm = params.grey[0] + params.grey[1] + params.grey[2];
    const bool apply_grey = params.grey[0] != 0.0 || params.grey[1] != 0.0 || params.grey[2] != 0.0;
    if (!params.normalize_grey || std::abs(grey_norm) <= 1.0e-12)
    {
        grey_norm = 1.0;
    }
    for (std::size_t channel = 0; channel < kChannelMixerChannelCount; ++channel)
    {
        saturation[channel] = -static_cast<float>(params.saturation[channel]) + saturation_norm;
        lightness[channel] = static_cast<float>(params.lightness[channel]) - lightness_norm;
        grey[channel] = static_cast<float>(params.grey[channel] / grey_norm);
    }

    ChannelAdaptation adaptation = ChannelAdaptation::kRgb;
    if (params.adaptation == kChannelMixerAdaptationCat16)
    {
        adaptation = ChannelAdaptation::kCat16;
    }
    else if (params.adaptation == kChannelMixerAdaptationLinearBradford)
    {
        adaptation = ChannelAdaptation::kLinearBradford;
    }
    else if (params.adaptation == kChannelMixerAdaptationFullBradford)
    {
        adaptation = ChannelAdaptation::kFullBradford;
    }
    else if (params.adaptation == kChannelMixerAdaptationXyz)
    {
        adaptation = ChannelAdaptation::kXyz;
    }

    const ChannelMatrix rgb_to_xyz =
        channel_matrix_multiply(kXyzD65ToD50Cat16, kLinearSrgbToXyzD65);
    ChannelMatrix xyz_to_rgb{};
    if (!channel_matrix_inverse(rgb_to_xyz, xyz_to_rgb))
    {
        return make_error(ErrorCode::kInternal,
                          "Color calibration working profile matrix is not invertible");
    }
    const ChannelMatrix xyz_to_adaptation = adaptation == ChannelAdaptation::kCat16 ?
                                                kXyzToCat16 :
                                            (adaptation == ChannelAdaptation::kLinearBradford ||
                                             adaptation == ChannelAdaptation::kFullBradford) ?
                                                kXyzToBradford :
                                                kIdentityMatrix;
    const ChannelMatrix adaptation_to_xyz = adaptation == ChannelAdaptation::kCat16 ?
                                                kCat16ToXyz :
                                            (adaptation == ChannelAdaptation::kLinearBradford ||
                                             adaptation == ChannelAdaptation::kFullBradford) ?
                                                kBradfordToXyz :
                                                kIdentityMatrix;
    const ChannelMatrix rgb_to_adaptation = channel_matrix_multiply(xyz_to_adaptation, rgb_to_xyz);
    const ChannelMatrix mix_to_xyz = adaptation == ChannelAdaptation::kRgb ?
                                         channel_matrix_multiply(rgb_to_xyz, mix) :
                                         channel_matrix_multiply(adaptation_to_xyz, mix);

    const ChannelVector illuminant_xyz{
        static_cast<float>(params.illuminant_x / params.illuminant_y), 1.0F,
        static_cast<float>((1.0 - params.illuminant_x - params.illuminant_y) /
                           params.illuminant_y)};
    const ChannelVector illuminant = channel_matrix_apply(xyz_to_adaptation, illuminant_xyz);
    if (adaptation != ChannelAdaptation::kRgb &&
        (illuminant[0] <= kChannelMixerNormMin || illuminant[1] <= kChannelMixerNormMin ||
         illuminant[2] <= kChannelMixerNormMin))
    {
        return make_error(ErrorCode::kValidation,
                          "Color calibration illuminant is invalid in the adaptation space");
    }
    const float bradford_power =
        std::pow(0.818155F / std::max(illuminant[2], kChannelMixerNormMin), 0.0834F);
    const float gamut = static_cast<float>(params.gamut);

    for (std::uint32_t row = 0; row < image.height; ++row)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t column = 0; column < image.width; ++column)
        {
            const std::size_t index = (static_cast<std::size_t>(row) * image.width + column) * 3U;
            ChannelVector input{image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]};
            if (!channel_vector_is_finite(input))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color calibration input contains a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
            if (params.clip)
            {
                channel_clip_negative(input);
            }

            ChannelVector adapted{};
            if (adaptation == ChannelAdaptation::kFullBradford)
            {
                const ChannelVector xyz = channel_matrix_apply(rgb_to_xyz, input);
                const float luminance = xyz[1];
                adapted = channel_matrix_apply(kXyzToBradford, xyz);
                channel_downscale(adapted, luminance);
                adapted[0] *= 0.996078F / illuminant[0];
                adapted[1] *= 1.020646F / illuminant[1];
                const float blue = adapted[2] / illuminant[2];
                adapted[2] = 0.818155F * (blue > 0.0F ? std::pow(blue, bradford_power) : blue);
                channel_upscale(adapted, luminance);
            }
            else if (adaptation == ChannelAdaptation::kLinearBradford)
            {
                adapted = channel_matrix_apply(rgb_to_adaptation, input);
                adapted[0] *= 0.996078F / illuminant[0];
                adapted[1] *= 1.020646F / illuminant[1];
                adapted[2] *= 0.818155F / illuminant[2];
            }
            else if (adaptation == ChannelAdaptation::kCat16)
            {
                adapted = channel_matrix_apply(rgb_to_adaptation, input);
                adapted[0] *= 0.994535F / illuminant[0];
                adapted[1] *= 1.000997F / illuminant[1];
                adapted[2] *= 0.833036F / illuminant[2];
            }
            else if (adaptation == ChannelAdaptation::kXyz)
            {
                adapted = channel_matrix_apply(rgb_to_xyz, input);
                adapted[0] *= 0.9642119944211994F / illuminant[0];
                adapted[1] /= illuminant[1];
                adapted[2] *= 0.8251882845188288F / illuminant[2];
            }
            else
            {
                adapted = input;
            }

            ChannelVector xyz = channel_matrix_apply(mix_to_xyz, adapted);
            if (params.clip)
            {
                channel_clip_negative(xyz);
            }
            ChannelVector gamut_mapped{};
            if (!channel_gamut_map(xyz, gamut, params.clip, gamut_mapped))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color calibration gamut mapping produced a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
            ChannelVector working = adaptation == ChannelAdaptation::kRgb ?
                                        channel_matrix_apply(xyz_to_rgb, gamut_mapped) :
                                        channel_matrix_apply(xyz_to_adaptation, gamut_mapped);
            if (params.clip)
            {
                channel_clip_negative(working);
            }
            ChannelVector adjusted = channel_luma_chroma(working, saturation, lightness);
            if (params.clip)
            {
                channel_clip_negative(adjusted);
            }

            ChannelVector output{};
            if (apply_grey)
            {
                const float value = std::max(
                    adjusted[0] * grey[0] + adjusted[1] * grey[1] + adjusted[2] * grey[2], 0.0F);
                output = {value, value, value};
            }
            else
            {
                xyz = adaptation == ChannelAdaptation::kRgb ?
                          channel_matrix_apply(rgb_to_xyz, adjusted) :
                          channel_matrix_apply(adaptation_to_xyz, adjusted);
                if (params.clip)
                {
                    channel_clip_negative(xyz);
                }
                output = channel_matrix_apply(xyz_to_rgb, xyz);
                if (params.clip)
                {
                    channel_clip_negative(output);
                }
            }
            if (!channel_vector_is_finite(output))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color calibration produced a non-finite sample",
                                  {{"sample_index", std::to_string(index)}});
            }
            image.rgb[index] = output[0];
            image.rgb[index + 1U] = output[1];
            image.rgb[index + 2U] = output[2];
        }
    }
    return {};
}

} // namespace ravo
