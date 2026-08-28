#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "ravo/foundation/cancellation.h"

#include "recursive_gaussian.h"

namespace ravo
{
namespace
{

constexpr float kFrozenMinimum = -1.0e9F;
constexpr float kFrozenMaximum = 1.0e9F;

struct OracleCoefficients
{
    float a0 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;
    float a3 = 0.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float coefp = 0.0F;
    float coefn = 0.0F;
};

[[nodiscard]] float frozen_clamp(const float value) noexcept
{
    return value >= kFrozenMinimum ? (value <= kFrozenMaximum ? value : kFrozenMaximum) :
                                     kFrozenMinimum;
}

[[nodiscard]] OracleCoefficients source_order_coefficients(const float sigma)
{
    const float alpha = 1.695F / sigma;
    const float ema = std::exp(-alpha);
    const float ema2 = std::exp(-2.0F * alpha);
    OracleCoefficients result;
    result.b1 = -2.0F * ema;
    result.b2 = ema2;
    const float k = (1.0F - ema) * (1.0F - ema) / (1.0F + (2.0F * alpha * ema) - ema2);
    result.a0 = k;
    result.a1 = k * (alpha - 1.0F) * ema;
    result.a2 = k * (alpha + 1.0F) * ema;
    result.a3 = -k * ema2;
    result.coefp = (result.a0 + result.a1) / (1.0F + result.b1 + result.b2);
    result.coefn = (result.a2 + result.a3) / (1.0F + result.b1 + result.b2);
    return result;
}

[[nodiscard]] std::vector<float> source_order_gaussian_oracle(std::vector<float> signal,
                                                              const std::uint32_t width,
                                                              const std::uint32_t height,
                                                              const float sigma,
                                                              const bool clamp_reads = true)
{
    const auto coefficients = source_order_coefficients(sigma);
    std::vector<float> scratch(signal.size());
    const auto clamp = [clamp_reads](const float value) noexcept
    { return clamp_reads ? frozen_clamp(value) : value; };

    for (std::uint32_t column = 0U; column < width; ++column)
    {
        float xp[2]{};
        float yb[2]{};
        float yp[2]{};
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xp[channel] = clamp(signal[static_cast<std::size_t>(column) * 2U + channel]);
            yb[channel] = xp[channel] * coefficients.coefp;
            yp[channel] = yb[channel];
        }

        float xc[2]{};
        float yc[2]{};
        float xn[2]{};
        float xa[2]{};
        float yn[2]{};
        float ya[2]{};
        for (std::uint32_t row = 0U; row < height; ++row)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = clamp(signal[offset + channel]);
                yc[channel] = (coefficients.a0 * xc[channel]) + (coefficients.a1 * xp[channel]) -
                              (coefficients.b1 * yp[channel]) - (coefficients.b2 * yb[channel]);
                scratch[offset + channel] = yc[channel];
                xp[channel] = xc[channel];
                yb[channel] = yp[channel];
                yp[channel] = yc[channel];
            }
        }

