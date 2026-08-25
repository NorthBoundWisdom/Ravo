#include "image_ops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <numbers>
#include <string>
#include <vector>

#include <png.h>

namespace ravo
{
namespace
{

[[nodiscard]] bool absorbed_operation(const std::string_view id) noexcept
{
    return id == "ravo.core.identity" || id == "ravo.raw.prepare" || id == "ravo.raw.demosaic" ||
           id == "ravo.color.input" || id == "ravo.color.output" || id == "ravo.output.scale";
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

[[nodiscard]] float srgb_encode(const float value)
{
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped <= 0.0031308F ? 12.92F * clamped :
                                   1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float srgb_decode(const float value)
{
    const float clamped = std::clamp(value, 0.0F, 1.0F);
    return clamped <= 0.04045F ? clamped / 12.92F : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float luma(const float r, const float g, const float b)
{
    return 0.2126F * r + 0.7152F * g + 0.0722F * b;
}

void kelvin_rgb(const double temperature, float &red, float &green, float &blue)
{
    const double kelvin = std::clamp(temperature, 1000.0, 40000.0) / 100.0;
    if (kelvin <= 66.0)
    {
        red = 1.0F;
        green = static_cast<float>(
            std::clamp((99.4708025861 * std::log(kelvin) - 161.1195681661) / 255.0, 0.0, 1.0));
    }
    else
    {
        red = static_cast<float>(
            std::clamp(329.698727446 * std::pow(kelvin - 60.0, -0.1332047592) / 255.0, 0.0, 1.0));
        green = static_cast<float>(
            std::clamp(288.1221695283 * std::pow(kelvin - 60.0, -0.0755148492) / 255.0, 0.0, 1.0));
    }
    if (kelvin >= 66.0)
    {
        blue = 1.0F;
    }
    else if (kelvin <= 19.0)
    {
        blue = 0.0F;
    }
    else
    {
        blue = static_cast<float>(std::clamp(
            (138.5177312231 * std::log(kelvin - 10.0) - 305.0447926307) / 255.0, 0.0, 1.0));
    }
}

void apply_white_balance(WorkingImage &image, const double temperature, const double tint)
{
    float sample_r = 1.0F;
    float sample_g = 1.0F;
    float sample_b = 1.0F;
    float ref_r = 1.0F;
    float ref_g = 1.0F;
    float ref_b = 1.0F;
    kelvin_rgb(temperature, sample_r, sample_g, sample_b);
    kelvin_rgb(6500.0, ref_r, ref_g, ref_b);
    const float tint_scale = static_cast<float>(std::exp2(tint / 150.0));
    const float mul_r =
        (sample_r / std::max(sample_g, 1.0e-6F)) / (ref_r / std::max(ref_g, 1.0e-6F));
    const float mul_g = tint_scale;
    const float mul_b =
        (sample_b / std::max(sample_g, 1.0e-6F)) / (ref_b / std::max(ref_g, 1.0e-6F));
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        image.rgb[index] *= mul_r;
        image.rgb[index + 1U] *= mul_g;
        image.rgb[index + 2U] *= mul_b;
    }
}

void apply_exposure(WorkingImage &image, const double ev)
{
    const float scale = static_cast<float>(std::exp2(ev));
    for (float &sample : image.rgb)
    {
        sample *= scale;
    }
}

void apply_contrast(WorkingImage &image, const double amount)
{
    const float pivot = 0.18F;
    const float gain = 1.0F + static_cast<float>(amount);
    for (float &sample : image.rgb)
    {
        sample = pivot + (sample - pivot) * gain;
    }
}

void linear_to_xyz(const float r, const float g, const float b, float &x, float &y, float &z)
{
    x = 0.4124564F * r + 0.3575761F * g + 0.1804375F * b;
    y = 0.2126729F * r + 0.7151522F * g + 0.0721750F * b;
    z = 0.0193339F * r + 0.1191920F * g + 0.9503041F * b;
}

void xyz_to_linear(const float x, const float y, const float z, float &r, float &g, float &b)
{
    r = 3.2404542F * x - 1.5371385F * y - 0.4985314F * z;
    g = -0.9692660F * x + 1.8760108F * y + 0.0415560F * z;
    b = 0.0556434F * x - 0.2040259F * y + 1.0572252F * z;
}

[[nodiscard]] float lab_f(const float t)
{
    return t > 0.008856F ? std::cbrt(t) : (7.787F * t + 16.0F / 116.0F);
}

[[nodiscard]] float lab_f_inv(const float t)
{
    const float t3 = t * t * t;
    return t3 > 0.008856F ? t3 : (t - 16.0F / 116.0F) / 7.787F;
}

void rgb_to_lab(const float r, const float g, const float b, float &L, float &a, float &b_ch)
{
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    linear_to_xyz(r, g, b, x, y, z);
    const float fx = lab_f(x / 0.95047F);
    const float fy = lab_f(y);
    const float fz = lab_f(z / 1.08883F);
    L = 116.0F * fy - 16.0F;
    a = 500.0F * (fx - fy);
    b_ch = 200.0F * (fy - fz);
}

void lab_to_rgb(const float L, const float a, const float b_ch, float &r, float &g, float &b)
{
    const float fy = (L + 16.0F) / 116.0F;
    const float fx = fy + a / 500.0F;
    const float fz = fy - b_ch / 200.0F;
    const float x = 0.95047F * lab_f_inv(fx);
    const float y = lab_f_inv(fy);
    const float z = 1.08883F * lab_f_inv(fz);
    xyz_to_linear(x, y, z, r, g, b);
}

void rgb_to_hsl(float r, float g, float b, float &h, float &s, float &l);
void hsl_to_rgb(float h, float s, float l, float &r, float &g, float &b);

void apply_highlights_shadows(WorkingImage &image, const double highlights, const double shadows)
{
    if (highlights == 0.0 && shadows == 0.0)
    {
        return;
    }
    const float highlight_ev = static_cast<float>(highlights);
    const float shadow_ev = static_cast<float>(shadows);
    const float compress = 0.5F;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float L = 0.0F;
        float a = 0.0F;
        float b = 0.0F;
        rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
        const float ln = std::clamp(L / 100.0F, 0.0F, 1.0F);
        const float hmask = std::clamp((ln - (0.5F + compress * 0.25F)) / (0.5F - compress * 0.25F),
                                       0.0F, 1.0F);
        const float smask = 1.0F - std::clamp(ln / std::max(0.15F, 0.5F - compress * 0.25F), 0.0F, 1.0F);
        L *= std::exp2(highlight_ev * hmask) * std::exp2(shadow_ev * smask);
        lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
}

void apply_whites_blacks(WorkingImage &image, const double whites, const double blacks)
{
    const float white = std::max(0.05F, 1.0F + static_cast<float>(whites) * 0.5F);
    const float black = static_cast<float>(blacks) * 0.25F;
    const float denom = std::max(1.0e-5F, white - black);
    for (float &sample : image.rgb)
    {
        sample = (sample - black) / denom;
    }
}

void apply_vibrance_saturation(WorkingImage &image, const double vibrance, const double saturation)
{
    if (vibrance == 0.0 && saturation == 0.0)
    {
        return;
    }
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float y = luma(r, g, b);
        const float maxc = std::max(r, std::max(g, b));
        const float minc = std::min(r, std::min(g, b));
        const float sat = maxc <= 1.0e-6F ? 0.0F : 1.0F - minc / maxc;
        const float vibrance_gain = 1.0F + static_cast<float>(vibrance) * (1.0F - sat);
        const float sat_gain = 1.0F + static_cast<float>(saturation);
        const float gain = vibrance_gain * sat_gain;
        r = y + (r - y) * gain;
        g = y + (g - y) * gain;
        b = y + (b - y) * gain;
    }
}

[[nodiscard]] Result<WorkingImage> rotate_working(WorkingImage image, const int quarters)
{
    const int turns = ((quarters % 4) + 4) % 4;
    if (turns == 0)
    {
        return image;
    }
    WorkingImage output;
    if (turns == 2)
    {
        output.width = image.width;
        output.height = image.height;
        output.rgb.resize(image.rgb.size());
        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
            {
                const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3U;
                const std::size_t dst =
                    (static_cast<std::size_t>(image.height - 1U - y) * image.width +
                     (image.width - 1U - x)) *
                    3U;
                output.rgb[dst] = image.rgb[src];
                output.rgb[dst + 1U] = image.rgb[src + 1U];
                output.rgb[dst + 2U] = image.rgb[src + 2U];
            }
        }
        return output;
    }

    output.width = image.height;
    output.height = image.width;
    output.rgb.resize(static_cast<std::size_t>(output.width) * output.height * 3U);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            const std::uint32_t dx = turns == 1 ? image.height - 1U - y : y;
            const std::uint32_t dy = turns == 1 ? x : image.width - 1U - x;
            const std::size_t dst = (static_cast<std::size_t>(dy) * output.width + dx) * 3U;
            output.rgb[dst] = image.rgb[src];
            output.rgb[dst + 1U] = image.rgb[src + 1U];
            output.rgb[dst + 2U] = image.rgb[src + 2U];
        }
    }
    return output;
}

