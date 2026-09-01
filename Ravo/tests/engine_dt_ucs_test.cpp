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

TEST(DtUcsBridgeTest, Cat16DirectionsBasisWhiteBlackAndSourceOrderMatchFrozenBits)
{
    struct MatrixCase
    {
        FrozenD50Triplet input;
        std::array<std::uint32_t, 3> d50_to_d65;
        std::array<std::uint32_t, 3> d65_to_d50;
    };
    const std::array cases{
        MatrixCase{{1.0F, 0.0F, 0.0F},
                   {0x3f7d4da9U, 0xbbb11dffU, 0xb9d3c55cU},
                   {0x3f8163adU, 0x3bb1de8eU, 0x39837366U}},
        MatrixCase{{0.0F, 1.0F, 0.0F},
                   {0xbd23f6fbU, 0x3f80da42U, 0x3c7704b2U},
                   {0x3d26be12U, 0x3f7e5b63U, 0xbc3c486cU}},
        MatrixCase{{0.0F, 0.0F, 1.0F},
                   {0x3d3470f4U, 0xbae61976U, 0x3fa6ab48U},
                   {0xbd0bdb31U, 0x3a978241U, 0x3f44995aU}},
    };
    for (const auto &[input, forward, inverse] : cases)
    {
        expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(input), frozen_dt_ucs_xyz_d50_to_d65(input),
                               forward);
        expect_frozen_d50_bits(dt_ucs::xyz_d65_to_d50(input), frozen_dt_ucs_xyz_d65_to_d50(input),
                               inverse);
    }

    constexpr FrozenD50Triplet black{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet d50_white{0.9642119944211994F, 1.0F, 0.8251882845188288F};
    constexpr FrozenD50Triplet d65_white{0.95047F, 1.0F, 1.08883F};
    expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(black), frozen_dt_ucs_xyz_d50_to_d65(black),
                           {0x00000000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(d50_white),
                           frozen_dt_ucs_xyz_d50_to_d65(d50_white),
                           {0x3f734be5U, 0x3f800003U, 0x3f8b69d0U});
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_d50(d65_white),
                           frozen_dt_ucs_xyz_d65_to_d50(d65_white),
                           {0x3f76dd87U, 0x3f7ffffdU, 0x3f532e98U});

    // This extended vector differs by one ULP when the first row is silently
    // contracted/reassociated with FMA. The fixed source-order result also
    // distinguishes the transposed CAT16 owner from the display matrix form.
    constexpr FrozenD50Triplet fma_discriminator{2.05470204F, 1.34176934F, -1.5565666F};
    const auto expected = frozen_dt_ucs_xyz_d50_to_d65(fma_discriminator);
    expect_frozen_d50_bits(dt_ucs::xyz_d50_to_d65(fma_discriminator), expected,
                           {0x3ff49449U, 0x3fabd192U, 0xc0007963U});
    const FrozenD50Triplet contracted{std::fma(0.989466254F, fma_discriminator[0],
                                               std::fma(-0.0400304626F, fma_discriminator[1],
                                                        0.0440530317F * fma_discriminator[2])),
                                      std::fma(-0.00540518733F, fma_discriminator[0],
                                               std::fma(1.00666069F, fma_discriminator[1],
                                                        -0.00175551955F * fma_discriminator[2])),
                                      std::fma(-0.000403920992F, fma_discriminator[0],
                                               std::fma(0.0150768030F, fma_discriminator[1],
                                                        1.30210211F * fma_discriminator[2]))};
    EXPECT_EQ(d50_triplet_bits(contracted),
              (std::array<std::uint32_t, 3>{0x3ff49448U, 0x3fabd192U, 0xc0007963U}));
    EXPECT_NE(d50_triplet_bits(contracted), d50_triplet_bits(expected));
}

