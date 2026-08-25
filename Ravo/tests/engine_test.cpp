#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <png.h>

#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"

#include "test_support.h"

namespace ravo
{
namespace
{

class RecordingProgressSink final : public ProgressSink
{
public:
    void on_progress(const ProgressEvent &event) override
    {
        events.push_back(event);
    }

    std::vector<ProgressEvent> events;
};

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

struct DecodedPng
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<png_byte> pixels;
};

[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::filesystem::path &path)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.string().c_str()) == 0)
    {
        return std::nullopt;
    }
    image.format = PNG_FORMAT_RGB;
    DecodedPng result{image.width, image.height, std::vector<png_byte>(PNG_IMAGE_SIZE(image))};
    if (png_image_finish_read(&image, nullptr, result.pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return std::nullopt;
    }
    png_image_free(&image);
    return result;
}

TEST(EngineFacadeTest, ExposesExactlyTheReservedPhaseOneDescriptors)
{
    const auto engine = EngineFacade::create_phase1();

    ASSERT_TRUE(engine) << engine.error().message;
    ASSERT_EQ(engine.value().operations().size(), kPhase1OperationCount);
    EXPECT_EQ(engine.value().operations().front().id, "ravo.core.identity");
    EXPECT_EQ(engine.value().operations().back().id, "ravo.output.scale");
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == "ravo.detail.sharpen"; }));
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == "ravo.core.tonecurve"; }));
    EXPECT_NE(engine.value().operations().end(),
              std::find_if(engine.value().operations().begin(), engine.value().operations().end(),
                           [](const OperationDescriptor &item)
                           { return item.id == "ravo.display.sigmoid"; }));
}

TEST(EngineFacadeTest, CancelledRequestsNeverReachRendering)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("test_cancel"));

    RenderRequest request;
    request.asset = test::valid_recipe().asset;
    request.recipe = test::valid_recipe();
    request.cancellation = cancellation.token();
    const auto rendered = engine.value().render(request);

    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kCancelled);
}

TEST(EngineFacadeTest, InspectReadsTheFrozenRawFixture)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    const auto inspected = engine.value().inspect(mire1_path(), CancellationToken{});
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected.value().is_raw);
    EXPECT_EQ(inspected.value().format, "raw");
    EXPECT_FALSE(inspected.value().make.empty());
    EXPECT_FALSE(inspected.value().model.empty());
    EXPECT_GT(inspected.value().width, 0U);
    EXPECT_GT(inspected.value().height, 0U);
}

TEST(EngineFacadeTest, ExtractsBoundedEmbeddedJpegPreview)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    const auto preview =
        engine.value().extract_embedded_preview(mire1_path(), 320, CancellationToken{});
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().mime_type, "image/jpeg");
    EXPECT_GT(preview.value().width, 0U);
    EXPECT_GT(preview.value().height, 0U);
    ASSERT_GE(preview.value().bytes.size(), 4U);
    EXPECT_EQ(preview.value().bytes[0], 0xff);
    EXPECT_EQ(preview.value().bytes[1], 0xd8);
}

TEST(EngineFacadeTest, InspectWithEmbeddedPreviewReturnsMetadataAndJpeg)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    const auto probed =
        engine.value().inspect_with_embedded_preview(mire1_path(), 320, CancellationToken{});
    ASSERT_TRUE(probed) << probed.error().message;
    EXPECT_TRUE(probed.value().inspection.is_raw);
    EXPECT_GT(probed.value().inspection.width, 0U);
    EXPECT_GT(probed.value().inspection.height, 0U);
    ASSERT_TRUE(probed.value().embedded_preview.has_value());
    EXPECT_EQ(probed.value().embedded_preview->mime_type, "image/jpeg");
    ASSERT_GE(probed.value().embedded_preview->bytes.size(), 4U);
    EXPECT_EQ(probed.value().embedded_preview->bytes[0], 0xff);
    EXPECT_EQ(probed.value().embedded_preview->bytes[1], 0xd8);
}

TEST(EngineFacadeTest, RenderWritesBoundedPngAndRejectsOutputConflict)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_uri = (std::filesystem::temp_directory_path() / "ravo-mire1-test.png").string();
    request.output_width = 64;
    request.output_height = 48;
    request.correlation_id = "fixture-render";
    std::error_code ignored;
    std::filesystem::remove(request.output_uri, ignored);

    const auto rendered = engine.value().render(request);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 64U);
    EXPECT_EQ(rendered.value().height, 48U);

    std::ifstream output(request.output_uri, std::ios::binary);
    ASSERT_TRUE(output);
    std::array<char, 8> signature{};
    output.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(std::string(signature.data(), signature.size()), std::string("\x89PNG\r\n\x1a\n", 8));
    output.close();
    const auto decoded = read_rgb_png(request.output_uri);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->width, 64U);
    EXPECT_EQ(decoded->height, 48U);
    EXPECT_TRUE(std::any_of(decoded->pixels.begin(), decoded->pixels.end(),
                            [](const png_byte value) { return value != 0; }));

    const auto conflict = engine.value().render(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    std::filesystem::remove(request.output_uri, ignored);
}