[[nodiscard]] Result<WorkingImage> crop_working(WorkingImage image, const double x, const double y,
                                                const double width, const double height)
{
    if (image.width == 0 || image.height == 0)
    {
        return make_error(ErrorCode::kValidation, "Cannot crop an empty image");
    }
    const auto left =
        static_cast<std::uint32_t>(std::clamp(std::llround(x * static_cast<double>(image.width)),
                                              0LL, static_cast<long long>(image.width - 1U)));
    const auto top =
        static_cast<std::uint32_t>(std::clamp(std::llround(y * static_cast<double>(image.height)),
                                              0LL, static_cast<long long>(image.height - 1U)));
    auto crop_w = static_cast<std::uint32_t>(
        std::clamp(std::llround(width * static_cast<double>(image.width)), 1LL,
                   static_cast<long long>(image.width)));
    auto crop_h = static_cast<std::uint32_t>(
        std::clamp(std::llround(height * static_cast<double>(image.height)), 1LL,
                   static_cast<long long>(image.height)));
    if (left + crop_w > image.width)
    {
        crop_w = image.width - left;
    }
    if (top + crop_h > image.height)
    {
        crop_h = image.height - top;
    }
    if (crop_w == 0 || crop_h == 0)
    {
        return make_error(ErrorCode::kValidation, "Crop rectangle is empty");
    }
    WorkingImage output;
    output.width = crop_w;
    output.height = crop_h;
    output.rgb.resize(static_cast<std::size_t>(crop_w) * crop_h * 3U);
    for (std::uint32_t row = 0; row < crop_h; ++row)
    {
        const float *src =
            image.rgb.data() + (static_cast<std::size_t>(top + row) * image.width + left) * 3U;
        float *dst = output.rgb.data() + static_cast<std::size_t>(row) * crop_w * 3U;
        std::copy_n(src, static_cast<std::size_t>(crop_w) * 3U, dst);
    }
    return output;
}

[[nodiscard]] Result<WorkingImage> flip_working(WorkingImage image, const bool horizontal,
                                                const bool vertical)
{
    if (!horizontal && !vertical)
    {
        return image;
    }
    WorkingImage output;
    output.width = image.width;
    output.height = image.height;
    output.rgb.resize(image.rgb.size());
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::uint32_t dx = horizontal ? image.width - 1U - x : x;
            const std::uint32_t dy = vertical ? image.height - 1U - y : y;
            const std::size_t src = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            const std::size_t dst = (static_cast<std::size_t>(dy) * output.width + dx) * 3U;
            output.rgb[dst] = image.rgb[src];
            output.rgb[dst + 1U] = image.rgb[src + 1U];
            output.rgb[dst + 2U] = image.rgb[src + 2U];
        }
    }
    return output;
}

