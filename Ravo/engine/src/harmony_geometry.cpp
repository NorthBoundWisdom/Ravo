#include "harmony_geometry.h"

#include "dt_ucs.h"

#include "ravo/recipe/color_harmonizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ravo::harmony_geometry
{
namespace
{

using Triplet = std::array<float, 3>;

[[nodiscard]] bool valid_hue(const float hue) noexcept
{
    return std::isfinite(hue) && hue >= 0.0F && hue <= 1.0F;
}

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

[[nodiscard]] Triplet xyz_d65_to_linear_rec709(const Triplet xyz) noexcept
{
    // Frozen dt_XYZ_to_Rec709_D65() applies its transposed matrix by output
    // channel. Keep the multiply/add stages separate from FMA and dot-product
    // abstractions.
    return {matrix_row(3.2404542F, xyz[0], -1.5371385F, xyz[1], -0.4985314F, xyz[2]),
            matrix_row(-0.9692660F, xyz[0], 1.8760108F, xyz[1], 0.0415560F, xyz[2]),
            matrix_row(0.0556434F, xyz[0], -0.2040259F, xyz[1], 1.0572252F, xyz[2])};
}

[[nodiscard]] Triplet jch_to_srgb(const Triplet jch, const float white_lightness) noexcept
{
    const auto xyy = dt_ucs::jch_to_xyy(jch, white_lightness);
    const auto xyz_d65 = dt_ucs::xyy_to_xyz_d65(xyy);
    const auto linear = xyz_d65_to_linear_rec709(xyz_d65);
    Triplet srgb{};
    for (std::size_t channel = 0U; channel < srgb.size(); ++channel)
    {
        if (linear[channel] <= 0.0031308F)
        {
            srgb[channel] = 12.92F * linear[channel];
        }
        else
        {
            const float curved = std::pow(linear[channel], 1.0F / 2.4F);
            const float scaled = 1.055F * curved;
            srgb[channel] = scaled - 0.055F;
        }
    }
    return srgb;
}

[[nodiscard]] float find_max_chroma(const float hue) noexcept
{
    const float white_lightness = dt_ucs::y_to_lightness(1.0F);
    const float scaled_hue = hue * 6.28318530717958647693F;
    const float angle = scaled_hue - 3.14159265358979323846F;
    constexpr float lightness = 0.65F;
    float lower = 0.0F;
    float upper = 2.0F;
    for (int iteration = 0; iteration < 16; ++iteration)
    {
        const float middle = (lower + upper) * 0.5F;
        const auto srgb = jch_to_srgb({lightness, middle, angle}, white_lightness);
        const bool inside = srgb[0] >= 0.0F && srgb[1] >= 0.0F && srgb[2] >= 0.0F &&
                            srgb[0] <= 1.0F && srgb[1] <= 1.0F && srgb[2] <= 1.0F;
        if (inside)
        {
            lower = middle;
        }
        else
        {
            upper = middle;
        }
    }
    return lower;
}

[[nodiscard]] float clamp01(const float value) noexcept
{
    return value >= 0.0F ? (value <= 1.0F ? value : 1.0F) : 0.0F;
}

[[nodiscard]] float srgb_to_linear(const float srgb) noexcept
{
    if (srgb <= 0.04045F)
    {
        return srgb / 12.92F;
    }
    const float offset_srgb = srgb + 0.055F;
    const float scaled_srgb = offset_srgb / 1.055F;
    return std::pow(scaled_srgb, 2.4F);
}

[[nodiscard]] float rgb_hue_to_ryb(const float hue) noexcept
{
    constexpr std::array<float, 7> input_knots{0.0F,        1.0F / 6.0F, 2.0F / 6.0F, 3.0F / 6.0F,
                                               4.0F / 6.0F, 5.0F / 6.0F, 1.0F};
    constexpr std::array<float, 7> output_knots{0.0F,      1.0F / 3.0F, 0.472217F, 0.611105F,
                                                0.715271F, 5.0F / 6.0F, 1.0F};
    const float wrapped = hue - std::floor(hue);
    std::size_t index = 0U;
    while (index < 5U && wrapped >= input_knots[index + 1U])
    {
        ++index;
    }
    const float numerator = wrapped - input_knots[index];
    const float denominator = input_knots[index + 1U] - input_knots[index];
    const float fraction = numerator / denominator;
    const float output_delta = output_knots[index + 1U] - output_knots[index];
    const float scaled_delta = fraction * output_delta;
    return output_knots[index] + scaled_delta;
}

[[nodiscard]] float ucs_hue_to_ryb(const float hue) noexcept
{
    const float white_lightness = dt_ucs::y_to_lightness(1.0F);
    const float scaled_hue = hue * 6.28318530717958647693F;
    const float angle = scaled_hue - 3.14159265358979323846F;
    const float chroma = find_max_chroma(hue) * 0.85F;
    auto srgb = jch_to_srgb({0.65F, chroma, angle}, white_lightness);
    for (float &channel : srgb)
    {
        channel = clamp01(channel);
        channel = srgb_to_linear(channel);
    }

    const float minimum = std::fmin(std::fmin(srgb[0], srgb[1]), srgb[2]);
    const float maximum = std::fmax(std::fmax(srgb[0], srgb[1]), srgb[2]);
    const float delta = maximum - minimum;
    float rgb_hue = 0.0F;
    if (std::fabs(maximum) > 1.0e-6F && std::fabs(delta) > 1.0e-6F)
    {
        if (srgb[0] == maximum)
        {
            rgb_hue = (srgb[1] - srgb[2]) / delta;
        }
        else if (srgb[1] == maximum)
        {
            rgb_hue = 2.0F + (srgb[2] - srgb[0]) / delta;
        }
        else
        {
            rgb_hue = 4.0F + (srgb[0] - srgb[1]) / delta;
        }
        rgb_hue /= 6.0F;
        rgb_hue -= std::floor(rgb_hue);
    }
    return rgb_hue_to_ryb(rgb_hue);
}

[[nodiscard]] float hue_lerp(float first, float second, const float fraction) noexcept
{
    if (second - first > 0.5F)
    {
        second -= 1.0F;
    }
    else if (first - second > 0.5F)
    {
        first -= 1.0F;
    }
    const float difference = second - first;
    const float scaled_difference = fraction * difference;
    float result = first + scaled_difference;
    if (result < 0.0F)
    {
        result += 1.0F;
    }
    return result;
}

[[nodiscard]] Result<float> lookup_hue(const HueTable &table, const float hue)
{
    if (!valid_hue(hue))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_hue");
    }
    const float position = hue * static_cast<float>(table.size());
    const int integral_position = static_cast<int>(position);
    const std::size_t first = static_cast<std::size_t>(integral_position) % table.size();
    const std::size_t second = (first + 1U) % table.size();
    if (!valid_hue(table[first]) || !valid_hue(table[second]))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_table");
    }
    const float result =
        hue_lerp(table[first], table[second], position - static_cast<float>(integral_position));
    if (!valid_hue(result))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_table");
    }
    return result;
}

} // namespace

