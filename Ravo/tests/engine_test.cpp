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
    ASSERT_EQ(engine.value().operations().size(), 7U);
    EXPECT_EQ(engine.value().operations().front().id, "ravo.core.identity");
    EXPECT_EQ(engine.value().operations().back().id, "ravo.output.scale");
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

} // namespace
} // namespace ravo
