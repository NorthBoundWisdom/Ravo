#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

#include "bayer_demosaic.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

namespace ravo
{
namespace
{

[[nodiscard]] DecodedRaw smooth_bayer(const std::uint32_t width = 64U,
                                      const std::uint32_t height = 64U)
{
    DecodedRaw raw;
    raw.width = width;
    raw.height = height;
    raw.cfa_width = 2U;
    raw.cfa_height = 2U;
    raw.cfa_channels = {0U, 1U, 1U, 2U};
    raw.black_level = 0U;
    raw.white_level = 65535U;
    raw.has_as_shot_white_balance = true;
    raw.as_shot_white_balance = {1.0F, 1.0F, 1.0F, 1.0F};
    raw.color_profile.kind = ColorProfileKind::kMatrix;
    raw.color_profile.model = ColorModel::kRgb;
    raw.color_profile.identifier = "synthetic-camera";
    raw.color_profile.has_matrix = true;
    raw.color_profile.camera_input = true;
    raw.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::uint8_t channel = raw.cfa_channels[(y & 1U) * 2U + (x & 1U)];
            const float xf = static_cast<float>(x) / static_cast<float>(width - 1U);
            const float yf = static_cast<float>(y) / static_cast<float>(height - 1U);
            const std::array<float, 3> rgb{0.08F + 0.72F * xf,
                                           0.12F + 0.66F * yf,
                                           0.06F + 0.36F * (xf + yf)};
            raw.pixels[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::uint16_t>(std::lround(rgb[channel] * 65535.0F));
        }
    }
    return raw;
}

[[nodiscard]] std::array<float, 3> expected_smooth(const std::uint32_t x,
                                                   const std::uint32_t y,
                                                   const std::uint32_t width,
                                                   const std::uint32_t height)
{
    const float xf = static_cast<float>(x) / static_cast<float>(width - 1U);
    const float yf = static_cast<float>(y) / static_cast<float>(height - 1U);
    return {0.08F + 0.72F * xf, 0.12F + 0.66F * yf, 0.06F + 0.36F * (xf + yf)};
}

[[nodiscard]] double interior_mean_absolute_error(const WorkingImage &image)
{
    double error = 0.0;
    std::uint64_t count = 0U;
    for (std::uint32_t y = 10U; y + 10U < image.height; ++y)
    {
        for (std::uint32_t x = 10U; x + 10U < image.width; ++x)
        {
            const auto expected = expected_smooth(x, y, image.width, image.height);
            const std::size_t base = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                error += std::abs(static_cast<double>(image.rgb[base + channel]) -
                                  expected[channel]);
                ++count;
            }
        }
    }
    return error / static_cast<double>(count);
}

[[nodiscard]] DecodedRaw monochrome_edge_bayer()
{
    DecodedRaw raw = smooth_bayer(96U, 96U);
    for (std::uint32_t y = 0U; y < raw.height; ++y)
    {
        for (std::uint32_t x = 0U; x < raw.width; ++x)
        {
            const float edge = x + y / 3U < 52U ? 0.08F : 0.82F;
            raw.pixels[static_cast<std::size_t>(y) * raw.width + x] =
                static_cast<std::uint16_t>(std::lround(edge * 65535.0F));
        }
    }
    return raw;
}

[[nodiscard]] double mean_false_colour(const WorkingImage &image)
{
    double total = 0.0;
    std::uint64_t count = 0U;
    for (std::uint32_t y = 10U; y + 10U < image.height; ++y)
    {
        for (std::uint32_t x = 10U; x + 10U < image.width; ++x)
        {
            const std::size_t base = (static_cast<std::size_t>(y) * image.width + x) * 3U;
            const auto [minimum, maximum] = std::minmax(
                {image.rgb[base], image.rgb[base + 1U], image.rgb[base + 2U]});
            total += static_cast<double>(maximum - minimum);
            ++count;
        }
    }
    return total / static_cast<double>(count);
}

