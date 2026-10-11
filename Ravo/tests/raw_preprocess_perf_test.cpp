#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

#include "ravo/recipe/develop.h"

#include "capability_ops_internal.h"

namespace ravo
{
namespace
{
float three_root_reference(const std::vector<float> &input, const DecodedRaw &raw,
                           const std::uint32_t row, const std::uint32_t col)
{
    const auto channel = [&](const std::uint32_t x, const std::uint32_t y)
    { return raw.cfa_channels[(y % raw.cfa_height) * raw.cfa_width + x % raw.cfa_width]; };
    const auto color = channel(col, row);
    std::array<float, 3> means{}, counts{};
    const int first_y = std::max(0, static_cast<int>(row) - 1);
    const int first_x = std::max(0, static_cast<int>(col) - 1);
    const int last_y = std::min(static_cast<int>(raw.height) - 1, static_cast<int>(row) + 2);
    const int last_x = std::min(static_cast<int>(raw.width) - 1, static_cast<int>(col) + 2);
    for (int y = first_y; y < last_y; ++y)
        for (int x = first_x; x < last_x; ++x)
        {
            const auto c = channel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            means[c] += std::max(
                0.0F, input[static_cast<std::size_t>(y) * raw.width + static_cast<std::size_t>(x)]);
            counts[c] += 1.0F;
        }
    for (std::size_t c = 0U; c < 3U; ++c)
        means[c] = counts[c] > 0.0F ? std::cbrt(means[c] / counts[c]) : 0.0F;
    const std::array<float, 3> opposed{0.5F * (means[1] + means[2]), 0.5F * (means[0] + means[2]),
                                       0.5F * (means[0] + means[1])};
    return opposed[color] * opposed[color] * opposed[color];
}

TEST(RawPreparationPerfTest, CfaSpecializationsMatchGenericModuloAtBoundaryCoordinates)
{
    for (const std::uint32_t period : {2U, 6U, 3U})
    {
        DecodedRaw raw;
        raw.cfa_width = raw.cfa_height = period;
        raw.cfa_channels.resize(period * period);
        for (std::uint32_t phase = 0U; phase < 4U; ++phase)
        {
            for (std::size_t i = 0U; i < raw.cfa_channels.size(); ++i)
                raw.cfa_channels[i] = static_cast<std::uint8_t>((i * 7U + i / period + phase) % 3U);
            for (const auto x :
                 {0U, 1U, 2U, 5U, 6U, 31U, std::numeric_limits<std::uint32_t>::max()})
                for (const auto y :
                     {0U, 1U, 2U, 5U, 6U, 33U, std::numeric_limits<std::uint32_t>::max()})
                    EXPECT_EQ(capability_internal::cfa_channel(raw, x, y),
                              raw.cfa_channels[(y % period) * period + x % period]);
        }
    }
}

TEST(RawPreparationPerfTest, OpposedAverageMatchesOriginalThreeRootFloatBits)
{
    for (const std::uint32_t period : {2U, 6U, 3U})
        for (const auto [width, height] : std::array<std::pair<std::uint32_t, std::uint32_t>, 4>{
                 {{1U, 1U}, {3U, 2U}, {7U, 11U}, {33U, 65U}}})
        {
            DecodedRaw raw;
            raw.width = width;
            raw.height = height;
            raw.cfa_width = raw.cfa_height = period;
            raw.cfa_channels.resize(period * period);
            for (std::size_t i = 0U; i < raw.cfa_channels.size(); ++i)
                raw.cfa_channels[i] = static_cast<std::uint8_t>((i * 7U + i / period) % 3U);
            std::vector<float> input(static_cast<std::size_t>(width) * height);
            for (std::size_t i = 0U; i < input.size(); ++i)
                input[i] = static_cast<float>(
                               (static_cast<std::uint32_t>(i) * 1664525U + 1013904223U) % 65521U) /
                               8192.0F -
                           0.2F;
            for (std::uint32_t y = 0U; y < height; ++y)
                for (std::uint32_t x = 0U; x < width; ++x)
                {
                    const auto actual =
                        capability_internal::calc_refavg(input, raw, width, height, y, x);
                    ASSERT_EQ(std::bit_cast<std::uint32_t>(actual),
                              std::bit_cast<std::uint32_t>(three_root_reference(input, raw, y, x)))
                        << period << " at " << x << ',' << y;
                }
            const auto original = input;
            CancellationSource cancellation;
            ASSERT_TRUE(cancellation.cancel("opposed-scan"));
            auto rejected = capability_internal::process_highlights_opposed(
                input, raw, {1.0F, 1.0F, 1.0F}, cancellation.token());
            ASSERT_FALSE(rejected);
            EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
            EXPECT_EQ(input, original);
        }
}
} // namespace
} // namespace ravo
