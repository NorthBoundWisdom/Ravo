#include "image_ops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

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

void apply_highlights_shadows(WorkingImage &image, const double highlights, const double shadows)
{
    if (highlights == 0.0 && shadows == 0.0)
    {
        return;
    }
    const float highlight_ev = static_cast<float>(highlights);
    const float shadow_ev = static_cast<float>(shadows);
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        float &r = image.rgb[index];
        float &g = image.rgb[index + 1U];
        float &b = image.rgb[index + 2U];
        const float y = luma(r, g, b);
        const float hmask = std::clamp((y - 0.35F) / 0.45F, 0.0F, 1.0F);
        const float smask = 1.0F - std::clamp((y - 0.02F) / 0.38F, 0.0F, 1.0F);
        const float scale = std::exp2(highlight_ev * hmask) * std::exp2(shadow_ev * smask);
        r *= scale;
        g *= scale;
        b *= scale;
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