TEST(DtUcsBridgeTest, XyYSourceClampZeroAndMinNormalDenominatorsMatchFrozenBits)
{
    constexpr FrozenD50Triplet black{0.0F, 0.0F, 0.0F};
    constexpr FrozenD50Triplet negative_extended{-0.25F, 0.5F, 1.75F};
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy(black), frozen_dt_ucs_xyz_d65_to_xyy(black),
                           {0x3ea01b86U, 0x3ea8754fU, 0x00000000U});
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy({1.0F, 0.0F, 0.0F}),
                           frozen_dt_ucs_xyz_d65_to_xyy({1.0F, 0.0F, 0.0F}),
                           {0x3f800000U, 0x00000000U, 0x00000000U});
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy(negative_extended),
                           frozen_dt_ucs_xyz_d65_to_xyy(negative_extended),
                           {0x00000000U, 0x3e638e39U, 0x3f000000U});

    constexpr float minimum = std::numeric_limits<float>::min();
    const FrozenD50Triplet positive_minimum_y{0.25F, minimum, minimum};
    const FrozenD50Triplet negative_minimum_y{0.25F, -minimum, minimum};
    expect_frozen_d50_bits(dt_ucs::xyy_to_xyz_d65(positive_minimum_y),
                           frozen_dt_ucs_xyy_to_xyz_d65(positive_minimum_y),
                           {0x3e800000U, 0x00800000U, 0x3f400000U});
    expect_frozen_d50_bits(dt_ucs::xyy_to_xyz_d65(negative_minimum_y),
                           frozen_dt_ucs_xyy_to_xyz_d65(negative_minimum_y),
                           {0xbe800000U, 0x00800000U, 0xbf400000U});
    for (const float signed_zero : {0.0F, -0.0F})
    {
        const FrozenD50Triplet xyy{0.25F, signed_zero, 0.5F};
        expect_frozen_d50_bits(dt_ucs::xyy_to_xyz_d65(xyy), frozen_dt_ucs_xyy_to_xyz_d65(xyy),
                               {0x00000000U, 0x00000000U, 0x00000000U});
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    expect_frozen_d50_bits(dt_ucs::xyz_d65_to_xyy({nan, 0.0F, 0.0F}),
                           frozen_dt_ucs_xyz_d65_to_xyy({nan, 0.0F, 0.0F}),
                           {0x3ea01b86U, 0x3ea8754fU, 0x00000000U});
    const auto infinite =
        dt_ucs::xyz_d65_to_xyy({std::numeric_limits<float>::infinity(), 0.0F, 0.0F});
    EXPECT_TRUE(std::isnan(infinite[0]));
    EXPECT_EQ(infinite[1], 0.0F);
    EXPECT_EQ(infinite[2], 0.0F);

    // This representable xy pair makes the frozen UVD denominator exactly
    // +0. The source's +FLT_MIN substitution keeps the forward JCH finite.
    constexpr FrozenD50Triplet singular_xyy{0.0F, -0.134407863F, 0.25F};
    const float singular_denominator =
        (0.318707282433486F * singular_xyy[0] + 2.16743692732158F * singular_xyy[1]) +
        0.291320554395942F;
    EXPECT_EQ(std::bit_cast<std::uint32_t>(singular_denominator), 0x00000000U);
    const auto singular_oracle =
        frozen_dt_ucs_xyy_to_jch_oracle(singular_xyy, frozen_dt_ucs_y_to_lightness(1.0F));
    const auto singular_actual = dt_ucs::xyy_to_jch(singular_xyy, dt_ucs::y_to_lightness(1.0F));
    expect_dt_ucs_local_oracle(singular_actual, singular_oracle.jch);
    EXPECT_TRUE(std::ranges::all_of(singular_actual,
                                    [](const float value) { return std::isfinite(value); }));
}

