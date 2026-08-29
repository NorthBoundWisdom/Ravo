#include "color_contrast.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

#include "d50_lab.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{

struct FrozenColorContrastData
{
    float a_steepness = 1.0F;
    float a_offset = 0.0F;
    float b_steepness = 1.0F;
    float b_offset = 0.0F;
    bool unbound = true;
};

[[nodiscard]] FrozenColorContrastData
commit_color_contrast(const ColorContrastParams &params) noexcept
{
    return {static_cast<float>(params.a_steepness), static_cast<float>(params.a_offset),
            static_cast<float>(params.b_steepness), static_cast<float>(params.b_offset),
            params.unbound};
}

[[nodiscard]] Result<std::array<float, 3>>
apply_color_contrast_lab_committed(const FrozenColorContrastData &data,
                                   const std::array<float, 3> &lab)
{
    for (std::size_t channel = 0U; channel < lab.size(); ++channel)
    {
        if (!std::isfinite(lab[channel]))
        {
            return make_error(ErrorCode::kValidation,
                              "Color Contrast input contains a non-finite Lab sample",
                              {{"channel", std::to_string(channel)},
                               {"reason", "nonfinite_colorcontrast_lab_input"}});
        }
    }
    float a = lab[1] * data.a_steepness + data.a_offset;
    float b = lab[2] * data.b_steepness + data.b_offset;
    if (!data.unbound)
    {
        a = a > -128.0F ? (a < 128.0F ? a : 128.0F) : -128.0F;
        b = b > -128.0F ? (b < 128.0F ? b : 128.0F) : -128.0F;
    }
    const std::array<float, 3> result{lab[0], a, b};
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        if (!std::isfinite(result[channel]))
        {
            return make_error(ErrorCode::kValidation,
                              "Color Contrast produced a non-finite Lab sample",
                              {{"channel", std::to_string(channel)},
                               {"reason", "nonfinite_colorcontrast_lab_output"}});
        }
    }
    return result;
}

[[nodiscard]] Result<FrozenColorContrastData>
validated_color_contrast_data(const ColorContrastParams &params)
{
    auto canonical = color_contrast_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    return commit_color_contrast(params);
}

} // namespace

Result<std::array<float, 3>> apply_color_contrast_lab(const ColorContrastParams &params,
                                                      const std::array<float, 3> &lab,
                                                      const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto data = validated_color_contrast_data(params);
    if (!data)
    {
        return data.error();
    }
    return apply_color_contrast_lab_committed(data.value(), lab);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Contrast operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_contrast(const WorkingImage &input,
                                          const ColorContrastParams &params,
                                          const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto data = validated_color_contrast_data(params);
    if (!data)
    {
        return data.error();
    }
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Contrast input dimensions must be non-zero",
                          {{"reason", "invalid_colorcontrast_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Contrast input buffer does not match its dimensions",
                          {{"reason", "invalid_colorcontrast_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Contrast requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_colorcontrast_working_space"}});
    }
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const std::size_t begin = static_cast<std::size_t>(row) * input.width * 3U;
        const std::size_t end = begin + static_cast<std::size_t>(input.width) * 3U;
        for (std::size_t index = begin; index < end; ++index)
        {
            if (!std::isfinite(input.rgb[index]))
            {
                return make_error(ErrorCode::kValidation,
                                  "Color Contrast input contains a non-finite RGB sample",
                                  {{"sample_index", std::to_string(index)},
                                   {"reason", "nonfinite_colorcontrast_input"}});
            }
        }
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
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t index = (static_cast<std::size_t>(row) * input.width + column) * 3U;
            const std::array<float, 3> rgb{input.rgb[index], input.rgb[index + 1U],
                                           input.rgb[index + 2U]};
            const auto lab = d50_lab::xyz_to_lab(d50_lab::linear_rec709_to_xyz(rgb));
            auto contrasted = apply_color_contrast_lab_committed(data.value(), lab);
            if (!contrasted)
            {
                return contrasted.error();
            }
            const auto contrasted_rgb =
                d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(contrasted.value()));
            for (std::size_t channel = 0U; channel < contrasted_rgb.size(); ++channel)
            {
                if (!std::isfinite(contrasted_rgb[channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color Contrast produced a non-finite RGB sample",
                                      {{"sample_index", std::to_string(index + channel)},
                                       {"reason", "nonfinite_colorcontrast_output"}});
                }
                output.rgb[index + channel] = contrasted_rgb[channel];
            }
        }
    }
    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Contrast output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_contrast(const WorkingImage &input,
                                          const OperationInstance &operation,
                                          const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kColorContrastOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Color Contrast",
                          {{"operation_id", operation.id}});
    }
    OperationInstance canonical = operation;
    auto upgraded = upgrade_color_contrast_operation(canonical);
    if (!upgraded)
    {
        return upgraded.error();
    }
    if (canonical.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Color Contrast mask evaluation is unavailable",
            {{"operation_id", canonical.id}, {"reason", "colorcontrast_mask_graph_unavailable"}});
    }
    if (!canonical.enabled)
    {
        return input;
    }
    auto params = color_contrast_from_parameters(canonical.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_color_contrast(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Contrast operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