TEST(EngineFacadeTest, ExposureOperationRaisesRenderedFixtureBrightness)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    Recipe base_recipe;
    base_recipe.asset = {"mire1", mire1_path(), std::nullopt};
    Recipe exposed_recipe = base_recipe;
    exposed_recipe.operations.push_back({"ravo.core.exposure",
                                         1,
                                         "exposure-1",
                                         true,
                                         {{"exposure_ev", ParameterValue{1.0}}},
                                         std::nullopt});
    const auto directory = std::filesystem::temp_directory_path();
    const auto base_path = directory / "ravo-mire1-base.png";
    const auto exposed_path = directory / "ravo-mire1-exposed.png";
    std::error_code ignored;
    std::filesystem::remove(base_path, ignored);
    std::filesystem::remove(exposed_path, ignored);

    RenderRequest base_request;
    base_request.asset = base_recipe.asset;
    base_request.recipe = base_recipe;
    base_request.output_uri = base_path.string();
    base_request.output_width = 32;
    base_request.output_height = 24;
    RenderRequest exposed_request = base_request;
    exposed_request.recipe = exposed_recipe;
    exposed_request.output_uri = exposed_path.string();

    const auto base = engine.value().render(base_request);
    const auto exposed = engine.value().render(exposed_request);
    ASSERT_TRUE(base) << base.error().message;
    ASSERT_TRUE(exposed) << exposed.error().message;
    const auto base_png = read_rgb_png(base_path);
    const auto exposed_png = read_rgb_png(exposed_path);
    ASSERT_TRUE(base_png.has_value());
    ASSERT_TRUE(exposed_png.has_value());
    const auto base_sum =
        std::accumulate(base_png->pixels.begin(), base_png->pixels.end(), std::uint64_t{0});
    const auto exposed_sum =
        std::accumulate(exposed_png->pixels.begin(), exposed_png->pixels.end(), std::uint64_t{0});
    EXPECT_GT(exposed_sum, base_sum);

    std::filesystem::remove(base_path, ignored);
    std::filesystem::remove(exposed_path, ignored);
}

TEST(EngineFacadeTest, ValidatedRenderReportsProgressAndMissingInput)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    RecordingProgressSink progress;

    RenderRequest request;
    request.asset = test::valid_recipe().asset;
    request.recipe = test::valid_recipe();
    request.output_uri =
        (std::filesystem::temp_directory_path() / "ravo-missing-test.png").string();
    request.correlation_id = "request-1";
    const auto rendered = engine.value().render(request, &progress);

    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kNotFound);
    ASSERT_EQ(progress.events.size(), 1U);
    EXPECT_EQ(progress.events.front().stage, "validation_complete");
}

TEST(EngineFacadeTest, RasterDevelopOpsRotateAndDesaturate)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    RasterBuffer raster;
    raster.width = 8;
    raster.height = 4;
    raster.srgb.resize(8U * 4U * 3U, 0);
    for (std::uint32_t y = 0; y < raster.height; ++y)
    {
        for (std::uint32_t x = 0; x < raster.width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * raster.width + x) * 3U;
            raster.srgb[index] = 220;
            raster.srgb[index + 1U] = 20;
            raster.srgb[index + 2U] = 20;
        }
    }

    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    recipe.operations.push_back({"ravo.color.saturation",
                                 1,
                                 "saturation-1",
                                 true,
                                 {{"amount", ParameterValue{-1.0}}},
                                 std::nullopt});
    recipe.operations.push_back({"ravo.geometry.rotate",
                                 1,
                                 "rotate-1",
                                 true,
                                 {{"quarters", ParameterValue{std::int64_t{1}}}},
                                 std::nullopt});
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    const auto rendered = engine.value().render_to_image(request, &raster);
    ASSERT_TRUE(rendered) << rendered.error().message;
    EXPECT_EQ(rendered.value().width, 4U);
    EXPECT_EQ(rendered.value().height, 8U);
    ASSERT_GE(rendered.value().rgb.size(), 3U);
    EXPECT_NEAR(rendered.value().rgb[0], rendered.value().rgb[1], 8);
    EXPECT_NEAR(rendered.value().rgb[1], rendered.value().rgb[2], 8);
}

[[nodiscard]] RasterBuffer solid_raster(const std::uint32_t width, const std::uint32_t height,
                                        const std::uint8_t r, const std::uint8_t g,
                                        const std::uint8_t b)
{
    RasterBuffer raster;
    raster.width = width;
    raster.height = height;
    raster.srgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0; index < raster.srgb.size(); index += 3)
    {
        raster.srgb[index] = r;
        raster.srgb[index + 1U] = g;
        raster.srgb[index + 2U] = b;
    }
    return raster;
}