void sample_bilinear(const WorkingImage &image, double sx, double sy, float *rgb)
{
    if (image.width == 0 || image.height == 0)
    {
        rgb[0] = 0.0F;
        rgb[1] = 0.0F;
        rgb[2] = 0.0F;
        return;
    }
    sx = std::clamp(sx, 0.0, static_cast<double>(image.width - 1U));
    sy = std::clamp(sy, 0.0, static_cast<double>(image.height - 1U));
    const auto x0 = static_cast<std::uint32_t>(sx);
    const auto y0 = static_cast<std::uint32_t>(sy);
    const auto x1 = std::min(x0 + 1U, image.width - 1U);
    const auto y1 = std::min(y0 + 1U, image.height - 1U);
    const float tx = static_cast<float>(sx - static_cast<double>(x0));
    const float ty = static_cast<float>(sy - static_cast<double>(y0));
    const std::size_t i00 = (static_cast<std::size_t>(y0) * image.width + x0) * 3U;
    const std::size_t i10 = (static_cast<std::size_t>(y0) * image.width + x1) * 3U;
    const std::size_t i01 = (static_cast<std::size_t>(y1) * image.width + x0) * 3U;
    const std::size_t i11 = (static_cast<std::size_t>(y1) * image.width + x1) * 3U;
    for (std::size_t channel = 0; channel < 3; ++channel)
    {
        const float top = image.rgb[i00 + channel] * (1.0F - tx) + image.rgb[i10 + channel] * tx;
        const float bottom = image.rgb[i01 + channel] * (1.0F - tx) + image.rgb[i11 + channel] * tx;
        rgb[channel] = top * (1.0F - ty) + bottom * ty;
    }
}

[[nodiscard]] float coverage_smoothstep(const double inset, const double width)
{
    const double t = std::clamp((inset + width) / std::max(2.0 * width, 1.0e-6), 0.0, 1.0);
    return static_cast<float>(t * t * (3.0 - 2.0 * t));
}

[[nodiscard]] Result<WorkingImage> straighten_working(WorkingImage image, const double degrees)
{
    if (image.width == 0 || image.height == 0 || std::abs(degrees) < 1e-4)
    {
        return image;
    }
    const double rad = degrees * std::numbers::pi / 180.0;
    const double inv_c = std::cos(-rad);
    const double inv_s = std::sin(-rad);
    const double width = static_cast<double>(image.width);
    const double height = static_cast<double>(image.height);
    const double cx = (width - 1.0) * 0.5;
    const double cy = (height - 1.0) * 0.5;
    WorkingImage output;
    output.width = image.width;
    output.height = image.height;
    output.rgb.assign(image.rgb.size(), 0.0F);
    constexpr double kEdgeAa = 1.25;
    for (std::uint32_t y = 0; y < output.height; ++y)
    {
        for (std::uint32_t x = 0; x < output.width; ++x)
        {
            const double dx = static_cast<double>(x) - cx;
            const double dy = static_cast<double>(y) - cy;
            const double sx = inv_c * dx - inv_s * dy + cx;
            const double sy = inv_s * dx + inv_c * dy + cy;
            const double inset = std::min(std::min(sx + 0.5, width - 0.5 - sx),
                                          std::min(sy + 0.5, height - 0.5 - sy));
            const float cover = coverage_smoothstep(inset, kEdgeAa);
            float *dst =
                output.rgb.data() + (static_cast<std::size_t>(y) * output.width + x) * 3U;
            if (cover <= 0.0F)
            {
                continue;
            }
            sample_bilinear(image, sx, sy, dst);
            if (cover < 1.0F)
            {
                dst[0] *= cover;
                dst[1] *= cover;
                dst[2] *= cover;
            }
        }
    }
    return output;
}

void box_blur(WorkingImage &image, const int radius, const int passes)
{
    if (radius <= 0 || image.width == 0 || image.height == 0)
    {
        return;
    }
    const int rad = std::min(radius, 8);
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    std::vector<float> source = image.rgb;
    std::vector<float> destination(image.rgb.size());
    const auto sample = [&](const std::vector<float> &buffer, const int x, const int y,
                            const int channel) -> float
    {
        const int sx = std::clamp(x, 0, width - 1);
        const int sy = std::clamp(y, 0, height - 1);
        return buffer[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(width) +
                       static_cast<std::size_t>(sx)) *
                          3U +
                      static_cast<std::size_t>(channel)];
    };
    for (int pass = 0; pass < passes; ++pass)
    {
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    float sum = 0.0F;
                    int count = 0;
                    for (int offset = -rad; offset <= rad; ++offset)
                    {
                        sum += sample(source, x + offset, y, channel);
                        ++count;
                    }
                    destination[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(x)) *
                                    3U +
                                static_cast<std::size_t>(channel)] = sum / static_cast<float>(count);
                }
            }
        }
        source.swap(destination);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    float sum = 0.0F;
                    int count = 0;
                    for (int offset = -rad; offset <= rad; ++offset)
                    {
                        sum += sample(source, x, y + offset, channel);
                        ++count;
                    }
                    destination[(static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                                 static_cast<std::size_t>(x)) *
                                    3U +
                                static_cast<std::size_t>(channel)] = sum / static_cast<float>(count);
                }
            }
        }
        source.swap(destination);
    }
    image.rgb = std::move(source);
}

void blur_luma_gaussian(std::vector<float> &luma, const std::uint32_t width,
                        const std::uint32_t height, const int radius)
{
    if (radius <= 0 || width == 0 || height == 0)
    {
        return;
    }
    const int rad = std::min(radius, 12);
    const float sigma2 = (1.0F / (2.5F * 2.5F)) * static_cast<float>(rad * rad);
    std::vector<float> kernel(static_cast<std::size_t>(2 * rad + 1));
    float weight = 0.0F;
    for (int offset = -rad; offset <= rad; ++offset)
    {
        kernel[static_cast<std::size_t>(offset + rad)] =
            std::exp(-static_cast<float>(offset * offset) / (2.0F * sigma2));
        weight += kernel[static_cast<std::size_t>(offset + rad)];
    }
    for (float &sample : kernel)
    {
        sample /= weight;
    }
    std::vector<float> temp(luma.size());
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0F;
            for (int offset = -rad; offset <= rad; ++offset)
            {
                const int xx = std::clamp(x + offset, 0, w - 1);
                sum += luma[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(xx)] *
                       kernel[static_cast<std::size_t>(offset + rad)];
            }
            temp[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(x)] = sum;
        }
    }
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0F;
            for (int offset = -rad; offset <= rad; ++offset)
            {
                const int yy = std::clamp(y + offset, 0, h - 1);
                sum += temp[static_cast<std::size_t>(yy) * width + static_cast<std::uint32_t>(x)] *
                       kernel[static_cast<std::size_t>(offset + rad)];
            }
            luma[static_cast<std::size_t>(y) * width + static_cast<std::uint32_t>(x)] = sum;
        }
    }
}

