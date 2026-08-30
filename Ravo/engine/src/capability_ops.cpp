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
#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTwoPi = 2.0F * kPi;
constexpr int kColorNodes = 8;
constexpr int kUcsLutSize = 512;
constexpr int kToneAnchorCount = 9;
constexpr int kToneLutResolution = 10000;
constexpr float kToneLutMinEv = -8.0F;
constexpr float kToneLutMaxEv = 0.0F;
constexpr float kToneMaskRadiusOriginalPixels = 240.0F;
constexpr float kToneMaskEpsilonEv = 0.04F;
constexpr int kVignetteSplines = 512;
constexpr int kDenoiseBands = 7;
constexpr float kDenoisePFulcrum = 0.05F;
constexpr float kSatEffect = 2.0F;
constexpr float kBrightEffect = 8.0F;
constexpr float kUcsLStarRange = 2.098883786377F;
constexpr float kUcsLStarUpper = 2.09885F;
constexpr float kAngleShiftDeg = 20.0F;
constexpr float kGenericNoiseA = 1.0e-4F;
constexpr float kGenericNoiseB = 0.0F;
constexpr std::size_t kDenoiseNoiseSampleLimit = 1U << 18U;
constexpr float kDenoiseGaussianMad = 0.67448975F;
constexpr float kChannelMixerNormMin = 1.52587890625e-05F;
constexpr float kChannelMixerInverseSqrt3 = 0.5773502691896258F;

using detail::for_each_row;

using ChannelVector = std::array<float, 3>;
using ChannelMatrix = std::array<ChannelVector, 3>;

constexpr ChannelMatrix kLinearSrgbToXyzD65 = {ChannelVector{0.4124564F, 0.3575761F, 0.1804375F},
                                               ChannelVector{0.2126729F, 0.7151522F, 0.0721750F},
                                               ChannelVector{0.0193339F, 0.1191920F, 0.9503041F}};
constexpr ChannelMatrix kXyzD65ToD50Cat16 = {
    ChannelVector{1.01085433F, 0.0407086103F, -0.0341425825F},
    ChannelVector{0.00542814201F, 0.993581926F, 0.00115592039F},
    ChannelVector{0.000250722468F, -0.0114918759F, 0.767964947F}};
constexpr ChannelMatrix kXyzToCat16 = {ChannelVector{0.401288F, 0.650173F, -0.051461F},
                                       ChannelVector{-0.250268F, 1.204414F, 0.045854F},
                                       ChannelVector{-0.002079F, 0.048952F, 0.953127F}};
constexpr ChannelMatrix kCat16ToXyz = {ChannelVector{1.862068F, -1.011255F, 0.149187F},
                                       ChannelVector{0.38752F, 0.621447F, -0.008974F},
                                       ChannelVector{-0.015841F, -0.034123F, 1.049964F}};
constexpr ChannelMatrix kXyzToBradford = {ChannelVector{0.8951F, 0.2664F, -0.1614F},
                                          ChannelVector{-0.7502F, 1.7135F, 0.0367F},
                                          ChannelVector{0.0389F, -0.0685F, 1.0296F}};
constexpr ChannelMatrix kBradfordToXyz = {ChannelVector{0.9870F, -0.1471F, 0.1600F},
                                          ChannelVector{0.4323F, 0.5184F, 0.0493F},
                                          ChannelVector{-0.0085F, 0.0400F, 0.9685F}};
constexpr ChannelMatrix kIdentityMatrix = {ChannelVector{1.0F, 0.0F, 0.0F},
                                           ChannelVector{0.0F, 1.0F, 0.0F},
                                           ChannelVector{0.0F, 0.0F, 1.0F}};

enum class ChannelAdaptation
{
    kRgb,
    kCat16,
    kLinearBradford,
    kFullBradford,
    kXyz,
};

[[nodiscard]] ChannelVector channel_matrix_apply(const ChannelMatrix &matrix,
                                                 const ChannelVector &input) noexcept
{
    ChannelVector output{};
    for (std::size_t row = 0; row < output.size(); ++row)
    {
        output[row] = std::fma(matrix[row][0], input[0],
                               std::fma(matrix[row][1], input[1], matrix[row][2] * input[2]));
    }
    return output;
}

[[nodiscard]] ChannelMatrix channel_matrix_multiply(const ChannelMatrix &left,
                                                    const ChannelMatrix &right) noexcept
{
    ChannelMatrix output{};
    for (std::size_t row = 0; row < output.size(); ++row)
    {
        for (std::size_t column = 0; column < output[row].size(); ++column)
        {
            output[row][column] =
                std::fma(left[row][0], right[0][column],
                         std::fma(left[row][1], right[1][column], left[row][2] * right[2][column]));
        }
    }
    return output;
}

[[nodiscard]] bool channel_matrix_inverse(const ChannelMatrix &input,
                                          ChannelMatrix &output) noexcept
{
    const float determinant =
        input[0][0] * (input[1][1] * input[2][2] - input[1][2] * input[2][1]) -
        input[0][1] * (input[1][0] * input[2][2] - input[1][2] * input[2][0]) +
        input[0][2] * (input[1][0] * input[2][1] - input[1][1] * input[2][0]);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-8F)
    {
        return false;
    }
    const float inverse = 1.0F / determinant;
    output = {{{(input[1][1] * input[2][2] - input[1][2] * input[2][1]) * inverse,
                (input[0][2] * input[2][1] - input[0][1] * input[2][2]) * inverse,
                (input[0][1] * input[1][2] - input[0][2] * input[1][1]) * inverse},
               {(input[1][2] * input[2][0] - input[1][0] * input[2][2]) * inverse,
                (input[0][0] * input[2][2] - input[0][2] * input[2][0]) * inverse,
                (input[0][2] * input[1][0] - input[0][0] * input[1][2]) * inverse},
               {(input[1][0] * input[2][1] - input[1][1] * input[2][0]) * inverse,
                (input[0][1] * input[2][0] - input[0][0] * input[2][1]) * inverse,
                (input[0][0] * input[1][1] - input[0][1] * input[1][0]) * inverse}}};
    return true;
}

[[nodiscard]] bool channel_vector_is_finite(const ChannelVector &value) noexcept
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

void channel_clip_negative(ChannelVector &value) noexcept
{
    for (float &sample : value)
    {
        sample = std::max(sample, 0.0F);
    }
}

void channel_downscale(ChannelVector &value, const float scale) noexcept
{
    const float divisor =
        scale > kChannelMixerNormMin ? scale + kChannelMixerNormMin : kChannelMixerNormMin;
    for (float &sample : value)
    {
        sample /= divisor;
    }
}

void channel_upscale(ChannelVector &value, const float scale) noexcept
{
    const float factor =
        scale > kChannelMixerNormMin ? scale + kChannelMixerNormMin : kChannelMixerNormMin;
    for (float &sample : value)
    {
        sample *= factor;
    }
}

