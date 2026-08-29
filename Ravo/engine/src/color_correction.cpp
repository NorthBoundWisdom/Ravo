#include "color_correction.h"

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

struct FrozenColorCorrectionData
{
    float a_scale = 0.0F;
    float a_base = 0.0F;
    float b_scale = 0.0F;
    float b_base = 0.0F;
    float saturation = 1.0F;
};

[[nodiscard]] FrozenColorCorrectionData
commit_color_correction(const ColorCorrectionParams &params) noexcept
{
    const float highlight_a = static_cast<float>(params.highlight_a);
    const float highlight_b = static_cast<float>(params.highlight_b);
    const float shadow_a = static_cast<float>(params.shadow_a);
    const float shadow_b = static_cast<float>(params.shadow_b);
    return {(highlight_a - shadow_a) / 100.0F, shadow_a, (highlight_b - shadow_b) / 100.0F,
            shadow_b, static_cast<float>(params.saturation)};
}

[[nodiscard]] Result<std::array<float, 3>>
apply_color_correction_lab_committed(const FrozenColorCorrectionData &data,
                                     const std::array<float, 3> &lab)
{
    for (std::size_t channel = 0U; channel < lab.size(); ++channel)
    {
        if (!std::isfinite(lab[channel]))
        {
            return make_error(ErrorCode::kValidation,
                              "Color Correction input contains a non-finite Lab sample",
                              {{"channel", std::to_string(channel)},
                               {"reason", "nonfinite_colorcorrection_lab_input"}});
        }
    }
    const std::array<float, 3> result{
        lab[0], data.saturation * (lab[1] + lab[0] * data.a_scale + data.a_base),
        data.saturation * (lab[2] + lab[0] * data.b_scale + data.b_base)};
    for (std::size_t channel = 0U; channel < result.size(); ++channel)
    {
        if (!std::isfinite(result[channel]))
        {
            return make_error(ErrorCode::kValidation,
                              "Color Correction produced a non-finite Lab sample",
                              {{"channel", std::to_string(channel)},
                               {"reason", "nonfinite_colorcorrection_lab_output"}});
        }
    }
    return result;
}

[[nodiscard]] Result<FrozenColorCorrectionData>
validated_color_correction_data(const ColorCorrectionParams &params)
{
    auto canonical = color_correction_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    return commit_color_correction(params);
}

} // namespace

Result<std::array<float, 3>> apply_color_correction_lab(const ColorCorrectionParams &params,
                                                        const std::array<float, 3> &lab,
                                                        const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto data = validated_color_correction_data(params);
    if (!data)
    {
        return data.error();
    }
    return apply_color_correction_lab_committed(data.value(), lab);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Correction operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_correction(const WorkingImage &input,
                                            const ColorCorrectionParams &params,
                                            const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto data = validated_color_correction_data(params);
    if (!data)
    {
        return data.error();
    }
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Correction input dimensions must be non-zero",
                          {{"reason", "invalid_colorcorrection_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Correction input buffer does not match its dimensions",
                          {{"reason", "invalid_colorcorrection_buffer"}});
    }
    if (input.color_profile.model != ColorModel::kRgb ||
        input.color_profile.identifier != kInputProfileLinearRec709)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Correction requires declared linear Rec709 working pixels",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_colorcorrection_working_space"}});
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
                                  "Color Correction input contains a non-finite RGB sample",
                                  {{"sample_index", std::to_string(index)},
                                   {"reason", "nonfinite_colorcorrection_input"}});
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
            auto corrected = apply_color_correction_lab_committed(data.value(), lab);
            if (!corrected)
            {
                return corrected.error();
            }
            const auto corrected_rgb =
                d50_lab::xyz_to_linear_rec709(d50_lab::lab_to_xyz(corrected.value()));
            for (std::size_t channel = 0U; channel < corrected_rgb.size(); ++channel)
            {
                if (!std::isfinite(corrected_rgb[channel]))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Color Correction produced a non-finite RGB sample",
                                      {{"sample_index", std::to_string(index + channel)},
                                       {"reason", "nonfinite_colorcorrection_output"}});
                }
                output.rgb[index + channel] = corrected_rgb[channel];
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
    return make_error(ErrorCode::kIo, "Color Correction output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_correction(const WorkingImage &input,
                                            const OperationInstance &operation,
                                            const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kColorCorrectionOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Color Correction",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kColorCorrectionOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Correction operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Color Correction mask evaluation is unavailable",
            {{"operation_id", operation.id}, {"reason", "colorcorrection_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = color_correction_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_color_correction(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Correction operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
