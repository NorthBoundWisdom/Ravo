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

TEST(EngineFacadeTest, ToneCurveMapsSyntheticRasterAndRejectsLab)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto gray = solid_raster(8, 8, 128, 128, 128);
    auto identity = render_op(engine.value(), gray,
                              {"ravo.core.tonecurve",
                               1,
                               "curve-identity",
                               true,
                               {{"working_space", ParameterValue{"srgb"}},
                                {"interpolation", ParameterValue{"monotone_hermite"}},
                                {"channel_mode", ParameterValue{"rgb"}},
                                {"points", tone_curve_points({{0.0, 0.0}, {1.0, 1.0}})}},
                               std::nullopt});
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().rgb[0], 128);

    const auto lifted_points = tone_curve_points({{0.0, 0.0}, {128.0 / 255.0, 0.75}, {1.0, 1.0}});
    auto srgb = render_op(engine.value(), gray,
                          {"ravo.core.tonecurve",
                           1,
                           "curve-srgb",
                           true,
                           {{"working_space", ParameterValue{"srgb"}},
                            {"interpolation", ParameterValue{"monotone_hermite"}},
                            {"channel_mode", ParameterValue{"rgb"}},
                            {"points", lifted_points}},
                           std::nullopt});
    ASSERT_TRUE(srgb) << srgb.error().message;
    EXPECT_NEAR(srgb.value().rgb[0], 191, 2);

    auto linear = render_op(engine.value(), gray,
                            {"ravo.core.tonecurve",
                             1,
                             "curve-linear",
                             true,
                             {{"working_space", ParameterValue{"linear_rgb"}},
                              {"interpolation", ParameterValue{"monotone_hermite"}},
                              {"channel_mode", ParameterValue{"rgb"}},
                              {"points", lifted_points}},
                             std::nullopt});
    ASSERT_TRUE(linear) << linear.error().message;
    EXPECT_NE(linear.value().rgb[0], srgb.value().rgb[0]);

    auto lab = render_op(engine.value(), gray,
                         {"ravo.core.tonecurve",
                          1,
                          "curve-lab",
                          true,
                          {{"working_space", ParameterValue{"lab"}}, {"points", lifted_points}},
                          std::nullopt});
    ASSERT_FALSE(lab);
    EXPECT_EQ(lab.error().code, ErrorCode::kValidation);
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