[[nodiscard]] RasterBuffer gradient_raster()
{
    RasterBuffer raster;
    raster.width = 16;
    raster.height = 16;
    raster.srgb.resize(16U * 16U * 3U);
    for (std::uint32_t y = 0; y < 16; ++y)
    {
        for (std::uint32_t x = 0; x < 16; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * 16U + x) * 3U;
            raster.srgb[index] = static_cast<std::uint8_t>(x * 16U);
            raster.srgb[index + 1U] = static_cast<std::uint8_t>(y * 16U);
            raster.srgb[index + 2U] = 80;
        }
    }
    return raster;
}

[[nodiscard]] std::uint64_t mean_luma(const RenderedImage &image)
{
    std::uint64_t sum = 0;
    for (std::size_t index = 0; index + 2 < image.rgb.size(); index += 3)
    {
        sum += static_cast<std::uint64_t>(image.rgb[index]) * 21U +
               static_cast<std::uint64_t>(image.rgb[index + 1U]) * 72U +
               static_cast<std::uint64_t>(image.rgb[index + 2U]) * 7U;
    }
    return sum / std::max<std::size_t>(1, image.rgb.size() / 3U);
}

[[nodiscard]] Result<RenderedImage> render_op(const EngineFacade &engine, RasterBuffer raster,
                                              OperationInstance operation)
{
    Recipe recipe;
    recipe.asset = {"raster", "memory:raster", std::nullopt};
    recipe.operations.push_back(std::move(operation));
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    return engine.render_to_image(request, &raster);
}

