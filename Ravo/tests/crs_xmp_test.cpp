#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/primaries.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::filesystem::path fixture_path()
{
    return std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
           "crs_pv2012_heishijiao.xmp";
}

[[nodiscard]] std::string read_fixture()
{
    std::ifstream input(fixture_path(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

[[nodiscard]] const OperationInstance *operation_by_id(const Recipe &recipe,
                                                       const std::string_view id)
{
    const auto found =
        std::find_if(recipe.operations.begin(), recipe.operations.end(),
                     [id](const OperationInstance &operation) { return operation.id == id; });
    return found == recipe.operations.end() ? nullptr : &*found;
}

TEST(CrsXmpTest, LeftoverImporterRejectsCameraRawDocuments)
{
    const auto xmp = read_fixture();
    ASSERT_FALSE(xmp.empty());
    EXPECT_TRUE(is_crs_xmp_document(xmp));
    auto leftover = import_legacy_xmp({xmp, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_FALSE(leftover);
    EXPECT_EQ(leftover.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(leftover.error().context.at("reason"), "crs_requires_crs_importer");
}

TEST(CrsXmpTest, MapsPv2012PresetOntoDevelopOwners)
{
    const auto xmp = read_fixture();
    ASSERT_FALSE(xmp.empty());
    auto imported = import_crs_xmp({xmp, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().name, "黑石礁大坝");
    EXPECT_TRUE(imported.value().mask.white_balance);
    EXPECT_TRUE(imported.value().mask.exposure);
    EXPECT_TRUE(imported.value().mask.highlights);
    EXPECT_TRUE(imported.value().mask.color_eq_sat);
    EXPECT_TRUE(imported.value().mask.rgb_curve);
    EXPECT_TRUE(imported.value().mask.tone_curve);
    EXPECT_TRUE(imported.value().mask.primaries);
    EXPECT_TRUE(imported.value().mask.vignette);
    EXPECT_TRUE(imported.value().mask.sharpen);
    EXPECT_TRUE(imported.value().mask.denoise);
    const auto &look = imported.value().look;
    EXPECT_EQ(look.temperature.mode, kTemperatureModeAsShot);
    EXPECT_NEAR(look.exposure_ev, -0.41, 1e-9);
    EXPECT_NEAR(look.highlights, -0.70, 1e-9);
    EXPECT_NEAR(look.shadows, 0.57, 1e-9);
    EXPECT_NEAR(look.whites, -0.77, 1e-9);
    EXPECT_NEAR(look.blacks, 0.20, 1e-9);
    EXPECT_NEAR(look.vibrance, 0.13, 1e-9);
    EXPECT_NEAR(look.saturation, -0.05, 1e-9);
    EXPECT_NEAR(look.color_eq_sat[4], -0.50, 1e-9);
    EXPECT_NEAR(look.color_eq_hue[0], 0.15 * 0.5, 1e-9);
    EXPECT_EQ(look.rgb_curve.mode, kRgbLevelsModeIndependent);
    EXPECT_FALSE(tone_curve_is_identity(look.tone_curve));
    EXPECT_FALSE(tone_curve_is_identity(look.rgb_curve.channels[0]));
    EXPECT_NEAR(look.vignette, 0.21, 1e-9);
    EXPECT_NEAR(look.vignette_midpoint, 0.31, 1e-9);
    EXPECT_NEAR(look.sharpen, 0.56, 1e-9);
    EXPECT_NEAR(look.sharpen_radius, 1.0, 1e-9);
    EXPECT_NEAR(look.sharpen_threshold, 72.0, 1e-9);
    EXPECT_NEAR(look.denoise, 0.39, 1e-9);
    EXPECT_NEAR(look.denoise_chroma, 0.25, 1e-9);
    EXPECT_NEAR(look.primaries.red_purity, 1.45, 1e-9);
    EXPECT_LT(look.primaries.achromatic_tint_purity, 0.02);
    bool omitted_profile = false;
    bool omitted_vignette_style = false;
    for (const auto &item : imported.value().omitted)
    {
        if (item.key == "CameraProfile")
        {
            omitted_profile = true;
            EXPECT_EQ(item.reason, "adobe_camera_profile_not_applied");
        }
        if (item.key == "PostCropVignetteStyle")
        {
            omitted_vignette_style = true;
            EXPECT_EQ(item.reason, "crs_vignette_style_approximated");
        }
    }
    EXPECT_TRUE(omitted_profile);
    EXPECT_TRUE(omitted_vignette_style);
    EXPECT_NE(operation_by_id(imported.value().recipe, "ravo.effect.vignette"), nullptr);
    EXPECT_NE(operation_by_id(imported.value().recipe, "ravo.color.rgbcurve"), nullptr);
    EXPECT_NE(operation_by_id(imported.value().recipe, "ravo.core.tonecurve"), nullptr);
    EXPECT_NE(operation_by_id(imported.value().recipe, "ravo.color.primaries"), nullptr);
}

TEST(CrsXmpTest, OverlayKeepsDestinationCropAndAppliesLook)
{
    const auto xmp = read_fixture();
    auto imported = import_crs_xmp({xmp, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    DevelopParams dest;
    dest.crop_width = 0.6;
    dest.crop_x = 0.1;
    dest.exposure_ev = 1.25;
    dest.vibrance = 0.9;
    apply_crs_look(dest, imported.value().look, imported.value().mask);
    EXPECT_NEAR(dest.crop_width, 0.6, 1e-9);
    EXPECT_NEAR(dest.crop_x, 0.1, 1e-9);
    EXPECT_NEAR(dest.exposure_ev, -0.41, 1e-9);
    EXPECT_NEAR(dest.vibrance, 0.13, 1e-9);
}

TEST(CrsXmpTest, UnknownKeyAndKelvinFailClosed)
{
    auto unknown = read_fixture();
    const auto marker = unknown.find("crs:HasSettings=\"True\"");
    ASSERT_NE(marker, std::string::npos);
    unknown.insert(marker, "crs:Texture=\"20\" ");
    auto rejected = import_crs_xmp({unknown, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_crs_key");
    EXPECT_EQ(rejected.error().context.at("key"), "Texture");

    auto kelvin = read_fixture();
    const auto wb = kelvin.find("crs:WhiteBalance=\"As Shot\"");
    ASSERT_NE(wb, std::string::npos);
    kelvin.replace(wb, std::string("crs:WhiteBalance=\"As Shot\"").size(),
                   "crs:WhiteBalance=\"Custom\"");
    auto wb_rejected = import_crs_xmp({kelvin, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_FALSE(wb_rejected);
    EXPECT_EQ(wb_rejected.error().context.at("reason"), "unsupported_crs_white_balance");
}

TEST(CrsXmpTest, ImportXmpCliWritesMappedRecipe)
{
    const auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto xmp = fixture_path().string();
    const auto out = std::filesystem::temp_directory_path() / "ravo-crs-import.recipe.json";
    const auto output_path = out.string();
    std::filesystem::remove(out);
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    CliApplication application(engine.value(), stdout_stream, stderr_stream);
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "recipe", "import-xmp", xmp, "--asset-id", "asset-1", "--input",
                  "file:///fixture.jpg", "--output", output_path, "--json"}),
              0)
        << stdout_stream.str() << stderr_stream.str();
    auto parsed = parse_json(stdout_stream.str());
    ASSERT_TRUE(parsed) << parsed.error().message;
    const auto *data = parsed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *dialect = data->find("dialect");
    ASSERT_NE(dialect, nullptr);
    ASSERT_NE(dialect->string_if(), nullptr);
    EXPECT_EQ(*dialect->string_if(), "crs");
    const auto *count = data->find("operation_count");
    ASSERT_NE(count, nullptr);
    ASSERT_NE(count->number_if(), nullptr);
    EXPECT_GT(std::stoi(count->number_if()->text), 2);
    auto text = read_utf8_text_file(out.string());
    ASSERT_TRUE(text) << text.error().message;
    auto recipe = parse_recipe_json(text.value());
    ASSERT_TRUE(recipe) << recipe.error().message;
    EXPECT_NE(operation_by_id(recipe.value(), "ravo.effect.vignette"), nullptr);
    std::filesystem::remove(out);
}

} // namespace
} // namespace ravo