void apply_unsharp(WorkingImage &image, const double amount, const double radius,
                   const double threshold)
{
    if (amount == 0.0 || radius <= 0.0)
    {
        return;
    }
    const int rad = static_cast<int>(std::lround(2.5 * std::clamp(radius, 0.0, 12.0)));
    if (rad <= 0 || image.width < static_cast<std::uint32_t>(2 * rad + 1) ||
        image.height < static_cast<std::uint32_t>(2 * rad + 1))
    {
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> L(pixels);
    std::vector<float> a(pixels);
    std::vector<float> b(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        rgb_to_lab(image.rgb[index * 3U], image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U],
                   L[index], a[index], b[index]);
    }
    std::vector<float> blurred = L;
    blur_luma_gaussian(blurred, image.width, image.height, rad);
    const float gain = static_cast<float>(amount);
    const float limit = static_cast<float>(threshold) * 10.0F;
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float diff = L[index] - blurred[index];
        const float absdiff = std::abs(diff);
        const float detail = absdiff > limit ? std::copysign(std::max(absdiff - limit, 0.0F), diff) : 0.0F;
        lab_to_rgb(L[index] + detail * gain, a[index], b[index], image.rgb[index * 3U],
                   image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U]);
    }
}

void apply_clarity(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> L(pixels);
    std::vector<float> a(pixels);
    std::vector<float> b(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        rgb_to_lab(image.rgb[index * 3U], image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U],
                   L[index], a[index], b[index]);
    }
    std::vector<float> inverted(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        inverted[index] = 100.0F - std::clamp(L[index], 0.0F, 100.0F);
    }
    WorkingImage luma_image;
    luma_image.width = image.width;
    luma_image.height = image.height;
    luma_image.rgb.resize(pixels * 3U);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        luma_image.rgb[index * 3U] = inverted[index] / 100.0F;
        luma_image.rgb[index * 3U + 1U] = inverted[index] / 100.0F;
        luma_image.rgb[index * 3U + 2U] = inverted[index] / 100.0F;
    }
    box_blur(luma_image, 8, 2);
    const float contrast_scale = static_cast<float>(amount) * 3.75F;
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float mixed = (luma_image.rgb[index * 3U] * 100.0F + L[index]) - 100.0F;
        const float hipass = std::clamp(mixed * contrast_scale + 50.0F, 0.0F, 100.0F);
        const float out_L = L[index] + (hipass - 50.0F);
        lab_to_rgb(out_L, a[index], b[index], image.rgb[index * 3U], image.rgb[index * 3U + 1U],
                   image.rgb[index * 3U + 2U]);
    }
}

void apply_soften(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    WorkingImage orton = image;
    const float brightness = 1.0F / std::exp2(-0.33F);
    const float saturation = 0.5F;
    for (std::size_t index = 0; index + 2 < orton.rgb.size(); index += 3)
    {
        float h = 0.0F;
        float s = 0.0F;
        float l = 0.0F;
        rgb_to_hsl(orton.rgb[index], orton.rgb[index + 1U], orton.rgb[index + 2U], h, s, l);
        s *= saturation;
        l *= brightness;
        hsl_to_rgb(h, std::clamp(s, 0.0F, 1.0F), std::clamp(l, 0.0F, 1.0F), orton.rgb[index],
                   orton.rgb[index + 1U], orton.rgb[index + 2U]);
    }
    const float hypot = std::hypot(static_cast<float>(image.width), static_cast<float>(image.height));
    const int radius = std::max(1, static_cast<int>(std::lround(hypot * 0.01F * std::clamp(amount, 0.0, 1.0))));
    box_blur(orton, radius, 2);
    const float mix = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    for (std::size_t index = 0; index < image.rgb.size(); ++index)
    {
        image.rgb[index] = image.rgb[index] * (1.0F - mix) + orton.rgb[index] * mix;
    }
}

void apply_bloom(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const std::size_t pixels = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> L(pixels);
    std::vector<float> a(pixels);
    std::vector<float> b(pixels);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        rgb_to_lab(image.rgb[index * 3U], image.rgb[index * 3U + 1U], image.rgb[index * 3U + 2U],
                   L[index], a[index], b[index]);
    }
    const float threshold = 50.0F;
    const float scale = 1.0F / std::exp2(-static_cast<float>(amount));
    WorkingImage lights;
    lights.width = image.width;
    lights.height = image.height;
    lights.rgb.resize(pixels * 3U, 0.0F);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float sample = L[index] * scale;
        const float keep = sample > threshold ? sample : 0.0F;
        lights.rgb[index * 3U] = keep / 100.0F;
        lights.rgb[index * 3U + 1U] = keep / 100.0F;
        lights.rgb[index * 3U + 2U] = keep / 100.0F;
    }
    box_blur(lights, std::max(1, static_cast<int>(std::lround(8.0 * amount))), 2);
    for (std::size_t index = 0; index < pixels; ++index)
    {
        const float glow = lights.rgb[index * 3U] * 100.0F;
        const float screened = 100.0F - ((100.0F - L[index]) * (100.0F - glow) / 100.0F);
        lab_to_rgb(screened, a[index], b[index], image.rgb[index * 3U], image.rgb[index * 3U + 1U],
                   image.rgb[index * 3U + 2U]);
    }
}