[[nodiscard]] bool channel_gamut_map(const ChannelVector &input, const float gamut, const bool clip,
                                     ChannelVector &output) noexcept
{
    constexpr float d50_u = 0.20915914598542354F;
    constexpr float d50_v = 0.488075320769787F;
    const float sum = input[0] + input[1] + input[2];
    float x = sum > 0.0F ? input[0] / sum : 0.34567F;
    float y = sum > 0.0F ? input[1] / sum : 0.35850F;
    const float luminance = input[1];
    const float uv_denominator = -2.0F * x + 12.0F * y + 3.0F;
    if (!std::isfinite(uv_denominator) || std::abs(uv_denominator) <= kChannelMixerNormMin)
    {
        return false;
    }
    float u = 4.0F * x / uv_denominator;
    float v = 9.0F * y / uv_denominator;
    const float du = d50_u - u;
    const float dv = d50_v - v;
    const float delta = luminance * (du * du + dv * dv);
    const float correction = gamut == 0.0F ? 0.0F : std::pow(delta, 1.0F / gamut);
    if (!std::isfinite(correction))
    {
        return false;
    }
    const float adjusted_u = std::fma(correction, du, u);
    const float adjusted_v = std::fma(correction, dv, v);
    u = u > d50_u ? std::max(adjusted_u, d50_u) : std::min(adjusted_u, d50_u);
    v = v > d50_v ? std::max(adjusted_v, d50_v) : std::min(adjusted_v, d50_v);
    const float xy_denominator = 6.0F * u - 16.0F * v + 12.0F;
    if (!std::isfinite(xy_denominator) || std::abs(xy_denominator) <= kChannelMixerNormMin)
    {
        return false;
    }
    x = 9.0F * u / xy_denominator;
    y = 4.0F * v / xy_denominator;
    if (clip)
    {
        x = std::max(x, 0.0F);
        y = std::max(y, 0.0F);
    }
    y = std::max(y, kChannelMixerNormMin);
    const float chroma_sum = x + y;
    if (chroma_sum >= 1.0F)
    {
        x /= chroma_sum;
        y /= chroma_sum;
    }
    output = {luminance * x / y, luminance, luminance * (1.0F - x - y) / y};
    return channel_vector_is_finite(output);
}

[[nodiscard]] ChannelVector channel_luma_chroma(const ChannelVector &input,
                                                const ChannelVector &saturation,
                                                const ChannelVector &lightness) noexcept
{
    float norm =
        std::max(std::sqrt(input[0] * input[0] + input[1] * input[1] + input[2] * input[2]),
                 kChannelMixerNormMin);
    const float average = std::max((input[0] + input[1] + input[2]) / 3.0F, kChannelMixerNormMin);
    const float light_mix =
        std::fma(input[0], lightness[0], std::fma(input[1], lightness[1], input[2] * lightness[2]));
    norm *= kChannelMixerInverseSqrt3;
    ChannelVector output{input[0] / norm, input[1] / norm, input[2] / norm};
    const float coefficient =
        (output[0] * saturation[0] + output[1] * saturation[1] + output[2] * saturation[2]) / 3.0F;
    for (float &ratio : output)
    {
        const float minimum = ratio < 0.0F ? ratio : 0.0F;
        ratio = std::max(std::fma(1.0F - ratio, coefficient, ratio), minimum);
    }
    const float adjusted_norm =
        std::max(std::sqrt(output[0] * output[0] + output[1] * output[1] + output[2] * output[2]),
                 kChannelMixerNormMin);
    norm /= adjusted_norm * kChannelMixerInverseSqrt3;
    norm *= std::max(1.0F + light_mix / average, 0.0F);
    for (float &ratio : output)
    {
        ratio *= norm;
    }
    return output;
}

[[nodiscard]] double as_number(const ParameterValue &value, const double fallback)
{
    if (std::holds_alternative<double>(value.value))
    {
        return std::get<double>(value.value);
    }
    if (std::holds_alternative<std::int64_t>(value.value))
    {
        return static_cast<double>(std::get<std::int64_t>(value.value));
    }
    return fallback;
}

[[nodiscard]] double parameter(const OperationInstance &operation, const std::string_view name,
                               const double fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    return as_number(found->second, fallback);
}

[[nodiscard]] std::string parameter_string(const OperationInstance &operation,
                                           const std::string_view name, const std::string &fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *text = std::get_if<std::string>(&found->second.value); text != nullptr)
    {
        return *text;
    }
    return fallback;
}

[[nodiscard]] bool parameter_bool(const OperationInstance &operation, const std::string_view name,
                                  const bool fallback)
{
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return fallback;
    }
    if (const auto *flag = std::get_if<bool>(&found->second.value); flag != nullptr)
    {
        return *flag;
    }
    return fallback;
}

[[nodiscard]] Result<std::array<double, kColorEqualizerBandCount>>
parameter_band_array(const OperationInstance &operation, const std::string_view name)
{
    std::array<double, kColorEqualizerBandCount> values{};
    const auto found = operation.parameters.find(std::string(name));
    if (found == operation.parameters.end())
    {
        return values;
    }
    const auto *array = std::get_if<ParameterValue::Array>(&found->second.value);
    if (array == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Color equalizer band parameter must be an array",
                          {{"parameter", std::string(name)}});
    }
    if (array->size() != kColorEqualizerBandCount)
    {
        return make_error(
            ErrorCode::kValidation, "Color equalizer band array must have 8 values",
            {{"parameter", std::string(name)}, {"count", std::to_string(array->size())}});
    }
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const double value = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(value))
        {
            return make_error(ErrorCode::kValidation, "Color equalizer band value must be finite",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        values[index] = value;
    }
    return values;
}

[[nodiscard]] bool ascii_iequals(const std::string_view left, const std::string_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index)
    {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] float clip01(const float value) noexcept
{
    return std::clamp(value, 0.0F, 1.0F);
}

void hsl_to_rgb(const float h, const float s, const float l, float &r, float &g, float &b) noexcept
{
    if (s <= 1.0e-6F)
    {
        r = g = b = l;
        return;
    }
    const auto hue_to_rgb = [](const float p, const float q, float t)
    {
        if (t < 0.0F)
        {
            t += 1.0F;
        }
        if (t > 1.0F)
        {
            t -= 1.0F;
        }
        if (t < 1.0F / 6.0F)
        {
            return p + (q - p) * 6.0F * t;
        }
        if (t < 0.5F)
        {
            return q;
        }
        if (t < 2.0F / 3.0F)
        {
            return p + (q - p) * (2.0F / 3.0F - t) * 6.0F;
        }
        return p;
    };
    const float q = l < 0.5F ? l * (1.0F + s) : l + s - l * s;
    const float p = 2.0F * l - q;
    r = hue_to_rgb(p, q, h + 1.0F / 3.0F);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0F / 3.0F);
}

[[nodiscard]] float sample_channel(const WorkingImage &image, const float x, const float y,
                                   const std::size_t channel) noexcept
{
    const float max_x = static_cast<float>(image.width - 1U);
    const float max_y = static_cast<float>(image.height - 1U);
    const float sx = std::clamp(x, 0.0F, max_x);
    const float sy = std::clamp(y, 0.0F, max_y);
    const std::uint32_t x0 = static_cast<std::uint32_t>(sx);
    const std::uint32_t y0 = static_cast<std::uint32_t>(sy);
    const std::uint32_t x1 = std::min(x0 + 1U, image.width - 1U);
    const std::uint32_t y1 = std::min(y0 + 1U, image.height - 1U);
    const float tx = sx - static_cast<float>(x0);
    const float ty = sy - static_cast<float>(y0);
    const auto at = [&](const std::uint32_t px, const std::uint32_t py)
    { return image.rgb[(static_cast<std::size_t>(py) * image.width + px) * 3U + channel]; };
    const float top = at(x0, y0) * (1.0F - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0F - tx) + at(x1, y1) * tx;
    return top * (1.0F - ty) + bottom * ty;
}

[[nodiscard]] std::uint8_t cfa_channel(const DecodedRaw &raw, const std::uint32_t x,
                                       const std::uint32_t y) noexcept
{
    return raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width + (x % raw.cfa_width)];
}

struct LensCalibration
{
    std::string_view camera_make;
    std::string_view camera_model;
    std::string_view lens;
    double focal_mm = 50.0;
    double k1 = 0.0;
    double k2 = 0.0;
    double tca_r = 1.0;
    double tca_b = 1.0;
    double vignetting = 0.0;
};

constexpr LensCalibration kLensCalibrations[] = {
    {"RavoTest", "RavoSensor", "FixtureLens", 50.0, -0.15, 0.02, 1.002, 0.998, 0.35},
};

