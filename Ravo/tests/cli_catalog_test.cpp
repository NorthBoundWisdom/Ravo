#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QString>
#include <QProcess>
#include <QXmlStreamReader>
#include <gtest/gtest.h>
#include <png.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"

#include "capture_metadata_test_support.h"
#include "cli_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{

TEST_F(CliTest, LegacyXmpColorCheckerIgnoresStaleAndNonFiniteInactiveTailBits)
{
    ColorCheckerParams source;
    const auto payload = legacy_color_checker_v2_hex(source, 1, 1U);
    LegacyColorCheckerXmpOptions options;
    options.parameters = payload;

    auto imported = import_legacy_xmp(
        {legacy_color_checker_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});

    ASSERT_TRUE(imported) << imported.error().message;
    auto params = color_checker_from_parameters(imported.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    ASSERT_EQ(params.value().patches.size(), 1U);
    EXPECT_EQ(params.value().patches.front(), source.patches.front());
}

TEST_F(CliTest, LegacyXmpColorCheckerRejectsUnfrozenVersionPresentationAndActiveData)
{
    const auto expect_rejected = [&](const LegacyColorCheckerXmpOptions &options,
                                     const ErrorCode code, const std::string_view reason)
    {
        auto imported = import_legacy_xmp(
            {legacy_color_checker_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyColorCheckerXmpOptions unsupported_version;
    unsupported_version.version = "3";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorchecker_version");
    LegacyColorCheckerXmpOptions disabled;
    disabled.enabled = "0";
    expect_rejected(disabled, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorchecker_enabled_state");
    LegacyColorCheckerXmpOptions noncanonical_revision;
    noncanonical_revision.history_position = "08";
    expect_rejected(noncanonical_revision, ErrorCode::kValidation,
                    "invalid_legacy_colorchecker_revision");
    for (const std::string_view priority : {"1", "00"})
    {
        LegacyColorCheckerXmpOptions multi;
        multi.multi_priority = priority;
        expect_rejected(multi, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorchecker_multi_state");
    }
    LegacyColorCheckerXmpOptions named;
    named.multi_name = "second";
    expect_rejected(named, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_multi_state");
    LegacyColorCheckerXmpOptions hand_edited;
    hand_edited.multi_name_hand_edited = "1";
    expect_rejected(hand_edited, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorchecker_multi_state");
    LegacyColorCheckerXmpOptions custom_blend;
    custom_blend.blend_parameters = kLegacyGammaBlendV9;
    expect_rejected(custom_blend, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_blend");
    LegacyColorCheckerXmpOptions explicit_mask;
    explicit_mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(explicit_mask, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_mask");
    LegacyColorCheckerXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_rejected(unknown, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_attribute");

    LegacyColorCheckerXmpOptions wrong_length;
    wrong_length.parameters = "00000000";
    expect_rejected(wrong_length, ErrorCode::kValidation, "invalid_legacy_colorchecker_parameters");
    ColorCheckerParams finite;
    const auto too_many_payload = legacy_color_checker_v2_hex(finite, 50);
    LegacyColorCheckerXmpOptions too_many;
    too_many.parameters = too_many_payload;
    expect_rejected(too_many, ErrorCode::kValidation, "invalid_legacy_colorchecker_parameters");
    ColorCheckerParams nonfinite;
    nonfinite.patches[0].source_lab[0] = std::numeric_limits<double>::quiet_NaN();
    const auto nonfinite_payload = legacy_color_checker_v2_hex(nonfinite, 1);
    LegacyColorCheckerXmpOptions invalid_component;
    invalid_component.parameters = nonfinite_payload;
    expect_rejected(invalid_component, ErrorCode::kValidation,
                    "invalid_legacy_colorchecker_parameters");

    LegacyColorCheckerXmpOptions duplicate;
    auto duplicate_result = import_legacy_xmp({legacy_color_checker_xmp({duplicate, duplicate}),
                                               {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate_result.error().context.at("reason"), "duplicate_legacy_colorchecker");
}

TEST_F(CliTest, LegacyXmpColorCheckerCensusPinsTheOneRealRecordAndFullDocumentNegative)
{
    const auto fixture_root =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen";
    std::vector<std::filesystem::path> xmp_paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(fixture_root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xmp")
        {
            xmp_paths.push_back(entry.path());
        }
    }
    std::sort(xmp_paths.begin(), xmp_paths.end());
    ASSERT_EQ(xmp_paths.size(), 158U);

    std::size_t documents_naming_colorchecker = 0U;
    std::size_t record_count = 0U;
    std::optional<std::filesystem::path> record_path;
    for (const auto &path : xmp_paths)
    {
        const auto content = read_utf8_text_file(path.generic_string());
        ASSERT_TRUE(content) << content.error().message;
        documents_naming_colorchecker += content.value().find("colorchecker") != std::string::npos;
        QXmlStreamReader reader(
            QByteArray(content.value().data(), static_cast<qsizetype>(content.value().size())));
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "colorchecker")
            {
                continue;
            }
            ++record_count;
            record_path = path;
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"num"), "8");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"modversion"), "2");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"enabled"), "1");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_priority"), "0");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_name"), "");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_name_hand_edited"),
                      std::nullopt);
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"blendop_version"), "11");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"blendop_params"),
                      kLegacyColorCheckerDefaultBlend);
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"params"),
                      kLegacyColorCheckerV2FixturePayload);
            for (const auto &attribute : reader.attributes())
            {
                EXPECT_FALSE(attribute.name().contains(u"mask"));
            }
        }
        ASSERT_FALSE(reader.hasError()) << path << ": " << reader.errorString().toStdString();
    }
    EXPECT_EQ(documents_naming_colorchecker, 23U);
    ASSERT_EQ(record_count, 1U);
    ASSERT_TRUE(record_path.has_value());
    EXPECT_NE(record_path->generic_string().find("0098-colorchecker/colorchecker.xmp"),
              std::string::npos);

    const auto document = read_utf8_text_file(record_path->generic_string());
    ASSERT_TRUE(document) << document.error().message;
    const auto imported =
        import_legacy_xmp({document.value(), {"fixture", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "rawprepare");
}

TEST_F(CliTest, LegacyXmpAllowsAnEmptyMaskHistoryContainer)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6">
    <darktable:masks_history><rdf:Seq/></darktable:masks_history>
  </rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_TRUE(imported.value().masks.empty());
}