void apply_vignette(WorkingImage &image, const double amount, const double midpoint,
                    const double falloff)
{
    if (amount <= 0.0 || image.width == 0 || image.height == 0)
    {
        return;
    }
    const float brightness = -static_cast<float>(amount);
    const float saturation = -static_cast<float>(amount) * 0.5F;
    const float dscale = static_cast<float>(std::clamp(midpoint, 0.0, 1.0));
    const float fscale = std::max(0.05F, static_cast<float>(falloff));
    const float shape = 1.0F;
    const float exp1 = 2.0F / shape;
    const float exp2 = shape / 2.0F;
    const float xscale = 2.0F / static_cast<float>(image.width);
    const float yscale = 2.0F / static_cast<float>(image.height);
    const float cx = 1.0F;
    const float cy = 1.0F;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float pv_x = std::abs(static_cast<float>(x) * xscale - cx);
            const float pv_y = std::abs(static_cast<float>(y) * yscale - cy);
            const float cplen = std::pow(std::pow(pv_x, exp1) + std::pow(pv_y, exp1), exp2);
            float weight = 0.0F;
            if (cplen >= dscale)
            {
                weight = std::clamp((cplen - dscale) / fscale, 0.0F, 1.0F);
            }
            if (weight <= 0.0F)
            {
                continue;
            }
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            float r = image.rgb[index];
            float g = image.rgb[index + 1U];
            float b = image.rgb[index + 2U];
            if (brightness < 0.0F)
            {
                const float fall = 1.0F + weight * brightness;
                r *= fall;
                g *= fall;
                b *= fall;
            }
            else
            {
                r += weight * brightness;
                g += weight * brightness;
                b += weight * brightness;
            }
            const float mean = (r + g + b) / 3.0F;
            const float wss = weight * saturation;
            r -= (mean - r) * wss;
            g -= (mean - g) * wss;
            b -= (mean - b) * wss;
            image.rgb[index] = r;
            image.rgb[index + 1U] = g;
            image.rgb[index + 2U] = b;
        }
    }
}

constexpr std::array<int, 256> kSimplexPermutation = {
    151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103,
    30,  69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,
    197, 62,  94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174,
    20,  125, 136, 171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231,
    83,  111, 229, 122, 60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143,
    54,  65,  25,  63,  161, 1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196,
    135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124,
    123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,
    182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101,
    155, 167, 43,  172, 9,   129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178, 185,
    112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,
    51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176,
    115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243,
    141, 128, 195, 78,  66,  215, 61,  156, 180};

constexpr std::array<std::array<double, 3>, 12> kSimplexGrad = {
    {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
     {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}}};

std::array<std::size_t, 512> g_simplex_perm{};
std::array<std::size_t, 512> g_simplex_perm_mod{};
std::once_flag g_simplex_once;

void init_simplex()
{
    std::call_once(g_simplex_once, []
                   {
                       for (int index = 0; index < 512; ++index)
                       {
                           g_simplex_perm[static_cast<std::size_t>(index)] =
                               static_cast<std::size_t>(kSimplexPermutation[index & 255]);
                           g_simplex_perm_mod[static_cast<std::size_t>(index)] =
                               g_simplex_perm[static_cast<std::size_t>(index)] % 12U;
                       }
                   });
}

[[nodiscard]] double simplex_noise(double xin, double yin, double zin)
{
    init_simplex();
    constexpr double f3 = 1.0 / 3.0;
    constexpr double g3 = 1.0 / 6.0;
    const double s = (xin + yin + zin) * f3;
    const int i = static_cast<int>(std::floor(xin + s));
    const int j = static_cast<int>(std::floor(yin + s));
    const int k = static_cast<int>(std::floor(zin + s));
    const double t = (i + j + k) * g3;
    const double x0 = xin - (i - t);
    const double y0 = yin - (j - t);
    const double z0 = zin - (k - t);
    int i1 = 0;
    int j1 = 0;
    int k1 = 0;
    int i2 = 0;
    int j2 = 0;
    int k2 = 0;
    if (x0 >= y0)
    {
        if (y0 >= z0)
        {
            i1 = 1;
            i2 = 1;
            j2 = 1;
        }
        else if (x0 >= z0)
        {
            i1 = 1;
            i2 = 1;
            k2 = 1;
        }
        else
        {
            k1 = 1;
            i2 = 1;
            k2 = 1;
        }
    }
    else if (y0 < z0)
    {
        k1 = 1;
        j2 = 1;
        k2 = 1;
    }
    else if (x0 < z0)
    {
        j1 = 1;
        j2 = 1;
        k2 = 1;
    }
    else
    {
        j1 = 1;
        i2 = 1;
        j2 = 1;
    }
    const double x1 = x0 - i1 + g3;
    const double y1 = y0 - j1 + g3;
    const double z1 = z0 - k1 + g3;
    const double x2 = x0 - i2 + 2.0 * g3;
    const double y2 = y0 - j2 + 2.0 * g3;
    const double z2 = z0 - k2 + 2.0 * g3;
    const double x3 = x0 - 1.0 + 3.0 * g3;
    const double y3 = y0 - 1.0 + 3.0 * g3;
    const double z3 = z0 - 1.0 + 3.0 * g3;
    const std::size_t ii = static_cast<std::size_t>(i) & 255U;
    const std::size_t jj = static_cast<std::size_t>(j) & 255U;
    const std::size_t kk = static_cast<std::size_t>(k) & 255U;
    const auto contrib = [](const std::array<double, 3> &grad, const double x, const double y,
                            const double z)
    {
        double t0 = 0.6 - x * x - y * y - z * z;
        if (t0 < 0.0)
        {
            return 0.0;
        }
        t0 *= t0;
        return t0 * t0 * (grad[0] * x + grad[1] * y + grad[2] * z);
    };
    const auto gi0 = g_simplex_perm_mod[ii + g_simplex_perm[jj + g_simplex_perm[kk]]];
    const auto gi1 =
        g_simplex_perm_mod[ii + static_cast<std::size_t>(i1) +
                           g_simplex_perm[jj + static_cast<std::size_t>(j1) +
                                          g_simplex_perm[kk + static_cast<std::size_t>(k1)]]];
    const auto gi2 =
        g_simplex_perm_mod[ii + static_cast<std::size_t>(i2) +
                           g_simplex_perm[jj + static_cast<std::size_t>(j2) +
                                          g_simplex_perm[kk + static_cast<std::size_t>(k2)]]];
    const auto gi3 = g_simplex_perm_mod[ii + 1U + g_simplex_perm[jj + 1U + g_simplex_perm[kk + 1U]]];
    return 32.0 * (contrib(kSimplexGrad[gi0], x0, y0, z0) + contrib(kSimplexGrad[gi1], x1, y1, z1) +
                   contrib(kSimplexGrad[gi2], x2, y2, z2) + contrib(kSimplexGrad[gi3], x3, y3, z3));
}

