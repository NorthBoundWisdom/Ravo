#include "capability_ops.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

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
        return make_error(ErrorCode::kValidation, "Color equalizer band array must have 8 values",
                          {{"parameter", std::string(name)},
                           {"count", std::to_string(array->size())}});
    }
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        const double value = as_number((*array)[index], std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(value))
        {
            return make_error(ErrorCode::kValidation,
                              "Color equalizer band value must be finite",
                              {{"parameter", std::string(name)}, {"index", std::to_string(index)}});
        }
        values[index] = value;
    }
    return values;
}

[[nodiscard]] float luma(const float r, const float g, const float b) noexcept
{
    return 0.2126F * r + 0.7152F * g + 0.0722F * b;
}

void rgb_to_hsl(const float r, const float g, const float b, float &h, float &s, float &l) noexcept
{
    const float max_c = std::max(r, std::max(g, b));
    const float min_c = std::min(r, std::min(g, b));
    l = 0.5F * (max_c + min_c);
    const float delta = max_c - min_c;
    if (delta <= 1.0e-6F)
    {
        h = 0.0F;
        s = 0.0F;
        return;
    }
    s = l > 0.5F ? delta / (2.0F - max_c - min_c) : delta / (max_c + min_c);
    if (max_c == r)
    {
        h = (g - b) / delta + (g < b ? 6.0F : 0.0F);
    }
    else if (max_c == g)
    {
        h = (b - r) / delta + 2.0F;
    }
    else
    {
        h = (r - g) / delta + 4.0F;
    }
    h /= 6.0F;
}

[[nodiscard]] float hue_to_rgb(const float p, const float q, float t) noexcept
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

void hsl_to_rgb(const float h, const float s, const float l, float &r, float &g, float &b) noexcept
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

void blur_plane(std::vector<float> &plane, const std::uint32_t width, const std::uint32_t height,
                const float sigma)
{
    if (plane.empty() || width == 0 || height == 0 || sigma <= 0.01F)
    {
        return;
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
    for (std::uint32_t y = 0; y < height; ++y)
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
    }
    for (std::uint32_t x = 0; x < width; ++x)
    {
        for (std::uint32_t y = 0; y < height; ++y)
        {
            float acc = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset)
            {
                const int sample_y = std::clamp(static_cast<int>(y) + offset, 0,
                                                static_cast<int>(height) - 1);
                acc += temp[static_cast<std::size_t>(static_cast<std::uint32_t>(sample_y)) * width +
                            x] *
                       kernel[static_cast<std::size_t>(offset + radius)];
            }
            plane[static_cast<std::size_t>(y) * width + x] = acc;
        }
    }
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
    {
        return image.rgb[(static_cast<std::size_t>(py) * image.width + px) * 3U + channel];
    };
    const float top = at(x0, y0) * (1.0F - tx) + at(x1, y0) * tx;
    const float bottom = at(x0, y1) * (1.0F - tx) + at(x1, y1) * tx;
    return top * (1.0F - ty) + bottom * ty;
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