TEST(DtUcsBridgeTest, LightnessJchPowAndAdditionOrderMatchIndependentOracle)
{
    for (const float luminance : {0.0F, 0.18F, 1.0F, 4.0F, 1.0e8F})
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(dt_ucs::y_to_lightness(luminance)),
                  std::bit_cast<std::uint32_t>(frozen_dt_ucs_y_to_lightness(luminance)));
    }
    for (const float lightness : {0.0F, 0.25F, 1.0F, 2.0F, 2.09885F})
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(dt_ucs::lightness_to_y(lightness)),
                  std::bit_cast<std::uint32_t>(frozen_dt_ucs_lightness_to_y(lightness)));
    }
    const float white_lightness = dt_ucs::y_to_lightness(1.0F);
    EXPECT_NEAR(white_lightness, 0.98805058F, 1.0e-7F);

    constexpr FrozenD50Triplet black_xyy{0.31271F, 0.32902F, 0.0F};
    constexpr FrozenD50Triplet white_xyy{0.31271F, 0.32902F, 1.0F};
    expect_dt_ucs_local_oracle(
        dt_ucs::xyy_to_jch(black_xyy, white_lightness),
        frozen_dt_ucs_xyy_to_jch_oracle(black_xyy, frozen_dt_ucs_y_to_lightness(1.0F)).jch);
    const auto white = dt_ucs::xyy_to_jch(white_xyy, white_lightness);
    const auto white_oracle =
        frozen_dt_ucs_xyy_to_jch_oracle(white_xyy, frozen_dt_ucs_y_to_lightness(1.0F));
    expect_dt_ucs_local_oracle(white, white_oracle.jch);
    EXPECT_EQ(white[0], 1.0F);
    EXPECT_GT(white[1], 0.0F);

    // This source-derived extended vector changes by one ULP if the two powf
    // products and white division are reassociated. We compare production to
    // the independent local-libm oracle, not to a cross-platform decimal bit
    // promise.
    constexpr FrozenD50Triplet pow_order_discriminator{0.71667999F, -0.921235025F, 0.585777998F};
    const auto oracle = frozen_dt_ucs_xyy_to_jch_oracle(pow_order_discriminator, white_lightness);
    EXPECT_NE(std::bit_cast<std::uint32_t>(oracle.source_order_chroma),
              std::bit_cast<std::uint32_t>(oracle.reassociated_chroma));
    expect_dt_ucs_local_oracle(dt_ucs::xyy_to_jch(pow_order_discriminator, white_lightness),
                               oracle.jch);
}

TEST(DtUcsBridgeTest, InverseClampsOnlyFrozenLightnessAndPropagatesOtherInvalidMath)
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    constexpr float upper_lightness = 2.09885F;
    const float upper_j = upper_lightness / white_lightness;
    const FrozenD50Triplet below{std::nextafter(upper_j, 0.0F), 0.4F, 0.7F};
    const FrozenD50Triplet exact{upper_j, 0.4F, 0.7F};
    const FrozenD50Triplet above{std::nextafter(upper_j, std::numeric_limits<float>::infinity()),
                                 0.4F, 0.7F};
    const auto below_actual = dt_ucs::jch_to_xyy(below, white_lightness);
    const auto exact_actual = dt_ucs::jch_to_xyy(exact, white_lightness);
    const auto above_actual = dt_ucs::jch_to_xyy(above, white_lightness);
    expect_dt_ucs_local_oracle(below_actual, frozen_dt_ucs_jch_to_xyy(below, white_lightness));
    expect_dt_ucs_local_oracle(exact_actual, frozen_dt_ucs_jch_to_xyy(exact, white_lightness));
    expect_dt_ucs_local_oracle(above_actual, frozen_dt_ucs_jch_to_xyy(above, white_lightness));
    EXPECT_NE(d50_triplet_bits(below_actual), d50_triplet_bits(exact_actual));
    EXPECT_EQ(d50_triplet_bits(exact_actual), d50_triplet_bits(above_actual));

    const auto negative = dt_ucs::jch_to_xyy({-1.0F, 0.4F, 0.7F}, white_lightness);
    expect_dt_ucs_local_oracle(negative,
                               frozen_dt_ucs_jch_to_xyy({-1.0F, 0.4F, 0.7F}, white_lightness));
    EXPECT_EQ(negative[2], 0.0F);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const auto nan_j = dt_ucs::jch_to_xyy({nan, 0.0F, 0.0F}, white_lightness);
    expect_dt_ucs_local_oracle(nan_j, frozen_dt_ucs_jch_to_xyy({nan, 0.0F, 0.0F}, white_lightness));
    EXPECT_TRUE(std::ranges::all_of(nan_j, [](const float value) { return std::isfinite(value); }));
    const auto nan_chroma = dt_ucs::jch_to_xyy({1.0F, nan, 0.0F}, white_lightness);
    EXPECT_TRUE(std::isnan(nan_chroma[0]));
    EXPECT_TRUE(std::isnan(nan_chroma[1]));
    EXPECT_TRUE(std::isfinite(nan_chroma[2]));
    const auto infinite_hue = dt_ucs::jch_to_xyy({1.0F, 0.4F, infinity}, white_lightness);
    EXPECT_TRUE(std::isnan(infinite_hue[0]));
    EXPECT_TRUE(std::isnan(infinite_hue[1]));
    EXPECT_TRUE(std::isfinite(infinite_hue[2]));
}