[[nodiscard]] const LensCalibration *find_lens_calibration(const std::string_view make,
                                                           const std::string_view model,
                                                           const std::string_view lens,
                                                           const double focal_mm)
{
    const LensCalibration *best = nullptr;
    double best_delta = 1.0e9;
    for (const auto &entry : kLensCalibrations)
    {
        if (!ascii_iequals(entry.camera_make, make) || !ascii_iequals(entry.camera_model, model) ||
            !ascii_iequals(entry.lens, lens))
        {
            continue;
        }
        const double delta = std::abs(entry.focal_mm - focal_mm);
        if (delta <= 2.0 && delta < best_delta)
        {
            best = &entry;
            best_delta = delta;
        }
    }
    return best;
}

Result<void> blur_plane(std::vector<float> &plane, const std::uint32_t width,
                        const std::uint32_t height, const float sigma,
                        const CancellationToken &cancellation)
{
    if (plane.empty() || width == 0 || height == 0 || sigma <= 0.01F)
    {
        return {};
    }
    const int radius = std::max(1, static_cast<int>(std::ceil(static_cast<double>(sigma) * 3.0)));
    std::vector<float> kernel(static_cast<std::size_t>(radius) * 2U + 1U);
    float kernel_sum = 0.0F;
    const float denom = 2.0F * sigma * sigma;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const float weight = std::exp(-static_cast<float>(offset * offset) / denom);
        kernel[static_cast<std::size_t>(offset + radius)] = weight;
        kernel_sum += weight;
    }
    for (float &weight : kernel)
    {
        weight /= kernel_sum;
    }

    std::vector<float> temp(plane.size());
    auto horizontal =
        for_each_row(height, cancellation,
                     [&](const std::uint32_t y)
                     {
                         for (std::uint32_t x = 0; x < width; ++x)
                         {
                             float acc = 0.0F;
                             for (int offset = -radius; offset <= radius; ++offset)
                             {
                                 const int sample_x = std::clamp(static_cast<int>(x) + offset, 0,
                                                                 static_cast<int>(width) - 1);
                                 acc += plane[static_cast<std::size_t>(y) * width +
                                              static_cast<std::uint32_t>(sample_x)] *
                                        kernel[static_cast<std::size_t>(offset + radius)];
                             }
                             temp[static_cast<std::size_t>(y) * width + x] = acc;
                         }
                     });
    if (!horizontal)
    {
        return horizontal.error();
    }

    auto vertical = for_each_row(
        width, cancellation,
        [&](const std::uint32_t x)
        {
            for (std::uint32_t y = 0; y < height; ++y)
            {
                float acc = 0.0F;
                for (int offset = -radius; offset <= radius; ++offset)
                {
                    const int sample_y =
                        std::clamp(static_cast<int>(y) + offset, 0, static_cast<int>(height) - 1);
                    acc += temp[static_cast<std::size_t>(static_cast<std::uint32_t>(sample_y)) *
                                    width +
                                x] *
                           kernel[static_cast<std::size_t>(offset + radius)];
                }
                plane[static_cast<std::size_t>(y) * width + x] = acc;
            }
        });
    if (!vertical)
    {
        return vertical.error();
    }
    return {};
}

Result<void> box_blur_plane(const std::vector<float> &input, std::vector<float> &output,
                            const std::uint32_t width, const std::uint32_t height, const int radius,
                            const CancellationToken &cancellation)
{
    output.assign(input.size(), 0.0F);
    if (input.empty() || width == 0 || height == 0)
    {
        return {};
    }
    if (radius <= 0)
    {
        output = input;
        return {};
    }
    const int window = radius * 2 + 1;
    std::vector<float> temp(input.size());
    auto horizontal = for_each_row(
        height, cancellation,
        [&](const std::uint32_t y)
        {
            double acc = 0.0;
            for (int x = -radius; x <= radius; ++x)
            {
                const int sx = std::clamp(x, 0, static_cast<int>(width) - 1);
                acc += input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(sx)];
            }
            for (std::uint32_t x = 0; x < width; ++x)
            {
                temp[static_cast<std::size_t>(y) * width + x] = static_cast<float>(acc / window);
                const int drop =
                    std::clamp(static_cast<int>(x) - radius, 0, static_cast<int>(width) - 1);
                const int add =
                    std::clamp(static_cast<int>(x) + radius + 1, 0, static_cast<int>(width) - 1);
                acc +=
                    input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(add)] -
                    input[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(drop)];
            }
        });
    if (!horizontal)
    {
        return horizontal.error();
    }

    auto vertical = for_each_row(
        width, cancellation,
        [&](const std::uint32_t x)
        {
            double acc = 0.0;
            for (int y = -radius; y <= radius; ++y)
            {
                const int sy = std::clamp(y, 0, static_cast<int>(height) - 1);
                acc += temp[static_cast<std::size_t>(sy) * width + x];
            }
            for (std::uint32_t y = 0; y < height; ++y)
            {
                output[static_cast<std::size_t>(y) * width + x] = static_cast<float>(acc / window);
                const int drop =
                    std::clamp(static_cast<int>(y) - radius, 0, static_cast<int>(height) - 1);
                const int add =
                    std::clamp(static_cast<int>(y) + radius + 1, 0, static_cast<int>(height) - 1);
                acc += temp[static_cast<std::size_t>(add) * width + x] -
                       temp[static_cast<std::size_t>(drop) * width + x];
            }
        });
    if (!vertical)
    {
        return vertical.error();
    }
    return {};
}

Result<void> self_guided_filter_plane(std::vector<float> &plane, const std::uint32_t width,
                                      const std::uint32_t height, const int radius, const float eps,
                                      const CancellationToken &cancellation)
{
    if (radius <= 0)
    {
        return {};
    }
    const std::size_t count = plane.size();
    std::vector<float> mean;
    std::vector<float> corr;
    if (auto blurred = box_blur_plane(plane, mean, width, height, radius, cancellation); !blurred)
    {
        return blurred.error();
    }
    {
        std::vector<float> squared(count);
        auto products = for_each_row(height, cancellation,
                                     [&](const std::uint32_t row)
                                     {
                                         const std::size_t begin =
                                             static_cast<std::size_t>(row) * width;
                                         const std::size_t end = begin + width;
                                         for (std::size_t index = begin; index < end; ++index)
                                         {
                                             squared[index] = plane[index] * plane[index];
                                         }
                                     });
        if (!products)
        {
            return products.error();
        }
        if (auto blurred = box_blur_plane(squared, corr, width, height, radius, cancellation);
            !blurred)
        {
            return blurred.error();
        }
    }
    auto coefficients =
        for_each_row(height, cancellation,
                     [&](const std::uint32_t row)
                     {
                         const std::size_t begin = static_cast<std::size_t>(row) * width;
                         const std::size_t end = begin + width;
                         for (std::size_t index = begin; index < end; ++index)
                         {
                             const float cov = corr[index] - mean[index] * mean[index];
                             const float var = std::max(0.0F, cov);
                             corr[index] = var / (var + eps);
                             mean[index] -= corr[index] * mean[index];
                         }
                     });
    if (!coefficients)
    {
        return coefficients.error();
    }
    std::vector<float> mean_a;
    if (auto blurred = box_blur_plane(corr, mean_a, width, height, radius, cancellation); !blurred)
    {
        return blurred.error();
    }
    if (auto blurred = box_blur_plane(mean, corr, width, height, radius, cancellation); !blurred)
    {
        return blurred.error();
    }
    return for_each_row(height, cancellation,
                        [&](const std::uint32_t row)
                        {
                            const std::size_t begin = static_cast<std::size_t>(row) * width;
                            const std::size_t end = begin + width;
                            for (std::size_t index = begin; index < end; ++index)
                            {
                                plane[index] = mean_a[index] * plane[index] + corr[index];
                            }
                        });
}

