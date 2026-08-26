#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QProcess>
#include <gtest/gtest.h>
#include <png.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/primaries.h"
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

inline constexpr std::string_view kLegacyPrimariesPayload =
    "cc56143f4c37093e22c9293ed9ce573f448d12be7b149e3f1e206a3e85eb513f";
inline constexpr std::string_view kLegacyPrimariesDefaultBlend =
    "gz09eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi48L/AcCEA0AmawnoA==";

[[nodiscard]] std::string
legacy_primaries_xmp(const std::string_view version = "1", const std::string_view enabled = "1",
                     const std::string_view parameters = kLegacyPrimariesPayload,
                     const std::string_view blend = kLegacyPrimariesDefaultBlend,
                     const std::size_t instances = 1U, const std::string_view blend_version = "13",
                     const std::string_view extra_attributes = {})
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    for (std::size_t index = 0; index < instances; ++index)
    {
        document += R"(<rdf:li darktable:operation="primaries" darktable:modversion=")";
        document += version;
        document += R"(" darktable:enabled=")";
        document += enabled;
        document += R"(" darktable:params=")";
        document += parameters;
        document += R"(" darktable:blendop_version=")";
        document += blend_version;
        document += R"(" darktable:blendop_params=")";
        document += blend;
        document += '"';
        document += extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] bool png_has_chunk(const std::filesystem::path &path, const std::array<char, 4> &type)
{
    std::ifstream stream(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                                          std::istreambuf_iterator<char>()};
    if (bytes.size() < 8U)
    {
        return false;
    }
    std::size_t offset = 8U;
    while (offset + 12U <= bytes.size())
    {
        const std::uint32_t size = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                                   (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
                                   (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
                                   static_cast<std::uint32_t>(bytes[offset + 3U]);
        if (size > bytes.size() - offset - 12U)
        {
            return false;
        }
        if (std::equal(type.begin(), type.end(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4U)))
        {
            return true;
        }
        offset += static_cast<std::size_t>(size) + 12U;
    }
    return false;
}

[[nodiscard]] std::vector<std::uint8_t> read_png_rgb(const std::filesystem::path &path)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.string().c_str()) == 0)
    {
        return {};
    }
    image.format = PNG_FORMAT_RGB;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return {};
    }
    png_image_free(&image);
    return pixels;
}

TEST_F(CliTest, VersionJsonUsesTheVersionedEnvelopeAndNoStderrLogs)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"--version", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    const std::string expected =
        R"({"data":{"name":"Ravo","protocol":"ravo-cli/v1","version":")" RAVO_TEST_VERSION
        R"("},"diagnostics":[],"ok":true,"type":"ravo.cli.result","version":1})"
        "\n";
    EXPECT_EQ(stdout_stream.str(), expected);
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, RealCliJsonStdoutContainsOnlyTheProtocolEnvelope)
{
    QProcess process;
    process.start(QStringLiteral(RAVO_CLI_EXECUTABLE),
                  {QStringLiteral("--version"), QStringLiteral("--json")});
    ASSERT_TRUE(process.waitForStarted());
    ASSERT_TRUE(process.waitForFinished());
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    const QByteArray stdout_bytes = process.readAllStandardOutput();
    const QByteArray stderr_bytes = process.readAllStandardError();
    const auto parsed = parse_json(stdout_bytes.toStdString());
    ASSERT_TRUE(parsed) << parsed.error().message << " stdout=" << stdout_bytes.constData();
    const auto *ok = parsed.value().find("ok");
    ASSERT_NE(ok, nullptr);
    ASSERT_NE(ok->boolean_if(), nullptr);
    EXPECT_TRUE(*ok->boolean_if());
    EXPECT_TRUE(stderr_bytes.isEmpty()) << stderr_bytes.constData();
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
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.channelmixerrgb";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.temperature";
                           }));
    EXPECT_EQ(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.white_balance";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.colorbalancergb";
                           }));
    EXPECT_EQ(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.colorbalance";
                           }));
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
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    PrimariesParams primaries;
    primaries.red_hue = 0.18;
    primaries.red_purity = 1.15;
    recipe.operations.push_back({std::string(kPrimariesOperationId), 1, "primaries-1", true,
                                 primaries_to_parameters(primaries), std::nullopt});
    OutputColorParams output_color;
    output_color.output_profile = std::string(kInputProfileDisplayP3);
    recipe.operations.push_back({"ravo.color.output", 1, "color-output-1", true,
                                 output_color_to_parameters(output_color), std::nullopt});
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
    EXPECT_TRUE(png_has_chunk(output_path, {'i', 'C', 'C', 'P'}));
    EXPECT_FALSE(png_has_chunk(output_path, {'s', 'R', 'G', 'B'}));
    EXPECT_TRUE(stderr_stream.str().empty());
    output.close();

    Recipe direct_recipe = recipe;
    direct_recipe.asset.input_uri = input_argument;
    RenderRequest direct_request;
    direct_request.asset = direct_recipe.asset;
    direct_request.recipe = std::move(direct_recipe);
    direct_request.output_width = 64U;
    direct_request.output_height = 48U;
    auto direct = engine.render_to_image(direct_request);
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(read_png_rgb(output_path), direct.value().rgb);

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
                      std::filesystem::path(std::u8string(u8"ravo-path-contract.json"));
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

