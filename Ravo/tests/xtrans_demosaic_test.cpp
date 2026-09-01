#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ravo/recipe/color_input.h"
#include "ravo/recipe/operation.h"
#include "xtrans_demosaic.h"

namespace ravo
{
namespace
{

constexpr std::array<std::uint8_t, 36> kXTransCfa{1U, 2U, 1U, 1U, 0U, 1U, 0U, 1U, 0U, 2U, 1U, 2U,
                                                  1U, 2U, 1U, 1U, 0U, 1U, 1U, 0U, 1U, 1U, 2U, 1U,
                                                  2U, 1U, 2U, 0U, 1U, 0U, 1U, 0U, 1U, 1U, 2U, 1U};

[[nodiscard]] DecodedRaw smooth_xtrans(const std::uint32_t width = 96U,
                                       const std::uint32_t height = 96U)
{
    DecodedRaw raw;
    raw.width = width;
    raw.height = height;
    raw.cfa_width = 6U;
    raw.cfa_height = 6U;
    raw.cfa_channels.assign(kXTransCfa.begin(), kXTransCfa.end());
    raw.black_level = 0;
    raw.white_level = 65535U;
    raw.has_as_shot_white_balance = true;
    raw.as_shot_white_balance = {1.0F, 1.0F, 1.0F, 1.0F};
    raw.color_profile.kind = ColorProfileKind::kMatrix;
    raw.color_profile.model = ColorModel::kRgb;
    raw.color_profile.identifier = "synthetic-xtrans-camera";
    raw.color_profile.has_matrix = true;
    raw.color_profile.camera_input = true;
    raw.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t row = 0U; row < height; ++row)
    {
        for (std::uint32_t column = 0U; column < width; ++column)
        {
            const std::uint8_t channel = kXTransCfa[(row % 6U) * 6U + (column % 6U)];
            const float x = static_cast<float>(column) / static_cast<float>(width - 1U);
            const float y = static_cast<float>(row) / static_cast<float>(height - 1U);
            const std::array<float, 3> rgb{0.08F + 0.72F * x, 0.12F + 0.66F * y,
                                           0.06F + 0.36F * (x + y)};
            raw.pixels[static_cast<std::size_t>(row) * width + column] =
                static_cast<std::uint16_t>(std::lround(rgb[channel] * 65535.0F));
        }
    }
    return raw;
}

[[nodiscard]] std::array<float, 3> expected_smooth(const std::uint32_t column,
                                                   const std::uint32_t row,
                                                   const std::uint32_t width,
                                                   const std::uint32_t height)
{
    const float x = static_cast<float>(column) / static_cast<float>(width - 1U);
    const float y = static_cast<float>(row) / static_cast<float>(height - 1U);
    return {0.08F + 0.72F * x, 0.12F + 0.66F * y, 0.06F + 0.36F * (x + y)};
}

[[nodiscard]] double interior_error(const WorkingImage &image)
{
    double error = 0.0;
    std::uint64_t count = 0U;
    for (std::uint32_t row = 18U; row + 18U < image.height; ++row)
    {
        for (std::uint32_t column = 18U; column + 18U < image.width; ++column)
        {
            const auto expected = expected_smooth(column, row, image.width, image.height);
            const std::size_t base = (static_cast<std::size_t>(row) * image.width + column) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                error +=
                    std::abs(static_cast<double>(image.rgb[base + channel]) - expected[channel]);
                ++count;
            }
        }
    }
    return error / static_cast<double>(count);
}

