#include "dt_ucs.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace ravo::dt_ucs
{
namespace
{

[[nodiscard]] float matrix_row(const float coefficient0, const float value0,
                               const float coefficient1, const float value1,
                               const float coefficient2, const float value2) noexcept
{
    // Preserve the frozen dt_apply_transposed_color_matrix float stages and
    // left-to-right addition order. Named products keep this boundary distinct
    // from an FMA or generic dot-product implementation.
    const float product0 = coefficient0 * value0;
    const float product1 = coefficient1 * value1;
    const float first_sum = product0 + product1;
    const float product2 = coefficient2 * value2;
    return first_sum + product2;
}

[[nodiscard]] float nonnegative_component(const float value) noexcept
{
    // This matches frozen dt_vector_max(value, 0): NaN, negative values, and
    // either signed zero select the positive-zero second operand.
    return value > 0.0F ? value : 0.0F;
}

[[nodiscard]] float signed_denominator(const float value) noexcept
{
    constexpr float minimum = std::numeric_limits<float>::min();
    return value >= 0.0F ? (minimum > value ? minimum : value) :
                           (-minimum < value ? -minimum : value);
}

} // namespace

float y_to_lightness(const float luminance) noexcept
{
    const float powered = std::pow(luminance, 0.631651345306265F);
    const float numerator = 2.098883786377F * powered;
    const float denominator = powered + 1.12426773749357F;
    return numerator / denominator;
}

float lightness_to_y(const float lightness) noexcept
{
    const float numerator = 1.12426773749357F * lightness;
    const float denominator = 2.098883786377F - lightness;
    const float ratio = numerator / denominator;
    return std::pow(ratio, 1.5831518565279648F);
}

Triplet xyz_d50_to_d65(const Triplet xyz) noexcept
{
    return {matrix_row(0.989466254F, xyz[0], -0.0400304626F, xyz[1], 0.0440530317F, xyz[2]),
            matrix_row(-0.00540518733F, xyz[0], 1.00666069F, xyz[1], -0.00175551955F, xyz[2]),
            matrix_row(-0.000403920992F, xyz[0], 0.0150768030F, xyz[1], 1.30210211F, xyz[2])};
}

Triplet xyz_d65_to_d50(const Triplet xyz) noexcept
{
    return {matrix_row(1.01085433F, xyz[0], 0.0407086103F, xyz[1], -0.0341445825F, xyz[2]),
            matrix_row(0.00542814201F, xyz[0], 0.993581926F, xyz[1], 0.00115592039F, xyz[2]),
            matrix_row(0.000250722468F, xyz[0], -0.0114918759F, xyz[1], 0.767964947F, xyz[2])};
}

Triplet xyz_d65_to_xyy(const Triplet xyz) noexcept
{
    const Triplet nonnegative{nonnegative_component(xyz[0]), nonnegative_component(xyz[1]),
                              nonnegative_component(xyz[2])};
    const float sum = nonnegative[0] + nonnegative[1] + nonnegative[2];
    return {sum > 0.0F ? nonnegative[0] / sum : static_cast<float>(0.31271),
            sum > 0.0F ? nonnegative[1] / sum : static_cast<float>(0.32902), nonnegative[1]};
}

Triplet xyy_to_xyz_d65(const Triplet xyy) noexcept
{
    const bool zero_denominator = xyy[1] == 0.0F;
    return {zero_denominator ? 0.0F : xyy[2] * xyy[0] / xyy[1], zero_denominator ? 0.0F : xyy[2],
            zero_denominator ? 0.0F : xyy[2] * (1.0F - xyy[0] - xyy[1]) / xyy[1]};
}

Triplet xyy_to_jch(const Triplet xyy, const float white_lightness) noexcept
{
    constexpr Triplet x_factors{-0.783941002840055F, 0.745273540913283F, 0.318707282433486F};
    constexpr Triplet y_factors{0.277512987809202F, -0.205375866083878F, 2.16743692732158F};
    constexpr Triplet offsets{0.153836578598858F, -0.165478376301988F, 0.291320554395942F};
    Triplet uvd{};
    for (std::size_t channel = 0U; channel < uvd.size(); ++channel)
    {
        const float x_product = x_factors[channel] * xyy[0];
        const float y_product = y_factors[channel] * xyy[1];
        uvd[channel] = (x_product + y_product) + offsets[channel];
    }
    const float divisor = signed_denominator(uvd[2]);
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
    const float lightness = y_to_lightness(xyy[2]);
    const float lightness_power = std::pow(lightness, 0.6523997524738018F);
    const float colorfulness_power = std::pow(squared_colorfulness, 0.6007557017508491F);
    const float coefficient_times_lightness = 15.932993652962535F * lightness_power;
    const float product = coefficient_times_lightness * colorfulness_power;
    return {lightness / white_lightness, product / white_lightness, std::atan2(v_prime, u_prime)};
}

Triplet jch_to_xyy(const Triplet jch, const float white_lightness) noexcept
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
        const float denominator =
            signed_denominator(std::fabs(uv_star[channel]) - factors[channel]);
        uv[channel] = numerator / denominator;
    }

    constexpr Triplet u_factors{0.167171472114775F, -0.150959086409163F, 0.940254742367256F};
    constexpr Triplet v_factors{0.141299802443708F, -0.155185060382272F, 1.000000000000000F};
    constexpr Triplet offsets{-0.00801531300850582F, -0.00843312433578007F, -0.0256325967652889F};
    Triplet xyd{};
    for (std::size_t channel = 0U; channel < xyd.size(); ++channel)
    {
        const float u_product = u_factors[channel] * uv[0];
        const float v_product = v_factors[channel] * uv[1];
        xyd[channel] = (u_product + v_product) + offsets[channel];
    }
    const float divisor = signed_denominator(xyd[2]);
    return {xyd[0] / divisor, xyd[1] / divisor, lightness_to_y(lightness)};
}

Triplet xyz_d50_to_jch(const Triplet xyz, const float white_lightness) noexcept
{
    return xyy_to_jch(xyz_d65_to_xyy(xyz_d50_to_d65(xyz)), white_lightness);
}

Triplet jch_to_xyz_d50(const Triplet jch, const float white_lightness) noexcept
{
    return xyz_d65_to_d50(xyy_to_xyz_d65(jch_to_xyy(jch, white_lightness)));
}

} // namespace ravo::dt_ucs