bool cholesky_solve(std::vector<float> &a_square, std::vector<float> &y, const std::size_t n)
{
    std::vector<float> l(n * n, 0.0F);
    std::vector<float> b(n, 0.0F);
    std::vector<float> x(n, 0.0F);
    if (a_square[0] <= 0.0F)
    {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < i + 1U; ++j)
        {
            float sum = 0.0F;
            for (std::size_t k = 0; k < j; ++k)
            {
                sum += l[i * n + k] * l[j * n + k];
            }
            if (i == j)
            {
                const float temp = a_square[i * n + i] - sum;
                if (temp <= 0.0F)
                {
                    return false;
                }
                l[i * n + j] = std::sqrt(temp);
            }
            else
            {
                if (l[j * n + j] == 0.0F)
                {
                    return false;
                }
                l[i * n + j] = (a_square[i * n + j] - sum) / l[j * n + j];
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        float sum = y[i];
        for (std::size_t j = 0; j < i; ++j)
        {
            sum -= l[i * n + j] * b[j];
        }
        if (l[i * n + i] == 0.0F)
        {
            return false;
        }
        b[i] = sum / l[i * n + i];
    }
    for (int i = static_cast<int>(n) - 1; i >= 0; --i)
    {
        float sum = b[static_cast<std::size_t>(i)];
        for (int j = static_cast<int>(n) - 1; j > i; --j)
        {
            sum -= l[static_cast<std::size_t>(j) * n + static_cast<std::size_t>(i)] *
                   x[static_cast<std::size_t>(j)];
        }
        if (l[static_cast<std::size_t>(i) * n + static_cast<std::size_t>(i)] == 0.0F)
        {
            return false;
        }
        x[static_cast<std::size_t>(i)] =
            sum / l[static_cast<std::size_t>(i) * n + static_cast<std::size_t>(i)];
    }
    y = x;
    return true;
}

bool pseudo_solve(std::vector<float> &a, std::vector<float> &y, const std::size_t m,
                  const std::size_t n)
{
    if (m < n || n < 2)
    {
        return false;
    }
    std::vector<float> a_square(n * n, 0.0F);
    std::vector<float> y_square(n, 0.0F);
    for (std::size_t i = 0; i < n; ++i)
    {
        for (std::size_t j = 0; j < n; ++j)
        {
            float sum = 0.0F;
            for (std::size_t k = 0; k < m; ++k)
            {
                sum += a[k * n + i] * a[k * n + j];
            }
            a_square[i * n + j] = sum;
        }
        float sum = 0.0F;
        for (std::size_t k = 0; k < m; ++k)
        {
            sum += a[k * n + i] * y[k];
        }
        y_square[i] = sum;
    }
    if (!cholesky_solve(a_square, y_square, n))
    {
        return false;
    }
    for (std::size_t i = 0; i < n; ++i)
    {
        y[i] = y_square[i];
    }
    return true;
}

void rgb_to_xyz_d65(const float r, const float g, const float b, float &x, float &y,
                    float &z) noexcept
{
    x = 0.4124564F * r + 0.3575761F * g + 0.1804375F * b;
    y = 0.2126729F * r + 0.7151522F * g + 0.0721750F * b;
    z = 0.0193339F * r + 0.1191920F * g + 0.9503041F * b;
}

void xyz_d65_to_rgb(const float x, const float y, const float z, float &r, float &g,
                    float &b) noexcept
{
    r = 3.2404542F * x - 1.5371385F * y - 0.4985314F * z;
    g = -0.9692660F * x + 1.8760108F * y + 0.0415560F * z;
    b = 0.0556434F * x - 0.2040259F * y + 1.0572252F * z;
}

void xyz_to_xyy(const float x, const float y, const float z, float &xyx, float &xyy,
                float &xy_y) noexcept
{
    const float sum = x + y + z;
    if (sum <= 1.0e-12F)
    {
        xyx = 0.3127F;
        xyy = 0.3290F;
        xy_y = y;
        return;
    }
    xyx = x / sum;
    xyy = y / sum;
    xy_y = y;
}

void xyy_to_xyz(const float xyx, const float xyy, const float xy_y, float &x, float &y,
                float &z) noexcept
{
    if (xyy <= 1.0e-12F)
    {
        x = y = z = 0.0F;
        return;
    }
    x = xyx * xy_y / xyy;
    y = xy_y;
    z = (1.0F - xyx - xyy) * xy_y / xyy;
}

[[nodiscard]] float y_to_ucs_l_star(const float y) noexcept
{
    const float y_hat = std::pow(std::max(y, 0.0F), 0.631651345306265F);
    return kUcsLStarRange * y_hat / (y_hat + 1.12426773749357F);
}

[[nodiscard]] float ucs_l_star_to_y(const float l_star) noexcept
{
    const float clamped = std::clamp(l_star, 0.0F, kUcsLStarUpper);
    return std::pow((1.12426773749357F * clamped) / (kUcsLStarRange - clamped),
                    1.5831518565279648F);
}

void xyy_to_ucs_uv(const float xyx, const float xyy, float uv[2]) noexcept
{
    const float uvd0 = -0.783941002840055F * xyx + 0.277512987809202F * xyy + 0.153836578598858F;
    const float uvd1 = 0.745273540913283F * xyx - 0.205375866083878F * xyy - 0.165478376301988F;
    const float uvd2 = 0.318707282433486F * xyx + 2.16743692732158F * xyy + 0.291320554395942F;
    const float div = uvd2 >= 0.0F ? std::max(uvd2, std::numeric_limits<float>::min()) :
                                     std::min(uvd2, -std::numeric_limits<float>::min());
    const float uv0 = uvd0 / div;
    const float uv1 = uvd1 / div;
    const float uv_star0 = 1.39656225667F * uv0 / (std::abs(uv0) + 1.49217352929F);
    const float uv_star1 = 1.4513954287F * uv1 / (std::abs(uv1) + 1.52488637914F);
    uv[0] = -1.124983854323892F * uv_star0 - 0.980483721769325F * uv_star1;
    uv[1] = 1.86323315098672F * uv_star0 + 1.971853092390862F * uv_star1;
}

void ucs_luv_to_jch(const float l_star, const float l_white, const float uv[2],
                    float jch[3]) noexcept
{
    const float m2 = uv[0] * uv[0] + uv[1] * uv[1];
    jch[0] = l_star / l_white;
    jch[1] = 15.932993652962535F * std::pow(std::max(l_star, 0.0F), 0.6523997524738018F) *
             std::pow(m2, 0.6007557017508491F) / l_white;
    jch[2] = std::atan2(uv[1], uv[0]);
}

void ucs_jch_to_hsb(const float jch[3], float hsb[3]) noexcept
{
    hsb[2] = jch[0] * (std::pow(std::max(jch[1], 0.0F), 1.33654221029386F) + 1.0F);
    hsb[1] = hsb[2] > 0.0F ? jch[1] / hsb[2] : 0.0F;
    hsb[0] = jch[2];
}

void ucs_hsb_to_jch(const float hsb[3], float jch[3]) noexcept
{
    jch[2] = hsb[0];
    jch[1] = hsb[1] * hsb[2];
    jch[0] = hsb[2] / (std::pow(std::max(jch[1], 0.0F), 1.33654221029386F) + 1.0F);
}

void ucs_jch_to_xyy(const float jch[3], const float l_white, float &xyx, float &xyy, float &xy_y)
{
    const float l_star = std::clamp(jch[0] * l_white, 0.0F, kUcsLStarUpper);
    const float m = l_star != 0.0F ?
                        std::pow(jch[1] * l_white /
                                     (15.932993652962535F * std::pow(l_star, 0.6523997524738018F)),
                                 0.8322850678616855F) :
                        0.0F;
    const float u_star_prime = m * std::cos(jch[2]);
    const float v_star_prime = m * std::sin(jch[2]);
    const float uv_star0 = -5.037522385190711F * u_star_prime - 2.504856328185843F * v_star_prime;
    const float uv_star1 = 4.760029407436461F * u_star_prime + 2.874012963239247F * v_star_prime;
    const float uv0 = -1.49217352929F * uv_star0 / (std::abs(uv_star0) - 1.39656225667F);
    const float uv1 = -1.52488637914F * uv_star1 / (std::abs(uv_star1) - 1.4513954287F);
    const float xyd0 = 0.167171472114775F * uv0 + 0.141299802443708F * uv1 - 0.00801531300850582F;
    const float xyd1 = -0.150959086409163F * uv0 - 0.155185060382272F * uv1 - 0.00843312433578007F;
    const float xyd2 = 0.940254742367256F * uv0 + 1.000000000000000F * uv1 - 0.0256325967652889F;
    const float div = xyd2 >= 0.0F ? std::max(xyd2, std::numeric_limits<float>::min()) :
                                     std::min(xyd2, -std::numeric_limits<float>::min());
    xyx = xyd0 / div;
    xyy = xyd1 / div;
    xy_y = ucs_l_star_to_y(l_star);
}

void ucs_hsb_to_rgb(const float hsb[3], const float l_white, float &r, float &g, float &b)
{
    float jch[3]{};
    ucs_hsb_to_jch(hsb, jch);
    float xyx = 0.0F;
    float xyy = 0.0F;
    float xy_y = 0.0F;
    ucs_jch_to_xyy(jch, l_white, xyx, xyy, xy_y);
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    xyy_to_xyz(xyx, xyy, xy_y, x, y, z);
    xyz_d65_to_rgb(x, y, z, r, g, b);
}

[[nodiscard]] float lookup_lut_periodic(const std::array<float, kUcsLutSize> &lut,
                                        const float hue) noexcept
{
    const float x_test = static_cast<float>(kUcsLutSize) * (hue + kPi) / kTwoPi;
    const float x_prev = std::floor(x_test);
    const float x_next = std::ceil(x_test);
    const int xi = static_cast<int>(x_prev) & (kUcsLutSize - 1);
    const int xii = static_cast<int>(x_next) & (kUcsLutSize - 1);
    const float y_prev = lut[static_cast<std::size_t>(xi)];
    return y_prev +
           ((xi != xii) ? (x_test - x_prev) * (lut[static_cast<std::size_t>(xii)] - y_prev) : 0.0F);
}

[[nodiscard]] float conventional_hue_deg_to_ucs_rad(const float angle) noexcept
{
    return (angle + kAngleShiftDeg) * kPi / 180.0F;
}

void periodic_rbf_interpolate(std::array<float, kColorNodes> nodes, const float smoothing,
                              std::array<float, kUcsLutSize> &lut, const float hue_shift,
                              const bool clip_positive)
{
    const int terms = std::max(1, static_cast<int>(std::ceil(3.0F * std::sqrt(smoothing))));
    std::vector<float> matrix(static_cast<std::size_t>(kColorNodes * kColorNodes), 0.0F);
    const auto node_hue = [hue_shift](const int k)
    {
        return conventional_hue_deg_to_ucs_rad(
            static_cast<float>(k) * 360.0F / static_cast<float>(kColorNodes) + hue_shift);
    };
    for (int i = 0; i < kColorNodes; ++i)
    {
        for (int j = 0; j < kColorNodes; ++j)
        {
            float value = 0.0F;
            for (int l = 0; l < terms; ++l)
            {
                value += std::exp(-static_cast<float>(l * l) / smoothing) *
                         std::cos(static_cast<float>(l) * std::abs(node_hue(i) - node_hue(j)));
            }
            matrix[static_cast<std::size_t>(i * kColorNodes + j)] = std::exp(value);
        }
    }
    std::vector<float> rhs(nodes.begin(), nodes.end());
    if (!pseudo_solve(matrix, rhs, kColorNodes, kColorNodes))
    {
        rhs.assign(nodes.begin(), nodes.end());
    }
    for (int i = 0; i < kUcsLutSize; ++i)
    {
        const float hue =
            static_cast<float>(i) * 360.0F / static_cast<float>(kUcsLutSize) * kPi / 180.0F - kPi;
        float acc = 0.0F;
        for (int k = 0; k < kColorNodes; ++k)
        {
            float result = 0.0F;
            for (int l = 0; l < terms; ++l)
            {
                result += std::exp(-static_cast<float>(l * l) / smoothing) *
                          std::cos(static_cast<float>(l) * std::abs(hue - node_hue(k)));
            }
            acc += rhs[static_cast<std::size_t>(k)] * std::exp(result);
        }
        lut[static_cast<std::size_t>(i)] = clip_positive ? std::max(0.0F, acc) : acc;
    }
}

[[nodiscard]] float compute_density(const float dens, const float length) noexcept
{
    return std::exp2(dens * clip01(0.5F + length));
}

[[nodiscard]] float calc_refavg(const std::vector<float> &input, const DecodedRaw &raw,
                                const std::uint32_t width, const std::uint32_t height,
                                const std::uint32_t row, const std::uint32_t col) noexcept
{
    const auto color = cfa_channel(raw, col, row);
    std::array<float, 3> mean{};
    std::array<float, 3> count{};
    const int dymin = std::max(0, static_cast<int>(row) - 1);
    const int dxmin = std::max(0, static_cast<int>(col) - 1);
    const int dymax = std::min(static_cast<int>(height) - 1, static_cast<int>(row) + 2);
    const int dxmax = std::min(static_cast<int>(width) - 1, static_cast<int>(col) + 2);
    for (int dy = dymin; dy < dymax; ++dy)
    {
        for (int dx = dxmin; dx < dxmax; ++dx)
        {
            const auto c =
                cfa_channel(raw, static_cast<std::uint32_t>(dx), static_cast<std::uint32_t>(dy));
            mean[c] += std::max(
                0.0F, input[static_cast<std::size_t>(dy) * width + static_cast<std::uint32_t>(dx)]);
            count[c] += 1.0F;
        }
    }
    for (int c = 0; c < 3; ++c)
    {
        const auto channel = static_cast<std::size_t>(c);
        mean[channel] = count[channel] > 0.0F ? std::cbrt(mean[channel] / count[channel]) : 0.0F;
    }
    const std::array<float, 3> opp{0.5F * (mean[1] + mean[2]), 0.5F * (mean[0] + mean[2]),
                                   0.5F * (mean[0] + mean[1])};
    return opp[color] * opp[color] * opp[color];
}

void process_highlights_clip(std::vector<float> &buffer, const DecodedRaw &raw,
                             const std::array<float, 3> &clips)
{
    for (std::uint32_t y = 0; y < raw.height; ++y)
    {
        for (std::uint32_t x = 0; x < raw.width; ++x)
        {
            const std::size_t index = static_cast<std::size_t>(y) * raw.width + x;
            const auto channel = cfa_channel(raw, x, y);
            buffer[index] = std::min(buffer[index], clips[channel]);
        }
    }
}

void process_highlights_lch(std::vector<float> &buffer, const DecodedRaw &raw, const float clip)
{
    constexpr float kSqrt3 = 1.7320508075688772F;
    constexpr float kSqrt12 = 3.4641016151377546F;
    std::vector<float> out = buffer;
    for (std::uint32_t j = 0; j < raw.height; ++j)
    {
        for (std::uint32_t i = 0; i < raw.width; ++i)
        {
            const std::size_t index = static_cast<std::size_t>(j) * raw.width + i;
            if (i == raw.width - 1U || j == raw.height - 1U)
            {
                out[index] = std::min(clip, buffer[index]);
                continue;
            }
            float r = 0.0F;
            float gmin = std::numeric_limits<float>::max();
            float gmax = -std::numeric_limits<float>::max();
            float b = 0.0F;
            bool clipped = false;
            for (int jj = 0; jj <= 1; ++jj)
            {
                for (int ii = 0; ii <= 1; ++ii)
                {
                    const float val =
                        buffer[static_cast<std::size_t>(j + static_cast<std::uint32_t>(jj)) *
                                   raw.width +
                               (i + static_cast<std::uint32_t>(ii))];
                    clipped = clipped || val > clip;
                    const auto c = cfa_channel(raw, i + static_cast<std::uint32_t>(ii),
                                               j + static_cast<std::uint32_t>(jj));
                    if (c == 0)
                    {
                        r = val;
                    }
                    else if (c == 1)
                    {
                        gmin = std::min(gmin, val);
                        gmax = std::max(gmax, val);
                    }
                    else
                    {
                        b = val;
                    }
                }
            }
            if (!clipped)
            {
                out[index] = buffer[index];
                continue;
            }
            const float ro = std::min(r, clip);
            const float go = std::min(gmin, clip);
            const float bo = std::min(b, clip);
            const float l = (r + gmax + b) / 3.0F;
            float c = kSqrt3 * (r - gmax);
            float h = 2.0F * b - gmax - r;
            const float co = kSqrt3 * (ro - go);
            const float ho = 2.0F * bo - go - ro;
            if (r != gmax && gmax != b)
            {
                const float ratio = std::sqrt((co * co + ho * ho) / (c * c + h * h));
                c *= ratio;
                h *= ratio;
            }
            std::array<float, 3> rgb{l - h / 6.0F + c / kSqrt12, l - h / 6.0F - c / kSqrt12,
                                     l + h / 3.0F};
            out[index] = rgb[cfa_channel(raw, i, j)];
        }
    }
    buffer.swap(out);
}

void interpolate_color_bayer(const std::vector<float> &in, std::vector<float> &out,
                             const DecodedRaw &raw, const int dim, const int dir, const int other,
                             const std::array<float, 3> &clip, const int pass)
{
    const int width = static_cast<int>(raw.width);
    const int height = static_cast<int>(raw.height);
    int i = dim == 0 ? 0 : other;
    int j = dim == 0 ? other : 0;
    const int offs = (dim ? width : 1) * (dir < 0 ? -1 : 1);
    int beg = 0;
    int end = 0;
    if (dim == 0 && dir == 1)
    {
        beg = 0;
        end = width;
    }
    else if (dim == 0 && dir == -1)
    {
        beg = width - 1;
        end = -1;
    }
    else if (dim == 1 && dir == 1)
    {
        beg = 0;
        end = height;
    }
    else
    {
        beg = height - 1;
        end = -1;
    }
    std::ptrdiff_t pos = dim == 1 ? i + static_cast<std::ptrdiff_t>(beg) * width :
                                    beg + static_cast<std::ptrdiff_t>(j) * width;
    float ratio = 1.0F;
    for (int k = beg; k != end; k += dir)
    {
        if (dim == 1)
        {
            j = k;
        }
        else
        {
            i = k;
        }
        const float clip0 =
            clip[cfa_channel(raw, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(j))];
        const float clip1 = clip[cfa_channel(raw, static_cast<std::uint32_t>(dim ? i : i + 1),
                                             static_cast<std::uint32_t>(dim ? j + 1 : j))];
        if (i == 0 || i == width - 1 || j == 0 || j == height - 1)
        {
            if (pass == 3)
            {
                out[static_cast<std::size_t>(pos)] = in[static_cast<std::size_t>(pos)];
            }
        }
        else
        {
            const float current = in[static_cast<std::size_t>(pos)];
            const float neighbor = in[static_cast<std::size_t>(pos + offs)];
            if (current < clip0 && current > 1.0e-5F && neighbor < clip1 && neighbor > 1.0e-5F)
            {
                if ((k & 1) != 0)
                {
                    ratio = (3.0F * ratio + current / neighbor) / 4.0F;
                }
                else
                {
                    ratio = (3.0F * ratio + neighbor / current) / 4.0F;
                }
            }
            if (current >= clip0 - 1.0e-5F)
            {
                float add = 0.0F;
                if (neighbor >= clip1 - 1.0e-5F)
                {
                    add = std::max(clip0, clip1);
                }
                else if ((k & 1) != 0)
                {
                    add = neighbor * ratio;
                }
                else
                {
                    add = neighbor / ratio;
                }
                if (pass == 0)
                {
                    out[static_cast<std::size_t>(pos)] = add;
                }
                else if (pass == 3)
                {
                    out[static_cast<std::size_t>(pos)] =
                        (out[static_cast<std::size_t>(pos)] + add) / 4.0F;
                }
                else
                {
                    out[static_cast<std::size_t>(pos)] += add;
                }
            }
            else if (pass == 3)
            {
                out[static_cast<std::size_t>(pos)] = current;
            }
        }
        pos += offs;
    }
}

void process_highlights_inpaint(std::vector<float> &buffer, const DecodedRaw &raw,
                                const std::array<float, 3> &clips)
{
    std::vector<float> out = buffer;
    for (std::uint32_t j = 0; j < raw.height; ++j)
    {
        interpolate_color_bayer(buffer, out, raw, 0, 1, static_cast<int>(j), clips, 0);
        interpolate_color_bayer(buffer, out, raw, 0, -1, static_cast<int>(j), clips, 1);
    }
    for (std::uint32_t i = 0; i < raw.width; ++i)
    {
        interpolate_color_bayer(buffer, out, raw, 1, 1, static_cast<int>(i), clips, 2);
        interpolate_color_bayer(buffer, out, raw, 1, -1, static_cast<int>(i), clips, 3);
    }
    buffer.swap(out);
}

void process_highlights_opposed(std::vector<float> &buffer, const DecodedRaw &raw,
                                const std::array<float, 3> &clips)
{
    const std::size_t width = raw.width;
    const std::size_t height = raw.height;
    const std::size_t mwidth = width / 3U;
    const std::size_t mheight = height / 3U;
    std::array<float, 3> chrominance{};
    if (mwidth > 7 && mheight > 7)
    {
        const std::size_t msize = mwidth * mheight;
        std::vector<char> mask(msize * 3U, 0);
        bool any_clipped = false;
        for (std::size_t mrow = 0; mrow < mheight; ++mrow)
        {
            for (std::size_t mcol = 0; mcol < mwidth; ++mcol)
            {
                std::array<char, 3> mbuff{};
                for (int y = 0; y < 3; ++y)
                {
                    for (int x = 0; x < 3; ++x)
                    {
                        const std::size_t row = 3U * mrow + static_cast<std::size_t>(y);
                        const std::size_t col = 3U * mcol + static_cast<std::size_t>(x);
                        if (row >= height || col >= width)
                        {
                            continue;
                        }
                        const auto color = cfa_channel(raw, static_cast<std::uint32_t>(col),
                                                       static_cast<std::uint32_t>(row));
                        if (buffer[row * width + col] >= clips[color])
                        {
                            mbuff[color] = 1;
                            any_clipped = true;
                        }
                    }
                }
                const std::size_t mx = mrow * mwidth + mcol;
                mask[mx] = mbuff[0];
                mask[msize + mx] = mbuff[1];
                mask[2U * msize + mx] = mbuff[2];
            }
        }
        if (any_clipped)
        {
            std::vector<char> dilated(msize * 3U, 0);
            for (int c = 0; c < 3; ++c)
            {
                for (std::size_t row = 0; row < mheight; ++row)
                {
                    for (std::size_t col = 0; col < mwidth; ++col)
                    {
                        const std::size_t mx = row * mwidth + col;
                        const bool safe =
                            col >= 3 && row >= 3 && col + 4 < mwidth && row + 4 < mheight;
                        if (!safe)
                        {
                            dilated[static_cast<std::size_t>(c) * msize + mx] =
                                mask[static_cast<std::size_t>(c) * msize + mx];
                            continue;
                        }
                        char value = mask[static_cast<std::size_t>(c) * msize + mx];
                        if (value == 0)
                        {
                            const auto stride = static_cast<std::ptrdiff_t>(mwidth);
                            const auto base = static_cast<std::ptrdiff_t>(
                                static_cast<std::size_t>(c) * msize + mx);
                            const auto at = [&](const std::ptrdiff_t offset) -> char
                            {
                                const auto index = base + offset;
                                if (index < 0 || index >= static_cast<std::ptrdiff_t>(mask.size()))
                                {
                                    return 0;
                                }
                                return mask[static_cast<std::size_t>(index)];
                            };
                            value = static_cast<char>(at(-stride - 1) | at(-stride) |
                                                      at(-stride + 1) | at(-1) | at(1) |
                                                      at(stride - 1) | at(stride) | at(stride + 1));
                        }
                        dilated[static_cast<std::size_t>(c) * msize + mx] = value;
                    }
                }
            }
            std::array<float, 3> sums{};
            std::array<float, 3> counts{};
            for (std::size_t row = 0; row < height; ++row)
            {
                for (std::size_t col = 0; col < width; ++col)
                {
                    const auto color = cfa_channel(raw, static_cast<std::uint32_t>(col),
                                                   static_cast<std::uint32_t>(row));
                    const float inval = buffer[row * width + col];
                    const std::size_t mx =
                        std::min(row / 3U, mheight - 1U) * mwidth + std::min(col / 3U, mwidth - 1U);
                    if (inval < clips[color] && inval > 0.2F * clips[color] &&
                        dilated[static_cast<std::size_t>(color) * msize + mx] != 0)
                    {
                        sums[color] += inval - calc_refavg(buffer, raw, raw.width, raw.height,
                                                           static_cast<std::uint32_t>(row),
                                                           static_cast<std::uint32_t>(col));
                        counts[color] += 1.0F;
                    }
                }
            }
            for (int c = 0; c < 3; ++c)
            {
                chrominance[static_cast<std::size_t>(c)] =
                    counts[static_cast<std::size_t>(c)] > 100.0F ?
                        sums[static_cast<std::size_t>(c)] / counts[static_cast<std::size_t>(c)] :
                        0.0F;
            }
        }
    }

    for (std::uint32_t row = 0; row < raw.height; ++row)
    {
        for (std::uint32_t col = 0; col < raw.width; ++col)
        {
            const std::size_t index = static_cast<std::size_t>(row) * raw.width + col;
            const auto color = cfa_channel(raw, col, row);
            const float inval = buffer[index];
            if (inval >= clips[color])
            {
                const float ref = calc_refavg(buffer, raw, raw.width, raw.height, row, col);
                buffer[index] = std::max(inval, ref + chrominance[color]);
            }
        }
    }
}

[[nodiscard]] Result<std::vector<float>> raw_to_float(const DecodedRaw &raw)
{
    const std::size_t count = static_cast<std::size_t>(raw.width) * raw.height;
    if (raw.pixels.size() < count)
    {
        return make_error(ErrorCode::kValidation,
                          "RAW buffer is undersized for highlight reconstruction");
    }
    const float black = static_cast<float>(std::max(0, raw.black_level));
    const float scale = 1.0F / std::max(1.0F, static_cast<float>(raw.white_level) - black);
    std::vector<float> buffer(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        buffer[index] = (static_cast<float>(raw.pixels[index]) - black) * scale;
    }
    return buffer;
}

void float_to_raw(DecodedRaw &raw, const std::vector<float> &buffer, const double amount)
{
    const float black = static_cast<float>(std::max(0, raw.black_level));
    const float range = std::max(1.0F, static_cast<float>(raw.white_level) - black);
    const std::size_t count = buffer.size();
    for (std::size_t index = 0; index < count; ++index)
    {
        const float restored = buffer[index] * range + black;
        const double mixed = static_cast<double>(raw.pixels[index]) * (1.0 - amount) +
                             static_cast<double>(restored) * amount;
        raw.pixels[index] = static_cast<std::uint16_t>(std::clamp(std::lround(mixed), 0L, 65535L));
    }
}

Result<void> eaw_dn_decompose(std::vector<float> &coarse, const std::vector<float> &input,
                              std::vector<float> &detail, std::array<float, 3> &sum_squared,
                              const int scale, const float radius, const float inv_sigma2,
                              const int width, const int height,
                              const CancellationToken &cancellation)
{
    static constexpr float kFilter[25] = {
        1.0F / 256.0F, 4.0F / 256.0F,  6.0F / 256.0F,  4.0F / 256.0F,  1.0F / 256.0F,
        4.0F / 256.0F, 16.0F / 256.0F, 24.0F / 256.0F, 16.0F / 256.0F, 4.0F / 256.0F,
        6.0F / 256.0F, 24.0F / 256.0F, 36.0F / 256.0F, 24.0F / 256.0F, 6.0F / 256.0F,
        4.0F / 256.0F, 16.0F / 256.0F, 24.0F / 256.0F, 16.0F / 256.0F, 4.0F / 256.0F,
        1.0F / 256.0F, 4.0F / 256.0F,  6.0F / 256.0F,  4.0F / 256.0F,  1.0F / 256.0F};
    const int maximum_multiplier = std::max(1, std::max(width, height) - 1);
    const int mult =
        std::clamp(static_cast<int>(std::lround(static_cast<float>(1 << scale) * radius)), 1,
                   maximum_multiplier);
    sum_squared = {};
    if (coarse.size() != input.size())
    {
        coarse.assign(input.size(), 0.0F);
    }
    if (detail.size() != input.size())
    {
        detail.assign(input.size(), 0.0F);
    }
    std::vector<int> x_samples(static_cast<std::size_t>(width) * 5U);
    std::vector<int> y_samples(static_cast<std::size_t>(height) * 5U);
    for (int x = 0; x < width; ++x)
    {
        for (int ii = 0; ii < 5; ++ii)
        {
            x_samples[static_cast<std::size_t>(x) * 5U + static_cast<std::size_t>(ii)] =
                std::clamp(x + mult * (ii - 2), 0, width - 1);
        }
    }
    for (int y = 0; y < height; ++y)
    {
        for (int jj = 0; jj < 5; ++jj)
        {
            y_samples[static_cast<std::size_t>(y) * 5U + static_cast<std::size_t>(jj)] =
                std::clamp(y + mult * (jj - 2), 0, height - 1);
        }
    }
    const auto rows = for_each_row(
        static_cast<std::uint32_t>(height), cancellation,
        [&](const std::uint32_t row)
        {
            const int y = static_cast<int>(row);
            const int *const y_indices = y_samples.data() + static_cast<std::size_t>(y) * 5U;
            for (int x = 0; x < width; ++x)
            {
                const float *px =
                    input.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(x)) *
                                       3U;
                float sum0 = 0.0F;
                float sum1 = 0.0F;
                float sum2 = 0.0F;
                float weight_sum = 0.0F;
                int filter_idx = 0;
                const int *const x_indices = x_samples.data() + static_cast<std::size_t>(x) * 5U;
                for (int jj = 0; jj < 5; ++jj)
                {
                    const int sy = y_indices[jj];
                    for (int ii = 0; ii < 5; ++ii)
                    {
                        const int sx = x_indices[ii];
                        const float *px2 = input.data() + (static_cast<std::size_t>(sy) *
                                                               static_cast<std::size_t>(width) +
                                                           static_cast<std::size_t>(sx)) *
                                                              3U;
                        const float diff0 = px[0] - px2[0];
                        const float diff1 = px[1] - px2[1];
                        const float diff2 = px[2] - px2[2];
                        float dot = diff0 * diff0;
                        dot += diff1 * diff1;
                        dot += diff2 * diff2;
                        dot *= inv_sigma2;
                        const float exponent = dot * 0.02F - 9.0F;
                        const float adaptive = exponent > 0.0F ? std::exp2(-exponent) : 1.0F;
                        const float weight = kFilter[filter_idx++] * adaptive;
                        weight_sum += weight;
                        sum0 += weight * px2[0];
                        sum1 += weight * px2[1];
                        sum2 += weight * px2[2];
                    }
                }
                const std::size_t index =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) *
                    3U;
                const float denominator = std::max(weight_sum, 1.0e-12F);
                const float mean0 = sum0 / denominator;
                const float mean1 = sum1 / denominator;
                const float mean2 = sum2 / denominator;
                coarse[index] = mean0;
                coarse[index + 1U] = mean1;
                coarse[index + 2U] = mean2;
                detail[index] = px[0] - mean0;
                detail[index + 1U] = px[1] - mean1;
                detail[index + 2U] = px[2] - mean2;
            }
        });
    if (!rows)
    {
        return rows.error();
    }

    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    float sum0 = 0.0F;
    float sum1 = 0.0F;
    float sum2 = 0.0F;
    const float *detail_pixel = detail.data();
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        sum0 += detail_pixel[0] * detail_pixel[0];
        sum1 += detail_pixel[1] * detail_pixel[1];
        sum2 += detail_pixel[2] * detail_pixel[2];
        detail_pixel += 3;
    }
    sum_squared = {sum0, sum1, sum2};
    return {};
}

