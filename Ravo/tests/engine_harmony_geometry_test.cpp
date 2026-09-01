#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numeric>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include <QBuffer>
#include <QColor>
#include <QFile>
#include <QImage>
#include <zlib.h>

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "color_balance_fixture.h"
#include "capability_ops.h"
#include "capture_metadata_test_support.h"
#include "color_balance_rgb.h"
#include "color_checker.h"
#include "color_harmonizer.h"
#include "color_contrast.h"
#include "d50_lab.h"
#include "dt_ucs.h"
#include "harmony_geometry.h"
#include "image_ops.h"
#include "input_color.h"
#include "primaries.h"
#include "raw_pipeline.h"
#include "raw_temperature.h"
#include "recursive_gaussian.h"
#include "temperature_fixture.h"
#include "test_support.h"

namespace ravo
{
namespace
{
using FrozenD50Triplet = std::array<float, 3>;
inline constexpr float kPlatformLibmReferenceTolerance = 1.0e-5F;

[[nodiscard]] std::array<std::uint32_t, 3>
d50_triplet_bits(const FrozenD50Triplet &triplet) noexcept
{
    return {std::bit_cast<std::uint32_t>(triplet[0]), std::bit_cast<std::uint32_t>(triplet[1]),
            std::bit_cast<std::uint32_t>(triplet[2])};
}

void expect_frozen_d50_bits(const FrozenD50Triplet &actual, const FrozenD50Triplet &oracle,
                            const std::array<std::uint32_t, 3> &golden)
{
    EXPECT_EQ(d50_triplet_bits(oracle), golden);
    EXPECT_EQ(d50_triplet_bits(actual), golden);
}

void expect_frozen_d50_cbrt_reference(const FrozenD50Triplet &actual,
                                      const FrozenD50Triplet &oracle,
                                      const std::array<std::uint32_t, 3> &reference)
{
    EXPECT_EQ(d50_triplet_bits(actual), d50_triplet_bits(oracle));
    for (std::size_t channel = 0U; channel < reference.size(); ++channel)
    {
        // cbrtf is platform libm code. Preserve exact host-local source-order
        // agreement while retaining a tight, recorded cross-platform envelope.
        EXPECT_NEAR(actual[channel], std::bit_cast<float>(reference[channel]),
                    kPlatformLibmReferenceTolerance);
    }
}

[[nodiscard]] float frozen_dt_ucs_matrix_row(const float coefficient0, const float value0,
                                             const float coefficient1, const float value1,
                                             const float coefficient2, const float value2) noexcept
{
    // Keep every source multiplication and addition at its declared float
    // stage. In particular, this is neither a dot-product abstraction nor FMA.
    const float product0 = coefficient0 * value0;
    const float product1 = coefficient1 * value1;
    const float first_sum = product0 + product1;
    const float product2 = coefficient2 * value2;
    return first_sum + product2;
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d50_to_d65(const FrozenD50Triplet xyz) noexcept
{
    return {frozen_dt_ucs_matrix_row(0.989466254F, xyz[0], -0.0400304626F, xyz[1], 0.0440530317F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(-0.00540518733F, xyz[0], 1.00666069F, xyz[1], -0.00175551955F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(-0.000403920992F, xyz[0], 0.0150768030F, xyz[1], 1.30210211F,
                                     xyz[2])};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d65_to_d50(const FrozenD50Triplet xyz) noexcept
{
    return {frozen_dt_ucs_matrix_row(1.01085433F, xyz[0], 0.0407086103F, xyz[1], -0.0341445825F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(0.00542814201F, xyz[0], 0.993581926F, xyz[1], 0.00115592039F,
                                     xyz[2]),
            frozen_dt_ucs_matrix_row(0.000250722468F, xyz[0], -0.0114918759F, xyz[1], 0.767964947F,
                                     xyz[2])};
}

[[nodiscard]] float frozen_dt_ucs_nonnegative(const float value) noexcept
{
    // Frozen dt_vector_max(value, 0) selects its second operand for NaN and
    // signed zero as well as for negative values.
    return value > 0.0F ? value : 0.0F;
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d65_to_xyy(const FrozenD50Triplet xyz) noexcept
{
    const FrozenD50Triplet nonnegative{frozen_dt_ucs_nonnegative(xyz[0]),
                                       frozen_dt_ucs_nonnegative(xyz[1]),
                                       frozen_dt_ucs_nonnegative(xyz[2])};
    const float sum = nonnegative[0] + nonnegative[1] + nonnegative[2];
    return {sum > 0.0F ? nonnegative[0] / sum : static_cast<float>(0.31271),
            sum > 0.0F ? nonnegative[1] / sum : static_cast<float>(0.32902), nonnegative[1]};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyy_to_xyz_d65(const FrozenD50Triplet xyy) noexcept
{
    const bool zero_denominator = xyy[1] == 0.0F;
    return {zero_denominator ? 0.0F : xyy[2] * xyy[0] / xyy[1], zero_denominator ? 0.0F : xyy[2],
            zero_denominator ? 0.0F : xyy[2] * (1.0F - xyy[0] - xyy[1]) / xyy[1]};
}

[[nodiscard]] float frozen_dt_ucs_signed_denominator(const float value) noexcept
{
    constexpr float minimum = std::numeric_limits<float>::min();
    return value >= 0.0F ? (minimum > value ? minimum : value) :
                           (-minimum < value ? -minimum : value);
}

[[nodiscard]] float frozen_dt_ucs_y_to_lightness(const float luminance) noexcept
{
    const float powered = std::pow(luminance, 0.631651345306265F);
    const float numerator = 2.098883786377F * powered;
    const float denominator = powered + 1.12426773749357F;
    return numerator / denominator;
}

[[nodiscard]] float frozen_dt_ucs_lightness_to_y(const float lightness) noexcept
{
    const float numerator = 1.12426773749357F * lightness;
    const float denominator = 2.098883786377F - lightness;
    const float ratio = numerator / denominator;
    return std::pow(ratio, 1.5831518565279648F);
}

struct FrozenDtUcsJchOracle
{
    FrozenD50Triplet jch{};
    float lightness = 0.0F;
    float squared_colorfulness = 0.0F;
    float source_order_chroma = 0.0F;
    float reassociated_chroma = 0.0F;
};

[[nodiscard]] FrozenDtUcsJchOracle
frozen_dt_ucs_xyy_to_jch_oracle(const FrozenD50Triplet xyy, const float white_lightness) noexcept
{
    constexpr FrozenD50Triplet x_factors{-0.783941002840055F, 0.745273540913283F,
                                         0.318707282433486F};
    constexpr FrozenD50Triplet y_factors{0.277512987809202F, -0.205375866083878F,
                                         2.16743692732158F};
    constexpr FrozenD50Triplet offsets{0.153836578598858F, -0.165478376301988F, 0.291320554395942F};
    FrozenD50Triplet uvd{};
    for (std::size_t channel = 0U; channel < uvd.size(); ++channel)
    {
        const float x_product = x_factors[channel] * xyy[0];
        const float y_product = y_factors[channel] * xyy[1];
        uvd[channel] = (x_product + y_product) + offsets[channel];
    }
    const float divisor = frozen_dt_ucs_signed_denominator(uvd[2]);
    uvd[0] /= divisor;
    uvd[1] /= divisor;

    constexpr std::array<float, 2> factors{1.39656225667F, 1.4513954287F};
    constexpr std::array<float, 2> half_values{1.49217352929F, 1.52488637914F};
    std::array<float, 2> uv_star{};
    for (std::size_t channel = 0U; channel < uv_star.size(); ++channel)
    {
        const float numerator = factors[channel] * uvd[channel];
        const float denominator = std::fabs(uvd[channel]) + half_values[channel];
        uv_star[channel] = numerator / denominator;
    }
    const float u_prime_first = -1.124983854323892F * uv_star[0];
    const float u_prime_second = 0.980483721769325F * uv_star[1];
    const float u_prime = u_prime_first - u_prime_second;
    const float v_prime_first = 1.86323315098672F * uv_star[0];
    const float v_prime_second = 1.971853092390862F * uv_star[1];
    const float v_prime = v_prime_first + v_prime_second;
    const float squared_u_prime = u_prime * u_prime;
    const float squared_v_prime = v_prime * v_prime;
    const float squared_colorfulness = squared_u_prime + squared_v_prime;
    const float lightness = frozen_dt_ucs_y_to_lightness(xyy[2]);
    const float lightness_power = std::pow(lightness, 0.6523997524738018F);
    const float colorfulness_power = std::pow(squared_colorfulness, 0.6007557017508491F);
    const float coefficient_times_lightness = 15.932993652962535F * lightness_power;
    const float product = coefficient_times_lightness * colorfulness_power;
    const float chroma = product / white_lightness;
    const float reassociated_chroma =
        coefficient_times_lightness * (colorfulness_power / white_lightness);
    return {{lightness / white_lightness, chroma, std::atan2(v_prime, u_prime)},
            lightness,
            squared_colorfulness,
            chroma,
            reassociated_chroma};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_jch_to_xyy(const FrozenD50Triplet jch,
                                                        const float white_lightness) noexcept
{
    constexpr float lightness_upper_limit = 2.09885F;
    const float raw_lightness = jch[0] * white_lightness;
    const float lightness =
        raw_lightness >= 0.0F ?
            (raw_lightness <= lightness_upper_limit ? raw_lightness : lightness_upper_limit) :
            0.0F;
    float colorfulness = 0.0F;
    if (lightness != 0.0F)
    {
        const float powered_lightness = std::pow(lightness, 0.6523997524738018F);
        const float denominator = 15.932993652962535F * powered_lightness;
        const float ratio = jch[1] * white_lightness / denominator;
        colorfulness = std::pow(ratio, 0.8322850678616855F);
    }
    const float u_prime = colorfulness * std::cos(jch[2]);
    const float v_prime = colorfulness * std::sin(jch[2]);
    const float u_star_first = -5.037522385190711F * u_prime;
    const float u_star_second = 2.504856328185843F * v_prime;
    const float v_star_first = 4.760029407436461F * u_prime;
    const float v_star_second = 2.874012963239247F * v_prime;
    const std::array<float, 2> uv_star{u_star_first - u_star_second, v_star_first + v_star_second};
    constexpr std::array<float, 2> factors{1.39656225667F, 1.4513954287F};
    constexpr std::array<float, 2> half_values{1.49217352929F, 1.52488637914F};
    std::array<float, 2> uv{};
    for (std::size_t channel = 0U; channel < uv.size(); ++channel)
    {
        const float numerator = -half_values[channel] * uv_star[channel];
        const float denominator = std::fabs(uv_star[channel]) - factors[channel];
        uv[channel] = numerator / denominator;
    }

    constexpr FrozenD50Triplet u_factors{0.167171472114775F, -0.150959086409163F,
                                         0.940254742367256F};
    constexpr FrozenD50Triplet v_factors{0.141299802443708F, -0.155185060382272F,
                                         1.000000000000000F};
    constexpr FrozenD50Triplet offsets{-0.00801531300850582F, -0.00843312433578007F,
                                       -0.0256325967652889F};
    FrozenD50Triplet xyd{};
    for (std::size_t channel = 0U; channel < xyd.size(); ++channel)
    {
        const float u_product = u_factors[channel] * uv[0];
        const float v_product = v_factors[channel] * uv[1];
        xyd[channel] = (u_product + v_product) + offsets[channel];
    }
    const float divisor = frozen_dt_ucs_signed_denominator(xyd[2]);
    return {xyd[0] / divisor, xyd[1] / divisor, frozen_dt_ucs_lightness_to_y(lightness)};
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_xyz_d50_to_jch(const FrozenD50Triplet xyz,
                                                            const float white_lightness) noexcept
{
    return frozen_dt_ucs_xyy_to_jch_oracle(
               frozen_dt_ucs_xyz_d65_to_xyy(frozen_dt_ucs_xyz_d50_to_d65(xyz)), white_lightness)
        .jch;
}

[[nodiscard]] FrozenD50Triplet frozen_dt_ucs_jch_to_xyz_d50(const FrozenD50Triplet jch,
                                                            const float white_lightness) noexcept
{
    return frozen_dt_ucs_xyz_d65_to_d50(
        frozen_dt_ucs_xyy_to_xyz_d65(frozen_dt_ucs_jch_to_xyy(jch, white_lightness)));
}

void expect_dt_ucs_local_oracle(const FrozenD50Triplet actual, const FrozenD50Triplet oracle)
{
    // powf/atan2f/cosf/sinf are platform libm boundaries, so this is a
    // same-platform bit comparison to an independent source-order oracle, not
    // a claim that their decimal results are cross-platform bit-stable.
    EXPECT_EQ(d50_triplet_bits(actual), d50_triplet_bits(oracle));
}

using FrozenHarmonyHueTable = std::array<float, harmony_geometry::kHueTableSteps>;

struct FrozenHarmonyHueTables
{
    FrozenHarmonyHueTable ucs_to_ryb{};
    FrozenHarmonyHueTable ryb_to_ucs{};
};

struct FrozenHarmonyNodes
{
    std::array<float, harmony_geometry::kMaxHarmonyNodes> hues{};
    std::size_t count = 0U;
};

struct FrozenHarmonyAttraction
{
    float shift = 0.0F;
    std::size_t winning_index = 0U;
    float weight = 0.0F;
};

[[nodiscard]] float frozen_harmony_clamp01(const float value) noexcept
{
    return value >= 0.0F ? (value <= 1.0F ? value : 1.0F) : 0.0F;
}

[[nodiscard]] FrozenD50Triplet
frozen_harmony_xyz_d65_to_linear_rec709(const FrozenD50Triplet xyz,
                                        const bool transpose_discriminator = false) noexcept
{
    if (transpose_discriminator)
    {
        return {
            frozen_dt_ucs_matrix_row(3.2404542F, xyz[0], -0.9692660F, xyz[1], 0.0556434F, xyz[2]),
            frozen_dt_ucs_matrix_row(-1.5371385F, xyz[0], 1.8760108F, xyz[1], -0.2040259F, xyz[2]),
            frozen_dt_ucs_matrix_row(-0.4985314F, xyz[0], 0.0415560F, xyz[1], 1.0572252F, xyz[2])};
    }
    return {frozen_dt_ucs_matrix_row(3.2404542F, xyz[0], -1.5371385F, xyz[1], -0.4985314F, xyz[2]),
            frozen_dt_ucs_matrix_row(-0.9692660F, xyz[0], 1.8760108F, xyz[1], 0.0415560F, xyz[2]),
            frozen_dt_ucs_matrix_row(0.0556434F, xyz[0], -0.2040259F, xyz[1], 1.0572252F, xyz[2])};
}

[[nodiscard]] float frozen_harmony_srgb_to_linear(const float srgb,
                                                  const float threshold = 0.04045F) noexcept
{
    const float toe = srgb / 12.92F;
    const float offset_srgb = srgb + 0.055F;
    const float scaled_srgb = offset_srgb / 1.055F;
    const float linearized = std::pow(scaled_srgb, 2.4F);
    return srgb <= threshold ? toe : linearized;
}

[[nodiscard]] FrozenD50Triplet
frozen_harmony_jch_to_srgb(const FrozenD50Triplet jch, const float white_lightness,
                           const bool transpose_discriminator = false) noexcept
{
    const auto xyy = frozen_dt_ucs_jch_to_xyy(jch, white_lightness);
    const auto xyz_d65 = frozen_dt_ucs_xyy_to_xyz_d65(xyy);
    const auto linear = frozen_harmony_xyz_d65_to_linear_rec709(xyz_d65, transpose_discriminator);
    FrozenD50Triplet srgb{};
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

[[nodiscard]] float frozen_harmony_max_chroma(const float hue, const int iterations = 16,
                                              const bool transpose_discriminator = false) noexcept
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const float scaled_hue = hue * 6.28318530717958647693F;
    const float angle = scaled_hue - 3.14159265358979323846F;
    constexpr float lightness = 0.65F;
    float lower = 0.0F;
    float upper = 2.0F;
    for (int iteration = 0; iteration < iterations; ++iteration)
    {
        const float middle = (lower + upper) * 0.5F;
        const auto srgb = frozen_harmony_jch_to_srgb({lightness, middle, angle}, white_lightness,
                                                     transpose_discriminator);
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

[[nodiscard]] float frozen_harmony_rgb_hue_to_ryb(const float hue,
                                                  const float middle_knot = 0.472217F) noexcept
{
    constexpr std::array<float, 7> input_knots{0.0F,        1.0F / 6.0F, 2.0F / 6.0F, 3.0F / 6.0F,
                                               4.0F / 6.0F, 5.0F / 6.0F, 1.0F};
    const std::array<float, 7> output_knots{0.0F,      1.0F / 3.0F, middle_knot, 0.611105F,
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

[[nodiscard]] float frozen_harmony_ucs_to_ryb(const float hue, const int iterations = 16,
                                              const bool transpose_discriminator = false,
                                              const float middle_knot = 0.472217F) noexcept
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const float scaled_hue = hue * 6.28318530717958647693F;
    const float angle = scaled_hue - 3.14159265358979323846F;
    const float chroma =
        frozen_harmony_max_chroma(hue, iterations, transpose_discriminator) * 0.85F;
    auto srgb = frozen_harmony_jch_to_srgb({0.65F, chroma, angle}, white_lightness,
                                           transpose_discriminator);
    for (float &channel : srgb)
    {
        channel = frozen_harmony_clamp01(channel);
        channel = frozen_harmony_srgb_to_linear(channel);
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
    return frozen_harmony_rgb_hue_to_ryb(rgb_hue, middle_knot);
}

[[nodiscard]] FrozenHarmonyHueTable
frozen_harmony_forward_table(const int iterations = 16, const bool transpose_discriminator = false,
                             const float middle_knot = 0.472217F) noexcept
{
    FrozenHarmonyHueTable table{};
    for (std::size_t index = 0U; index < table.size(); ++index)
    {
        table[index] =
            frozen_harmony_ucs_to_ryb(static_cast<float>(index) / static_cast<float>(table.size()),
                                      iterations, transpose_discriminator, middle_knot);
    }
    return table;
}

[[nodiscard]] FrozenHarmonyHueTable
frozen_harmony_inverse_table(const FrozenHarmonyHueTable &forward) noexcept
{
    FrozenHarmonyHueTable inverse{};
    for (std::size_t target_index = 0U; target_index < inverse.size(); ++target_index)
    {
        const float target = static_cast<float>(target_index) / static_cast<float>(inverse.size());
        float best_distance = 1.0F;
        float best_ucs = 0.0F;
        for (std::size_t index = 0U; index < forward.size(); ++index)
        {
            float distance = std::fabs(forward[index] - target);
            if (distance > 0.5F)
            {
                distance = 1.0F - distance;
            }
            if (distance < best_distance)
            {
                best_distance = distance;
                best_ucs = static_cast<float>(index) / static_cast<float>(forward.size());
            }
        }
        inverse[target_index] = best_ucs;
    }
    return inverse;
}

[[nodiscard]] FrozenHarmonyHueTables frozen_harmony_tables() noexcept
{
    FrozenHarmonyHueTables tables;
    tables.ucs_to_ryb = frozen_harmony_forward_table();
    tables.ryb_to_ucs = frozen_harmony_inverse_table(tables.ucs_to_ryb);
    return tables;
}

[[nodiscard]] float frozen_harmony_lerp(float first, float second, const float fraction) noexcept
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

[[nodiscard]] float frozen_harmony_lookup(const FrozenHarmonyHueTable &table,
                                          const float hue) noexcept
{
    const float position = hue * static_cast<float>(table.size());
    const int integral_position = static_cast<int>(position);
    const std::size_t first = static_cast<std::size_t>(integral_position) % table.size();
    const std::size_t second = (first + 1U) % table.size();
    return frozen_harmony_lerp(table[first], table[second],
                               position - static_cast<float>(integral_position));
}

[[nodiscard]] FrozenHarmonyNodes
frozen_predefined_harmony_nodes(const harmony_geometry::StandardRule rule, const float anchor_hue,
                                const FrozenHarmonyHueTables &tables) noexcept
{
    constexpr std::array<std::size_t, 9> counts{1U, 3U, 4U, 2U, 3U, 2U, 3U, 4U, 4U};
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
    const std::size_t rule_index = static_cast<std::size_t>(rule);
    FrozenHarmonyNodes nodes;
    nodes.count = counts[rule_index];
    const float mapped_anchor = frozen_harmony_lookup(tables.ucs_to_ryb, anchor_hue);
    const int rotation = static_cast<int>(std::round(mapped_anchor * 360.0F)) % 360;
    const float sector_anchor = static_cast<float>(rotation) / 360.0F;
    for (std::size_t index = 0U; index < nodes.count; ++index)
    {
        float angle = offsets[rule_index][index] + sector_anchor;
        angle -= std::floor(angle);
        nodes.hues[index] = frozen_harmony_lookup(tables.ryb_to_ucs, angle);
    }
    return nodes;
}

[[nodiscard]] FrozenHarmonyAttraction frozen_harmony_attraction(const float pixel_hue,
                                                                const std::span<const float> nodes,
                                                                const float pull_width) noexcept
{
    const float sigma = pull_width * 0.5F / static_cast<float>(nodes.size());
    const float inverse_two_sigma_squared = 1.0F / (2.0F * sigma * sigma);
    FrozenHarmonyAttraction result;
    for (std::size_t index = 0U; index < nodes.size(); ++index)
    {
        float distance = std::fabs(pixel_hue - nodes[index]);
        if (distance > 0.5F)
        {
            distance = 1.0F - distance;
        }
        const float weight = std::exp(-distance * distance * inverse_two_sigma_squared);
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
            result.shift = difference;
        }
    }
    result.shift *= result.weight;
    return result;
}

[[nodiscard]] std::uint64_t frozen_harmony_table_hash(const FrozenHarmonyHueTable &table) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const float value : table)
    {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        for (const unsigned int shift : {0U, 8U, 16U, 24U})
        {
            hash ^= static_cast<std::uint8_t>(bits >> shift);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

[[nodiscard]] std::array<float, 9>
frozen_color_harmonizer_inverse(const std::array<float, 9> &matrix) noexcept
{
    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double g = matrix[6];
    const double h = matrix[7];
    const double i = matrix[8];
    const volatile double ei = e * i;
    const volatile double fh = f * h;
    const volatile double di = d * i;
    const volatile double fg = f * g;
    const volatile double dh = d * h;
    const volatile double eg = e * g;
    const double minor0 = ei - fh;
    const double minor1 = di - fg;
    const double minor2 = dh - eg;
    const volatile double first = a * minor0;
    const volatile double second = b * minor1;
    const volatile double third = c * minor2;
    const double determinant = (first - second) + third;
    const double inverse = 1.0 / determinant;
    const auto difference =
        [](const double lhs0, const double lhs1, const double rhs0, const double rhs1)
    {
        const volatile double lhs = lhs0 * lhs1;
        const volatile double rhs = rhs0 * rhs1;
        return lhs - rhs;
    };
    return {static_cast<float>(minor0 * inverse),
            static_cast<float>(difference(c, h, b, i) * inverse),
            static_cast<float>(difference(b, f, c, e) * inverse),
            static_cast<float>(difference(f, g, d, i) * inverse),
            static_cast<float>(difference(a, i, c, g) * inverse),
            static_cast<float>(difference(c, d, a, f) * inverse),
            static_cast<float>(difference(d, h, e, g) * inverse),
            static_cast<float>(difference(b, g, a, h) * inverse),
            static_cast<float>(difference(a, e, b, d) * inverse)};
}

[[nodiscard]] FrozenD50Triplet frozen_color_harmonizer_matrix(const std::array<float, 9> &matrix,
                                                              const FrozenD50Triplet value) noexcept
{
    const auto row = [](const float coefficient0, const float value0, const float coefficient1,
                        const float value1, const float coefficient2, const float value2)
    {
        const volatile float product0 = coefficient0 * value0;
        const volatile float product1 = coefficient1 * value1;
        const volatile float first_sum = product0 + product1;
        const volatile float product2 = coefficient2 * value2;
        return first_sum + product2;
    };
    return {row(matrix[0], value[0], matrix[1], value[1], matrix[2], value[2]),
            row(matrix[3], value[0], matrix[4], value[1], matrix[5], value[2]),
            row(matrix[6], value[0], matrix[7], value[1], matrix[8], value[2])};
}

[[nodiscard]] FrozenHarmonyNodes
frozen_color_harmonizer_nodes(const ColorHarmonizerParams &params,
                              const FrozenHarmonyHueTables &tables) noexcept
{
    FrozenHarmonyNodes nodes;
    if (params.rule == ColorHarmonizerRule::kCustom)
    {
        nodes.count = static_cast<std::size_t>(params.num_custom_nodes);
        for (std::size_t index = 0U; index < nodes.count; ++index)
        {
            nodes.hues[index] = static_cast<float>(params.custom_hue[index]);
        }
        return nodes;
    }
    return frozen_predefined_harmony_nodes(static_cast<harmony_geometry::StandardRule>(params.rule),
                                           static_cast<float>(params.anchor_hue), tables);
}

// Independent scalar composition of the frozen smoothing-zero process. The
// conversion, geometry, matrix, and correction stages call only the source
// transcriptions above, never apply_color_harmonizer or a production bridge.
[[nodiscard]] FrozenD50Triplet frozen_color_harmonizer_rgb(
    const ColorHarmonizerParams &params, const FrozenD50Triplet input,
    const std::array<float, 9> &working_to_xyz_d50, const FrozenHarmonyHueTables &tables,
    const bool skip_negative_clip = false, const bool linear_neutral_protection = false) noexcept
{
    const auto inverse = frozen_color_harmonizer_inverse(working_to_xyz_d50);
    const FrozenD50Triplet nonnegative{skip_negative_clip ? input[0] : std::fmax(input[0], 0.0F),
                                       skip_negative_clip ? input[1] : std::fmax(input[1], 0.0F),
                                       skip_negative_clip ? input[2] : std::fmax(input[2], 0.0F)};
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    auto jch = frozen_dt_ucs_xyz_d50_to_jch(
        frozen_color_harmonizer_matrix(working_to_xyz_d50, nonnegative), white_lightness);
    constexpr float pi = 3.14159265358979323846F;
    constexpr float two_pi = 6.28318530717958647693F;
    const float hue = (jch[2] + pi) / two_pi;
    const float chroma = jch[1];
    const auto nodes = frozen_color_harmonizer_nodes(params, tables);
    const auto attraction =
        frozen_harmony_attraction(hue, std::span<const float>(nodes.hues.data(), nodes.count),
                                  static_cast<float>(params.pull_width));
    const volatile float saturation_delta =
        (static_cast<float>(params.node_saturation[attraction.winning_index]) - 1.0F) *
        attraction.weight;
    const float neutral = static_cast<float>(params.neutral_protection);
    const volatile float neutral_squared = neutral * neutral;
    const volatile float neutral_cubed = neutral_squared * neutral;
    const volatile float cutoff = (linear_neutral_protection ? neutral : neutral_cubed) * 0.03F;
    const volatile float chroma_plus_cutoff = chroma + cutoff;
    const volatile float denominator = chroma_plus_cutoff + 1.0e-5F;
    const volatile float chroma_weight = chroma / denominator;
    const volatile float strength_shift =
        attraction.shift * static_cast<float>(params.pull_strength);
    const volatile float weighted_shift = strength_shift * chroma_weight;
    const volatile float shifted_hue = hue + weighted_shift;
    float corrected_hue = std::fmod(shifted_hue, 1.0F);
    if (corrected_hue < 0.0F)
    {
        corrected_hue += 1.0F;
    }
    const volatile float scaled_hue = corrected_hue * two_pi;
    jch[2] = scaled_hue - pi;
    const volatile float weighted_saturation = saturation_delta * chroma_weight;
    const volatile float saturation_scale = 1.0F + weighted_saturation;
    const volatile float corrected_chroma = chroma * saturation_scale;
    jch[1] = std::fmax(corrected_chroma, 0.0F);
    return frozen_color_harmonizer_matrix(inverse,
                                          frozen_dt_ucs_jch_to_xyz_d50(jch, white_lightness));
}

[[nodiscard]] ColorHarmonizerParams frozen_color_harmonizer_0176_record13() noexcept
{
    ColorHarmonizerParams params;
    params.rule = ColorHarmonizerRule::kSplitComplementary;
    params.anchor_hue = 0.55000001192092896;
    params.pull_strength = 0.81999999284744263;
    params.pull_width = 1.8400000333786011;
    params.node_saturation = {1.2599999904632568, 0.18000000715255737, 1.5199999809265137, 1.0};
    return params;
}

[[nodiscard]] WorkingImage color_harmonizer_working_fixture()
{
    WorkingImage input;
    input.width = 4U;
    input.height = 1U;
    input.rgb = {0.03F, 0.18F, 0.72F, 0.91F, 0.42F, 0.07F, -0.25F, 0.5F, 1.7F, 0.0F, 0.0F, 0.0F};
    input.color_profile.kind = ColorProfileKind::kIcc;
    input.color_profile.model = ColorModel::kRgb;
    input.color_profile.identifier = std::string(kInputProfileLinearRec709);
    input.color_profile.icc_bytes = {1U, 2U, 3U, 4U};
    input.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                             0.2225045F, 0.7168786F, 0.0606169F,
                                             0.0139322F, 0.0971045F, 0.7141733F};
    input.color_profile.has_matrix = true;
    auto analysis = std::make_shared<ExposureAnalysisContext>();
    analysis->raw_pixel_count = 4U;
    input.exposure_analysis = std::move(analysis);
    return input;
}

TEST(HarmonyGeometryTest, FullTablesMatchIndependentOracleAndReferenceInvariants)
{
    // The oracle is a scalar transcription of colorharmonizer.c's 16-step
    // gamut search, D65 sRGB bridge, HCV conversion, Gossett knots, and strict
    // nearest inverse scan. It never calls a production harmony helper.
    const auto oracle = frozen_harmony_tables();
    const std::uint64_t forward_hash = frozen_harmony_table_hash(oracle.ucs_to_ryb);
    const std::uint64_t inverse_hash = frozen_harmony_table_hash(oracle.ryb_to_ucs);

    const auto actual = harmony_geometry::build_harmony_hue_tables();
    EXPECT_EQ(frozen_harmony_table_hash(actual.ucs_to_ryb), forward_hash);
    EXPECT_EQ(frozen_harmony_table_hash(actual.ryb_to_ucs), inverse_hash);
    for (std::size_t index = 0U; index < harmony_geometry::kHueTableSteps; ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.ucs_to_ryb[index]),
                  std::bit_cast<std::uint32_t>(oracle.ucs_to_ryb[index]));
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual.ryb_to_ucs[index]),
                  std::bit_cast<std::uint32_t>(oracle.ryb_to_ucs[index]));
    }

    // These deliberate oracle perturbations prove that the table hash catches
    // the frozen search count, transposed matrix orientation, and RYB knot
    // constants rather than merely hashing an arbitrary smooth curve.
    EXPECT_NE(frozen_harmony_table_hash(frozen_harmony_forward_table(15)), forward_hash);
    EXPECT_NE(frozen_harmony_table_hash(frozen_harmony_forward_table(16, true)), forward_hash);
    EXPECT_NE(frozen_harmony_table_hash(
                  frozen_harmony_forward_table(16, false, std::nextafter(0.472217F, 1.0F))),
              forward_hash);

    // Frozen CLAMP routes NaN to zero even though valid table construction does
    // not normally feed non-finite swatch samples.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_clamp01(nan)), 0x00000000U);

    // The legacy conditional transfer curve is lazy: negative linear sRGB
    // takes the toe and must not evaluate powf on the discarded branch.
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    constexpr FrozenD50Triplet extended_jch{0.65F, 2.0F, 0.0F};
    const auto extended_linear = frozen_harmony_xyz_d65_to_linear_rec709(
        frozen_dt_ucs_xyy_to_xyz_d65(frozen_dt_ucs_jch_to_xyy(extended_jch, white_lightness)));
    ASSERT_TRUE(
        std::ranges::any_of(extended_linear, [](const float value) { return value < 0.0F; }));
    std::feclearexcept(FE_ALL_EXCEPT);
    static_cast<void>(frozen_harmony_jch_to_srgb(extended_jch, white_lightness));
    EXPECT_EQ(std::fetestexcept(FE_INVALID), 0);
    std::feclearexcept(FE_ALL_EXCEPT);
    static_cast<void>(harmony_geometry::build_harmony_hue_tables());
    EXPECT_EQ(std::fetestexcept(FE_INVALID), 0);

    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(0.0F)), 0x00000000U);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(1.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(1.0F / 3.0F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(2.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(0.472217F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(5.0F / 6.0F)),
              std::bit_cast<std::uint32_t>(5.0F / 6.0F));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(frozen_harmony_rgb_hue_to_ryb(1.0F)), 0x00000000U);

    const float transfer_input = 0.045F;
    EXPECT_NE(std::bit_cast<std::uint32_t>(frozen_harmony_srgb_to_linear(transfer_input)),
              std::bit_cast<std::uint32_t>(frozen_harmony_srgb_to_linear(transfer_input, 0.05F)));
}


} // namespace
} // namespace ravo
