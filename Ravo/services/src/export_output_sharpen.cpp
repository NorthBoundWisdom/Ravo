#include "export_output_sharpen.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>
#include <vector>

namespace ravo
{
namespace
{

[[nodiscard]] Result<void> check_cancel(const CancellationToken &cancellation)
{
    return cancellation.check();
}

[[nodiscard]] std::vector<float> gaussian_kernel(const double radius)
{
    const double sigma = std::max(0.05, radius);
    const int half = std::max(1, static_cast<int>(std::ceil(sigma * 3.0)));
    std::vector<float> kernel(static_cast<std::size_t>(half * 2 + 1));
    double sum = 0.0;
    for (int i = -half; i <= half; ++i)
    {
        const double value = std::exp(-(static_cast<double>(i) * i) / (2.0 * sigma * sigma));
        kernel[static_cast<std::size_t>(i + half)] = static_cast<float>(value);
        sum += value;
    }
    for (float &weight : kernel)
        weight = static_cast<float>(weight / sum);
    return kernel;
}

void blur_separable(const std::vector<float> &input, std::vector<float> &output,
                    const std::uint32_t width, const std::uint32_t height,
                    const std::vector<float> &kernel)
{
    const int half = static_cast<int>(kernel.size() / 2U);
    std::vector<float> temp(input.size());
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                double acc = 0.0;
                for (int k = -half; k <= half; ++k)
                {
                    const int sample_x =
                        std::clamp(static_cast<int>(x) + k, 0, static_cast<int>(width) - 1);
                    const std::size_t index =
                        (static_cast<std::size_t>(y) * width + static_cast<std::size_t>(sample_x)) *
                            3U +
                        static_cast<std::size_t>(channel);
                    acc += input[index] * kernel[static_cast<std::size_t>(k + half)];
                }
                temp[(static_cast<std::size_t>(y) * width + x) * 3U +
                     static_cast<std::size_t>(channel)] = static_cast<float>(acc);
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            for (int channel = 0; channel < 3; ++channel)
            {
                double acc = 0.0;
                for (int k = -half; k <= half; ++k)
                {
                    const int sample_y =
                        std::clamp(static_cast<int>(y) + k, 0, static_cast<int>(height) - 1);
                    const std::size_t index =
                        (static_cast<std::size_t>(sample_y) * width + x) * 3U +
                        static_cast<std::size_t>(channel);
                    acc += temp[index] * kernel[static_cast<std::size_t>(k + half)];
                }
                output[(static_cast<std::size_t>(y) * width + x) * 3U +
                       static_cast<std::size_t>(channel)] = static_cast<float>(acc);
            }
        }
    }
}

void apply_usm(std::vector<float> &samples, const std::uint32_t width, const std::uint32_t height,
               const ExportOutputSharpenOptions &options)
{
    if (samples.empty() || width == 0 || height == 0)
        return;
    const auto kernel = gaussian_kernel(options.radius);
    std::vector<float> blurred(samples.size());
    blur_separable(samples, blurred, width, height, kernel);
    const float amount = static_cast<float>(options.amount);
    const float threshold = static_cast<float>(options.threshold / 100.0);
    for (std::size_t index = 0; index < samples.size(); ++index)
    {
        const float diff = samples[index] - blurred[index];
        if (std::fabs(diff) >= threshold)
            samples[index] = std::clamp(samples[index] + amount * diff, 0.0f, 1.0f);
    }
}

template <typename T>
[[nodiscard]] std::vector<float> to_unit(const std::vector<T> &samples)
{
    std::vector<float> out(samples.size());
    if constexpr (std::is_same_v<T, float>)
    {
        for (std::size_t i = 0; i < samples.size(); ++i)
            out[i] = std::clamp(samples[i], 0.0f, 1.0f);
    }
    else if constexpr (std::is_same_v<T, std::uint16_t>)
    {
        constexpr float scale = 1.0f / 65535.0f;
        for (std::size_t i = 0; i < samples.size(); ++i)
            out[i] = static_cast<float>(samples[i]) * scale;
    }
    else
    {
        constexpr float scale = 1.0f / 255.0f;
        for (std::size_t i = 0; i < samples.size(); ++i)
            out[i] = static_cast<float>(samples[i]) * scale;
    }
    return out;
}

template <typename T>
[[nodiscard]] std::vector<T> from_unit(const std::vector<float> &samples)
{
    std::vector<T> out(samples.size());
    if constexpr (std::is_same_v<T, float>)
    {
        for (std::size_t i = 0; i < samples.size(); ++i)
            out[i] = samples[i];
    }
    else if constexpr (std::is_same_v<T, std::uint16_t>)
    {
        for (std::size_t i = 0; i < samples.size(); ++i)
            out[i] = static_cast<std::uint16_t>(std::lround(samples[i] * 65535.0f));
    }
    else
    {
        for (std::size_t i = 0; i < samples.size(); ++i)
            out[i] = static_cast<std::uint8_t>(std::lround(samples[i] * 255.0f));
    }
    return out;
}

} // namespace

Result<RenderedExportImage> apply_export_output_sharpen(RenderedExportImage image,
                                                        const ExportOutputSharpenOptions &options,
                                                        const CancellationToken &cancellation)
{
    auto cancelled = check_cancel(cancellation);
    if (!cancelled)
        return cancelled.error();
    auto valid = validate_export_output_sharpen_options(options);
    if (!valid)
        return valid.error();
    if (!options.enabled || options.amount <= 0.0)
        return image;
    if (image.width == 0 || image.height == 0)
        return image;

    const auto expected = static_cast<std::size_t>(image.width) * image.height * 3U;
    auto visit = [&](auto &samples) -> Result<RenderedExportImage>
    {
        using Sample = std::decay_t<decltype(samples)>;
        if (samples.size() != expected)
        {
            return make_error(ErrorCode::kValidation, "Export sharpen buffer size is invalid",
                              {{"reason", "invalid_export_sharpen_buffer"}});
        }
        cancelled = check_cancel(cancellation);
        if (!cancelled)
            return cancelled.error();
        auto unit = to_unit(samples);
        apply_usm(unit, image.width, image.height, options);
        cancelled = check_cancel(cancellation);
        if (!cancelled)
            return cancelled.error();
        samples = from_unit<typename Sample::value_type>(unit);
        return image;
    };

    if (std::holds_alternative<std::vector<std::uint8_t>>(image.samples))
        return visit(std::get<std::vector<std::uint8_t>>(image.samples));
    if (std::holds_alternative<std::vector<std::uint16_t>>(image.samples))
        return visit(std::get<std::vector<std::uint16_t>>(image.samples));
    return visit(std::get<std::vector<float>>(image.samples));
}

} // namespace ravo