Result<void> eaw_synthesize(std::vector<float> &accum, const std::vector<float> &detail,
                            const std::array<float, 3> &threshold, const int width,
                            const int height, const CancellationToken &cancellation)
{
    const float threshold0 = threshold[0];
    const float threshold1 = threshold[1];
    const float threshold2 = threshold[2];
    return for_each_row(static_cast<std::uint32_t>(height), cancellation,
                        [&](const std::uint32_t row)
                        {
                            const std::size_t begin = static_cast<std::size_t>(row) *
                                                      static_cast<std::size_t>(width) * 3U;
                            float *accum_pixel = accum.data() + begin;
                            const float *detail_pixel = detail.data() + begin;
                            for (int x = 0; x < width; ++x)
                            {
                                const float positive0 = detail_pixel[0] - threshold0;
                                const float negative0 = detail_pixel[0] + threshold0;
                                accum_pixel[0] += (positive0 < 0.0F ? 0.0F : positive0) +
                                                  (0.0F < negative0 ? 0.0F : negative0);
                                const float positive1 = detail_pixel[1] - threshold1;
                                const float negative1 = detail_pixel[1] + threshold1;
                                accum_pixel[1] += (positive1 < 0.0F ? 0.0F : positive1) +
                                                  (0.0F < negative1 ? 0.0F : negative1);
                                const float positive2 = detail_pixel[2] - threshold2;
                                const float negative2 = detail_pixel[2] + threshold2;
                                accum_pixel[2] += (positive2 < 0.0F ? 0.0F : positive2) +
                                                  (0.0F < negative2 ? 0.0F : negative2);
                                accum_pixel += 3;
                                detail_pixel += 3;
                            }
                        });
}

