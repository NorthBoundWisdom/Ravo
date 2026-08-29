#include "split_toning.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <vector>

#include "hsl.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

void checkpoint(const detail::SplitToningControl &control,
                const detail::SplitToningCheckpoint stage, const std::uint32_t progress) noexcept
{
    if (control.checkpoint_callback != nullptr)
        control.checkpoint_callback(control.context, stage, progress);
}

[[nodiscard]] Result<void> validate_input(const WorkingImage &input)
{
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0U || input.height == 0U || pixels > std::vector<float>{}.max_size() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels) * 3U)
        return make_error(ErrorCode::kValidation, "Split Toning input dimensions are invalid",
                          {{"reason", "invalid_split_toning_input"}});
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
        return make_error(ErrorCode::kUnsupported,
                          "Split Toning requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_split_toning_working_space"}});
    for (std::size_t index = 0U; index < input.rgb.size(); ++index)
    {
        if (!std::isfinite(input.rgb[index]))
            return make_error(ErrorCode::kValidation,
                              "Split Toning input contains a non-finite sample",
                              {{"sample_index", std::to_string(index)},
                               {"reason", "nonfinite_split_toning_input"}});
    }
    return {};
}

} // namespace

Result<WorkingImage> detail::apply_split_toning_controlled(WorkingImage input,
                                                           const SplitToningParams &params,
                                                           const CancellationToken &cancellation,
                                                           const SplitToningControl control)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = split_toning_to_parameters(params);
    if (!canonical)
        return canonical.error();
    auto valid = validate_input(input);
    if (!valid)
        return valid.error();
    if (params.mix == 0.0)
    {
        checkpoint(control, SplitToningCheckpoint::kBeforePublication, 0U);
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
    const float compression = static_cast<float>(params.compress / 110.0 / 2.0);
    const float balance = static_cast<float>(params.balance);
    const float shadow_hue = static_cast<float>(params.shadow_hue);
    const float shadow_saturation = static_cast<float>(params.shadow_saturation);
    const float highlight_hue = static_cast<float>(params.highlight_hue);
    const float highlight_saturation = static_cast<float>(params.highlight_saturation);
    const float mix = static_cast<float>(params.mix);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        checkpoint(control, SplitToningCheckpoint::kProcessRow, row);
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t pixel = (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const float red = input.rgb[pixel];
            const float green = input.rgb[pixel + 1U];
            const float blue = input.rgb[pixel + 2U];
            float hue = 0.0F;
            float saturation = 0.0F;
            float lightness = 0.0F;
            hsl::rgb_to_hsl(red, green, blue, hue, saturation, lightness);
            float toned_red = red;
            float toned_green = green;
            float toned_blue = blue;
            float weight = 0.0F;
            if (lightness < balance - compression)
            {
                hsl::hsl_to_rgb(shadow_hue, shadow_saturation, lightness, toned_red, toned_green,
                                toned_blue);
                weight = std::clamp((balance - compression - lightness) * 2.0F, 0.0F, 1.0F);
            }
            else if (lightness > balance + compression)
            {
                hsl::hsl_to_rgb(highlight_hue, highlight_saturation, lightness, toned_red,
                                toned_green, toned_blue);
                weight = std::clamp((lightness - (balance + compression)) * 2.0F, 0.0F, 1.0F);
            }
            weight *= mix;
            output.rgb[pixel] = std::clamp(red * (1.0F - weight) + toned_red * weight, 0.0F, 1.0F);
            output.rgb[pixel + 1U] =
                std::clamp(green * (1.0F - weight) + toned_green * weight, 0.0F, 1.0F);
            output.rgb[pixel + 2U] =
                std::clamp(blue * (1.0F - weight) + toned_blue * weight, 0.0F, 1.0F);
        }
    }
    checkpoint(control, SplitToningCheckpoint::kBeforePublication, 0U);
    active = cancellation.check();
    return active ? Result<WorkingImage>{std::move(output)} : active.error();
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Split Toning allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_split_toning(WorkingImage input, const SplitToningParams &params,
                                        const CancellationToken &cancellation)
{
    return detail::apply_split_toning_controlled(std::move(input), params, cancellation);
}

Result<WorkingImage> apply_split_toning(WorkingImage input, const OperationInstance &operation,
                                        const CancellationToken &cancellation)
{
    if (operation.id != kSplitToningOperationId)
        return make_error(ErrorCode::kValidation, "Operation is not Split Toning");
    OperationInstance canonical = operation;
    auto upgraded = upgrade_split_toning_operation(canonical);
    if (!upgraded)
        return upgraded.error();
    auto params = split_toning_from_parameters(canonical.parameters);
    return params ? apply_split_toning(std::move(input), params.value(), cancellation) :
                    params.error();
}

} // namespace ravo