TEST(DtUcsBridgeTest, ExtendedRoundTripsAndNonFiniteClassificationFollowFrozenMath)
{
    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const std::array xyy_cases{
        FrozenD50Triplet{0.25F, 0.4F, 0.18F},
        FrozenD50Triplet{1.25F, -0.2F, 4.0F},
        FrozenD50Triplet{-0.4F, 0.8F, 0.75F},
    };
    for (const auto &xyy : xyy_cases)
    {
        const auto jch_oracle = frozen_dt_ucs_xyy_to_jch_oracle(xyy, white_lightness).jch;
        const auto jch = dt_ucs::xyy_to_jch(xyy, white_lightness);
        expect_dt_ucs_local_oracle(jch, jch_oracle);
        const auto actual = dt_ucs::jch_to_xyy(jch, white_lightness);
        const auto oracle = frozen_dt_ucs_jch_to_xyy(jch_oracle, white_lightness);
        expect_dt_ucs_local_oracle(actual, oracle);
        // The frozen forward/inverse fit is not algebraically exact for
        // extended chromaticities; the independent oracle above remains bit
        // exact, while this bound only characterizes its round-trip residual.
        EXPECT_NEAR(actual[0], xyy[0], 1.0e-5F);
        EXPECT_NEAR(actual[1], xyy[1], 1.0e-5F);
        EXPECT_NEAR(actual[2], xyy[2], 1.0e-5F);
    }

    constexpr FrozenD50Triplet extended_xyz{0.1938238604679151F, 0.36766030739017674F,
                                            0.38827863670090734F};
    const auto jch = dt_ucs::xyz_d50_to_jch(extended_xyz, white_lightness);
    expect_dt_ucs_local_oracle(jch, frozen_dt_ucs_xyz_d50_to_jch(extended_xyz, white_lightness));
    const auto roundtrip = dt_ucs::jch_to_xyz_d50(jch, white_lightness);
    expect_dt_ucs_local_oracle(roundtrip, frozen_dt_ucs_jch_to_xyz_d50(jch, white_lightness));
    for (std::size_t channel = 0U; channel < roundtrip.size(); ++channel)
    {
        EXPECT_NEAR(roundtrip[channel], extended_xyz[channel], 3.0e-7F);
    }

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    for (const float sample : {nan, infinity, -infinity})
    {
        const FrozenD50Triplet value{sample, sample, sample};
        const auto cat_forward = dt_ucs::xyz_d50_to_d65(value);
        const auto cat_inverse = dt_ucs::xyz_d65_to_d50(value);
        EXPECT_TRUE(std::ranges::all_of(cat_forward,
                                        [](const float item) { return !std::isfinite(item); }));
        EXPECT_TRUE(std::ranges::all_of(cat_inverse,
                                        [](const float item) { return !std::isfinite(item); }));
    }
    EXPECT_TRUE(std::isnan(dt_ucs::y_to_lightness(-1.0F)));
    EXPECT_TRUE(std::isnan(dt_ucs::y_to_lightness(nan)));
    EXPECT_TRUE(std::isnan(dt_ucs::y_to_lightness(infinity)));
    EXPECT_TRUE(std::isnan(dt_ucs::lightness_to_y(-1.0F)));
    EXPECT_TRUE(std::isnan(dt_ucs::lightness_to_y(nan)));

    const auto negative_luminance = dt_ucs::xyy_to_jch({0.25F, 0.4F, -1.0F}, white_lightness);
    EXPECT_TRUE(std::isnan(negative_luminance[0]));
    EXPECT_TRUE(std::isnan(negative_luminance[1]));
    EXPECT_TRUE(std::isfinite(negative_luminance[2]));
}