        const std::size_t last_offset =
            (static_cast<std::size_t>(height - 1U) * width + column) * 2U;
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xn[channel] = clamp(signal[last_offset + channel]);
            xa[channel] = xn[channel];
            yn[channel] = xn[channel] * coefficients.coefn;
            ya[channel] = yn[channel];
        }
        for (std::uint32_t row = height; row > 0U; --row)
        {
            const std::size_t offset = (static_cast<std::size_t>(row - 1U) * width + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = clamp(signal[offset + channel]);
                yc[channel] = (coefficients.a2 * xn[channel]) + (coefficients.a3 * xa[channel]) -
                              (coefficients.b1 * yn[channel]) - (coefficients.b2 * ya[channel]);
                xa[channel] = xn[channel];
                xn[channel] = xc[channel];
                ya[channel] = yn[channel];
                yn[channel] = yc[channel];
                scratch[offset + channel] += yc[channel];
            }
        }
    }

    for (std::uint32_t row = 0U; row < height; ++row)
    {
        float xp[2]{};
        float yb[2]{};
        float yp[2]{};
        const std::size_t first_offset = static_cast<std::size_t>(row) * width * 2U;
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xp[channel] = clamp(scratch[first_offset + channel]);
            yb[channel] = xp[channel] * coefficients.coefp;
            yp[channel] = yb[channel];
        }

        float xc[2]{};
        float yc[2]{};
        float xn[2]{};
        float xa[2]{};
        float yn[2]{};
        float ya[2]{};
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + column) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = clamp(scratch[offset + channel]);
                yc[channel] = (coefficients.a0 * xc[channel]) + (coefficients.a1 * xp[channel]) -
                              (coefficients.b1 * yp[channel]) - (coefficients.b2 * yb[channel]);
                signal[offset + channel] = yc[channel];
                xp[channel] = xc[channel];
                yb[channel] = yp[channel];
                yp[channel] = yc[channel];
            }
        }

        const std::size_t last_offset = (static_cast<std::size_t>(row + 1U) * width - 1U) * 2U;
        for (std::size_t channel = 0U; channel < 2U; ++channel)
        {
            xn[channel] = clamp(scratch[last_offset + channel]);
            xa[channel] = xn[channel];
            yn[channel] = xn[channel] * coefficients.coefn;
            ya[channel] = yn[channel];
        }
        for (std::uint32_t column = width; column > 0U; --column)
        {
            const std::size_t offset = (static_cast<std::size_t>(row) * width + (column - 1U)) * 2U;
            for (std::size_t channel = 0U; channel < 2U; ++channel)
            {
                xc[channel] = clamp(scratch[offset + channel]);
                yc[channel] = (coefficients.a2 * xn[channel]) + (coefficients.a3 * xa[channel]) -
                              (coefficients.b1 * yn[channel]) - (coefficients.b2 * ya[channel]);
                xa[channel] = xn[channel];
                xn[channel] = xc[channel];
                ya[channel] = yn[channel];
                yn[channel] = yc[channel];
                signal[offset + channel] += yc[channel];
            }
        }
    }
    return signal;
}

void expect_same_bits(const std::vector<float> &actual, const std::vector<float> &expected)
{
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0U; index < actual.size(); ++index)
    {
        EXPECT_EQ(std::bit_cast<std::uint32_t>(actual[index]),
                  std::bit_cast<std::uint32_t>(expected[index]))
            << index;
    }
}

struct GaussianCancellation
{
    CancellationSource *source = nullptr;
    detail::RecursiveGaussianCheckpoint target =
        detail::RecursiveGaussianCheckpoint::kBeforeValidation;
    std::uint32_t minimum_progress = 0U;
    bool fired = false;
};

void cancel_gaussian(void *const context, const detail::RecursiveGaussianCheckpoint checkpoint,
                     const std::uint32_t progress) noexcept
{
    auto &state = *static_cast<GaussianCancellation *>(context);
    if (!state.fired && checkpoint == state.target && progress >= state.minimum_progress)
    {
        state.fired = state.source->cancel("recursive-gaussian-checkpoint");
    }
}

TEST(RecursiveGaussianTest, MatchesIndependentSourceOrderOracleForBoundaryConstantAndImpulseVectors)
{
    struct Case
    {
        std::uint32_t width;
        std::uint32_t height;
        float sigma;
        std::vector<float> signal;
    };
    const std::vector<Case> cases{
        {1U, 1U, 1.5F, {0.25F, -0.75F}},
        {2U, 1U, 2.25F, {0.5F, -0.25F, 0.5F, -0.25F}},
        {1U, 3U, 3.0F, {0.0F, 0.0F, 1.0F, -0.5F, 0.0F, 0.0F}},
        {3U, 2U, 4.75F, {0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, -0.5F, 0.0F, 0.0F}},
    };

    for (const auto &test_case : cases)
    {
        const auto original = test_case.signal;
        const auto expected = source_order_gaussian_oracle(test_case.signal, test_case.width,
                                                           test_case.height, test_case.sigma);
        const auto actual =
            detail::recursive_gaussian_zero_2c(test_case.signal, test_case.width, test_case.height,
                                               test_case.sigma, CancellationToken{});
        ASSERT_TRUE(actual) << actual.error().message;
        expect_same_bits(actual.value(), expected);
        expect_same_bits(test_case.signal, original);
    }
}

