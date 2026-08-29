#include "monochrome.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "d50_lab.h"
#include "retouch.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    if (left == 0U || right == 0U)
        return 0U;
    return left > std::numeric_limits<std::uint64_t>::max() / right ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept
{
    return left > std::numeric_limits<std::uint64_t>::max() - right ?
               std::numeric_limits<std::uint64_t>::max() :
               left + right;
}

void checkpoint(const detail::MonochromeControl &control, const detail::MonochromeCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
        control.checkpoint_callback(control.context, stage, progress);
}

[[nodiscard]] float fast_exp(const float value) noexcept
{
    constexpr std::int32_t one = 0x3f800000;
    constexpr std::int32_t exponential = 0x402df854;
    const auto bits = static_cast<std::int32_t>(static_cast<float>(one) +
                                                value * static_cast<float>(exponential - one));
    return std::bit_cast<float>(std::max(bits, std::int32_t{0}));
}

[[nodiscard]] float envelope(const float lightness) noexcept
{
    const float x = std::clamp(lightness / 100.0F, 0.0F, 1.0F);
    constexpr float beta = 0.6F;
    if (x < beta)
    {
        const float value = x / beta - 1.0F;
        return 1.0F - value * value;
    }
    const float value = (1.0F - x) / (1.0F - beta);
    const float squared = value * value;
    return 3.0F * squared - 2.0F * squared * value;
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0U || input.height == 0U || pixels > std::vector<float>{}.max_size() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels) * 3U)
        return make_error(ErrorCode::kValidation, "Monochrome input dimensions are invalid",
                          {{"reason", "invalid_monochrome_input"}});
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
        return make_error(ErrorCode::kUnsupported,
                          "Monochrome requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_monochrome_working_space"}});
    if (!input.canonical_roi_scale.valid())
        return make_error(ErrorCode::kValidation, "Monochrome requires canonical image scale",
                          {{"reason", "invalid_monochrome_roi_scale"}});
    for (std::size_t index = 0U; index < input.rgb.size(); ++index)
    {
        if (!std::isfinite(input.rgb[index]))
            return make_error(ErrorCode::kValidation,
                              "Monochrome input contains a non-finite sample",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_monochrome_input"}});
    }
    return {};
}

} // namespace

Result<WorkingImage> detail::apply_monochrome_controlled(WorkingImage input,
                                                         const MonochromeParams &params,
                                                         const CancellationToken &cancellation,
                                                         const MonochromeControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = monochrome_to_parameters(params);
    if (!canonical)
        return canonical.error();
    auto valid = validate_input(input);
    if (!valid)
        return valid.error();
    if (params.mix == 0.0)
    {
        checkpoint(control, MonochromeCheckpoint::kBeforePublication, 0U);
        active = cancellation.check();
        return active ? Result<WorkingImage>{std::move(input)} : active.error();
    }
    const std::size_t pixels = static_cast<std::size_t>(input.width) * input.height;
    std::vector<d50_lab::Triplet> lab(pixels);
    std::vector<float> filter(pixels);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, MonochromeCheckpoint::kConvertRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * input.width + column;
            lab[pixel] = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(
                {input.rgb[pixel * 3U], input.rgb[pixel * 3U + 1U], input.rgb[pixel * 3U + 2U]}));
        }
    }
    const float a = static_cast<float>(params.filter_a);
    const float b = static_cast<float>(params.filter_b);
    const float size = static_cast<float>(params.size);
    const float sigma_squared = 2.0F * (size * 128.0F) * (size * 128.0F);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, MonochromeCheckpoint::kFilterRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * input.width + column;
            const float da = lab[pixel][1] - a;
            const float db = lab[pixel][2] - b;
            const float exponent = -std::clamp((da * da + db * db) / sigma_squared, 0.0F, 1.0F);
            filter[pixel] = 100.0F * fast_exp(exponent);
        }
    }
    checkpoint(control, MonochromeCheckpoint::kBeforeBilateral, 0U);
    active = cancellation.check();
    if (!active)
        return active.error();
    const float sigma_s = 20.0F * input.canonical_roi_scale.value();
    auto smoothed = detail::bilateral_filter_lightness(filter, input.width, input.height, sigma_s,
                                                       250.0F, cancellation);
    if (!smoothed)
        return smoothed.error();
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    const float highlights = static_cast<float>(params.highlights);
    const float mix = static_cast<float>(params.mix);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, MonochromeCheckpoint::kOutputRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = static_cast<std::size_t>(row) * input.width + column;
            const float envelope_value = envelope(lab[pixel][0]);
            const float blend = envelope_value + (1.0F - envelope_value) * (1.0F - highlights);
            const float mono_lightness =
                (1.0F - blend) * lab[pixel][0] + blend * filter[pixel] * 0.01F * lab[pixel][0];
            const d50_lab::Triplet adjusted{
                (1.0F - mix) * lab[pixel][0] + mix * mono_lightness,
                (1.0F - mix) * lab[pixel][1],
                (1.0F - mix) * lab[pixel][2],
            };
            const auto rgb = d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(adjusted));
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                if (!std::isfinite(rgb[channel]))
                    return make_error(ErrorCode::kValidation,
                                      "Monochrome produced a non-finite sample",
                                      {{"pixel", std::to_string(pixel)},
                                       {"reason", "nonfinite_monochrome_output"}});
                output.rgb[pixel * 3U + channel] = rgb[channel];
            }
        }
    }
    checkpoint(control, MonochromeCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<WorkingImage>{std::move(output)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Monochrome allocation failed",
                      {{"reason", "allocation_failed"}});
}

std::uint64_t detail::monochrome_working_bytes(const std::uint32_t width,
                                               const std::uint32_t height,
                                               const float canonical_scale) noexcept
{
    if (!std::isfinite(canonical_scale) || canonical_scale <= 0.0F)
        return std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    std::uint64_t bytes = saturating_multiply(pixels, 4U * sizeof(float));
    bytes = saturating_add(bytes, saturating_multiply(pixels, 3U * sizeof(float)));
    bytes = saturating_add(bytes, detail::bilateral_filter_working_bytes(
                                      width, height, 20.0F * canonical_scale, 250.0F));
    return bytes;
}

Result<WorkingImage> apply_monochrome(WorkingImage input, const MonochromeParams &params,
                                      const CancellationToken &cancellation)
{
    return detail::apply_monochrome_controlled(std::move(input), params, cancellation);
}

Result<WorkingImage> apply_monochrome(WorkingImage input, const OperationInstance &operation,
                                      const CancellationToken &cancellation)
{
    if (operation.id != kMonochromeOperationId)
        return make_error(ErrorCode::kValidation, "Operation is not Monochrome");
    OperationInstance canonical = operation;
    auto upgraded = upgrade_monochrome_operation(canonical);
    if (!upgraded)
        return upgraded.error();
    auto params = monochrome_from_parameters(canonical.parameters);
    return params ? apply_monochrome(std::move(input), params.value(), cancellation) :
                    params.error();
}

} // namespace ravo
