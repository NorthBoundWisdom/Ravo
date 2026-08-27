#include "color_harmonizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string>

#include "dt_ucs.h"
#include "harmony_geometry.h"

namespace ravo
{
namespace
{

using Triplet = std::array<float, 3>;
using Matrix3 = std::array<float, 9>;

struct FrozenColorHarmonizerData
{
    std::array<float, harmony_geometry::kMaxHarmonyNodes> nodes{};
    std::array<float, harmony_geometry::kMaxHarmonyNodes> node_saturation{};
    std::size_t node_count = 0U;
    float pull_strength = 0.0F;
    float neutral_protection = 0.5F;
    float pull_width = 1.0F;
};

[[nodiscard]] float matrix_row(const float coefficient0, const float value0,
                               const float coefficient1, const float value1,
                               const float coefficient2, const float value2) noexcept
{
    const float product0 = coefficient0 * value0;
    const float product1 = coefficient1 * value1;
    const float first_sum = product0 + product1;
    const float product2 = coefficient2 * value2;
    return first_sum + product2;
}

[[nodiscard]] Triplet apply_matrix(const Matrix3 &matrix, const Triplet value) noexcept
{
    return {matrix_row(matrix[0], value[0], matrix[1], value[1], matrix[2], value[2]),
            matrix_row(matrix[3], value[0], matrix[4], value[1], matrix[5], value[2]),
            matrix_row(matrix[6], value[0], matrix[7], value[1], matrix[8], value[2])};
}

[[nodiscard]] Result<Matrix3> invert_matrix(const Matrix3 &matrix)
{
    if (!std::ranges::all_of(matrix, [](const float value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Harmonizer working matrix contains a non-finite value",
                          {{"reason", "invalid_colorharmonizer_profile_matrix"}});
    }
    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double g = matrix[6];
    const double h = matrix[7];
    const double i = matrix[8];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12)
    {
        return make_error(ErrorCode::kValidation, "Color Harmonizer working matrix is singular",
                          {{"reason", "invalid_colorharmonizer_profile_matrix"}});
    }
    const double inverse = 1.0 / determinant;
    const Matrix3 result{
        static_cast<float>((e * i - f * h) * inverse),
        static_cast<float>((c * h - b * i) * inverse),
        static_cast<float>((b * f - c * e) * inverse),
        static_cast<float>((f * g - d * i) * inverse),
        static_cast<float>((a * i - c * g) * inverse),
        static_cast<float>((c * d - a * f) * inverse),
        static_cast<float>((d * h - e * g) * inverse),
        static_cast<float>((b * g - a * h) * inverse),
        static_cast<float>((a * e - b * d) * inverse),
    };
    if (!std::ranges::all_of(result, [](const float value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Harmonizer inverse working matrix is non-finite",
                          {{"reason", "invalid_colorharmonizer_profile_matrix"}});
    }
    return result;
}

[[nodiscard]] float wrap_hue(float hue) noexcept
{
    hue = std::fmod(hue, 1.0F);
    if (hue < 0.0F)
    {
        hue += 1.0F;
    }
    return hue;
}

[[nodiscard]] Result<FrozenColorHarmonizerData>
commit_color_harmonizer(const ColorHarmonizerParams &params)
{
    auto canonical = color_harmonizer_to_parameters(params);
    if (!canonical)
    {
        return canonical.error();
    }
    if (params.smoothing > 0.0)
    {
        return make_error(
            ErrorCode::kUnsupported,
            "Color Harmonizer smoothing requires the recursive Gaussian and canonical ROI scale",
            {{"reason", "unsupported_smoothing_requires_recursive_gaussian"}});
    }

    FrozenColorHarmonizerData data;
    data.pull_strength = static_cast<float>(params.pull_strength);
    data.neutral_protection = static_cast<float>(params.neutral_protection);
    data.pull_width = static_cast<float>(params.pull_width);
    for (std::size_t index = 0U; index < data.node_saturation.size(); ++index)
    {
        data.node_saturation[index] = static_cast<float>(params.node_saturation[index]);
    }

    if (params.rule == ColorHarmonizerRule::kCustom)
    {
        data.node_count = static_cast<std::size_t>(params.num_custom_nodes);
        for (std::size_t index = 0U; index < data.node_count; ++index)
        {
            data.nodes[index] = static_cast<float>(params.custom_hue[index]);
        }
        return data;
    }

    const auto tables = harmony_geometry::build_harmony_hue_tables();
    const auto nodes = harmony_geometry::predefined_harmony_nodes(
        static_cast<harmony_geometry::StandardRule>(params.rule),
        static_cast<float>(params.anchor_hue), tables);
    if (!nodes)
    {
        return make_error(ErrorCode::kValidation, "Color Harmonizer harmony geometry is invalid",
                          {{"reason", "invalid_colorharmonizer_geometry"}});
    }
    data.nodes = nodes.value().hues;
    data.node_count = nodes.value().count;
    return data;
}

[[nodiscard]] Result<Triplet> apply_color_harmonizer_committed(
    const FrozenColorHarmonizerData &data, const Matrix3 &working_to_xyz_d50,
    const Matrix3 &xyz_d50_to_working, const Triplet input, const float white_lightness)
{
    const Triplet nonnegative{std::fmax(input[0], 0.0F), std::fmax(input[1], 0.0F),
                              std::fmax(input[2], 0.0F)};
    auto jch =
        dt_ucs::xyz_d50_to_jch(apply_matrix(working_to_xyz_d50, nonnegative), white_lightness);
    constexpr float pi = 3.14159265358979323846F;
    constexpr float two_pi = 6.28318530717958647693F;
    const float hue = (jch[2] + pi) / two_pi;
    const float chroma = jch[1];
    const auto attraction = harmony_geometry::harmony_attraction(
        hue, std::span<const float>(data.nodes.data(), data.node_count), data.pull_width);
    if (!attraction)
    {
        return make_error(ErrorCode::kValidation, "Color Harmonizer attraction geometry is invalid",
                          {{"reason", "invalid_colorharmonizer_geometry"}});
    }
    const float saturation_delta =
        (data.node_saturation[attraction.value().winning_index] - 1.0F) * attraction.value().weight;
    const float neutral_squared = data.neutral_protection * data.neutral_protection;
    const float neutral_cubed = neutral_squared * data.neutral_protection;
    const float cutoff = neutral_cubed * 0.03F;
    const float chroma_denominator = (chroma + cutoff) + 1.0e-5F;
    const float chroma_weight = chroma / chroma_denominator;
    const float strength_shift = attraction.value().shift * data.pull_strength;
    const float weighted_shift = strength_shift * chroma_weight;
    const float corrected_hue = wrap_hue(hue + weighted_shift);
    jch[2] = corrected_hue * two_pi - pi;
    const float weighted_saturation = saturation_delta * chroma_weight;
    const float saturation_scale = 1.0F + weighted_saturation;
    jch[1] = std::fmax(chroma * saturation_scale, 0.0F);
    const auto result =
        apply_matrix(xyz_d50_to_working, dt_ucs::jch_to_xyz_d50(jch, white_lightness));
    if (!std::ranges::all_of(result, [](const float value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Harmonizer produced a non-finite RGB sample",
                          {{"reason", "nonfinite_colorharmonizer_output"}});
    }
    return result;
}

} // namespace

Result<WorkingImage> apply_color_harmonizer(const WorkingImage &input,
                                            const ColorHarmonizerParams &params,
                                            const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto data = commit_color_harmonizer(params);
    if (!data)
    {
        return data.error();
    }
    if (input.width == 0U || input.height == 0U)
    {
        return make_error(ErrorCode::kValidation,
                          "Color Harmonizer input dimensions must be non-zero",
                          {{"reason", "invalid_colorharmonizer_dimensions"}});
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U))
    {
        return make_error(ErrorCode::kValidation,
                          "Color Harmonizer input buffer does not match its dimensions",
                          {{"reason", "invalid_colorharmonizer_buffer"}});
    }
    if (input.color_profile.kind == ColorProfileKind::kMissing ||
        input.color_profile.model != ColorModel::kRgb || !input.color_profile.has_matrix ||
        input.color_profile.identifier.empty())
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Harmonizer requires a declared matrix RGB working profile",
                          {{"profile", input.color_profile.identifier},
                           {"reason", "unsupported_colorharmonizer_working_space"}});
    }
    auto inverse = invert_matrix(input.color_profile.matrix_to_xyz_d50);
    if (!inverse)
    {
        return inverse.error();
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
                                  "Color Harmonizer input contains a non-finite RGB sample",
                                  {{"sample_index", std::to_string(index)},
                                   {"reason", "nonfinite_colorharmonizer_input"}});
            }
        }
    }

    WorkingImage output;
    output.width = input.width;
    output.height = input.height;
    output.color_profile = input.color_profile;
    output.exposure_analysis = input.exposure_analysis;
    output.rgb.resize(input.rgb.size());
    const float white_lightness = dt_ucs::y_to_lightness(1.0F);
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
            auto result = apply_color_harmonizer_committed(
                data.value(), input.color_profile.matrix_to_xyz_d50, inverse.value(),
                {input.rgb[index], input.rgb[index + 1U], input.rgb[index + 2U]}, white_lightness);
            if (!result)
            {
                auto error = result.error();
                error.context.emplace("sample_index", std::to_string(index));
                return error;
            }
            output.rgb[index] = result.value()[0];
            output.rgb[index + 1U] = result.value()[1];
            output.rgb[index + 2U] = result.value()[2];
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
    return make_error(ErrorCode::kIo, "Color Harmonizer output allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<WorkingImage> apply_color_harmonizer(const WorkingImage &input,
                                            const OperationInstance &operation,
                                            const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (operation.id != kColorHarmonizerOperationId)
    {
        return make_error(ErrorCode::kValidation, "Operation is not Color Harmonizer",
                          {{"operation_id", operation.id}});
    }
    if (operation.schema_version != kColorHarmonizerOperationSchemaVersion)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Color Harmonizer operation schema version is unsupported",
                          {{"operation_id", operation.id},
                           {"schema_version", std::to_string(operation.schema_version)}});
    }
    if (operation.mask_id.has_value())
    {
        return make_error(
            ErrorCode::kUnsupported, "Color Harmonizer mask evaluation is unavailable",
            {{"operation_id", operation.id}, {"reason", "colorharmonizer_mask_graph_unavailable"}});
    }
    if (!operation.enabled)
    {
        return input;
    }
    auto params = color_harmonizer_from_parameters(operation.parameters);
    if (!params)
    {
        return params.error();
    }
    return apply_color_harmonizer(input, params.value(), cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Color Harmonizer operation allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