HueTable build_ryb_to_ucs_table(const HueTable &ucs_to_ryb) noexcept
{
    HueTable inverse{};
    for (std::size_t target_index = 0U; target_index < inverse.size(); ++target_index)
    {
        const float target = static_cast<float>(target_index) / static_cast<float>(inverse.size());
        float best_distance = 1.0F;
        float best_ucs = 0.0F;
        for (std::size_t index = 0U; index < ucs_to_ryb.size(); ++index)
        {
            float distance = std::fabs(ucs_to_ryb[index] - target);
            if (distance > 0.5F)
            {
                distance = 1.0F - distance;
            }
            if (distance < best_distance)
            {
                best_distance = distance;
                best_ucs = static_cast<float>(index) / static_cast<float>(ucs_to_ryb.size());
            }
        }
        inverse[target_index] = best_ucs;
    }
    return inverse;
}

HarmonyHueTables build_harmony_hue_tables() noexcept
{
    HarmonyHueTables tables{};
    for (std::size_t index = 0U; index < tables.ucs_to_ryb.size(); ++index)
    {
        const float hue = static_cast<float>(index) / static_cast<float>(tables.ucs_to_ryb.size());
        tables.ucs_to_ryb[index] = ucs_hue_to_ryb(hue);
    }
    tables.ryb_to_ucs = build_ryb_to_ucs_table(tables.ucs_to_ryb);
    return tables;
}

Result<float> ucs_to_ryb_hue(const HarmonyHueTables &tables, const float hue)
{
    return lookup_hue(tables.ucs_to_ryb, hue);
}

Result<float> ryb_to_ucs_hue(const HarmonyHueTables &tables, const float hue)
{
    return lookup_hue(tables.ryb_to_ucs, hue);
}

