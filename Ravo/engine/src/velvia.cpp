#include "velvia.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{
void checkpoint(const detail::VelviaControl &control, const detail::VelviaCheckpoint stage,
                const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
        control.checkpoint_callback(control.context, stage, progress);
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0U || input.height == 0U ||
        pixels > std::vector<float>{}.max_size() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels) * 3U)
        return make_error(ErrorCode::kValidation, "Velvia input dimensions are invalid",
                          {{"reason", "invalid_velvia_input"}});
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
        return make_error(ErrorCode::kUnsupported,
                          "Velvia requires declared linear Rec709 working pixels",
                          {{"reason", "unsupported_velvia_working_space"}});
    for (std::size_t index = 0U; index < input.rgb.size(); ++index)
        if (!std::isfinite(input.rgb[index]))
            return make_error(ErrorCode::kValidation, "Velvia input is non-finite",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_velvia_input"}});
    return {};
}
} // namespace

Result<WorkingImage> detail::apply_velvia_controlled(WorkingImage input, const VelviaParams &params,
                                                     const CancellationToken &cancellation,
                                                     const VelviaControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = velvia_to_parameters(params);
    if (!canonical)
        return canonical.error();
    auto valid = validate_input(input);
    if (!valid)
        return valid.error();
    if (params.strength == 0.0)
    {
        checkpoint(control, VelviaCheckpoint::kBeforePublication, 0U);
        active = cancellation.check();
        return active ? Result<WorkingImage>{std::move(input)} : active.error();
    }
    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
    output.rgb.resize(input.rgb.size());
    const float strength = static_cast<float>(params.strength / 100.0);
    const float bias = static_cast<float>(params.bias);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, VelviaCheckpoint::kProcessRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel =
                (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const float red = input.rgb[pixel];
            const float green = input.rgb[pixel + 1U];
            const float blue = input.rgb[pixel + 2U];
            const float maximum = std::max(red, std::max(green, blue));
            const float minimum = std::min(red, std::min(green, blue));
            const float luminance = (maximum + minimum) * 0.5F;
            const float saturation =
                luminance <= 0.5F ?
                    (maximum - minimum) / (1.0e-5F + maximum + minimum) :
                    (maximum - minimum) /
                        (1.0e-5F + std::max(0.0F, 2.0F - maximum - minimum));
            const float weight = std::clamp(
                ((1.0F - 1.5F * saturation) +
                 (1.0F + std::abs(luminance - 0.5F) * 2.0F) * (1.0F - bias)) /
                    (1.0F + (1.0F - bias)),
                0.0F, 1.0F);
            const float amount = strength * weight;
            output.rgb[pixel] =
                std::clamp(red + amount * (red - 0.5F * (green + blue)), 0.0F, 1.0F);
            output.rgb[pixel + 1U] =
                std::clamp(green + amount * (green - 0.5F * (red + blue)), 0.0F, 1.0F);
            output.rgb[pixel + 2U] =
                std::clamp(blue + amount * (blue - 0.5F * (red + green)), 0.0F, 1.0F);
        }
    }
    checkpoint(control, VelviaCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<WorkingImage>{std::move(output)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Velvia allocation failed", {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_velvia(WorkingImage input, const VelviaParams &params,
                                  const CancellationToken &cancellation)
{
    return detail::apply_velvia_controlled(std::move(input), params, cancellation);
}

Result<WorkingImage> apply_velvia(WorkingImage input, const OperationInstance &operation,
                                  const CancellationToken &cancellation)
{
    if (operation.id != kVelviaOperationId)
        return make_error(ErrorCode::kValidation, "Operation is not Velvia");
    OperationInstance canonical = operation;
    auto upgraded = upgrade_velvia_operation(canonical);
    if (!upgraded)
        return upgraded.error();
    auto params = velvia_from_parameters(canonical.parameters);
    return params ? apply_velvia(std::move(input), params.value(), cancellation) : params.error();
}
} // namespace ravo