TEST_F(CliTest, LegacyXmpImportCreatesAnExplicitInputRecipeAtomically)
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
    ASSERT_EQ(parsed.value().operations.size(), 2U);
    EXPECT_EQ(parsed.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, FrozenNopXmpMapsItsInputProfileExplicitly)
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
    ASSERT_EQ(imported.value().operations.size(), 2U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    auto input = input_color_from_parameters(imported.value().operations.front().parameters);
    ASSERT_TRUE(input) << input.error().message;
    EXPECT_EQ(input.value().input_profile, kInputProfileEnhancedMatrix);
    EXPECT_EQ(input.value().working_profile, kInputProfileLinearRec2020);
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(imported.value().masks.empty());
}

TEST_F(CliTest, LegacyXmpDecodesAndImportsTheProvenRgbPrimariesSingleton)
{
    const auto decoded = decode_legacy_primaries_v1_parameters(kLegacyPrimariesPayload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_NEAR(decoded.value().achromatic_tint_hue, 0.5794494152, 1e-7);
    EXPECT_NEAR(decoded.value().achromatic_tint_purity, 0.1340000033, 1e-7);
    EXPECT_NEAR(decoded.value().red_hue, 0.1658063233, 1e-7);
    EXPECT_NEAR(decoded.value().red_purity, 0.8429999948, 1e-7);
    EXPECT_NEAR(decoded.value().green_hue, -0.1431170106, 1e-7);
    EXPECT_NEAR(decoded.value().green_purity, 1.2350000143, 1e-7);
    EXPECT_NEAR(decoded.value().blue_hue, 0.2286381423, 1e-7);
    EXPECT_NEAR(decoded.value().blue_purity, 0.8199999928, 1e-7);
    auto canonical = primaries_from_parameters(primaries_to_parameters(decoded.value()));
    ASSERT_TRUE(canonical) << canonical.error().message;
    EXPECT_EQ(canonical.value(), decoded.value());

    const auto short_payload = decode_legacy_primaries_v1_parameters("00000000");
    ASSERT_FALSE(short_payload);
    EXPECT_EQ(short_payload.error().code, ErrorCode::kValidation);
    const auto non_finite_payload = decode_legacy_primaries_v1_parameters(
        "0000c07f00000000000000000000803f000000000000803f000000000000803f");
    ASSERT_FALSE(non_finite_payload);
    EXPECT_EQ(non_finite_payload.error().code, ErrorCode::kValidation);

    const auto document = legacy_primaries_xmp();
    const LegacyXmpImportRequest request{document,
                                         {"asset-1", "file:///fixture.raw", std::nullopt}};
    const auto imported = import_legacy_xmp(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    const auto &primaries = imported.value().operations[1];
    EXPECT_EQ(primaries.id, kPrimariesOperationId);
    EXPECT_EQ(primaries.instance_id, "legacy-primaries-0");
    EXPECT_TRUE(primaries.enabled);
    auto imported_params = primaries_from_parameters(primaries.parameters);
    ASSERT_TRUE(imported_params) << imported_params.error().message;
    EXPECT_EQ(imported_params.value(), decoded.value());
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    ASSERT_TRUE(validate_recipe(imported.value(), registry.value()));
}

TEST_F(CliTest, LegacyXmpRejectsUnprovenRgbPrimariesHistoryStates)
{
    const auto import_document = [](std::string document)
    { return import_legacy_xmp({document, {"asset-1", "file:///fixture.raw", std::nullopt}}); };

    auto disabled = import_document(legacy_primaries_xmp("1", "0"));
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(disabled.error().context.at("reason"), "unsupported_legacy_primaries_disabled");

    auto version = import_document(legacy_primaries_xmp("2"));
    ASSERT_FALSE(version);
    EXPECT_EQ(version.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(version.error().context.at("reason"), "unsupported_legacy_primaries_version");

    auto blend = import_document(
        legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload, "unsupported-blend"));
    ASSERT_FALSE(blend);
    EXPECT_EQ(blend.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(blend.error().context.at("reason"), "unsupported_legacy_blend");

    auto mask = import_document(legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload,
                                                     kLegacyPrimariesDefaultBlend, 1U, "13",
                                                     R"( darktable:mask_id="mask-1")"));
    ASSERT_FALSE(mask);
    EXPECT_EQ(mask.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(mask.error().context.at("reason"), "unsupported_legacy_mask");

    auto duplicate = import_document(
        legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload, kLegacyPrimariesDefaultBlend, 2U));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_legacy_primaries");
}

TEST_F(CliTest, LegacyXmpPrimariesImportCommandWritesCanonicalRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-primaries-v1.xmp";
    const auto recipe_path = directory / "ravo-primaries-v1.recipe.json";
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << legacy_primaries_xmp();
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

    const auto exit_code = application.run(std::span{arguments});
    const auto recipe = read_utf8_text_file(recipe_argument);
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().operations.size(), 3U);
    EXPECT_EQ(parsed.value().operations[1].id, kPrimariesOperationId);
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(stderr_stream.str().empty());
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
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    const auto &operation = imported.value().operations[1];
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
    ASSERT_EQ(parsed.value().operations.size(), 3U);
    EXPECT_EQ(parsed.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(parsed.value().operations[1].id, "ravo.core.exposure");
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
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

TEST_F(CliTest, CatalogDevelopProbeIsReadOnlyAndReportsDeterministicPixelStatistics)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-probe-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0000-nop" / "expected.png")
                         .generic_u8string();
    const std::string png_path(png.begin(), png.end());

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
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 1U);
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const auto id = *asset_id->string_if();

    const auto run_probe = [&](const std::optional<std::string_view> override,
                               const bool baseline) -> Result<JsonValue>
    {
        stdout_stream.str({});
        stdout_stream.clear();
        std::vector<std::string_view> arguments{"catalog",    "probe", "--catalog",  catalog,
                                                "--asset-id", id,      "--max-edge", "64"};
        if (baseline)
        {
            arguments.push_back("--baseline");
        }
        if (override)
        {
            arguments.push_back("--set");
            arguments.push_back(*override);
        }
        arguments.push_back("--json");
        if (application.run(arguments) != 0)
        {
            return make_error(ErrorCode::kIo, "Develop probe command failed",
                              {{"stdout", stdout_stream.str()}});
        }
        return parse_json(stdout_stream.str());
    };
    const auto luma_mean = [](const JsonValue &response) -> std::optional<double>
    {
        const auto *data = response.find("data");
        const auto *statistics = data == nullptr ? nullptr : data->find("statistics");
        const auto *luma = statistics == nullptr ? nullptr : statistics->find("display_luma_mean");
        if (luma == nullptr || luma->number_if() == nullptr)
        {
            return std::nullopt;
        }
        return std::stod(luma->number_if()->text);
    };

    auto baseline = run_probe(std::nullopt, true);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto minus_one = run_probe("exposure=-1", true);
    ASSERT_TRUE(minus_one) << minus_one.error().message;
    auto plus_one = run_probe("exposure=1", true);
    ASSERT_TRUE(plus_one) << plus_one.error().message;
    const auto baseline_luma = luma_mean(baseline.value());
    const auto minus_one_luma = luma_mean(minus_one.value());
    const auto plus_one_luma = luma_mean(plus_one.value());
    ASSERT_TRUE(baseline_luma);
    ASSERT_TRUE(minus_one_luma);
    ASSERT_TRUE(plus_one_luma);
    EXPECT_GT(*minus_one_luma, 8.0);
    EXPECT_LT(*minus_one_luma, *baseline_luma);
    EXPECT_GT(*plus_one_luma, *baseline_luma);
    const auto *minus_data = minus_one.value().find("data");
    ASSERT_NE(minus_data, nullptr);
    const auto *unchanged = minus_data->find("recipe_unchanged");
    ASSERT_NE(unchanged, nullptr);
    ASSERT_NE(unchanged->boolean_if(), nullptr);
    EXPECT_TRUE(*unchanged->boolean_if());
    const auto *preview_records_unchanged = minus_data->find("preview_records_unchanged");
    ASSERT_NE(preview_records_unchanged, nullptr);
    ASSERT_NE(preview_records_unchanged->boolean_if(), nullptr);
    EXPECT_TRUE(*preview_records_unchanged->boolean_if());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "develop", "--catalog",
                                                            catalog, "--asset-id", id, "--set",
                                                            "exposure=1", "--json"}),
              0)
        << stdout_stream.str();
    auto current = run_probe(std::nullopt, false);
    ASSERT_TRUE(current) << current.error().message;
    auto baseline_after_save = run_probe(std::nullopt, true);
    ASSERT_TRUE(baseline_after_save) << baseline_after_save.error().message;
    ASSERT_TRUE(luma_mean(current.value()));
    ASSERT_TRUE(luma_mean(baseline_after_save.value()));
    EXPECT_DOUBLE_EQ(*luma_mean(baseline_after_save.value()), *baseline_luma);
    EXPECT_GT(*luma_mean(current.value()), *baseline_luma);

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "probe", "--catalog",
                                                            catalog, "--asset-id", id, "--baseline",
                                                            "--set", "exposure=11", "--json"}),
              2);
    auto rejected = parse_json(stdout_stream.str());
    ASSERT_TRUE(rejected) << rejected.error().message;
    const auto *error = rejected.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "invalid_argument");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "develop", "--catalog",
                                                            catalog, "--asset-id", id, "--set",
                                                            "exposure=11", "--json"}),
              2);
    auto rejected_save = parse_json(stdout_stream.str());
    ASSERT_TRUE(rejected_save) << rejected_save.error().message;
    error = rejected_save.value().find("error");
    ASSERT_NE(error, nullptr);
    code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "invalid_argument");

    auto current_after_reject = run_probe(std::nullopt, false);
    ASSERT_TRUE(current_after_reject) << current_after_reject.error().message;
    ASSERT_TRUE(luma_mean(current_after_reject.value()));
    EXPECT_DOUBLE_EQ(*luma_mean(current_after_reject.value()), *luma_mean(current.value()));
    EXPECT_TRUE(stderr_stream.str().empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace
} // namespace ravo
