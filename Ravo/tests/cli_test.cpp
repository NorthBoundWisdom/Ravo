#include <filesystem>
#include <fstream>
#include <array>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <QCoreApplication>
#include <gtest/gtest.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

#include "test_support.h"

namespace ravo
{
namespace
{

void ensure_qt_core()
{
    static const bool logging = []
    {
        ravo::init_logging("ravo-cli-tests");
        return true;
    }();
    static_cast<void>(logging);
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argc = 1;
    static char dummy[] = "ravo-cli-tests";
    static char *argv[] = {dummy, nullptr};
    static auto *app = new QCoreApplication(argc, argv);
    static_cast<void>(app);
}

class CliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_qt_core();
        const auto created = EngineFacade::create_phase1();
        ASSERT_TRUE(created) << created.error().message;
        engine = std::move(created).value();
    }

    EngineFacade engine = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
};

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

TEST_F(CliTest, VersionJsonUsesTheVersionedEnvelopeAndNoStderrLogs)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"--version", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    EXPECT_EQ(
        stdout_stream.str(),
        R"({"data":{"name":"Ravo","protocol":"ravo-cli/v1","version":"0.1.2"},"diagnostics":[],"ok":true,"type":"ravo.cli.result","version":1})"
        "\n");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, OperationsJsonContainsTheReservedDescriptors)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"operations", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *operations = data->find("operations");
    ASSERT_NE(operations, nullptr);
    ASSERT_NE(operations->array_if(), nullptr);
    EXPECT_EQ(operations->array_if()->size(), kPhase1OperationCount);
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, InspectCommandReturnsFrozenRawMetadata)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto input_argument = mire1_path();
    const std::vector<std::string_view> arguments{"inspect", input_argument, "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *raw = data->find("is_raw");
    ASSERT_NE(raw, nullptr);
    ASSERT_NE(raw->boolean_if(), nullptr);
    EXPECT_TRUE(*raw->boolean_if());
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, RenderCommandUsesItsInputAndWritesBoundedPngFromCanonicalRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto recipe_path = directory / "ravo-cli-render.recipe.json";
    const auto output_path = directory / "ravo-cli-render.png";
    std::error_code ignored;
    std::filesystem::remove(recipe_path, ignored);
    std::filesystem::remove(output_path, ignored);

    Recipe recipe;
    recipe.asset = {"mire1", "file:///recipe-placeholder.raw", std::nullopt};
    const auto serialized = serialize_recipe(recipe);
    ASSERT_TRUE(serialized) << serialized.error().message;
    {
        std::ofstream output(recipe_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << serialized.value();
    }

    const auto recipe_u8 = recipe_path.generic_u8string();
    const std::string recipe_argument(recipe_u8.begin(), recipe_u8.end());
    const auto output_u8 = output_path.generic_u8string();
    const std::string output_argument(output_u8.begin(), output_u8.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto input_argument = mire1_path();
    const std::vector<std::string_view> arguments{
        "render",        input_argument, "--recipe", recipe_argument, "--output",
        output_argument, "--backend",    "cpu",      "--width",       "64",
        "--height",      "48",           "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    ASSERT_TRUE(std::filesystem::exists(output_path));
    std::ifstream output(output_path, std::ios::binary);
    ASSERT_TRUE(output);
    std::array<char, 8> signature{};
    output.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(std::string(signature.data(), signature.size()), std::string("\x89PNG\r\n\x1a\n", 8));
    EXPECT_TRUE(stderr_stream.str().empty());

    std::filesystem::remove(recipe_path, ignored);
    std::filesystem::remove(output_path, ignored);
}

TEST_F(CliTest, RecipeValidateUsesTheFacadeAndReturnsMachineData)
{
    const auto recipe = serialize_recipe(test::valid_recipe());
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto path = std::filesystem::temp_directory_path() / "ravo-cli-contract-recipe.json";
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        output << recipe.value();
    }

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto utf8_path = path.generic_u8string();
    const std::string path_argument(utf8_path.begin(), utf8_path.end());
    const std::vector<std::string_view> arguments{"recipe", "validate", path_argument, "--json"};
    const int exit_code = application.run(std::span{arguments});
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    EXPECT_EQ(exit_code, 0);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *asset_id = data->find("asset_id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    EXPECT_EQ(*asset_id->string_if(), "asset-1");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, Utf8FilePathsAreResolvedInsideTheQtAdapter)
{
    const auto path = std::filesystem::temp_directory_path() /
                      std::filesystem::path(std::u8string(u8"ravo-路径-契约.json"));
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        output << "unicode-path-contract";
    }
    const auto qt_path = path.generic_u8string();
    const std::string path_utf8(qt_path.begin(), qt_path.end());

    const auto content = read_utf8_text_file(path_utf8);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content.value(), "unicode-path-contract");
}

TEST_F(CliTest, LegacyXmpImportCreatesAnEmptyCanonicalRecipeAtomically)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-empty-history.xmp";
    const auto recipe_path = directory / "ravo-empty-history.recipe.json";
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
    const auto recipe = read_utf8_text_file(recipe_argument);
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_TRUE(parsed.value().operations.empty());
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, FrozenNopXmpIsAbsorbedByTheBuiltinRawPipeline)
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "0000-nop" / "nop.xmp";
    const auto path_u8 = path.generic_u8string();
    const std::string path_argument(path_u8.begin(), path_u8.end());
    const auto xmp = read_utf8_text_file(path_argument);
    ASSERT_TRUE(xmp) << xmp.error().message;
    const LegacyXmpImportRequest request{xmp.value(), {"mire1", mire1_path(), std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_TRUE(imported.value().operations.empty());
    EXPECT_TRUE(imported.value().masks.empty());
}

TEST_F(CliTest, LegacyXmpOperationsRemainExplicitlyUnsupported)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description><darktable:history><rdf:Seq><rdf:li darktable:operation="exposure"/></rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "exposure");
}