TEST(DtUcsBridgeTest, CommonXyzBoundaryIsIndependentOfRec709OrRec2020Coordinates)
{
    const auto apply_matrix = [](const std::array<float, 9> &matrix, const FrozenD50Triplet value)
    {
        return FrozenD50Triplet{
            frozen_dt_ucs_matrix_row(matrix[0], value[0], matrix[1], value[1], matrix[2], value[2]),
            frozen_dt_ucs_matrix_row(matrix[3], value[0], matrix[4], value[1], matrix[5], value[2]),
            frozen_dt_ucs_matrix_row(matrix[6], value[0], matrix[7], value[1], matrix[8],
                                     value[2])};
    };
    constexpr std::array<float, 9> rec709_to_xyz_d65{0.4124564F, 0.3575761F, 0.1804375F,
                                                     0.2126729F, 0.7151522F, 0.0721750F,
                                                     0.0193339F, 0.1191920F, 0.9503041F};
    constexpr std::array<float, 9> xyz_d65_to_rec2020{
        1.7166511880F, -0.3556707838F, -0.2533662814F, -0.6666843518F, 1.6164812366F,
        0.0157685458F, 0.0176398574F,  -0.0427706133F, 0.9421031212F};
    constexpr std::array<float, 9> rec2020_to_xyz_d65{0.6369580483F, 0.1446169036F, 0.1688809752F,
                                                      0.2627002120F, 0.6779980715F, 0.0593017165F,
                                                      0.0000000000F, 0.0280726930F, 1.0609850577F};
    constexpr FrozenD50Triplet rec709{0.15F, 0.4F, 0.8F};
    const auto xyz_from_rec709 = apply_matrix(rec709_to_xyz_d65, rec709);
    const auto rec2020 = apply_matrix(xyz_d65_to_rec2020, xyz_from_rec709);
    const auto xyz_from_rec2020 = apply_matrix(rec2020_to_xyz_d65, rec2020);
    for (std::size_t channel = 0U; channel < xyz_from_rec709.size(); ++channel)
    {
        EXPECT_NEAR(xyz_from_rec2020[channel], xyz_from_rec709[channel], 2.0e-7F);
    }

    const float white_lightness = frozen_dt_ucs_y_to_lightness(1.0F);
    const auto d50_from_rec709 = dt_ucs::xyz_d65_to_d50(xyz_from_rec709);
    const auto d50_from_rec2020 = dt_ucs::xyz_d65_to_d50(xyz_from_rec2020);
    const auto rec709_jch = dt_ucs::xyz_d50_to_jch(d50_from_rec709, white_lightness);
    const auto rec2020_jch = dt_ucs::xyz_d50_to_jch(d50_from_rec2020, white_lightness);
    expect_dt_ucs_local_oracle(rec709_jch,
                               frozen_dt_ucs_xyz_d50_to_jch(d50_from_rec709, white_lightness));
    expect_dt_ucs_local_oracle(rec2020_jch,
                               frozen_dt_ucs_xyz_d50_to_jch(d50_from_rec2020, white_lightness));
    for (std::size_t channel = 0U; channel < rec709_jch.size(); ++channel)
    {
        EXPECT_NEAR(rec2020_jch[channel], rec709_jch[channel], 6.0e-7F);
    }
}


} // namespace
} // namespace ravo