[[nodiscard]] std::uint8_t cfa_channel(const DecodedRaw &raw, const std::uint32_t x,
                                       const std::uint32_t y) noexcept
{
    return raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width + (x % raw.cfa_width)];
}

} // namespace

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
    const std::string mode = parameter_string(operation, "mode", "inpaint");
    if (mode != kRawHighlightsModeClip && mode != kRawHighlightsModeInpaint)
    {
        return make_error(ErrorCode::kUnsupported, "RAW highlight reconstruction mode is unsupported",
                          {{"mode", mode}});
    }
    const double amount = std::clamp(parameter(operation, "amount", 1.0), 0.0, 1.0);
    const double clip = std::clamp(parameter(operation, "clip", 0.987), 0.5, 1.0);
    if (amount <= 0.0 || raw.width < 3 || raw.height < 3)
    {
        return {};
    }
    const auto clip_level = static_cast<std::uint16_t>(
        std::lround(static_cast<double>(raw.white_level) * clip));
    const std::size_t pixel_count = static_cast<std::size_t>(raw.width) * raw.height;
    if (raw.pixels.size() < pixel_count)
    {
        return make_error(ErrorCode::kValidation, "RAW buffer is undersized for highlight reconstruction");
    }

    if (mode == kRawHighlightsModeClip)
    {
        for (std::size_t index = 0; index < pixel_count; ++index)
        {
            if (raw.pixels[index] > clip_level)
            {
                const auto mixed = static_cast<double>(raw.pixels[index]) * (1.0 - amount) +
                                   static_cast<double>(clip_level) * amount;
                raw.pixels[index] = static_cast<std::uint16_t>(std::lround(mixed));
            }
        }
        return {};
    }

    std::vector<std::uint16_t> reconstructed = raw.pixels;
    constexpr int kPasses = 6;
    constexpr int kRadius = 2;
    for (int pass = 0; pass < kPasses; ++pass)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        std::vector<std::uint16_t> next = reconstructed;
        for (std::uint32_t y = 0; y < raw.height; ++y)
        {
            for (std::uint32_t x = 0; x < raw.width; ++x)
            {
                const std::size_t index = static_cast<std::size_t>(y) * raw.width + x;
                if (reconstructed[index] <= clip_level)
                {
                    continue;
                }
                const auto channel = cfa_channel(raw, x, y);
                double same_sum = 0.0;
                int same_count = 0;
                std::array<double, 3> other_sum{};
                std::array<int, 3> other_count{};
                for (int oy = -kRadius; oy <= kRadius; ++oy)
                {
                    const int ny = static_cast<int>(y) + oy;
                    if (ny < 0 || ny >= static_cast<int>(raw.height))
                    {
                        continue;
                    }
                    for (int ox = -kRadius; ox <= kRadius; ++ox)
                    {
                        if (ox == 0 && oy == 0)
                        {
                            continue;
                        }
                        const int nx = static_cast<int>(x) + ox;
                        if (nx < 0 || nx >= static_cast<int>(raw.width))
                        {
                            continue;
                        }
                        const auto neighbor_x = static_cast<std::uint32_t>(nx);
                        const auto neighbor_y = static_cast<std::uint32_t>(ny);
                        const std::size_t neighbor =
                            static_cast<std::size_t>(neighbor_y) * raw.width + neighbor_x;
                        if (reconstructed[neighbor] > clip_level)
                        {
                            continue;
                        }
                        const auto neighbor_channel = cfa_channel(raw, neighbor_x, neighbor_y);
                        if (neighbor_channel == channel)
                        {
                            same_sum += static_cast<double>(reconstructed[neighbor]);
                            ++same_count;
                        }
                        else
                        {
                            other_sum[neighbor_channel] +=
                                static_cast<double>(reconstructed[neighbor]);
                            ++other_count[neighbor_channel];
                        }
                    }
                }
                double estimate = static_cast<double>(clip_level);
                if (same_count > 0)
                {
                    estimate = same_sum / static_cast<double>(same_count);
                }
                else
                {
                    double ratio_sum = 0.0;
                    int ratio_count = 0;
                    for (std::size_t other = 0; other < other_sum.size(); ++other)
                    {
                        if (other_count[other] == 0)
                        {
                            continue;
                        }
                        ratio_sum += other_sum[other] / static_cast<double>(other_count[other]);
                        ++ratio_count;
                    }
                    if (ratio_count > 0)
                    {
                        estimate = ratio_sum / static_cast<double>(ratio_count);
                    }
                }
                estimate = std::clamp(estimate, 0.0, static_cast<double>(clip_level));
                next[index] = static_cast<std::uint16_t>(std::lround(estimate));
            }
        }
        reconstructed.swap(next);
    }

    for (std::size_t index = 0; index < pixel_count; ++index)
    {
        if (raw.pixels[index] <= clip_level)
        {
            continue;
        }
        const double mixed = static_cast<double>(raw.pixels[index]) * (1.0 - amount) +
                             static_cast<double>(reconstructed[index]) * amount;
        raw.pixels[index] =
            static_cast<std::uint16_t>(std::clamp(std::lround(mixed), 0L, 65535L));
    }
    return {};
}