Result<HarmonyNodes> predefined_harmony_nodes(const StandardRule rule, const float anchor_hue,
                                              const HarmonyHueTables &tables)
{
    constexpr std::array<std::array<float, 4>, 9> offsets{
        std::array<float, 4>{0.0F / 12.0F, 0.0F, 0.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 0.0F / 12.0F, 1.0F / 12.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 0.0F / 12.0F, 1.0F / 12.0F, 6.0F / 12.0F},
        std::array<float, 4>{0.0F / 12.0F, 6.0F / 12.0F, 0.0F, 0.0F},
        std::array<float, 4>{0.0F / 12.0F, 5.0F / 12.0F, 7.0F / 12.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 1.0F / 12.0F, 0.0F, 0.0F},
        std::array<float, 4>{0.0F / 12.0F, 4.0F / 12.0F, 8.0F / 12.0F, 0.0F},
        std::array<float, 4>{-1.0F / 12.0F, 1.0F / 12.0F, 5.0F / 12.0F, 7.0F / 12.0F},
        std::array<float, 4>{0.0F / 12.0F, 3.0F / 12.0F, 6.0F / 12.0F, 9.0F / 12.0F},
    };
    static_assert(kColorHarmonizerPredefinedNodeCounts.size() == offsets.size());

    if (rule < StandardRule::kMonochromatic || rule > StandardRule::kSquare)
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_rule");
    }
    if (!valid_hue(anchor_hue))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_hue");
    }
    const auto rule_index = static_cast<std::size_t>(rule);
    const auto mapped_anchor = ucs_to_ryb_hue(tables, anchor_hue);
    if (!mapped_anchor)
    {
        return mapped_anchor.error();
    }
    const float rotation_value = std::round(mapped_anchor.value() * 360.0F);
    if (!std::isfinite(rotation_value))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_table");
    }
    const int rotation = static_cast<int>(rotation_value) % 360;
    const float sector_anchor = static_cast<float>(rotation) / 360.0F;
    HarmonyNodes nodes;
    nodes.count = static_cast<std::size_t>(kColorHarmonizerPredefinedNodeCounts[rule_index]);
    for (std::size_t index = 0U; index < nodes.count; ++index)
    {
        float angle = offsets[rule_index][index] + sector_anchor;
        angle -= std::floor(angle);
        const auto node = ryb_to_ucs_hue(tables, angle);
        if (!node)
        {
            return node.error();
        }
        nodes.hues[index] = node.value();
    }
    return nodes;
}

Result<HarmonyAttraction> harmony_attraction(const float pixel_hue,
                                             const std::span<const float> nodes,
                                             const float pull_width)
{
    if (!valid_hue(pixel_hue))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_hue");
    }
    if (nodes.empty() || nodes.size() > kMaxHarmonyNodes ||
        !std::ranges::all_of(nodes, [](const float node) { return valid_hue(node); }))
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_nodes");
    }
    if (!std::isfinite(pull_width) || pull_width < 0.25F || pull_width > 4.0F)
    {
        return make_error(ErrorCode::kValidation, "invalid_harmony_pull_width");
    }

    const float scaled_width = pull_width * 0.5F;
    const float sigma = scaled_width / static_cast<float>(nodes.size());
    const float twice_sigma = 2.0F * sigma;
    const float twice_sigma_squared = twice_sigma * sigma;
    const float inverse_two_sigma_squared = 1.0F / twice_sigma_squared;
    HarmonyAttraction result;
    float winning_difference = 0.0F;
    for (std::size_t index = 0U; index < nodes.size(); ++index)
    {
        float distance = std::fabs(pixel_hue - nodes[index]);
        if (distance > 0.5F)
        {
            distance = 1.0F - distance;
        }
        const float negative_distance = -distance;
        const float squared_distance = negative_distance * distance;
        const float exponent = squared_distance * inverse_two_sigma_squared;
        const float weight = std::exp(exponent);
        float difference = nodes[index] - pixel_hue;
        if (difference > 0.5F)
        {
            difference -= 1.0F;
        }
        else if (difference < -0.5F)
        {
            difference += 1.0F;
        }
        if (weight > result.weight)
        {
            result.weight = weight;
            result.winning_index = index;
            winning_difference = difference;
        }
    }
    result.shift = winning_difference * result.weight;
    return result;
}

} // namespace ravo::harmony_geometry