TEST(EngineFacadeTest, PhaseOneControlsChangeSyntheticRaster)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto base_raster = gradient_raster();
    Recipe identity;
    identity.asset = {"raster", "memory:raster", std::nullopt};
    RenderRequest identity_request;
    identity_request.asset = identity.asset;
    identity_request.recipe = identity;
    auto base = engine.value().render_to_image(identity_request, &base_raster);
    ASSERT_TRUE(base) << base.error().message;
    const auto base_mean = mean_luma(base.value());

    auto exposed = render_op(engine.value(), base_raster,
                             {"ravo.core.exposure",
                              1,
                              "exposure-1",
                              true,
                              {{"exposure_ev", ParameterValue{1.0}}},
                              std::nullopt});
    ASSERT_TRUE(exposed) << exposed.error().message;
    EXPECT_GT(mean_luma(exposed.value()), base_mean);

    auto shadowed = render_op(engine.value(), base_raster,
                              {"ravo.core.shadows",
                               1,
                               "shadows-1",
                               true,
                               {{"amount", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(shadowed) << shadowed.error().message;
    EXPECT_GT(mean_luma(shadowed.value()), base_mean);

    auto highlighted = render_op(engine.value(), base_raster,
                                 {"ravo.core.highlights",
                                  1,
                                  "highlights-1",
                                  true,
                                  {{"amount", ParameterValue{-0.8}}},
                                  std::nullopt});
    ASSERT_TRUE(highlighted) << highlighted.error().message;
    EXPECT_LT(mean_luma(highlighted.value()), base_mean);

    auto contrast = render_op(engine.value(), base_raster,
                              {"ravo.core.contrast",
                               1,
                               "contrast-1",
                               true,
                               {{"amount", ParameterValue{0.6}}},
                               std::nullopt});
    ASSERT_TRUE(contrast) << contrast.error().message;

    auto whites = render_op(engine.value(), base_raster,
                            {"ravo.core.whites",
                             1,
                             "whites-1",
                             true,
                             {{"amount", ParameterValue{-0.5}}},
                             std::nullopt});
    ASSERT_TRUE(whites) << whites.error().message;

    auto blacks = render_op(
        engine.value(), base_raster,
        {"ravo.core.blacks", 1, "blacks-1", true, {{"amount", ParameterValue{0.4}}}, std::nullopt});
    ASSERT_TRUE(blacks) << blacks.error().message;

    auto vibrance = render_op(engine.value(), base_raster,
                              {"ravo.color.vibrance",
                               1,
                               "vibrance-1",
                               true,
                               {{"amount", ParameterValue{0.8}}},
                               std::nullopt});
    ASSERT_TRUE(vibrance) << vibrance.error().message;

    auto wb = render_op(engine.value(), base_raster,
                        {"ravo.color.white_balance",
                         1,
                         "wb-1",
                         true,
                         {{"temperature", ParameterValue{4000.0}}, {"tint", ParameterValue{20.0}}},
                         std::nullopt});
    ASSERT_TRUE(wb) << wb.error().message;
    const auto mid = (8U * 16U + 8U) * 3U;
    EXPECT_NE(wb.value().rgb[mid], base.value().rgb[mid]);
    EXPECT_NE(wb.value().rgb[mid + 2U], base.value().rgb[mid + 2U]);

    auto gamma = render_op(
        engine.value(), base_raster,
        {"ravo.core.gamma", 1, "gamma-1", true, {{"gamma", ParameterValue{1.8}}}, std::nullopt});
    ASSERT_TRUE(gamma) << gamma.error().message;

    auto velvia = render_op(engine.value(), base_raster,
                            {"ravo.color.velvia",
                             1,
                             "velvia-1",
                             true,
                             {{"amount", ParameterValue{0.8}}, {"bias", ParameterValue{1.0}}},
                             std::nullopt});
    ASSERT_TRUE(velvia) << velvia.error().message;

    auto balance = render_op(engine.value(), base_raster,
                             {"ravo.color.colorbalance",
                              1,
                              "cb-1",
                              true,
                              {{"lift", ParameterValue{0.2}},
                               {"gamma", ParameterValue{-0.1}},
                               {"gain", ParameterValue{0.3}}},
                              std::nullopt});
    ASSERT_TRUE(balance) << balance.error().message;

    auto contrast_color = render_op(engine.value(), base_raster,
                                    {"ravo.color.colorcontrast",
                                     1,
                                     "cc-1",
                                     true,
                                     {{"amount", ParameterValue{0.7}}},
                                     std::nullopt});
    ASSERT_TRUE(contrast_color) << contrast_color.error().message;

    auto mono = render_op(engine.value(), solid_raster(8, 8, 220, 20, 20),
                          {"ravo.color.monochrome",
                           1,
                           "mono-1",
                           true,
                           {{"amount", ParameterValue{1.0}}},
                           std::nullopt});
    ASSERT_TRUE(mono) << mono.error().message;
    EXPECT_NEAR(mono.value().rgb[0], mono.value().rgb[1], 8);
    EXPECT_NEAR(mono.value().rgb[1], mono.value().rgb[2], 8);

    auto split = render_op(engine.value(), base_raster,
                           {"ravo.color.splittoning",
                            1,
                            "split-1",
                            true,
                            {{"shadows_hue", ParameterValue{0.6}},
                             {"highlights_hue", ParameterValue{0.1}},
                             {"balance", ParameterValue{0.5}},
                             {"amount", ParameterValue{0.8}}},
                            std::nullopt});
    ASSERT_TRUE(split) << split.error().message;

    auto crop = render_op(engine.value(), base_raster,
                          {"ravo.geometry.crop",
                           1,
                           "crop-1",
                           true,
                           {{"x", ParameterValue{0.25}},
                            {"y", ParameterValue{0.25}},
                            {"width", ParameterValue{0.5}},
                            {"height", ParameterValue{0.5}}},
                           std::nullopt});
    ASSERT_TRUE(crop) << crop.error().message;
    EXPECT_EQ(crop.value().width, 8U);
    EXPECT_EQ(crop.value().height, 8U);

    RasterBuffer sided;
    sided.width = 4;
    sided.height = 2;
    sided.srgb = {255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0,
                  255, 0, 0, 255, 0, 0, 0, 255, 0, 0, 255, 0};
    auto flipped = render_op(engine.value(), sided,
                             {"ravo.geometry.flip",
                              1,
                              "flip-1",
                              true,
                              {{"horizontal", ParameterValue{std::int64_t{1}}},
                               {"vertical", ParameterValue{std::int64_t{0}}}},
                              std::nullopt});
    ASSERT_TRUE(flipped) << flipped.error().message;
    EXPECT_EQ(flipped.value().rgb[0], 0);
    EXPECT_EQ(flipped.value().rgb[1], 255);

    auto vignette = render_op(engine.value(), solid_raster(32, 32, 200, 200, 200),
                              {"ravo.effect.vignette",
                               1,
                               "vig-1",
                               true,
                               {{"amount", ParameterValue{1.0}},
                                {"midpoint", ParameterValue{0.3}},
                                {"falloff", ParameterValue{0.4}}},
                               std::nullopt});
    ASSERT_TRUE(vignette) << vignette.error().message;
    const auto center = (16U * 32U + 16U) * 3U;
    const auto corner = 0U;
    EXPECT_GT(vignette.value().rgb[center], vignette.value().rgb[corner]);

    auto grain_a = render_op(
        engine.value(), solid_raster(12, 12, 120, 120, 120),
        {"ravo.effect.grain", 1, "grain-1", true, {{"amount", ParameterValue{0.8}}}, std::nullopt});
    auto grain_b = render_op(
        engine.value(), solid_raster(12, 12, 120, 120, 120),
        {"ravo.effect.grain", 1, "grain-1", true, {{"amount", ParameterValue{0.8}}}, std::nullopt});
    ASSERT_TRUE(grain_a) << grain_a.error().message;
    ASSERT_TRUE(grain_b) << grain_b.error().message;
    EXPECT_EQ(grain_a.value().rgb, grain_b.value().rgb);
    EXPECT_NE(grain_a.value().rgb, solid_raster(12, 12, 120, 120, 120).srgb);

    auto sharpen = render_op(engine.value(), base_raster,
                             {"ravo.detail.sharpen",
                              1,
                              "sharp-1",
                              true,
                              {{"amount", ParameterValue{1.2}},
                               {"radius", ParameterValue{1.0}},
                               {"threshold", ParameterValue{0.0}}},
                              std::nullopt});
    ASSERT_TRUE(sharpen) << sharpen.error().message;

    auto clarity = render_op(engine.value(), base_raster,
                             {"ravo.detail.clarity",
                              1,
                              "clarity-1",
                              true,
                              {{"amount", ParameterValue{0.6}}},
                              std::nullopt});
    ASSERT_TRUE(clarity) << clarity.error().message;

    auto bloom = render_op(
        engine.value(), base_raster,
        {"ravo.effect.bloom", 1, "bloom-1", true, {{"amount", ParameterValue{0.7}}}, std::nullopt});
    ASSERT_TRUE(bloom) << bloom.error().message;

    auto soften = render_op(engine.value(), base_raster,
                            {"ravo.effect.soften",
                             1,
                             "soften-1",
                             true,
                             {{"amount", ParameterValue{0.7}}},
                             std::nullopt});
    ASSERT_TRUE(soften) << soften.error().message;

    auto dehaze = render_op(engine.value(), base_raster,
                            {"ravo.effect.dehaze",
                             1,
                             "dehaze-1",
                             true,
                             {{"amount", ParameterValue{0.5}}},
                             std::nullopt});
    ASSERT_TRUE(dehaze) << dehaze.error().message;

    auto straightened = render_op(engine.value(), solid_raster(16, 16, 200, 20, 20),
                                  {"ravo.geometry.straighten",
                                   1,
                                   "straighten-1",
                                   true,
                                   {{"degrees", ParameterValue{15.0}}},
                                   std::nullopt});
    ASSERT_TRUE(straightened) << straightened.error().message;
    EXPECT_EQ(straightened.value().width, 16U);
    EXPECT_EQ(straightened.value().height, 16U);
    EXPECT_LT(straightened.value().rgb[0], 40);
    const auto straighten_mid = (8U * 16U + 8U) * 3U;
    EXPECT_GT(straightened.value().rgb[straighten_mid], 80);
    bool saw_antialiased_edge = false;
    for (std::size_t index = 0; index + 2 < straightened.value().rgb.size(); index += 3)
    {
        const auto red = straightened.value().rgb[index];
        if (red > 20 && red < 160)
        {
            saw_antialiased_edge = true;
            break;
        }
    }
    EXPECT_TRUE(saw_antialiased_edge);

    auto graduated = render_op(engine.value(), solid_raster(32, 32, 180, 180, 180),
                               {"ravo.effect.graduatednd",
                                1,
                                "grad-1",
                                true,
                                {{"density_ev", ParameterValue{1.5}},
                                 {"hardness", ParameterValue{0.8}},
                                 {"rotation_deg", ParameterValue{0.0}},
                                 {"offset", ParameterValue{0.0}}},
                                std::nullopt});
    ASSERT_TRUE(graduated) << graduated.error().message;
    const auto top = (2U * 32U + 16U) * 3U;
    const auto bottom = (30U * 32U + 16U) * 3U;
    EXPECT_LT(graduated.value().rgb[top], graduated.value().rgb[bottom]);

    const auto dark_raster = solid_raster(16, 16, 12, 12, 12);
    auto dark_base = engine.value().render_to_image(identity_request, &dark_raster);
    ASSERT_TRUE(dark_base) << dark_base.error().message;
    auto toneeq = render_op(engine.value(), dark_raster,
                            {"ravo.core.toneequal",
                             1,
                             "toneeq-1",
                             true,
                             {{"blacks", ParameterValue{1.2}},
                              {"shadows", ParameterValue{0.0}},
                              {"midtones", ParameterValue{0.0}},
                              {"highlights", ParameterValue{0.0}},
                              {"whites", ParameterValue{0.0}}},
                             std::nullopt});
    ASSERT_TRUE(toneeq) << toneeq.error().message;
    EXPECT_GT(mean_luma(toneeq.value()), mean_luma(dark_base.value()));

    ParameterValue::Array sat_bands(8, ParameterValue{0.0});
    sat_bands[0] = ParameterValue{0.8};
    auto coloreq = render_op(engine.value(), solid_raster(8, 8, 220, 30, 30),
                             {"ravo.color.colorequal",
                              1,
                              "ceq-1",
                              true,
                              {{"hue_shift", ParameterValue{ParameterValue::Array(8, ParameterValue{0.0})}},
                               {"saturation", ParameterValue{sat_bands}},
                               {"lightness", ParameterValue{ParameterValue::Array(8, ParameterValue{0.0})}}},
                              std::nullopt});
    ASSERT_TRUE(coloreq) << coloreq.error().message;

    auto lens = render_op(engine.value(), solid_raster(24, 24, 200, 80, 40),
                          {"ravo.geometry.lens",
                           1,
                           "lens-1",
                           true,
                           {{"mode", ParameterValue{"manual"}},
                            {"k1", ParameterValue{-0.4}},
                            {"k2", ParameterValue{0.1}},
                            {"tca_r", ParameterValue{1.02}},
                            {"tca_b", ParameterValue{0.98}},
                            {"vignetting", ParameterValue{0.6}}},
                           std::nullopt});
    ASSERT_TRUE(lens) << lens.error().message;
    EXPECT_NE(lens.value().rgb, solid_raster(24, 24, 200, 80, 40).srgb);

    auto missing_lens = render_op(engine.value(), solid_raster(8, 8, 120, 120, 120),
                                  {"ravo.geometry.lens",
                                   1,
                                   "lens-lookup-1",
                                   true,
                                   {{"mode", ParameterValue{"lookup"}},
                                    {"camera_make", ParameterValue{"Missing"}},
                                    {"camera_model", ParameterValue{"Camera"}},
                                    {"lens", ParameterValue{"Unknown"}},
                                    {"focal_mm", ParameterValue{50.0}}},
                                   std::nullopt});
    ASSERT_FALSE(missing_lens);
    EXPECT_EQ(missing_lens.error().code, ErrorCode::kNotFound);

    auto matched_lens = render_op(engine.value(), solid_raster(16, 16, 180, 180, 180),
                                  {"ravo.geometry.lens",
                                   1,
                                   "lens-lookup-2",
                                   true,
                                   {{"mode", ParameterValue{"lookup"}},
                                    {"camera_make", ParameterValue{"RavoTest"}},
                                    {"camera_model", ParameterValue{"RavoSensor"}},
                                    {"lens", ParameterValue{"FixtureLens"}},
                                    {"focal_mm", ParameterValue{50.0}}},
                                   std::nullopt});
    ASSERT_TRUE(matched_lens) << matched_lens.error().message;

    RasterBuffer noisy = solid_raster(32, 32, 120, 120, 120);
    for (std::size_t index = 0; index < noisy.srgb.size(); ++index)
    {
        const auto delta = static_cast<int>((index * 37U) % 41U) - 20;
        noisy.srgb[index] = static_cast<std::uint8_t>(std::clamp(120 + delta, 0, 255));
    }
    auto denoised = render_op(engine.value(), noisy,
                              {"ravo.detail.denoiseprofile",
                               1,
                               "denoise-1",
                               true,
                               {{"strength", ParameterValue{0.8}},
                                {"chroma", ParameterValue{1.0}},
                                {"radius", ParameterValue{1.5}}},
                               std::nullopt});
    ASSERT_TRUE(denoised) << denoised.error().message;

    auto raw_on_raster = render_op(engine.value(), solid_raster(8, 8, 10, 10, 10),
                                   {"ravo.raw.highlights",
                                    1,
                                    "raw-hl-1",
                                    true,
                                    {{"mode", ParameterValue{"inpaint"}},
                                     {"amount", ParameterValue{1.0}},
                                     {"clip", ParameterValue{0.98}}},
                                    std::nullopt});
    ASSERT_FALSE(raw_on_raster);
    EXPECT_EQ(raw_on_raster.error().code, ErrorCode::kUnsupported);
}

TEST(EngineFacadeTest, RawHighlightReconstructionChangesMire1)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe identity;
    identity.asset = {"mire1", mire1_path(), std::nullopt};
    RenderRequest base_request;
    base_request.asset = identity.asset;
    base_request.recipe = identity;
    base_request.output_width = 64;
    base_request.output_height = 48;
    auto base = engine.value().render_to_image(base_request);
    ASSERT_TRUE(base) << base.error().message;

    Recipe reconstructed = identity;
    reconstructed.operations.push_back({"ravo.raw.highlights",
                                        1,
                                        "raw-hl-1",
                                        true,
                                        {{"mode", ParameterValue{"opposed"}},
                                         {"amount", ParameterValue{1.0}},
                                         {"clip", ParameterValue{0.92}}},
                                        std::nullopt});
    RenderRequest request = base_request;
    request.recipe = reconstructed;
    auto rebuilt = engine.value().render_to_image(request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_EQ(rebuilt.value().width, base.value().width);
    EXPECT_EQ(rebuilt.value().height, base.value().height);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("highlights"));
    request.cancellation = cancellation.token();
    auto cancelled = engine.value().render_to_image(request);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

[[nodiscard]] ParameterValue tone_curve_points(const std::vector<ToneCurvePoint> &points)
{
    return tone_curve_points_to_parameter(points);
}

[[nodiscard]] OperationInstance
sigmoid_operation(const double contrast = kSigmoidContrastDefault,
                  const double skew = kSigmoidSkewDefault,
                  const double hue_preservation = kSigmoidHuePreservationDefault)
{
    return {"ravo.display.sigmoid",
            1,
            "sigmoid-1",
            true,
            {{"working_space", ParameterValue{"linear_srgb"}},
             {"color_processing", ParameterValue{"per_channel"}},
             {"middle_grey_contrast", ParameterValue{contrast}},
             {"contrast_skewness", ParameterValue{skew}},
             {"display_white_target", ParameterValue{kSigmoidDisplayWhiteDefault}},
             {"display_black_target", ParameterValue{kSigmoidDisplayBlackDefault}},
             {"hue_preservation", ParameterValue{hue_preservation}}},
            std::nullopt};
}

TEST(EngineFacadeTest, SigmoidMapsSyntheticPixelsAndPreservesHueByPolicy)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto source = gradient_raster();
    auto standard = render_op(engine.value(), source, sigmoid_operation());
    auto skewed = render_op(engine.value(), source, sigmoid_operation(1.5, -0.4, 1.0));
    ASSERT_TRUE(standard) << standard.error().message;
    ASSERT_TRUE(skewed) << skewed.error().message;
    EXPECT_NE(standard.value().rgb, source.srgb);
    EXPECT_NE(skewed.value().rgb, standard.value().rgb);

    const auto saturated = solid_raster(8, 8, 245, 100, 20);
    auto preserved = render_op(engine.value(), saturated, sigmoid_operation(1.5, 0.0, 1.0));
    auto shifted = render_op(engine.value(), saturated, sigmoid_operation(1.5, 0.0, 0.0));
    ASSERT_TRUE(preserved) << preserved.error().message;
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_NE(preserved.value().rgb, shifted.value().rgb);
    EXPECT_GT(preserved.value().rgb[0], preserved.value().rgb[1]);
    EXPECT_GT(preserved.value().rgb[1], preserved.value().rgb[2]);

    auto ratio = sigmoid_operation();
    ratio.parameters["color_processing"] = ParameterValue{"rgb_ratio"};
    auto ratio_rendered = render_op(engine.value(), saturated, std::move(ratio));
    ASSERT_TRUE(ratio_rendered) << ratio_rendered.error().message;
    EXPECT_NE(ratio_rendered.value().rgb, preserved.value().rgb);

    auto boundary_operation = sigmoid_operation(kSigmoidContrastMax, kSigmoidSkewMax, 0.0);
    boundary_operation.parameters["display_white_target"] = ParameterValue{kSigmoidDisplayWhiteMax};
    boundary_operation.parameters["display_black_target"] = ParameterValue{kSigmoidDisplayBlackMin};
    auto boundary = render_op(engine.value(), source, std::move(boundary_operation));
    ASSERT_TRUE(boundary) << boundary.error().message;
    EXPECT_EQ(boundary.value().rgb.size(), source.srgb.size());
}

TEST(EngineFacadeTest, SigmoidHasARealRawReference)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto rendered = engine.value().render_to_image(request, nullptr);
    ASSERT_TRUE(rendered) << rendered.error().message;
    ASSERT_EQ(rendered.value().width, 64U);
    ASSERT_EQ(rendered.value().height, 48U);
    std::array<std::uint64_t, 3> sums{};
    std::size_t clipped_channels = 0;
    for (std::size_t index = 0; index + 2 < rendered.value().rgb.size(); index += 3)
    {
        for (std::size_t channel = 0; channel < sums.size(); ++channel)
        {
            const auto value = rendered.value().rgb[index + channel];
            sums[channel] += value;
            clipped_channels += value == 255 ? 1U : 0U;
        }
    }
    // Ravo-owned macOS reference statistics for the pinned mire1.cr2 fixture.
    // The tolerance permits platform libm/SIMD rounding without accepting a changed look.
    EXPECT_NEAR(static_cast<double>(sums[0]), 304823.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[1]), 281792.0, 2000.0);
    EXPECT_NEAR(static_cast<double>(sums[2]), 263085.0, 2000.0);
    EXPECT_LT(clipped_channels, rendered.value().rgb.size() / 100U);
}