Result<void> apply_denoise_profile(WorkingImage &image, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
{
    const double strength = std::clamp(parameter(operation, "strength", 0.0), 0.0, 1.0);
    if (strength <= 0.0 || image.width < 3 || image.height < 3)
    {
        return {};
    }
    const double chroma = std::clamp(parameter(operation, "chroma", 1.0), 0.0, 1.0);
    const double radius = std::clamp(parameter(operation, "radius", 1.0), 0.5, 8.0);
    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> y_plane(count);
    std::vector<float> cb_plane(count);
    std::vector<float> cr_plane(count);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const float r = image.rgb[pixel * 3U];
        const float g = image.rgb[pixel * 3U + 1U];
        const float b = image.rgb[pixel * 3U + 2U];
        y_plane[pixel] = luma(r, g, b);
        cb_plane[pixel] = 0.5F + (b - y_plane[pixel]) * 0.5F;
        cr_plane[pixel] = 0.5F + (r - y_plane[pixel]) * 0.5F;
    }

    auto denoise_plane = [&](std::vector<float> &plane, const float amount) -> Result<void>
    {
        std::vector<float> g1 = plane;
        blur_plane(g1, image.width, image.height, static_cast<float>(radius));
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        std::vector<float> g2 = g1;
        blur_plane(g2, image.width, image.height, static_cast<float>(radius) * 2.0F);
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        std::vector<float> g3 = g2;
        blur_plane(g3, image.width, image.height, static_cast<float>(radius) * 4.0F);
        const float threshold = 0.02F + amount * 0.08F;
        for (std::size_t index = 0; index < plane.size(); ++index)
        {
            const float d1 = plane[index] - g1[index];
            const float d2 = g1[index] - g2[index];
            const float d3 = g2[index] - g3[index];
            const auto shrink = [threshold](const float detail)
            {
                const float mag = std::abs(detail);
                if (mag <= threshold)
                {
                    return 0.0F;
                }
                return std::copysign(mag - threshold, detail);
            };
            plane[index] = g3[index] + shrink(d3) * (1.0F - amount * 0.35F) +
                           shrink(d2) * (1.0F - amount * 0.55F) +
                           shrink(d1) * (1.0F - amount);
        }
        return {};
    };

    auto luma_done = denoise_plane(y_plane, static_cast<float>(strength));
    if (!luma_done)
    {
        return luma_done.error();
    }
    auto chroma_done =
        denoise_plane(cb_plane, static_cast<float>(std::min(1.0, strength * chroma * 1.4)));
    if (!chroma_done)
    {
        return chroma_done.error();
    }
    chroma_done =
        denoise_plane(cr_plane, static_cast<float>(std::min(1.0, strength * chroma * 1.4)));
    if (!chroma_done)
    {
        return chroma_done.error();
    }

    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        const float y = y_plane[pixel];
        const float cb = cb_plane[pixel] - 0.5F;
        const float cr = cr_plane[pixel] - 0.5F;
        image.rgb[pixel * 3U] = y + cr * 2.0F;
        image.rgb[pixel * 3U + 1U] = y - 0.344136F * (cb * 2.0F) - 0.714136F * (cr * 2.0F);
        image.rgb[pixel * 3U + 2U] = y + cb * 2.0F;
    }
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
    if (mode == kLensModeLookup)
    {
        const auto *calibration = find_lens_calibration(
            parameter_string(operation, "camera_make", ""),
            parameter_string(operation, "camera_model", ""),
            parameter_string(operation, "lens", ""), parameter(operation, "focal_mm", 50.0));
        if (calibration == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "No lens calibration matches the lookup request",
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
    if (!std::isfinite(k1) || !std::isfinite(k2) || !std::isfinite(tca_r) || !std::isfinite(tca_b) ||
        !std::isfinite(vignetting))
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

    WorkingImage source = image;
    const float cx = static_cast<float>(image.width - 1U) * 0.5F;
    const float cy = static_cast<float>(image.height - 1U) * 0.5F;
    const float norm = static_cast<float>(std::max(image.width, image.height));
    const auto scales = std::array<float, 3>{static_cast<float>(tca_r), 1.0F, static_cast<float>(tca_b)};
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float xd = (static_cast<float>(x) - cx) / norm;
            const float yd = (static_cast<float>(y) - cy) / norm;
            const float r2 = xd * xd + yd * yd;
            const float distortion = 1.0F + static_cast<float>(k1) * r2 + static_cast<float>(k2) * r2 * r2;
            const float falloff =
                1.0F - static_cast<float>(vignetting) * r2 * (1.0F + 0.7F * r2);
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            for (std::size_t channel = 0; channel < 3; ++channel)
            {
                const float scale = distortion * scales[channel];
                const float sx = cx + xd * scale * norm;
                const float sy = cy + yd * scale * norm;
                image.rgb[index + channel] =
                    sample_channel(source, sx, sy, channel) * std::max(falloff, 0.05F);
            }
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
    for (std::size_t index = 0; index < kColorEqualizerBandCount; ++index)
    {
        if (std::abs(hue_shifts.value()[index]) > 1.0e-8 ||
            std::abs(sat_shifts.value()[index]) > 1.0e-8 ||
            std::abs(light_shifts.value()[index]) > 1.0e-8)
        {
            identity = false;
            break;
        }
    }
    if (identity)
    {
        return {};
    }

    constexpr float kBandWidth = 1.0F / static_cast<float>(kColorEqualizerBandCount);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            float h = 0.0F;
            float s = 0.0F;
            float l = 0.0F;
            rgb_to_hsl(image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U], h, s, l);
            float hue_delta = 0.0F;
            float sat_delta = 0.0F;
            float light_delta = 0.0F;
            float weight_sum = 0.0F;
            for (std::size_t band = 0; band < kColorEqualizerBandCount; ++band)
            {
                const float center = (static_cast<float>(band) + 0.5F) * kBandWidth;
                float delta = std::abs(h - center);
                delta = std::min(delta, 1.0F - delta);
                const float weight = std::exp(-0.5F * (delta / (kBandWidth * 0.7F)) *
                                              (delta / (kBandWidth * 0.7F)));
                hue_delta += weight * static_cast<float>(hue_shifts.value()[band]);
                sat_delta += weight * static_cast<float>(sat_shifts.value()[band]);
                light_delta += weight * static_cast<float>(light_shifts.value()[band]);
                weight_sum += weight;
            }
            if (weight_sum > 1.0e-6F)
            {
                hue_delta /= weight_sum;
                sat_delta /= weight_sum;
                light_delta /= weight_sum;
            }
            h = h + hue_delta;
            h -= std::floor(h);
            s = std::clamp(s * (1.0F + sat_delta), 0.0F, 1.0F);
            l = std::max(0.0F, l * std::exp2(light_delta));
            hsl_to_rgb(h, s, l, image.rgb[index], image.rgb[index + 1U], image.rgb[index + 2U]);
        }
    }
    return {};
}

