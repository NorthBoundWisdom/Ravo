#include "sharpen.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <vector>

#include "d50_lab.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

constexpr int kMaximumRadius = 12;

struct FrozenSharpenData
{
    float radius = 5.0F;
    float amount = 0.5F;
    float threshold = 0.5F;
};

[[nodiscard]] FrozenSharpenData commit_sharpen(const SharpenParams &params) noexcept
{
    return {2.5F * static_cast<float>(params.radius), static_cast<float>(params.amount),
            static_cast<float>(params.threshold)};
}

void checkpoint(const detail::SharpenControl &control, const detail::SharpenCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
    {
        control.checkpoint_callback(control.context, stage, progress);
    }
}

[[nodiscard]] Result<void> validate_working_input(const WorkingImage &input)
{
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation, "Sharpen input dimensions must be non-zero",
                          {{"reason", "invalid_sharpen_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Sharpen input buffer does not match its dimensions",
                          {{"reason", "invalid_sharpen_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Sharpen requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_sharpen_working_space"}});
    }
    return {};
}

[[nodiscard]] std::uint64_t saturating_multiply(const std::uint64_t left,
                                                const std::uint64_t right) noexcept
{
    return left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left ?
               std::numeric_limits<std::uint64_t>::max() :
               left * right;
}

[[nodiscard]] std::uint64_t saturating_add(const std::uint64_t left,
                                           const std::uint64_t right) noexcept
{
    return right > std::numeric_limits<std::uint64_t>::max() - left ?
               std::numeric_limits<std::uint64_t>::max() :
               left + right;
}

} // namespace