TEST(RecursiveGaussianTest, PreservesFrozenPerReadPlusMinusOneBillionClampForExtendedSignals)
{
    const std::vector<float> signal{1.25e9F, -1.25e9F, 0.25F, -0.5F,
                                    -1.5e9F, 1.5e9F,   0.75F, 0.125F};
    const auto expected = source_order_gaussian_oracle(signal, 2U, 2U, 2.0F, true);
    const auto unclamped = source_order_gaussian_oracle(signal, 2U, 2U, 2.0F, false);
    const auto actual =
        detail::recursive_gaussian_zero_2c(signal, 2U, 2U, 2.0F, CancellationToken{});
    ASSERT_TRUE(actual) << actual.error().message;
    expect_same_bits(actual.value(), expected);

    bool differs_from_unclamped = false;
    for (std::size_t index = 0U; index < actual.value().size(); ++index)
    {
        differs_from_unclamped |= std::bit_cast<std::uint32_t>(actual.value()[index]) !=
                                  std::bit_cast<std::uint32_t>(unclamped[index]);
    }
    EXPECT_TRUE(differs_from_unclamped);
}

TEST(RecursiveGaussianTest, RejectsInvalidDimensionsBuffersSignalsAndSigmaWithoutPublishing)
{
    const std::vector<float> signal{0.25F, -0.5F};
    const auto expect_reason = [&](std::vector<float> input, const std::uint32_t width,
                                   const std::uint32_t height, const float sigma,
                                   const std::string_view reason)
    {
        const auto original = input;
        const auto result =
            detail::recursive_gaussian_zero_2c(input, width, height, sigma, CancellationToken{});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().context.at("reason"), reason);
        ASSERT_EQ(input.size(), original.size());
        for (std::size_t index = 0U; index < input.size(); ++index)
        {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(input[index]),
                      std::bit_cast<std::uint32_t>(original[index]));
        }
    };

    expect_reason(signal, 0U, 1U, 1.0F, "invalid_recursive_gaussian_dimensions");
    expect_reason(signal, 1U, 0U, 1.0F, "invalid_recursive_gaussian_dimensions");
    expect_reason(signal, std::numeric_limits<std::uint32_t>::max(),
                  std::numeric_limits<std::uint32_t>::max(), 1.0F,
                  "recursive_gaussian_dimensions_overflow");
    expect_reason({0.25F}, 1U, 1U, 1.0F, "invalid_recursive_gaussian_buffer");
    expect_reason({std::numeric_limits<float>::quiet_NaN(), 0.0F}, 1U, 1U, 1.0F,
                  "nonfinite_recursive_gaussian_signal");
    expect_reason(signal, 1U, 1U, 0.0F, "invalid_recursive_gaussian_sigma");
    expect_reason(signal, 1U, 1U, std::numeric_limits<float>::infinity(),
                  "invalid_recursive_gaussian_sigma");
    EXPECT_EQ(detail::recursive_gaussian_zero_2c_bytes(std::numeric_limits<std::uint32_t>::max(),
                                                       std::numeric_limits<std::uint32_t>::max()),
              std::numeric_limits<std::uint64_t>::max());
}

TEST(RecursiveGaussianTest, CancelsAtPreVerticalHorizontalAndPublicationCheckpointsWithoutResult)
{
    const auto run = [](const std::uint32_t width, const std::uint32_t height,
                        const detail::RecursiveGaussianCheckpoint checkpoint,
                        const std::uint32_t minimum_progress)
    {
        std::vector<float> signal(static_cast<std::size_t>(width) * height * 2U, 0.25F);
        const auto original = signal;
        CancellationSource cancellation;
        GaussianCancellation state{&cancellation, checkpoint, minimum_progress};
        const auto result = detail::recursive_gaussian_zero_2c(
            signal, width, height, 2.0F, cancellation.token(), {&state, cancel_gaussian});
        ASSERT_FALSE(result);
        EXPECT_TRUE(state.fired);
        EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(signal, original);
    };

    run(8U, 8U, detail::RecursiveGaussianCheckpoint::kBeforeValidation, 0U);
    run(1U, 129U, detail::RecursiveGaussianCheckpoint::kVerticalRow, 64U);
    run(129U, 1U, detail::RecursiveGaussianCheckpoint::kHorizontalColumn, 64U);
    run(2U, 2U, detail::RecursiveGaussianCheckpoint::kBeforePublication, 0U);
}

} // namespace
} // namespace ravo