Result<std::array<float, 3>> estimate_wavelet_noise_sigma(const std::vector<float> &detail,
                                                          const std::size_t pixel_count,
                                                          const CancellationToken &cancellation)
{
    if (pixel_count == 0U || pixel_count > std::numeric_limits<std::size_t>::max() / 3U ||
        detail.size() != pixel_count * 3U)
    {
        return make_error(ErrorCode::kValidation, "Denoise detail buffer is invalid",
                          {{"reason", "invalid_denoise_detail_buffer"}});
    }
    const std::size_t sample_count = std::min(pixel_count, kDenoiseNoiseSampleLimit);
    std::vector<float> samples;
    samples.reserve(sample_count);
    std::array<float, 3> sigma{};
    // Full small images are exact; large images use a fixed LCG sample so the MAD remains bounded,
    // spatially distributed, deterministic, and independent of row/pixel periodicity.
    for (std::size_t channel = 0U; channel < sigma.size(); ++channel)
    {
        samples.clear();
        std::uint64_t sample_state = 0x9e3779b97f4a7c15ULL + channel;
        for (std::size_t sample = 0U; sample < sample_count; ++sample)
        {
            if ((sample & 8191U) == 0U)
            {
                auto active = cancellation.check();
                if (!active)
                {
                    return active.error();
                }
            }
            sample_state = sample_state * 6364136223846793005ULL + 1442695040888963407ULL;
            const std::size_t pixel = pixel_count <= kDenoiseNoiseSampleLimit ?
                                          sample :
                                          static_cast<std::size_t>(sample_state % pixel_count);
            const float value = std::abs(detail[pixel * 3U + channel]);
            if (!std::isfinite(value))
            {
                return make_error(ErrorCode::kValidation, "Denoise wavelet detail must be finite",
                                  {{"reason", "non_finite_denoise_detail"},
                                   {"pixel_index", std::to_string(pixel)},
                                   {"channel_index", std::to_string(channel)}});
            }
            samples.push_back(value);
        }
        if (samples.empty())
        {
            return make_error(ErrorCode::kValidation, "Denoise noise sample is empty",
                              {{"reason", "empty_denoise_noise_sample"}});
        }
        const auto middle = samples.begin() + static_cast<std::ptrdiff_t>(samples.size() / 2U);
        std::nth_element(samples.begin(), middle, samples.end());
        sigma[channel] = std::clamp(*middle / kDenoiseGaussianMad, 0.25F, 4.0F);
    }
    return sigma;
}