[[nodiscard]] double simplex_2d_noise(const double x, const double y, const double z)
{
    static constexpr double f[] = {0.4910, 0.9441, 1.7280};
    static constexpr double a[] = {0.2340, 0.7850, 1.2150};
    double total = 0.0;
    for (int octave = 0; octave < 3; ++octave)
    {
        total += simplex_noise(x * f[octave] / z, y * f[octave] / z, octave) * a[octave];
    }
    return total;
}

void apply_grain(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float strength = static_cast<float>(amount);
    const double wd = std::min(image.width, image.height);
    const double zoom = (1.0 + 8.0 * 0.25) / 800.0;
    constexpr float kLightnessScale = 0.15F;
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            float L = 0.0F;
            float a = 0.0F;
            float b = 0.0F;
            rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
            const double nx = static_cast<double>(x) / wd;
            const double ny = static_cast<double>(y) / wd;
            const float noise = static_cast<float>(simplex_2d_noise(nx, ny, zoom));
            const float mid = 1.0F - std::abs(L / 100.0F - 0.5F) * 2.0F;
            L += noise * strength * kLightnessScale * 100.0F * (0.25F + 0.75F * mid);
            lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
        }
    }
}

void apply_gamma(WorkingImage &image, const double gamma)
{
    if (std::abs(gamma - 1.0) <= 1.0e-6)
    {
        return;
    }
    const float exponent = static_cast<float>(1.0 / std::max(0.2, gamma));
    for (float &sample : image.rgb)
    {
        sample = sample <= 0.0F ? sample : std::pow(sample, exponent);
    }
}

void apply_colorbalance(WorkingImage &image, const double lift, const double gamma,
                        const double gain)
{
    if (lift == 0.0 && gamma == 0.0 && gain == 0.0)
    {
        return;
    }
    const float lift_v = 1.0F - static_cast<float>(lift);
    const float gain_v = 1.0F + static_cast<float>(gain);
    const float gamma_v = std::max(0.2F, 1.0F + static_cast<float>(gamma) * 0.5F);
    const float gamma_inv = 2.2F / gamma_v;
    constexpr float kDisplay = 1.0F / 2.2F;
    for (float &sample : image.rgb)
    {
        float value = sample <= 0.0F ? 0.0F : std::pow(sample, kDisplay);
        value = ((value - 1.0F) * lift_v + 1.0F) * gain_v;
        value = value <= 0.0F ? 0.0F : std::pow(value, gamma_inv);
        sample = value;
    }
}

void apply_color_contrast(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    const float slope = 1.0F + static_cast<float>(amount);
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float L = 0.0F;
        float a = 0.0F;
        float b = 0.0F;
        rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
        a *= slope;
        b *= slope;
        lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
}

void apply_velvia(WorkingImage &image, const double amount, const double bias)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float strength = static_cast<float>(amount);
    const float velvia_bias = static_cast<float>(std::clamp(bias, 0.0, 1.0));
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float pmax = std::max(r, std::max(g, b));
        const float pmin = std::min(r, std::min(g, b));
        const float plum = (pmax + pmin) * 0.5F;
        const float psat =
            plum <= 0.5F ? (pmax - pmin) / (1.0e-5F + pmax + pmin)
                         : (pmax - pmin) / (1.0e-5F + std::max(0.0F, 2.0F - pmax - pmin));
        const float pweight = std::clamp(((1.0F - (1.5F * psat)) +
                                          ((1.0F + (std::abs(plum - 0.5F) * 2.0F)) * (1.0F - velvia_bias))) /
                                             (1.0F + (1.0F - velvia_bias)),
                                         0.0F, 1.0F);
        const float saturation = strength * pweight;
        const float others_r = g + b;
        const float others_g = r + b;
        const float others_b = r + g;
        r = std::clamp(r + saturation * (r - 0.5F * others_r), 0.0F, 1.0F);
        g = std::clamp(g + saturation * (g - 0.5F * others_g), 0.0F, 1.0F);
        b = std::clamp(b + saturation * (b - 0.5F * others_b), 0.0F, 1.0F);
    }
}

void apply_monochrome(WorkingImage &image, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float mix = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float L = 0.0F;
        float a = 0.0F;
        float b = 0.0F;
        rgb_to_lab(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], L, a, b);
        a *= 1.0F - mix;
        b *= 1.0F - mix;
        lab_to_rgb(L, a, b, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
    }
}

void rgb_to_hsl(const float r, const float g, const float b, float &h, float &s, float &l)
{
    const float maxc = std::max(r, std::max(g, b));
    const float minc = std::min(r, std::min(g, b));
    l = (maxc + minc) * 0.5F;
    if (maxc <= minc + 1.0e-6F)
    {
        h = 0.0F;
        s = 0.0F;
        return;
    }
    const float delta = maxc - minc;
    s = l > 0.5F ? delta / (2.0F - maxc - minc) : delta / (maxc + minc);
    if (maxc == r)
    {
        h = (g - b) / delta + (g < b ? 6.0F : 0.0F);
    }
    else if (maxc == g)
    {
        h = (b - r) / delta + 2.0F;
    }
    else
    {
        h = (r - g) / delta + 4.0F;
    }
    h /= 6.0F;
}

[[nodiscard]] float hue_to_rgb(const float p, const float q, float t)
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
}

void hsl_to_rgb(const float h, const float s, const float l, float &r, float &g, float &b)
{
    if (s <= 1.0e-6F)
    {
        r = g = b = l;
        return;
    }
    const float q = l < 0.5F ? l * (1.0F + s) : l + s - l * s;
    const float p = 2.0F * l - q;
    r = hue_to_rgb(p, q, h + 1.0F / 3.0F);
    g = hue_to_rgb(p, q, h);
    b = hue_to_rgb(p, q, h - 1.0F / 3.0F);
}