[[nodiscard]] std::uint64_t quantized_image_hash(const WorkingImage &image)
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const float sample : image.rgb)
    {
        const auto quantized = static_cast<std::uint16_t>(
            std::lround(std::clamp(sample, 0.0F, 1.0F) * 65535.0F));
        hash ^= static_cast<std::uint8_t>(quantized & 0xffU);
        hash *= 1099511628211ULL;
        hash ^= static_cast<std::uint8_t>(quantized >> 8U);
        hash *= 1099511628211ULL;
    }
    return hash;
}

TEST(BayerDemosaicTest, RcdAndPpgPreserveCfaSamplesAndSmoothSceneColor)
{
    const DecodedRaw raw = smooth_bayer();
    const auto original = raw.pixels;
    auto rcd = demosaic_bayer(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                              BayerDemosaicMode::kRcd, CancellationToken{});
    auto ppg = demosaic_bayer(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                              BayerDemosaicMode::kPpg, CancellationToken{});
    ASSERT_TRUE(rcd) << rcd.error().message;
    ASSERT_TRUE(ppg) << ppg.error().message;
    EXPECT_EQ(raw.pixels, original);
    EXPECT_NE(rcd.value().rgb, ppg.value().rgb);
    EXPECT_LT(interior_mean_absolute_error(rcd.value()), 0.002);
    EXPECT_LT(interior_mean_absolute_error(ppg.value()), 0.002);

    for (std::uint32_t y = 0U; y < raw.height; ++y)
    {
        for (std::uint32_t x = 0U; x < raw.width; ++x)
        {
            const std::uint8_t channel = raw.cfa_channels[(y & 1U) * 2U + (x & 1U)];
            const float expected = static_cast<float>(
                                       raw.pixels[static_cast<std::size_t>(y) * raw.width + x]) /
                                   65535.0F;
            const std::size_t index =
                (static_cast<std::size_t>(y) * raw.width + x) * 3U + channel;
            EXPECT_FLOAT_EQ(rcd.value().rgb[index], expected) << x << ',' << y;
            EXPECT_FLOAT_EQ(ppg.value().rgb[index], expected) << x << ',' << y;
        }
    }
}

TEST(BayerDemosaicTest, PreviewReductionIsDeterministicFiniteAndSourceOwned)
{
    const DecodedRaw raw = smooth_bayer(96U, 64U);
    const auto source = raw.pixels;
    auto first = demosaic_bayer(raw, 48U, 32U, {1.2F, 1.0F, 1.4F, 1.0F},
                                BayerDemosaicMode::kRcd, CancellationToken{});
    auto second = demosaic_bayer(raw, 48U, 32U, {1.2F, 1.0F, 1.4F, 1.0F},
                                 BayerDemosaicMode::kRcd, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(first.value().rgb, second.value().rgb);
    EXPECT_EQ(raw.pixels, source);
    EXPECT_TRUE(std::all_of(first.value().rgb.begin(), first.value().rgb.end(),
                            [](const float sample) { return std::isfinite(sample); }));
}

TEST(BayerDemosaicTest, RcdAndPpgBoundFalseColourOnAHighContrastMonochromeEdge)
{
    const DecodedRaw raw = monochrome_edge_bayer();
    auto rcd = demosaic_bayer(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                              BayerDemosaicMode::kRcd, CancellationToken{});
    auto ppg = demosaic_bayer(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                              BayerDemosaicMode::kPpg, CancellationToken{});
    ASSERT_TRUE(rcd) << rcd.error().message;
    ASSERT_TRUE(ppg) << ppg.error().message;
    EXPECT_LT(mean_false_colour(rcd.value()), 0.012);
    EXPECT_LT(mean_false_colour(ppg.value()), 0.018);
    for (const auto *image : {&rcd.value(), &ppg.value()})
    {
        EXPECT_TRUE(std::all_of(image->rgb.begin(), image->rgb.end(), [](const float sample)
                                { return std::isfinite(sample); }));
        const auto [minimum, maximum] = std::minmax_element(image->rgb.begin(), image->rgb.end());
        EXPECT_GE(*minimum, 0.0F);
        EXPECT_LE(*maximum, 1.25F);
    }
}

TEST(BayerDemosaicTest, RealCanonRawHasFrozenRcdAndPpgGoldens)
{
    const auto input = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                       "images" / "mire1.cr2";
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto raw = engine.value().decode_raw_frame(input.string(), CancellationToken{});
    ASSERT_TRUE(raw) << raw.error().message;
    const auto original = raw.value().pixels;
    auto rcd = demosaic_bayer(raw.value(), 320U, 213U, {1.0F, 1.0F, 1.0F, 1.0F},
                              BayerDemosaicMode::kRcd, CancellationToken{});
    auto ppg = demosaic_bayer(raw.value(), 320U, 213U, {1.0F, 1.0F, 1.0F, 1.0F},
                              BayerDemosaicMode::kPpg, CancellationToken{});
    ASSERT_TRUE(rcd) << rcd.error().message;
    ASSERT_TRUE(ppg) << ppg.error().message;
    EXPECT_EQ(raw.value().pixels, original);
    EXPECT_EQ(quantized_image_hash(rcd.value()), 6825562484246184936ULL);
    EXPECT_EQ(quantized_image_hash(ppg.value()), 17162427211048796534ULL);
    EXPECT_NE(quantized_image_hash(rcd.value()), quantized_image_hash(ppg.value()));
}

TEST(BayerDemosaicTest, RejectsUnsupportedModeAndCfaWithoutFallback)
{
    auto mode = parse_bayer_demosaic_mode("igv");
    ASSERT_FALSE(mode);
    EXPECT_EQ(mode.error().context.at("reason"), "unsupported_demosaic_mode");

    DecodedRaw raw = smooth_bayer();
    raw.cfa_channels = {0U, 1U, 2U, 2U};
    auto result = demosaic_bayer(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                                 BayerDemosaicMode::kRcd, CancellationToken{});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "unsupported_bayer_cfa");
}