TEST(EngineFacadeTest, LinearWorkingRenderMatchesDirectRawRender)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    Recipe recipe;
    recipe.asset = {"mire1", mire1_path(), std::nullopt};
    recipe.operations.push_back(sigmoid_operation());
    RenderRequest request;
    request.asset = recipe.asset;
    request.recipe = recipe;
    request.output_width = 64;
    request.output_height = 48;
    auto direct = engine.value().render_to_image(request);
    ASSERT_TRUE(direct) << direct.error().message;

    auto decoded = engine.value().decode_raw_frame(mire1_path(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto linear = engine.value().linear_working_from_raw(decoded.value(), recipe, 64, 48,
                                                         CancellationToken{});
    ASSERT_TRUE(linear) << linear.error().message;
    EXPECT_EQ(linear.value().width, 64U);
    EXPECT_EQ(linear.value().height, 48U);
    ASSERT_EQ(linear.value().rgb.size(), static_cast<std::size_t>(64U) * 48U * 3U);
    Recipe rgb_recipe = recipe;
    for (auto &operation : rgb_recipe.operations)
    {
        if (operation.id == "ravo.raw.highlights")
        {
            operation.enabled = false;
        }
    }
    auto from_working =
        engine.value().render_linear_working(linear.value(), rgb_recipe, CancellationToken{});
    ASSERT_TRUE(from_working) << from_working.error().message;
    EXPECT_EQ(from_working.value().rgb, direct.value().rgb);

    Recipe exposed = rgb_recipe;
    exposed.operations.insert(exposed.operations.begin(),
                              {"ravo.core.exposure",
                               1,
                               "exposure-1",
                               true,
                               {{"exposure_ev", ParameterValue{1.0}}},
                               std::nullopt});
    auto shifted =
        engine.value().render_linear_working(linear.value(), exposed, CancellationToken{});
    ASSERT_TRUE(shifted) << shifted.error().message;
    EXPECT_NE(shifted.value().rgb, from_working.value().rgb);
    EXPECT_EQ(linear.value().rgb.size(), static_cast<std::size_t>(64U) * 48U * 3U);
}

TEST(EngineFacadeTest, RgbHistogramMatchesFrozenDisplayBinning)
{
    const auto raster = solid_raster(8, 4, 220, 20, 20);
    auto histogram = collect_rgb_histogram(raster);
    ASSERT_TRUE(histogram) << histogram.error().message;
    EXPECT_EQ(histogram.value().red[220], 32U);
    EXPECT_EQ(histogram.value().green[20], 32U);
    EXPECT_EQ(histogram.value().blue[20], 32U);
    EXPECT_EQ(histogram.value().red[0], 0U);
    EXPECT_EQ(histogram.value().max_count, 32U);

    RasterBuffer empty;
    auto rejected = collect_rgb_histogram(empty);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);
}