void apply_split_toning(WorkingImage &image, const double shadows_hue, const double highlights_hue,
                        const double balance, const double amount)
{
    if (amount <= 0.0)
    {
        return;
    }
    const float mix_amount = static_cast<float>(std::clamp(amount, 0.0, 1.0));
    const float shadow_h = static_cast<float>(shadows_hue);
    const float highlight_h = static_cast<float>(highlights_hue);
    const float center = static_cast<float>(balance);
    const float compress = 0.15F;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float r = image.rgb[index];
        float g = image.rgb[index + 1U];
        float b = image.rgb[index + 2U];
        float h = 0.0F;
        float s = 0.0F;
        float l = 0.0F;
        rgb_to_hsl(r, g, b, h, s, l);
        float mix_r = r;
        float mix_g = g;
        float mix_b = b;
        float weight = 0.0F;
        if (l < center - compress)
        {
            hsl_to_rgb(shadow_h, 0.5F, l, mix_r, mix_g, mix_b);
            weight = std::clamp((center - compress - l) * 2.0F, 0.0F, 1.0F) * mix_amount;
        }
        else if (l > center + compress)
        {
            hsl_to_rgb(highlight_h, 0.5F, l, mix_r, mix_g, mix_b);
            weight = std::clamp((l - (center + compress)) * 2.0F, 0.0F, 1.0F) * mix_amount;
        }
        image.rgb[index] = std::clamp(r * (1.0F - weight) + mix_r * weight, 0.0F, 1.0F);
        image.rgb[index + 1U] = std::clamp(g * (1.0F - weight) + mix_g * weight, 0.0F, 1.0F);
        image.rgb[index + 2U] = std::clamp(b * (1.0F - weight) + mix_b * weight, 0.0F, 1.0F);
    }
}

void apply_dehaze(WorkingImage &image, const double amount)
{
    if (amount == 0.0)
    {
        return;
    }
    const float strength = static_cast<float>(amount);
    constexpr float kAirlight = 0.92F;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float dark = std::min(r, std::min(g, b));
        if (strength > 0.0F)
        {
            const float transmission = std::max(0.1F, 1.0F - strength * dark / kAirlight);
            r = (r - kAirlight) / transmission + kAirlight;
            g = (g - kAirlight) / transmission + kAirlight;
            b = (b - kAirlight) / transmission + kAirlight;
        }
        else
        {
            const float haze = -strength;
            r = r * (1.0F - haze) + kAirlight * haze;
            g = g * (1.0F - haze) + kAirlight * haze;
            b = b * (1.0F - haze) + kAirlight * haze;
        }
    }
}

} // namespace

Result<WorkingImage> working_from_raw(const DecodedRaw &raw, const std::uint32_t width,
                                      const std::uint32_t height,
                                      const CancellationToken &cancellation)
{
    if (width == 0 || height == 0)
    {
        return make_error(ErrorCode::kInvalidArgument, "Render output dimensions must be non-zero");
    }
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    const float denominator = static_cast<float>(
        std::max<std::int64_t>(1, static_cast<std::int64_t>(raw.white_level) - raw.black_level));

    for (std::uint32_t output_y = 0; output_y < height; ++output_y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::uint32_t source_y = std::min(
            raw.height - 1,
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_y) * raw.height / height));
        for (std::uint32_t output_x = 0; output_x < width; ++output_x)
        {
            const std::uint32_t source_x = std::min(
                raw.width - 1, static_cast<std::uint32_t>(static_cast<std::uint64_t>(output_x) *
                                                          raw.width / width));
            std::array<float, 3> sum{};
            std::array<std::uint32_t, 3> count{};
            for (int offset_y = -1; offset_y <= 1; ++offset_y)
            {
                const std::uint32_t y = static_cast<std::uint32_t>(std::clamp(
                    static_cast<int>(source_y) + offset_y, 0, static_cast<int>(raw.height) - 1));
                for (int offset_x = -1; offset_x <= 1; ++offset_x)
                {
                    const std::uint32_t x = static_cast<std::uint32_t>(std::clamp(
                        static_cast<int>(source_x) + offset_x, 0, static_cast<int>(raw.width) - 1));
                    const std::uint8_t channel =
                        raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width +
                                         (x % raw.cfa_width)];
                    sum[channel] +=
                        static_cast<float>(raw.pixels[static_cast<std::size_t>(y) * raw.width + x]);
                    ++count[channel];
                }
            }

            std::array<float, 3> camera_rgb{};
            for (std::size_t channel = 0; channel < camera_rgb.size(); ++channel)
            {
                const float sample =
                    count[channel] == 0 ? 0.0F : sum[channel] / static_cast<float>(count[channel]);
                camera_rgb[channel] =
                    std::max(0.0F, (sample - static_cast<float>(raw.black_level)) / denominator) *
                    raw.white_balance[channel];
            }
            const std::size_t output_index =
                (static_cast<std::size_t>(output_y) * width + output_x) * 3U;
            for (std::size_t output_channel = 0; output_channel < 3; ++output_channel)
            {
                float linear = 0.0F;
                for (std::size_t input_channel = 0; input_channel < 3; ++input_channel)
                {
                    linear += raw.camera_to_srgb[output_channel * 3U + input_channel] *
                              camera_rgb[input_channel];
                }
                image.rgb[output_index + output_channel] = linear;
            }
        }
    }
    return image;
}

Result<WorkingImage> working_from_srgb8(const RasterBuffer &raster)
{
    if (raster.width == 0 || raster.height == 0 ||
        raster.srgb.size() < static_cast<std::size_t>(raster.width) * raster.height * 3U)
    {
        return make_error(ErrorCode::kValidation, "Raster buffer is empty or undersized");
    }
    WorkingImage image;
    image.width = raster.width;
    image.height = raster.height;
    image.rgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
    for (std::size_t index = 0; index < image.rgb.size(); ++index)
    {
        image.rgb[index] = srgb_decode(static_cast<float>(raster.srgb[index]) / 255.0F);
    }
    return image;
}