TEST(BayerDemosaicTest, CancellationAndMemoryEstimateAreExplicit)
{
    const DecodedRaw raw = smooth_bayer();
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("demosaic-test"));
    auto result = demosaic_bayer(raw, raw.width, raw.height, {1.0F, 1.0F, 1.0F, 1.0F},
                                 BayerDemosaicMode::kPpg, cancellation.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
    EXPECT_GT(estimate_bayer_demosaic_memory(64U, 64U, BayerDemosaicMode::kRcd),
              estimate_bayer_demosaic_memory(64U, 64U, BayerDemosaicMode::kPpg));
}

TEST(BayerDemosaicTest, RecipeSelectsPpgAndRejectsDuplicateEnabledSelection)
{
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const DecodedRaw raw = smooth_bayer();
    Recipe rcd_recipe;
    rcd_recipe.asset = {"synthetic", "memory:raw", std::nullopt};
    rcd_recipe.operations.push_back(
        {"ravo.color.input", 1, "input", true,
         input_color_to_parameters(InputColorParams{}), std::nullopt});
    DevelopParams ppg_develop;
    ppg_develop.demosaic_mode = std::string(kDemosaicModePpg);
    auto ppg_recipe_result =
        recipe_from_develop({"synthetic", "memory:raw", std::nullopt}, ppg_develop);
    ASSERT_TRUE(ppg_recipe_result) << ppg_recipe_result.error().message;
    Recipe ppg_recipe = std::move(ppg_recipe_result).value();
    auto rcd = engine.value().linear_working_from_raw(raw, rcd_recipe, 64U, 64U,
                                                      CancellationToken{});
    auto ppg = engine.value().linear_working_from_raw(raw, ppg_recipe, 64U, 64U,
                                                      CancellationToken{});
    ASSERT_TRUE(rcd) << rcd.error().message;
    ASSERT_TRUE(ppg) << ppg.error().message;
    EXPECT_NE(rcd.value().rgb, ppg.value().rgb);

    ppg_recipe.operations.insert(
        ppg_recipe.operations.begin(),
        {std::string(kDemosaicOperationId), 1, "demosaic-rcd", true,
         {{"mode", ParameterValue{std::string(kDemosaicModeRcd)}}}, std::nullopt});
    auto duplicate = engine.value().linear_working_from_raw(raw, ppg_recipe, 64U, 64U,
                                                            CancellationToken{});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_demosaic_operation");
}

} // namespace
} // namespace ravo
