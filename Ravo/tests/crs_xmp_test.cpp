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

[[nodiscard]] std::filesystem::path response_fixture_path()
{
    return std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
           "crs_pv2012_response_calibration.xmp";
}

[[nodiscard]] std::string read_fixture()
{
    std::ifstream input(fixture_path(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

[[nodiscard]] std::string read_response_fixture()
{
    std::ifstream input(response_fixture_path(), std::ios::binary);
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
    EXPECT_FALSE(imported.value().mask.tone_curve);
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
    EXPECT_EQ(look.rgb_curve.application_space, kRgbCurveApplicationSpaceDisplaySrgb);
    EXPECT_EQ(look.rgb_curve.preserve_colors, kToneCurvePreserveColorsNone);
    EXPECT_TRUE(tone_curve_is_identity(look.tone_curve));
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
    EXPECT_EQ(operation_by_id(imported.value().recipe, "ravo.core.tonecurve"), nullptr);
    EXPECT_NE(operation_by_id(imported.value().recipe, "ravo.color.primaries"), nullptr);
}

TEST(CrsXmpTest, CalibratedSidecarUsesRawSigmoidAndDisplayEncodedPointCurve)
{
    const auto xmp = read_response_fixture();
    ASSERT_FALSE(xmp.empty());
    auto imported = import_crs_xmp({xmp, {"asset-1", "file:///fixture.ARW", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto &look = imported.value().look;
    EXPECT_TRUE(imported.value().mask.contrast);
    EXPECT_TRUE(imported.value().mask.rgb_curve);
    EXPECT_FALSE(imported.value().mask.tone_curve);
    EXPECT_NEAR(look.contrast, 1.0, 1e-9);
    EXPECT_NEAR(look.sigmoid_contrast, 3.25, 1e-9);
    EXPECT_EQ(look.rgb_curve.mode, kRgbLevelsModeIndependent);
    EXPECT_EQ(look.rgb_curve.preserve_colors, kToneCurvePreserveColorsNone);
    EXPECT_EQ(look.rgb_curve.application_space, kRgbCurveApplicationSpaceDisplaySrgb);
    for (const auto &channel : look.rgb_curve.channels)
        EXPECT_EQ(channel.size(), 20U);
    EXPECT_NEAR(look.rgb_curve.channels[0].front().y, 0.014607324411245974, 1e-12);
    EXPECT_NEAR(look.rgb_curve.channels[0][10].y, 0.65808094648711957, 1e-12);
    EXPECT_NEAR(look.rgb_curve.channels[1][10].y, 0.64032115727543737, 1e-12);
    EXPECT_NEAR(look.rgb_curve.channels[2][10].y, 0.63515736584290539, 1e-12);

    bool omitted_look = false;
    bool omitted_temperature = false;
    for (const auto &item : imported.value().omitted)
    {
        omitted_look =
            omitted_look || (item.key == "Look.Name" && item.reason == "adobe_look_not_applied");
        omitted_temperature =
            omitted_temperature || (item.key == "Temperature" &&
                                    item.reason == "as_shot_white_balance_metadata_not_applied");
    }
    EXPECT_TRUE(omitted_look);
    EXPECT_TRUE(omitted_temperature);

    DevelopParams raw;
    raw.sigmoid_enabled = true;
    raw.contrast = 0.4;
    apply_crs_look(raw, look, imported.value().mask);
    EXPECT_NEAR(raw.sigmoid_contrast, 3.25, 1e-9);
    EXPECT_NEAR(raw.contrast, 0.0, 1e-9);
    auto recipe = recipe_from_develop({"asset-1", "file:///fixture.ARW", std::nullopt}, raw);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto sigmoid = std::find_if(
        recipe.value().operations.begin(), recipe.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == "ravo.display.sigmoid"; });
    const auto curve = std::find_if(
        recipe.value().operations.begin(), recipe.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == "ravo.color.rgbcurve"; });
    ASSERT_NE(sigmoid, recipe.value().operations.end());
    ASSERT_NE(curve, recipe.value().operations.end());
    EXPECT_LT(std::distance(recipe.value().operations.begin(), sigmoid),
              std::distance(recipe.value().operations.begin(), curve));
    auto reopened = develop_from_recipe(recipe.value());
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened.value().rgb_curve.application_space, kRgbCurveApplicationSpaceDisplaySrgb);
    EXPECT_EQ(reopened.value().rgb_curve.channels, raw.rgb_curve.channels);

    auto negative_xmp = xmp;
    const auto contrast = negative_xmp.find("crs:Contrast2012=\"100\"");
    ASSERT_NE(contrast, std::string::npos);
    negative_xmp.replace(contrast, std::string("crs:Contrast2012=\"100\"").size(),
                         "crs:Contrast2012=\"-100\"");
    auto negative =
        import_crs_xmp({negative_xmp, {"asset-1", "file:///fixture.ARW", std::nullopt}});
    ASSERT_TRUE(negative) << negative.error().message;
    EXPECT_NEAR(negative.value().look.sigmoid_contrast,
                kSigmoidContrastDefault * kSigmoidContrastDefault / 3.25, 1e-12);
}

TEST(CrsXmpTest, NonIdentityPointColorsStillFailClosed)
{
    auto xmp = read_response_fixture();
    const auto point = xmp.find("-1, -1, -1");
    ASSERT_NE(point, std::string::npos);
    xmp.replace(point, 2U, "0");
    auto imported = import_crs_xmp({xmp, {"asset-1", "file:///fixture.ARW", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_crs_key");
    EXPECT_EQ(imported.error().context.at("key"), "PointColors");
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

TEST(CrsXmpTest, UnknownKeyKelvinAndMalformedNumberFailClosed)
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

    auto malformed_number = read_fixture();
    const auto exposure = malformed_number.find("crs:Exposure2012=\"-0.41\"");
    ASSERT_NE(exposure, std::string::npos);
    malformed_number.replace(exposure, std::string("crs:Exposure2012=\"-0.41\"").size(),
                             "crs:Exposure2012=\"-0.41junk\"");
    auto number_rejected =
        import_crs_xmp({malformed_number, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_FALSE(number_rejected);
    EXPECT_EQ(number_rejected.error().context.at("reason"), "invalid_crs_number");
    EXPECT_EQ(number_rejected.error().context.at("key"), "Exposure2012");
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

TEST(CrsXmpTest, ProcessVersionMatrixReportsSupportedAndFailClosedUnsupported)
{
    EXPECT_TRUE(crs_process_version_is_supported("11.0"));
    EXPECT_TRUE(crs_process_version_is_supported("17.0"));
    EXPECT_FALSE(crs_process_version_is_supported("5.0"));
    EXPECT_FALSE(crs_process_version_is_supported("18.0"));
    EXPECT_FALSE(crs_process_version_is_supported("CameraRaw"));

    auto supported = read_fixture();
    auto classified = classify_crs_process_version(supported);
    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified.value().version_class, CrsProcessVersionClass::kSupportedPv2012);
    ASSERT_TRUE(classified.value().process_version);
    EXPECT_FALSE(classified.value().process_version->empty());

    auto unsupported = supported;
    const auto marker = unsupported.find("crs:ProcessVersion=\"");
    ASSERT_NE(marker, std::string::npos);
    const auto value_begin = marker + std::string("crs:ProcessVersion=\"").size();
    const auto end = unsupported.find('"', value_begin);
    ASSERT_NE(end, std::string::npos);
    unsupported.replace(marker, end + 1 - marker, "crs:ProcessVersion=\"5.0\"");
    auto rejected_class = classify_crs_process_version(unsupported);
    ASSERT_TRUE(rejected_class) << rejected_class.error().message;
    EXPECT_EQ(rejected_class.value().version_class, CrsProcessVersionClass::kUnsupported);
    EXPECT_EQ(rejected_class.value().process_version.value_or(""), "5.0");
    EXPECT_EQ(rejected_class.value().reason.value_or(""), "unsupported_crs_process_version");

    auto imported = import_crs_xmp({unsupported, {"asset-1", "file:///fixture.jpg", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_crs_process_version");
    EXPECT_EQ(imported.error().context.at("key"), "ProcessVersion");
    EXPECT_EQ(imported.error().context.at("value"), "5.0");
}

} // namespace
} // namespace ravo