bool invert_matrix3(const float in[3][3], float out[3][3]) noexcept
{
    const float biga = in[1][1] * in[2][2] - in[1][2] * in[2][1];
    const float bigb = -in[1][0] * in[2][2] + in[1][2] * in[2][0];
    const float bigc = in[1][0] * in[2][1] - in[1][1] * in[2][0];
    const float det = in[0][0] * biga + in[0][1] * bigb + in[0][2] * bigc;
    if (det == 0.0F)
    {
        return false;
    }
    out[0][0] = biga / det;
    out[0][1] = (-in[0][1] * in[2][2] + in[0][2] * in[2][1]) / det;
    out[0][2] = (in[0][1] * in[1][2] - in[0][2] * in[1][1]) / det;
    out[1][0] = bigb / det;
    out[1][1] = (in[0][0] * in[2][2] - in[0][2] * in[2][0]) / det;
    out[1][2] = (-in[0][0] * in[1][2] + in[0][2] * in[1][0]) / det;
    out[2][0] = bigc / det;
    out[2][1] = (-in[0][0] * in[2][1] + in[0][1] * in[2][0]) / det;
    out[2][2] = (in[0][0] * in[1][1] - in[0][1] * in[1][0]) / det;
    return true;
}

void apply_matrix(const float matrix[3][3], const float in[3], float out[3]) noexcept
{
    out[0] = matrix[0][0] * in[0] + matrix[0][1] * in[1] + matrix[0][2] * in[2];
    out[1] = matrix[1][0] * in[0] + matrix[1][1] * in[1] + matrix[1][2] * in[2];
    out[2] = matrix[2][0] * in[0] + matrix[2][1] * in[1] + matrix[2][2] * in[2];
}

[[nodiscard]] float gaussian_denom(const float sigma) noexcept
{
    return 2.0F * sigma * sigma;
}

[[nodiscard]] float gaussian_func(const float radius, const float denominator) noexcept
{
    return std::exp(-radius * radius / denominator);
}

} // namespace

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
                                  const CancellationToken &cancellation)
{
    if (raw.cfa_width != 2 || raw.cfa_height != 2 || raw.cfa_channels.size() != 4U)
    {
        return make_error(ErrorCode::kUnsupported,
                          "RAW highlight reconstruction supports Bayer 2x2 CFA only",
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
    auto converted = raw_to_float(raw);
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
    const std::array<float, 3> clips{clipper, clipper, clipper};
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
    float_to_raw(raw, buffer, amount);
    return {};
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
        invert_matrix3(to_yuv, to_rgb);
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