Result<WorkingImage> apply_recipe_ops(WorkingImage image, const Recipe &recipe,
                                      const CancellationToken &cancellation)
{
    for (const auto &operation : recipe.operations)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        if (!operation.enabled || absorbed_operation(operation.id))
        {
            continue;
        }
        if (operation.id == "ravo.color.white_balance")
        {
            apply_white_balance(image, parameter(operation, "temperature", 6500.0),
                                parameter(operation, "tint", 0.0));
            continue;
        }
        if (operation.id == "ravo.core.exposure")
        {
            apply_exposure(image, parameter(operation, "exposure_ev", 0.0));
            continue;
        }
        if (operation.id == "ravo.core.contrast")
        {
            apply_contrast(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.core.highlights")
        {
            apply_highlights_shadows(image, parameter(operation, "amount", 0.0), 0.0);
            continue;
        }
        if (operation.id == "ravo.core.shadows")
        {
            apply_highlights_shadows(image, 0.0, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.core.whites")
        {
            apply_whites_blacks(image, parameter(operation, "amount", 0.0), 0.0);
            continue;
        }
        if (operation.id == "ravo.core.blacks")
        {
            apply_whites_blacks(image, 0.0, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.vibrance")
        {
            apply_vibrance_saturation(image, parameter(operation, "amount", 0.0), 0.0);
            continue;
        }
        if (operation.id == "ravo.color.saturation")
        {
            apply_vibrance_saturation(image, 0.0, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.geometry.rotate")
        {
            auto rotated = rotate_working(std::move(image),
                                          static_cast<int>(parameter(operation, "quarters", 0.0)));
            if (!rotated)
            {
                return rotated.error();
            }
            image = std::move(rotated).value();
            continue;
        }
        if (operation.id == "ravo.geometry.crop")
        {
            auto cropped = crop_working(
                std::move(image), parameter(operation, "x", 0.0), parameter(operation, "y", 0.0),
                parameter(operation, "width", 1.0), parameter(operation, "height", 1.0));
            if (!cropped)
            {
                return cropped.error();
            }
            image = std::move(cropped).value();
            continue;
        }
        if (operation.id == "ravo.geometry.flip")
        {
            auto flipped = flip_working(std::move(image), parameter(operation, "horizontal", 0.0) != 0.0,
                                        parameter(operation, "vertical", 0.0) != 0.0);
            if (!flipped)
            {
                return flipped.error();
            }
            image = std::move(flipped).value();
            continue;
        }
        if (operation.id == "ravo.geometry.straighten")
        {
            auto straightened = straighten_working(std::move(image),
                                                   parameter(operation, "degrees", 0.0));
            if (!straightened)
            {
                return straightened.error();
            }
            image = std::move(straightened).value();
            continue;
        }
        if (operation.id == "ravo.core.gamma")
        {
            apply_gamma(image, parameter(operation, "gamma", 1.0));
            continue;
        }
        if (operation.id == "ravo.color.colorbalance")
        {
            apply_colorbalance(image, parameter(operation, "lift", 0.0),
                               parameter(operation, "gamma", 0.0), parameter(operation, "gain", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.colorcontrast")
        {
            apply_color_contrast(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.velvia")
        {
            apply_velvia(image, parameter(operation, "amount", 0.0),
                         parameter(operation, "bias", 1.0));
            continue;
        }
        if (operation.id == "ravo.color.monochrome")
        {
            apply_monochrome(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.color.splittoning")
        {
            apply_split_toning(image, parameter(operation, "shadows_hue", 0.55),
                               parameter(operation, "highlights_hue", 0.08),
                               parameter(operation, "balance", 0.5),
                               parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.detail.sharpen")
        {
            apply_unsharp(image, parameter(operation, "amount", 0.0),
                          parameter(operation, "radius", 1.0),
                          parameter(operation, "threshold", 0.0));
            continue;
        }
        if (operation.id == "ravo.detail.clarity")
        {
            apply_clarity(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.vignette")
        {
            apply_vignette(image, parameter(operation, "amount", 0.0),
                           parameter(operation, "midpoint", 0.5),
                           parameter(operation, "falloff", 0.5));
            continue;
        }
        if (operation.id == "ravo.effect.grain")
        {
            apply_grain(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.bloom")
        {
            apply_bloom(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.soften")
        {
            apply_soften(image, parameter(operation, "amount", 0.0));
            continue;
        }
        if (operation.id == "ravo.effect.dehaze")
        {
            apply_dehaze(image, parameter(operation, "amount", 0.0));
            continue;
        }
        return make_error(ErrorCode::kUnsupported, "Operation has no CPU implementation",
                          {{"operation_id", operation.id}});
    }
    return image;
}

RenderedImage encode_working_srgb(const WorkingImage &image)
{
    RenderedImage result;
    result.width = image.width;
    result.height = image.height;
    result.rgb.resize(image.rgb.size());
    for (std::size_t index = 0; index < image.rgb.size(); ++index)
    {
        result.rgb[index] =
            static_cast<std::uint8_t>(std::lround(srgb_encode(image.rgb[index]) * 255.0F));
    }
    return result;
}

Result<std::vector<std::uint8_t>> encode_png_bytes(const RenderedImage &image)
{
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    png.width = image.width;
    png.height = image.height;
    png.format = PNG_FORMAT_RGB;
    png_alloc_size_t encoded_size = 0;
    if (png_image_write_to_memory(&png, nullptr, &encoded_size, 0, image.rgb.data(), 0, nullptr) ==
        0)
    {
        return make_error(ErrorCode::kIo, "Unable to size PNG output",
                          {{"png_error", png.message}});
    }
    std::vector<std::uint8_t> encoded(encoded_size);
    if (png_image_write_to_memory(&png, encoded.data(), &encoded_size, 0, image.rgb.data(), 0,
                                  nullptr) == 0)
    {
        return make_error(ErrorCode::kIo, "Unable to encode PNG output",
                          {{"png_error", png.message}});
    }
    encoded.resize(encoded_size);
    return encoded;
}

Result<RasterBuffer> decode_png_bytes(const std::vector<std::uint8_t> &bytes)
{
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&png, bytes.data(), bytes.size()) == 0)
    {
        return make_error(ErrorCode::kValidation, "Unable to read PNG buffer",
                          {{"png_error", png.message}});
    }
    png.format = PNG_FORMAT_RGB;
    RasterBuffer raster;
    raster.width = png.width;
    raster.height = png.height;
    raster.srgb.resize(static_cast<std::size_t>(PNG_IMAGE_SIZE(png)));
    if (png_image_finish_read(&png, nullptr, raster.srgb.data(), 0, nullptr) == 0)
    {
        png_image_free(&png);
        return make_error(ErrorCode::kValidation, "Unable to decode PNG buffer",
                          {{"png_error", png.message}});
    }
    png_image_free(&png);
    return raster;
}

} // namespace ravo