Result<void> apply_graduated_nd(WorkingImage &image, const OperationInstance &operation,
                                const CancellationToken &cancellation)
{
    const double density = std::clamp(parameter(operation, "density_ev", 0.0), -4.0, 4.0);
    if (std::abs(density) <= 1.0e-8)
    {
        return {};
    }
    const double hardness = std::clamp(parameter(operation, "hardness", 0.5), 0.0, 1.0);
    const double rotation_deg = std::clamp(parameter(operation, "rotation_deg", 0.0), -180.0, 180.0);
    const double offset = std::clamp(parameter(operation, "offset", 0.0), -1.0, 1.0);
    const float radians =
        static_cast<float>(rotation_deg) * static_cast<float>(std::numbers::pi_v<double> / 180.0);
    const float axis_x = std::sin(radians);
    const float axis_y = std::cos(radians);
    const float softness = std::max(0.02F, 0.55F - static_cast<float>(hardness) * 0.5F);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const float ny = image.height <= 1U ?
                             0.0F :
                             static_cast<float>(y) / static_cast<float>(image.height - 1U) - 0.5F;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const float nx = image.width <= 1U ?
                                 0.0F :
                                 static_cast<float>(x) / static_cast<float>(image.width - 1U) - 0.5F;
            const float axis = nx * axis_x + ny * axis_y - static_cast<float>(offset);
            const float t = std::clamp(0.5F + axis / softness, 0.0F, 1.0F);
            const float gain = std::exp2(static_cast<float>(-density) * t);
            const std::size_t index = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            image.rgb[index] *= gain;
            image.rgb[index + 1U] *= gain;
            image.rgb[index + 2U] *= gain;
        }
    }
    return {};
}

Result<void> apply_tone_equalizer(WorkingImage &image, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
{
    const std::array<double, 5> bands{
        parameter(operation, "blacks", 0.0),
        parameter(operation, "shadows", 0.0),
        parameter(operation, "midtones", 0.0),
        parameter(operation, "highlights", 0.0),
        parameter(operation, "whites", 0.0),
    };
    bool identity = true;
    for (const double band : bands)
    {
        if (!std::isfinite(band) || std::abs(band) > 4.0)
        {
            return make_error(ErrorCode::kValidation,
                              "Tone equalizer band must be a finite EV in [-4, 4]");
        }
        if (std::abs(band) > 1.0e-8)
        {
            identity = false;
        }
    }
    if (identity || image.width == 0 || image.height == 0)
    {
        return {};
    }

    const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> log_luma(count);
    for (std::size_t pixel = 0; pixel < count; ++pixel)
    {
        const float y = std::max(
            1.0e-6F, luma(image.rgb[pixel * 3U], image.rgb[pixel * 3U + 1U], image.rgb[pixel * 3U + 2U]));
        log_luma[pixel] = std::log2(y);
    }
    blur_plane(log_luma, image.width, image.height, 8.0F);
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }

    constexpr std::array<float, 5> kCenters{-6.0F, -3.0F, 0.0F, 3.0F, 6.0F};
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            const std::size_t pixel = static_cast<std::size_t>(y) * image.width + x;
            float weight_sum = 0.0F;
            float ev = 0.0F;
            for (std::size_t band = 0; band < bands.size(); ++band)
            {
                const float delta = log_luma[pixel] - kCenters[band];
                const float weight = std::exp(-0.5F * (delta / 1.6F) * (delta / 1.6F));
                ev += weight * static_cast<float>(bands[band]);
                weight_sum += weight;
            }
            if (weight_sum > 1.0e-6F)
            {
                ev /= weight_sum;
            }
            const float gain = std::exp2(ev);
            const std::size_t index = pixel * 3U;
            image.rgb[index] *= gain;
            image.rgb[index + 1U] *= gain;
            image.rgb[index + 2U] *= gain;
        }
    }
    return {};
}

} // namespace ravo