[[nodiscard]] std::uint64_t quantized_image_hash(const WorkingImage &image)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float sample : image.rgb)
    {
        const auto quantized =
            static_cast<std::uint16_t>(std::lround(std::clamp(sample, 0.0F, 1.0F) * 65535.0F));
        hash ^= static_cast<std::uint8_t>(quantized & 0xffU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint8_t>(quantized >> 8U);
        hash *= 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] Recipe input_recipe()
{
    Recipe recipe;
    recipe.asset = {"synthetic-xtrans", "memory:raw", std::nullopt};
    recipe.operations.push_back({"ravo.color.input", 1, "input", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    return recipe;
}

TEST(XTransDemosaicTest, MarkesteijnModesPreserveSamplesAndSmoothSceneColor)
{
    const DecodedRaw raw = smooth_xtrans();
    const auto source = raw.pixels;
    auto one = demosaic_xtrans(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                               XTransDemosaicMode::kMarkesteijn1, CancellationToken{});
    auto three = demosaic_xtrans(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                                 XTransDemosaicMode::kMarkesteijn3, CancellationToken{});
    ASSERT_TRUE(one) << one.error().message;
    ASSERT_TRUE(three) << three.error().message;
    EXPECT_EQ(raw.pixels, source);
    EXPECT_NE(one.value().rgb, three.value().rgb);
    EXPECT_LT(interior_error(one.value()), 0.015);
    EXPECT_LT(interior_error(three.value()), 0.015);
    for (const auto *image : {&one.value(), &three.value()})
    {
        EXPECT_TRUE(std::all_of(image->rgb.begin(), image->rgb.end(),
                                [](const float sample) { return std::isfinite(sample); }));
        for (std::uint32_t row = 0U; row < raw.height; ++row)
        {
            for (std::uint32_t column = 0U; column < raw.width; ++column)
            {
                const std::uint8_t channel = kXTransCfa[(row % 6U) * 6U + (column % 6U)];
                const float expected =
                    static_cast<float>(
                        raw.pixels[static_cast<std::size_t>(row) * raw.width + column]) /
                    65535.0F;
                const std::size_t index =
                    (static_cast<std::size_t>(row) * raw.width + column) * 3U + channel;
                EXPECT_NEAR(image->rgb[index], expected, 1.0e-6F) << column << ',' << row;
            }
        }
    }
}

TEST(XTransDemosaicTest, PreviewIsDeterministicFiniteAndSourceOwned)
{
    const DecodedRaw raw = smooth_xtrans(192U, 144U);
    const auto source = raw.pixels;
    auto first = demosaic_xtrans(raw, 96U, 72U, {1.2F, 1.0F, 1.4F, 1.0F},
                                 XTransDemosaicMode::kMarkesteijn3, CancellationToken{});
    auto second = demosaic_xtrans(raw, 96U, 72U, {1.2F, 1.0F, 1.4F, 1.0F},
                                  XTransDemosaicMode::kMarkesteijn3, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(first.value().rgb, second.value().rgb);
    EXPECT_EQ(raw.pixels, source);
    EXPECT_TRUE(std::all_of(first.value().rgb.begin(), first.value().rgb.end(),
                            [](const float sample) { return std::isfinite(sample); }));
}

TEST(XTransDemosaicTest, SensorDefaultAndExplicitModesHaveStrictBoundaries)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const DecodedRaw raw = smooth_xtrans();
    Recipe automatic = input_recipe();
    auto default_result = engine.value().linear_working_from_raw(raw, automatic, raw.width,
                                                                 raw.height, CancellationToken{});
    ASSERT_TRUE(default_result) << default_result.error().message;

    Recipe one_pass = automatic;
    one_pass.operations.insert(one_pass.operations.begin(),
                               {std::string(kDemosaicOperationId),
                                1,
                                "demosaic-markesteijn1",
                                true,
                                {{"mode", ParameterValue{std::string(kDemosaicModeMarkesteijn1)}}},
                                std::nullopt});
    auto one_result = engine.value().linear_working_from_raw(raw, one_pass, raw.width, raw.height,
                                                             CancellationToken{});
    ASSERT_TRUE(one_result) << one_result.error().message;
    EXPECT_NE(default_result.value().rgb, one_result.value().rgb);

    Recipe mismatch = automatic;
    mismatch.operations.insert(mismatch.operations.begin(),
                               {std::string(kDemosaicOperationId),
                                1,
                                "demosaic-rcd",
                                true,
                                {{"mode", ParameterValue{std::string(kDemosaicModeRcd)}}},
                                std::nullopt});
    auto rejected = engine.value().linear_working_from_raw(raw, mismatch, raw.width, raw.height,
                                                           CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "demosaic_sensor_mismatch");
    EXPECT_EQ(rejected.error().context.at("sensor"), "xtrans");
}

TEST(XTransDemosaicTest, DecodeAndRealFixtureHaveFrozenCfaAndPreviewGolden)
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" /
                      "mire1-xtrans.raf";
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto raw = engine.value().decode_raw_frame(path.string(), CancellationToken{});
    ASSERT_TRUE(raw) << raw.error().message;
    EXPECT_EQ(raw.value().cfa_width, 6U);
    EXPECT_EQ(raw.value().cfa_height, 6U);
    EXPECT_EQ(std::count(raw.value().cfa_channels.begin(), raw.value().cfa_channels.end(), 0U), 8);
    EXPECT_EQ(std::count(raw.value().cfa_channels.begin(), raw.value().cfa_channels.end(), 1U), 20);
    EXPECT_EQ(std::count(raw.value().cfa_channels.begin(), raw.value().cfa_channels.end(), 2U), 8);
    const auto source = raw.value().pixels;
    auto preview = demosaic_xtrans(raw.value(), 320U, 214U, {1.0F, 1.0F, 1.0F, 1.0F},
                                   XTransDemosaicMode::kMarkesteijn3, CancellationToken{});
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(raw.value().pixels, source);
    EXPECT_EQ(quantized_image_hash(preview.value()), 16117879839220596880ULL);
}

TEST(XTransDemosaicTest, InvalidModeCfaCancellationAndMemoryAreExplicit)
{
    auto mode = parse_xtrans_demosaic_mode("vng");
    ASSERT_FALSE(mode);
    EXPECT_EQ(mode.error().context.at("reason"), "unsupported_demosaic_mode");

    DecodedRaw invalid = smooth_xtrans();
    invalid.cfa_channels[0] = 0U;
    auto rejected =
        demosaic_xtrans(invalid, invalid.width, invalid.height, {1.0F, 1.0F, 1.0F, 1.0F},
                        XTransDemosaicMode::kMarkesteijn3, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_xtrans_cfa");

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("xtrans-test"));
    const DecodedRaw raw = smooth_xtrans();
    auto cancelled = demosaic_xtrans(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                                     XTransDemosaicMode::kMarkesteijn1, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_GT(estimate_xtrans_demosaic_memory(640U, 480U, XTransDemosaicMode::kMarkesteijn3),
              estimate_xtrans_demosaic_memory(640U, 480U, XTransDemosaicMode::kMarkesteijn1));
}

} // namespace
} // namespace ravo