TEST_F(CliTest, LegacyXmpRejectsMalformedRetouchMaskHistoryAttribute)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6">
    <darktable:masks_history><rdf:Seq><rdf:li darktable:num="0"/></rdf:Seq></darktable:masks_history>
  </rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_retouch_mask_attribute");
}

TEST_F(CliTest, LegacyXmpImportRejectsAnExistingOutputPathWithoutOverwritingIt)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-output-conflict.xmp";
    const auto recipe_path = directory / "ravo-output-conflict.recipe.json";
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/"
           xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
           xmlns:darktable="http://darktable.sf.net/">
  <rdf:RDF><rdf:Description><darktable:history><rdf:Seq/></darktable:history></rdf:Description></rdf:RDF>
</x:xmpmeta>)";
    }
    {
        std::ofstream output(recipe_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << "pre-existing recipe";
    }

    const auto xmp_u8 = xmp_path.generic_u8string();
    const std::string xmp_argument(xmp_u8.begin(), xmp_u8.end());
    const auto recipe_u8 = recipe_path.generic_u8string();
    const std::string recipe_argument(recipe_u8.begin(), recipe_u8.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{
        "recipe",  "import-xmp",          xmp_argument, "--asset-id",    "asset-1",
        "--input", "file:///fixture.raw", "--output",   recipe_argument, "--json"};

    const int exit_code = application.run(std::span{arguments});
    const auto content = read_utf8_text_file(recipe_argument);
    const auto response = parse_json(stdout_stream.str());
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 6);
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content.value(), "pre-existing recipe");
    ASSERT_TRUE(response) << response.error().message;
    const auto *error = response.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "conflict");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, JsonFailuresStayStructuredAndDoNotWriteHumanLogsToStdout)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"recipe", "validate", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 2);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *ok = response.value().find("ok");
    ASSERT_NE(ok, nullptr);
    ASSERT_NE(ok->boolean_if(), nullptr);
    EXPECT_FALSE(*ok->boolean_if());
    const auto *error = response.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "invalid_argument");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, RecipeStyleCreateValidateApplyAndLegacyRejectAreAtomic)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-style-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto recipe_path = (root / "source.recipe.json").string();
    const auto style_path = (root / "look.rstyle.json").string();
    const auto applied_path = (root / "applied.recipe.json").string();
    DevelopParams develop;
    develop.exposure_ev = 0.75;
    auto recipe = recipe_from_develop({"source", "file:///source.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    {
        std::ofstream output(recipe_path, std::ios::binary);
        output << serialized.value();
    }
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    EXPECT_EQ(application.run(std::vector<std::string_view>{"recipe", "style-create", recipe_path,
                                                            "--name", "Warm repair", "--output",
                                                            style_path, "--json"}),
              0)
        << stdout_stream.str();
    EXPECT_TRUE(std::filesystem::exists(style_path));

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"recipe", "style-validate", style_path, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "recipe", "style-apply", style_path, "--asset-id", "target", "--input",
                  "file:///target.jpg", "--output", applied_path, "--json"}),
              0)
        << stdout_stream.str();
    auto applied_text = read_utf8_text_file(applied_path);
    ASSERT_TRUE(applied_text) << applied_text.error().message;
    auto applied = parse_recipe_json(applied_text.value());
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().asset.id, "target");
    EXPECT_EQ(applied.value().asset.input_uri, "file:///target.jpg");
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, 0.75);

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"recipe", "style-create", recipe_path,
                                                            "--name", "Warm repair", "--output",
                                                            style_path, "--json"}),
              6);

    const auto legacy_path = (root / "legacy.dtstyle").string();
    {
        std::ofstream output(legacy_path, std::ios::binary);
        output << "<darktable_style version=\"1.0\"></darktable_style>";
    }
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(
                  std::vector<std::string_view>{"recipe", "style-validate", legacy_path, "--json"}),
              0);
    auto rejected = parse_json(stdout_stream.str());
    ASSERT_TRUE(rejected) << rejected.error().message;
    const auto *error = rejected.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "unsupported");
    EXPECT_TRUE(stderr_stream.str().empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, SelectiveRecipeStyleRequiresAndOverlaysTargetRecipe)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("ravo-cli-selective-style-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto style_path = (root / "selective.rstyle.json").string();
    const auto target_path = (root / "target.recipe.json").string();
    const auto output_path = (root / "applied.recipe.json").string();

    DevelopParams source;
    source.exposure_ev = 0.8;
    source.saturation = 0.5;
    auto source_recipe =
        recipe_from_develop({"source", "file:///source.raw", std::nullopt}, source);
    ASSERT_TRUE(source_recipe) << source_recipe.error().message;
    auto style =
        recipe_style_from_selected_fields("Exposure", {}, source_recipe.value(), {"exposure"});
    ASSERT_TRUE(style) << style.error().message;
    auto style_text = serialize_recipe_style(style.value());
    ASSERT_TRUE(style_text) << style_text.error().message;
    {
        std::ofstream output(style_path, std::ios::binary);
        output << style_text.value();
    }

    DevelopParams target;
    target.exposure_ev = -0.2;
    target.saturation = -0.4;
    target.whites = 0.3;
    auto target_recipe =
        recipe_from_develop({"target", "file:///target.raw", std::nullopt}, target);
    ASSERT_TRUE(target_recipe) << target_recipe.error().message;
    auto target_text = serialize_recipe(target_recipe.value());
    ASSERT_TRUE(target_text) << target_text.error().message;
    {
        std::ofstream output(target_path, std::ios::binary);
        output << target_text.value();
    }

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    EXPECT_EQ(application.run(std::vector<std::string_view>{"recipe", "style-apply", style_path,
                                                            "--target-recipe", target_path,
                                                            "--output", output_path, "--json"}),
              0)
        << stdout_stream.str();
    auto applied_text = read_utf8_text_file(output_path);
    ASSERT_TRUE(applied_text) << applied_text.error().message;
    auto applied = parse_recipe_json(applied_text.value());
    ASSERT_TRUE(applied) << applied.error().message;
    auto restored = develop_from_recipe(applied.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, source.exposure_ev);
    EXPECT_DOUBLE_EQ(restored.value().saturation, target.saturation);
    EXPECT_DOUBLE_EQ(restored.value().whites, target.whites);

    stdout_stream.str({});
    stdout_stream.clear();
    const auto missing_target = (root / "missing-target.json").string();
    EXPECT_NE(application.run(std::vector<std::string_view>{
                  "recipe", "style-apply", style_path, "--asset-id", "other", "--input",
                  "file:///other.raw", "--output", missing_target, "--json"}),
              0);
    auto rejected = parse_json(stdout_stream.str());
    ASSERT_TRUE(rejected) << rejected.error().message;
    const auto *error = rejected.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *context = error->find("context");
    ASSERT_NE(context, nullptr);
    const auto *reason = context->find("reason");
    ASSERT_NE(reason, nullptr);
    ASSERT_NE(reason->string_if(), nullptr);
    EXPECT_EQ(*reason->string_if(), "selective_recipe_style_requires_target_recipe");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogCreateImportListPreviewAndDevelop)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-catalog-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "0000-nop" / "expected.png")
                         .generic_u8string();
    const std::string png_path(png.begin(), png.end());

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);

    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto created = parse_json(stdout_stream.str());
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_TRUE(stderr_stream.str().empty());

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", png_path, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *data = imported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 1U);
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const auto id = *asset_id->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "refresh-metadata", "--catalog", catalog, "--asset-id", id, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "rate", "--catalog", catalog,
                                                      "--asset-id", id, "--rating", "4", "--json"}),
        0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog",
                                                            "develop",
                                                            "--catalog",
                                                            catalog,
                                                            "--asset-id",
                                                            id,
                                                            "--exposure-ev",
                                                            "0.5",
                                                            "--set",
                                                            "vignette=0.4",
                                                            "--set",
                                                            "texture=0.75",
                                                            "--set",
                                                            "textureDetailThreshold=4",
                                                            "--set",
                                                            "textureIterations=2",
                                                            "--set",
                                                            "velviaEnabled=1",
                                                            "--set",
                                                            "velviaStrength=80",
                                                            "--set",
                                                            "velviaBias=0.15",
                                                            "--set",
                                                            "outputDitherEnabled=1",
                                                            "--set",
                                                            "outputDitherMethodIndex=13",
                                                            "--set",
                                                            "outputDitherDamping=-100",
                                                            "--set",
                                                            "canvasEnabled=1",
                                                            "--set",
                                                            "canvasLeft=5",
                                                            "--set",
                                                            "canvasRight=10",
                                                            "--set",
                                                            "canvasTop=15",
                                                            "--set",
                                                            "canvasBottom=20",
                                                            "--set",
                                                            "canvasColorIndex=2",
                                                            "--set",
                                                            "outputFrameEnabled=1",
                                                            "--set",
                                                            "outputFrameSize=0.1",
                                                            "--set",
                                                            "outputFrameAspect=-1",
                                                            "--set",
                                                            "watermarkEnabled=1",
                                                            "--set",
                                                            "watermarkOpacity=0.7",
                                                            "--set",
                                                            "watermarkScale=5",
                                                            "--set",
                                                            "colorZonesEnabled=1",
                                                            "--set",
                                                            "colorZonesBandIndex=3",
                                                            "--set",
                                                            "colorZonesChroma=0.75",
                                                            "--set",
                                                            "monochromeEnabled=1",
                                                            "--set",
                                                            "monochromeFilterA=20",
                                                            "--set",
                                                            "monochromeHighlights=0.25",
                                                            "--set",
                                                            "splitToningEnabled=1",
                                                            "--set",
                                                            "splitShadowSaturation=0.9",
                                                            "--set",
                                                            "splitCompress=15",
                                                            "--watermark-text",
                                                            "RAVO {asset_id}",
                                                            "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    data = listed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *assets = data->find("assets");
    ASSERT_NE(assets, nullptr);
    ASSERT_NE(assets->array_if(), nullptr);
    ASSERT_EQ(assets->array_if()->size(), 1U);
    const auto *has_edits = assets->array_if()->front().find("has_edits");
    ASSERT_NE(has_edits, nullptr);
    ASSERT_NE(has_edits->boolean_if(), nullptr);
    EXPECT_TRUE(*has_edits->boolean_if());
    const auto *rating = assets->array_if()->front().find("rating");
    ASSERT_NE(rating, nullptr);
    ASSERT_NE(rating->number_if(), nullptr);
    EXPECT_EQ(rating->number_if()->text, "4");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "preview", "--catalog",
                                                            catalog, "--asset-id", id, "--json"}),
              0)
        << stdout_stream.str();
    auto previewed = parse_json(stdout_stream.str());
    ASSERT_TRUE(previewed) << previewed.error().message;
    data = previewed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *cache_path = data->find("cache_path");
    ASSERT_NE(cache_path, nullptr);
    ASSERT_NE(cache_path->string_if(), nullptr);
    EXPECT_TRUE(std::filesystem::exists(*cache_path->string_if()));
    EXPECT_TRUE(stderr_stream.str().empty());

    const auto export_png = (root / "cli-export.png").string();
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export", "--catalog", catalog, "--asset-id", id, "--output",
                  export_png, "--format", "png", "--json"}),
              0)
        << stdout_stream.str();
    auto exported = parse_json(stdout_stream.str());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_TRUE(std::filesystem::exists(export_png));

    const auto private_png = (root / "cli-export-private.png").string();
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export", "--catalog", catalog, "--asset-id", id, "--output",
                  private_png, "--format", "png", "--metadata", "none", "--json"}),
              0)
        << stdout_stream.str();
    auto private_export = parse_json(stdout_stream.str());
    ASSERT_TRUE(private_export) << private_export.error().message;
    const auto *private_data = private_export.value().find("data");
    ASSERT_NE(private_data, nullptr);
    const auto *metadata_mode = private_data->find("metadata_mode");
    ASSERT_NE(metadata_mode, nullptr);
    ASSERT_NE(metadata_mode->string_if(), nullptr);
    EXPECT_EQ(*metadata_mode->string_if(), "none");
    EXPECT_TRUE(std::filesystem::exists(private_png));

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{
                  "catalog", "export", "--catalog", catalog, "--asset-id", id, "--output",
                  (root / "bad-private.png").string(), "--metadata", "private-ish", "--json"}),
              0);
    auto bad_privacy = parse_json(stdout_stream.str());
    ASSERT_TRUE(bad_privacy) << bad_privacy.error().message;
    const auto *privacy_error = bad_privacy.value().find("error");
    ASSERT_NE(privacy_error, nullptr);
    const auto *privacy_context = privacy_error->find("context");
    ASSERT_NE(privacy_context, nullptr);
    const auto *privacy_reason = privacy_context->find("reason");
    ASSERT_NE(privacy_reason, nullptr);
    ASSERT_NE(privacy_reason->string_if(), nullptr);
    EXPECT_EQ(*privacy_reason->string_if(), "invalid_export_metadata_mode");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export", "--catalog", catalog, "--asset-id", id, "--output",
                  export_png, "--format", "png", "--json"}),
              6)
        << stdout_stream.str();
    auto conflict = parse_json(stdout_stream.str());
    ASSERT_TRUE(conflict) << conflict.error().message;
    const auto *error = conflict.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "conflict");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogDevelopApplyOverlaysSelectionAndRejectsPreflight)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("ravo-cli-develop-apply-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 90, 150));
    const auto first_photo = (root / "one.png").string();
    const auto second_photo = (root / "two.png").string();
    ASSERT_TRUE(image.save(QString::fromStdString(first_photo), "PNG"));
    image.fill(QColor(150, 90, 30));
    ASSERT_TRUE(image.save(QString::fromStdString(second_photo), "PNG"));

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "import", "--catalog",
                                                            catalog, "--input", first_photo,
                                                            "--input", second_photo, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *data = imported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 2U);
    const auto *first_asset = items->array_if()->front().find("asset");
    const auto *second_asset = items->array_if()->back().find("asset");
    ASSERT_NE(first_asset, nullptr);
    ASSERT_NE(second_asset, nullptr);
    const auto *first_id = first_asset->find("id");
    const auto *second_id = second_asset->find("id");
    ASSERT_NE(first_id, nullptr);
    ASSERT_NE(second_id, nullptr);
    ASSERT_NE(first_id->string_if(), nullptr);
    ASSERT_NE(second_id->string_if(), nullptr);
    const auto source_id = *first_id->string_if();
    const auto destination_id = *second_id->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "develop", "--catalog", catalog, "--asset-id", source_id,
                  "--exposure-ev", "0.6", "--set", "saturation=0.25", "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "develop", "--catalog",
                                                            catalog, "--asset-id", destination_id,
                                                            "--set", "contrast=0.2", "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(
        application.run(std::vector<std::string_view>{
            "catalog", "develop-apply", "--catalog", catalog, "--from-asset", source_id,
            "--asset-id", destination_id, "--fields", "exposure", "--revision", "0", "--json"}),
        0)
        << stdout_stream.str();
    auto stale = parse_json(stdout_stream.str());
    ASSERT_TRUE(stale) << stale.error().message;
    const auto *error = stale.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "conflict");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "develop-apply", "--catalog", catalog, "--from-asset", source_id,
                  "--asset-id", destination_id, "--fields", "exposure", "--json"}),
              0)
        << stdout_stream.str();
    auto applied = parse_json(stdout_stream.str());
    ASSERT_TRUE(applied) << applied.error().message;
    data = applied.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *applied_count = data->find("applied");
    ASSERT_NE(applied_count, nullptr);
    ASSERT_NE(applied_count->number_if(), nullptr);
    EXPECT_EQ(applied_count->number_if()->text, "1");
    const auto *failed = data->find("failed");
    ASSERT_NE(failed, nullptr);
    ASSERT_NE(failed->number_if(), nullptr);
    EXPECT_EQ(failed->number_if()->text, "0");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "recipe", "--catalog", catalog,
                                                      "--asset-id", destination_id, "--json"}),
        0)
        << stdout_stream.str();
    auto recipe = parse_json(stdout_stream.str());
    ASSERT_TRUE(recipe) << recipe.error().message;
    data = recipe.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *has_edits = data->find("has_edits");
    ASSERT_NE(has_edits, nullptr);
    ASSERT_NE(has_edits->boolean_if(), nullptr);
    EXPECT_TRUE(*has_edits->boolean_if());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogAiProposalStubProposeApplyAndReject)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-ai-proposal-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    const auto photo = (root / "photo.png").string();
    ASSERT_TRUE(image.save(QString::fromStdString(photo), "PNG"));

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "import", "--catalog",
                                                            catalog, "--input", photo, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *data = imported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_FALSE(items->array_if()->empty());
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const auto id = *asset_id->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{"catalog", "ai-propose", "--catalog",
                                                            catalog, "--asset-id", id, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "ai-propose", "--catalog",
                                                            catalog, "--asset-id", id,
                                                            "--user-initiated", "--json"}),
              0)
        << stdout_stream.str();
    auto proposed = parse_json(stdout_stream.str());
    ASSERT_TRUE(proposed) << proposed.error().message;
    data = proposed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *proposal_id = data->find("id");
    ASSERT_NE(proposal_id, nullptr);
    ASSERT_NE(proposal_id->string_if(), nullptr);
    const auto pid = *proposal_id->string_if();
    const auto *status = data->find("status");
    ASSERT_NE(status, nullptr);
    ASSERT_NE(status->string_if(), nullptr);
    EXPECT_EQ(*status->string_if(), "pending");

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "ai-proposal-reject", "--catalog",
                                                      catalog, "--proposal-id", pid, "--json"}),
        0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "ai-propose", "--catalog",
                                                            catalog, "--asset-id", id,
                                                            "--user-initiated", "--json"}),
              0)
        << stdout_stream.str();
    proposed = parse_json(stdout_stream.str());
    ASSERT_TRUE(proposed) << proposed.error().message;
    data = proposed.value().find("data");
    ASSERT_NE(data, nullptr);
    proposal_id = data->find("id");
    ASSERT_NE(proposal_id, nullptr);
    ASSERT_NE(proposal_id->string_if(), nullptr);
    const auto apply_id = *proposal_id->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "ai-proposal-apply",
                                                            "--catalog", catalog, "--proposal-id",
                                                            apply_id, "--json"}),
              0)
        << stdout_stream.str();
    auto applied = parse_json(stdout_stream.str());
    ASSERT_TRUE(applied) << applied.error().message;
    data = applied.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("history_id"), nullptr);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogAiSemanticMaskProposalStubProposeAndApply)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("ravo-cli-ai-semantic-mask-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(50, 90, 130));
    const auto photo = (root / "photo.png").string();
    ASSERT_TRUE(image.save(QString::fromStdString(photo), "PNG"));

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "import", "--catalog",
                                                            catalog, "--input", photo, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *data = imported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_FALSE(items->array_if()->empty());
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const auto id = *asset_id->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{
            "catalog", "ai-propose", "--catalog", catalog, "--asset-id", id, "--user-initiated",
            "--proposal-kind", "semantic-mask", "--semantic-label", "subject", "--json"}),
        0)
        << stdout_stream.str();
    auto proposed = parse_json(stdout_stream.str());
    ASSERT_TRUE(proposed) << proposed.error().message;
    data = proposed.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("kind"), nullptr);
    EXPECT_EQ(*data->find("kind")->string_if(), "semantic-mask");
    ASSERT_NE(data->find("semantic_label"), nullptr);
    EXPECT_EQ(*data->find("semantic_label")->string_if(), "subject");
    const auto *proposal_id = data->find("id");
    ASSERT_NE(proposal_id, nullptr);
    ASSERT_NE(proposal_id->string_if(), nullptr);
    const auto pid = *proposal_id->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "ai-proposal-apply", "--catalog",
                                                      catalog, "--proposal-id", pid, "--json"}),
        0)
        << stdout_stream.str();

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogImportProjectsRenameAndVerifiedSecondCopyJson)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-ingest-" + generate_catalog_id());
    const auto source_dir = root / "source";
    const auto destination = root / "primary";
    const auto second_copy = root / "second";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    const auto source = source_dir / "photo.png";
    QImage image(40, 30, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 120, 210));
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    const auto source_snapshot = source_file_snapshot(source.string());
    ASSERT_TRUE(source_snapshot);
    const auto catalog = (root / "library.sqlite").string();

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", source.string(), "--mode",
                  "copy", "--destination", destination.string(), "--rename-template",
                  "job-{sequence}-{stem}{ext}", "--second-copy", second_copy.string(), "--json"}),
              0)
        << stdout_stream.str();
    auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("verified_second_copies"), nullptr);
    ASSERT_NE(data->find("verified_second_copies")->number_if(), nullptr);
    EXPECT_EQ(data->find("verified_second_copies")->number_if()->text, "1");
    ASSERT_NE(data->find("rename_template"), nullptr);
    EXPECT_EQ(*data->find("rename_template")->string_if(), "job-{sequence}-{stem}{ext}");
    ASSERT_NE(data->find("items"), nullptr);
    ASSERT_NE(data->find("items")->array_if(), nullptr);
    ASSERT_EQ(data->find("items")->array_if()->size(), 1U);
    const auto &item = data->find("items")->array_if()->front();
    ASSERT_NE(item.find("copies_verified"), nullptr);
    ASSERT_NE(item.find("copies_verified")->boolean_if(), nullptr);
    EXPECT_TRUE(*item.find("copies_verified")->boolean_if());
    ASSERT_NE(item.find("second_copy_destination"), nullptr);
    const auto expected_second =
        normalize_local_input((second_copy / "job-0001-photo.png").string());
    ASSERT_TRUE(expected_second) << expected_second.error().message;
    EXPECT_EQ(*item.find("second_copy_destination")->string_if(), expected_second.value().path);
    EXPECT_TRUE(std::filesystem::exists(destination / "job-0001-photo.png"));
    EXPECT_TRUE(std::filesystem::exists(second_copy / "job-0001-photo.png"));
    EXPECT_EQ(source_file_snapshot(source.string()), source_snapshot);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace

TEST_F(CliTest, CatalogEditorOpenAndAutoStack)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-editor-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "0000-nop" / "expected.png")
                         .generic_u8string();
    const std::string png_path(png.begin(), png.end());
    const auto editor_out = (root / "edited.png").string();
    std::filesystem::copy_file(png_path, editor_out);

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);

    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", png_path, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *data = imported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    const auto *asset = items->array_if()->front().find("asset");
    const auto id = *asset->find("id")->string_if();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "editor-open", "--catalog", catalog, "--asset-id", id,
                  "--user-initiated", "--editor", "photoshop", "--json"}),
              0)
        << stdout_stream.str();
    auto opened = parse_json(stdout_stream.str());
    ASSERT_TRUE(opened) << opened.error().message;
    const auto *open_data = opened.value().find("data");
    ASSERT_NE(open_data, nullptr);
    ASSERT_NE(open_data->find("open_kind"), nullptr);
    EXPECT_EQ(*open_data->find("open_kind")->string_if(), "original");
    ASSERT_NE(open_data->find("os_open_invoked")->boolean_if(), nullptr);
    EXPECT_FALSE(*open_data->find("os_open_invoked")->boolean_if());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "editor-register", "--catalog", catalog, "--asset-id", id, "--input",
                  editor_out, "--editor", "photoshop", "--auto-stack", "--json"}),
              0)
        << stdout_stream.str();
    auto registered = parse_json(stdout_stream.str());
    ASSERT_TRUE(registered) << registered.error().message;
    const auto *reg = registered.value().find("data");
    ASSERT_NE(reg, nullptr);
    ASSERT_NE(reg->find("auto_stacked"), nullptr);
    ASSERT_NE(reg->find("auto_stacked")->boolean_if(), nullptr);
    EXPECT_TRUE(*reg->find("auto_stacked")->boolean_if());
    ASSERT_NE(reg->find("stack"), nullptr);
}

} // namespace ravo