std::uint64_t detail::sharpen_working_bytes(const std::uint32_t width, const std::uint32_t height,
                                            const SharpenParams &params) noexcept
{
    const auto bounded = [](const double value, const double minimum, const double maximum) noexcept
    {
        return std::isfinite(value) && std::isfinite(static_cast<float>(value)) &&
               value >= minimum && value <= maximum;
    };
    if (width == 0U || height == 0U ||
        !bounded(params.radius, kSharpenRadiusMin, kSharpenRadiusMax) ||
        !bounded(params.amount, kSharpenAmountMin, kSharpenAmountMax) ||
        !bounded(params.threshold, kSharpenThresholdMin, kSharpenThresholdMax))
    {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    std::uint64_t bytes = saturating_multiply(pixels, sizeof(SharpenLabPixel));
    bytes = saturating_add(bytes, saturating_multiply(width, sizeof(float)));
    bytes = saturating_add(bytes, (2U * kMaximumRadius + 1U) * sizeof(float));
    return bytes;
}

Result<std::vector<detail::SharpenLabPixel>>
detail::apply_sharpen_lab(const std::span<const SharpenLabPixel> input, const std::uint32_t width,
                          const std::uint32_t height, const float canonical_scale,
                          const SharpenParams &params, const CancellationToken &cancellation,
                          const SharpenControl control)
try
{
    checkpoint(control, SharpenCheckpoint::kBeforeValidation, 0U);
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto canonical = sharpen_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    if (width == 0U || height == 0U || static_cast<std::uint64_t>(width) * height != input.size())
    {
        return make_error(ErrorCode::kValidation, "Sharpen Lab input does not match its dimensions",
                          {{"reason", "invalid_sharpen_lab_buffer"}});
    }
    const FrozenSharpenData data = commit_sharpen(params);
    if (data.radius > 0.0F && (!std::isfinite(canonical_scale) || canonical_scale <= 0.0F))
    {
        return make_error(ErrorCode::kValidation,
                          "Sharpen requires a canonical ROI scale for a positive radius",
                          {{"reason", "invalid_sharpen_roi_scale"}});
    }
    for (std::size_t pixel = 0U; pixel < input.size(); ++pixel)
    {
        for (std::size_t channel = 0U; channel < input[pixel].size(); ++channel)
        {
            if (!std::isfinite(input[pixel][channel]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Sharpen input contains a non-finite Lab sample",
                                  {{"pixel", std::to_string(pixel)},
                                   {"channel", std::to_string(channel)},
                                   {"reason", "nonfinite_sharpen_lab_input"}});
            }
        }
    }
    std::vector<SharpenLabPixel> output(input.begin(), input.end());
    if (data.radius == 0.0F)
    {
        checkpoint(control, SharpenCheckpoint::kBeforePublication, 0U);
        active = cancellation.check();
        return active ? Result<std::vector<SharpenLabPixel>>(std::move(output)) : active.error();
    }

    const float scaled_radius = data.radius * canonical_scale;
    const int radius = std::min(kMaximumRadius, static_cast<int>(std::ceil(scaled_radius)));
    if (radius == 0 || width < static_cast<std::uint32_t>(2 * radius + 1) ||
        height < static_cast<std::uint32_t>(2 * radius + 1))
    {
        checkpoint(control, SharpenCheckpoint::kBeforePublication, 0U);
        active = cancellation.check();
        return active ? Result<std::vector<SharpenLabPixel>>(std::move(output)) : active.error();
    }

    const int kernel_width = 2 * radius + 1;
    const float sigma2 = (1.0F / (2.5F * 2.5F)) * scaled_radius * scaled_radius;
    if (!std::isfinite(sigma2) || sigma2 <= 0.0F)
    {
        return make_error(ErrorCode::kValidation, "Sharpen Gaussian sigma is invalid",
                          {{"reason", "invalid_sharpen_sigma"}});
    }
    std::vector<float> kernel(static_cast<std::size_t>(kernel_width));
    float weight = 0.0F;
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const float value = std::exp(-static_cast<float>(offset * offset) / (2.0F * sigma2));
        kernel[static_cast<std::size_t>(offset + radius)] = value;
        weight += value;
    }
    if (!std::isfinite(weight) || weight <= 0.0F)
    {
        return make_error(ErrorCode::kValidation, "Sharpen Gaussian weight is invalid",
                          {{"reason", "invalid_sharpen_kernel"}});
    }
    for (float &value : kernel)
    {
        value /= weight;
    }

    std::vector<float> vertical(width);
    for (std::uint32_t row = static_cast<std::uint32_t>(radius);
         row < height - static_cast<std::uint32_t>(radius); ++row)
    {
        checkpoint(control, SharpenCheckpoint::kVerticalRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::uint32_t start_row = row - static_cast<std::uint32_t>(radius);
        const std::uint32_t end_row = row + static_cast<std::uint32_t>(radius);
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            float sum = 0.0F;
            for (std::uint32_t source_row = start_row; source_row <= end_row; ++source_row)
            {
                const std::size_t kernel_index = source_row - start_row;
                sum += kernel[kernel_index] *
                       input[static_cast<std::size_t>(source_row) * width + column][0];
            }
            vertical[column] = sum;
        }

        checkpoint(control, SharpenCheckpoint::kHorizontalRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = static_cast<std::uint32_t>(radius);
             column < width - static_cast<std::uint32_t>(radius); ++column)
        {
            float sum = 0.0F;
            const std::uint32_t start_column = column - static_cast<std::uint32_t>(radius);
            const std::uint32_t end_column = column + static_cast<std::uint32_t>(radius);
            for (std::uint32_t source_column = start_column; source_column <= end_column;
                 ++source_column)
            {
                const std::size_t kernel_index = source_column - start_column;
                sum += kernel[kernel_index] * vertical[source_column];
            }
            const std::size_t index = static_cast<std::size_t>(row) * width + column;
            const float difference = input[index][0] - sum;
            const float absolute = std::fabs(difference);
            const float detail =
                absolute > data.threshold ?
                    std::copysign(std::fmax(absolute - data.threshold, 0.0F), difference) :
                    0.0F;
            output[index][0] = input[index][0] + detail * data.amount;
            if (!std::isfinite(output[index][0]))
            {
                return make_error(
                    ErrorCode::kValidation, "Sharpen produced a non-finite Lab sample",
                    {{"pixel", std::to_string(index)}, {"reason", "nonfinite_sharpen_lab_output"}});
            }
        }
    }
    checkpoint(control, SharpenCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Sharpen Lab allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> detail::apply_sharpen_controlled(const WorkingImage &input,
                                                      const SharpenParams &params,
                                                      const CancellationToken &cancellation,
                                                      const SharpenControl control)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto valid = validate_working_input(input);
    if (!valid)
    {
        return valid.error();
    }
    std::vector<SharpenLabPixel> lab(static_cast<std::size_t>(input.width) * input.height);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, SharpenCheckpoint::kConvertInputRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * input.width + column;
            const std::size_t rgb = index * 3U;
            if (!std::isfinite(input.rgb[rgb]) || !std::isfinite(input.rgb[rgb + 1U]) ||
                !std::isfinite(input.rgb[rgb + 2U]))
            {
                return make_error(
                    ErrorCode::kValidation, "Sharpen input contains a non-finite RGB sample",
                    {{"sample_index", std::to_string(rgb)}, {"reason", "nonfinite_sharpen_input"}});
            }
            lab[index] = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(
                {input.rgb[rgb], input.rgb[rgb + 1U], input.rgb[rgb + 2U]}));
        }
    }
    auto sharpened =
        apply_sharpen_lab(lab, input.width, input.height, input.canonical_roi_scale.value(), params,
                          cancellation, control);
    if (!sharpened)
    {
        return sharpened.error();
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, SharpenCheckpoint::kConvertOutputRow, row);
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t index = static_cast<std::size_t>(row) * input.width + column;
            const auto rgb =
                d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(sharpened.value()[index]));
            for (std::size_t channel = 0U; channel < rgb.size(); ++channel)
            {
                if (!std::isfinite(rgb[channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Sharpen produced a non-finite RGB sample",
                                      {{"sample_index", std::to_string(index * 3U + channel)},
                                       {"reason", "nonfinite_sharpen_output"}});
                }
                output.rgb[index * 3U + channel] = rgb[channel];
            }
        }
    }
    checkpoint(control, SharpenCheckpoint::kBeforePublication, 1U);
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Sharpen output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_sharpen(const WorkingImage &input, const SharpenParams &params,
                                   const CancellationToken &cancellation)
{
    return detail::apply_sharpen_controlled(input, params, cancellation, {});
}

Result<WorkingImage> apply_sharpen(const WorkingImage &input, const OperationInstance &operation,
                                   const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kSharpenOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Sharpen",
                          {{"operation_id", operation.id}});
    }
    OperationInstance canonical = operation;
    auto upgraded = upgrade_sharpen_operation(canonical);
    if (!upgraded)
    {
        return upgraded.error();
    }
    if (canonical.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Sharpen mask evaluation is unavailable",
            {{"operation_id", canonical.id}, {"reason", "sharpen_mask_graph_unavailable"}});
    }
    if (!canonical.enabled)
    {
        return input;
    }
    auto params = sharpen_from_parameters(canonical.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_sharpen(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Sharpen operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
