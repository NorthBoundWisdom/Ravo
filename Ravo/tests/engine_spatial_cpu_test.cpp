#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

#include "ravo/recipe/develop.h"

#include "image_ops.h"
#include "image_ops_internal.h"
#include "parallel_rows.h"
#include "raw_pipeline.h"

namespace ravo
{
namespace
{

// The preceding implementation blurred three identical RGB lightness channels
// serially. Keep that reference separate from the single-plane row executor.
WorkingImage serial_clarity_reference(WorkingImage image, const double amount)
{
    using namespace image_ops_internal;
    const auto count = static_cast<std::size_t>(image.width) * image.height;
    std::vector<float> l(count), a(count), b(count), source(count * 3U), destination(count * 3U);
    for (std::size_t i = 0U; i < count; ++i)
    {
        rgb_to_lab(image.rgb[i * 3U], image.rgb[i * 3U + 1U], image.rgb[i * 3U + 2U], l[i], a[i],
                   b[i]);
        const float inverted = 100.0F - std::clamp(l[i], 0.0F, 100.0F);
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            source[i * 3U + channel] = inverted / 100.0F;
        }
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        for (const bool vertical : {false, true})
        {
            for (std::uint32_t y = 0U; y < image.height; ++y)
            {
                for (std::uint32_t x = 0U; x < image.width; ++x)
                {
                    for (std::size_t channel = 0U; channel < 3U; ++channel)
                    {
                        float sum = 0.0F;
                        for (int offset = -8; offset <= 8; ++offset)
                        {
                            const auto sx =
                                std::clamp(static_cast<int>(x) + (vertical ? 0 : offset), 0,
                                           static_cast<int>(image.width) - 1);
                            const auto sy =
                                std::clamp(static_cast<int>(y) + (vertical ? offset : 0), 0,
                                           static_cast<int>(image.height) - 1);
                            sum += source[(static_cast<std::size_t>(sy) * image.width +
                                           static_cast<std::size_t>(sx)) *
                                              3U +
                                          channel];
                        }
                        destination[(static_cast<std::size_t>(y) * image.width + x) * 3U +
                                    channel] = sum / 17.0F;
                    }
                }
            }
            source.swap(destination);
        }
    }
    const float contrast_scale = static_cast<float>(amount) * 3.75F;
    for (std::size_t i = 0U; i < count; ++i)
    {
        const float mixed = (source[i * 3U] * 100.0F + l[i]) - 100.0F;
        const float hipass = std::clamp(mixed * contrast_scale + 50.0F, 0.0F, 100.0F);
        lab_to_rgb(l[i] + (hipass - 50.0F), a[i], b[i], image.rgb[i * 3U], image.rgb[i * 3U + 1U],
                   image.rgb[i * 3U + 2U]);
    }
    return image;
}

TEST(EngineSpatialCpuTest, SinglePlaneClarityMatchesSerialRgbFloatBits)
{
    auto rows = detail::ScopedParallelRowSession::create();
    ASSERT_TRUE(rows) << rows.error().message;
    for (const auto [width, height] : std::array<std::pair<std::uint32_t, std::uint32_t>, 5>{
             {{1U, 1U}, {3U, 2U}, {17U, 31U}, {65U, 37U}, {33U, 97U}}})
    {
        WorkingImage input;
        input.width = width;
        input.height = height;
        input.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
        for (std::size_t i = 0U; i < input.rgb.size(); ++i)
        {
            input.rgb[i] = static_cast<float>(
                               (static_cast<std::uint32_t>(i) * 1664525U + 1013904223U) % 65521U) /
                               16384.0F -
                           0.2F;
        }
        for (const double amount : {-0.8, 0.25, 0.9})
        {
            const auto expected = serial_clarity_reference(input, amount);
            auto actual = input;
            ASSERT_TRUE(image_ops_internal::apply_clarity(actual, amount, {}));
            ASSERT_EQ(actual.rgb.size(), expected.rgb.size());
            for (std::size_t i = 0U; i < actual.rgb.size(); ++i)
            {
                ASSERT_EQ(std::bit_cast<std::uint32_t>(actual.rgb[i]),
                          std::bit_cast<std::uint32_t>(expected.rgb[i]))
                    << width << 'x' << height << " amount=" << amount << " sample=" << i;
            }
        }
        CancellationSource cancellation;
        ASSERT_TRUE(cancellation.cancel("spatial-test"));
        auto actual = input;
        auto clarified = image_ops_internal::apply_clarity(actual, 0.5, cancellation.token());
        ASSERT_FALSE(clarified);
        EXPECT_EQ(clarified.error().code, ErrorCode::kCancelled);
        EXPECT_EQ(actual.rgb, input.rgb);
    }
}

TEST(EngineSpatialCpuTest, ClarityPreflightAccountsForFiveFloatPlanes)
{
    DecodedRaw raw;
    raw.width = 65U;
    raw.height = 37U;
    Recipe recipe;
    const auto baseline = estimate_raw_render_memory(raw, recipe, raw.width, raw.height);
    recipe.operations.push_back({"ravo.detail.clarity",
                                 1,
                                 "clarity",
                                 true,
                                 {{"amount", ParameterValue{0.5}}},
                                 std::nullopt});
    const auto with_clarity = estimate_raw_render_memory(raw, recipe, raw.width, raw.height);
    EXPECT_EQ(with_clarity - baseline,
              static_cast<std::uint64_t>(raw.width) * raw.height * 5U * sizeof(float));
    recipe.operations.back().enabled = false;
    EXPECT_EQ(estimate_raw_render_memory(raw, recipe, raw.width, raw.height), baseline);
}

} // namespace
} // namespace ravo