TEST(EngineFacadeTest, RgbParadePlacesFullWhiteAtEightNinths)
{
    const auto raster = solid_raster(12, 8, 255, 255, 255);
    auto parade = collect_rgb_parade(raster);
    ASSERT_TRUE(parade) << parade.error().message;
    EXPECT_EQ(parade.value().tones, kWaveformTones);
    ASSERT_GT(parade.value().bins, 0U);
    const std::uint32_t width = parade.value().bins * 3U;
    const std::uint32_t height = parade.value().tones;
    ASSERT_EQ(parade.value().rgb.size(), static_cast<std::size_t>(width) * height * 3U);
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y, const std::uint32_t ch)
    { return parade.value().rgb[(static_cast<std::size_t>(y) * width + x) * 3U + ch]; };
    // Frozen waveform maps 1.0 to 8/9 of the tone axis: ceil((8/9)*159) = 142.
    constexpr std::uint32_t kTone = 142;
    const std::uint32_t y = kWaveformTones - 1U - kTone;
    EXPECT_GT(sample(0, y, 0), 0U);
    EXPECT_EQ(sample(0, y, 1), 0U);
    EXPECT_EQ(sample(0, height - 1U, 0), 0U);
}

TEST(EngineFacadeTest, ToneCurveMapsSyntheticRasterAndAcceptsLab)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto gray = solid_raster(8, 8, 128, 128, 128);
    auto identity = render_op(engine.value(), gray,
                              {"ravo.core.tonecurve",
                               1,
                               "curve-identity",
                               true,
                               {{"working_space", ParameterValue{"rgb"}},
                                {"interpolation", ParameterValue{"monotone_hermite"}},
                                {"channel_mode", ParameterValue{"rgb"}},
                                {"preserve_colors", ParameterValue{"average"}},
                                {"points", tone_curve_points({{0.0, 0.0}, {1.0, 1.0}})}},
                               std::nullopt});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb[0], 128);

    const auto lifted_points = tone_curve_points({{0.0, 0.0}, {128.0 / 255.0, 0.75}, {1.0, 1.0}});
    auto rgb = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-rgb",
                          true,
                          {{"working_space", ParameterValue{"rgb"}},
                           {"interpolation", ParameterValue{"monotone_hermite"}},
                           {"channel_mode", ParameterValue{"rgb"}},
                           {"preserve_colors", ParameterValue{"average"}},
                           {"points", lifted_points}},
                          std::nullopt});
    ASSERT_TRUE(rgb) << rgb.error().message;
    EXPECT_GT(rgb.value().rgb[0], 128);

    auto lab = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-lab",
                          true,
                          {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                          std::nullopt});
    ASSERT_TRUE(lab) << lab.error().message;
    EXPECT_GT(lab.value().rgb[0], 128);

    const auto red = solid_raster(8, 8, 220, 40, 30);
    auto rgb_red = render_op(engine.value(), red,
                             {"ravo.core.tonecurve",
                              1,
                              "curve-rgb-red",
                              true,
                              {{"working_space", ParameterValue{"rgb"}},
                               {"preserve_colors", ParameterValue{"average"}},
                               {"points", lifted_points}},
                              std::nullopt});
    auto lab_red = render_op(engine.value(), red,
                             {"ravo.core.tonecurve",
                              1,
                              "curve-lab-red",
                              true,
                              {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                              std::nullopt});
    ASSERT_TRUE(rgb_red) << rgb_red.error().message;
    ASSERT_TRUE(lab_red) << lab_red.error().message;
    EXPECT_NE(lab_red.value().rgb, rgb_red.value().rgb);
}

TEST(EngineFacadeTest, UnknownCpuOperationFailsFast)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto rendered = render_op(engine.value(), solid_raster(4, 4, 10, 20, 30),
                              {"ravo.creative.unknown", 1, "x", true, {}, std::nullopt});
    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kUnsupported);
}

} // namespace
} // namespace ravo