TEST_F(CliTest, LegacyXmpMapsTheProvenManualExposureV5Subset)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="exposure" darktable:modversion="5" darktable:enabled="1"
            darktable:params="00000000000000000000803f00004842000080c0"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 1U);
    const auto &operation = imported.value().operations.front();
    EXPECT_EQ(operation.id, "ravo.core.exposure");
    EXPECT_EQ(operation.schema_version, 1);
    EXPECT_EQ(operation.instance_id, "legacy-exposure-0");
    EXPECT_TRUE(operation.enabled);
    ASSERT_TRUE(operation.parameters.contains("exposure_ev"));
    EXPECT_DOUBLE_EQ(std::get<double>(operation.parameters.at("exposure_ev").value), 1.0);
}

TEST_F(CliTest, LegacyXmpImportCommandWritesTheProvenManualExposureRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-manual-exposure-v5.xmp";
    const auto recipe_path = directory / "ravo-manual-exposure-v5.recipe.json";
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="exposure" darktable:modversion="5" darktable:enabled="1"
            darktable:params="00000000000000000000803f00004842000080c0"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
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
    const auto recipe = read_utf8_text_file(recipe_argument);
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().operations.size(), 1U);
    EXPECT_EQ(parsed.value().operations.front().id, "ravo.core.exposure");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, LegacyXmpRejectsAutomaticExposureV5InsteadOfGuessingHistogramState)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="exposure" darktable:modversion="5" darktable:enabled="1"
            darktable:params="01000000000000000000803f00004842000080c0"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_exposure_mode");
}

TEST_F(CliTest, LegacyXmpRejectsExposureBlendDataWithoutACanonicalMask)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="exposure" darktable:modversion="5" darktable:enabled="1"
            darktable:params="00000000000000000000803f00004842000080c0"
            darktable:blendop_params="legacy-blend"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_blend");
}

TEST_F(CliTest, LegacyXmpRejectsMultipleExposureEntriesWithoutGuessingInstanceSemantics)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="exposure" darktable:modversion="5" darktable:enabled="1"
            darktable:params="00000000000000000000803f00004842000080c0"/>
    <rdf:li darktable:operation="exposure" darktable:modversion="5" darktable:enabled="1"
            darktable:params="00000000000000000000803f00004842000080c0"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_history");
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

TEST_F(CliTest, LegacyXmpRejectsActualMaskHistoryWithoutACanonicalMaskGraph)
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
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_mask");
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

TEST_F(CliTest, CatalogCreateImportListPreviewAndDevelop)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-catalog-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0000-nop" / "expected.png")
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
    EXPECT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "rate", "--catalog", catalog,
                                                      "--asset-id", id, "--rating", "4", "--json"}),
        0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "develop", "--catalog", catalog, "--asset-id", id, "--exposure-ev",
                  "0.5", "--set", "vignette=0.4", "--set", "velvia=0.2", "--json"}),
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

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace
} // namespace ravo
