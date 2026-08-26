#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QProcess>
#include <QXmlStreamReader>
#include <gtest/gtest.h>
#include <png.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/profile_gamma.h"
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

struct FrozenLegacyGammaBlendTuple
{
    std::string_view version;
    std::string_view parameters;
    std::size_t fixture_count;
};

inline constexpr std::string_view kLegacyGammaBlendV9 =
    "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
inline constexpr std::string_view kLegacyGammaBlendGz14GuideOne =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
inline constexpr std::string_view kLegacyGammaBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";
inline constexpr std::string_view kLegacyGammaBlendGz12GuideOne =
    "gz12eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfOqC/0AAogFjBh0A";
inline constexpr std::string_view kLegacyGammaBlendGz12GuideFive =
    "gz12eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfOqC/0AAogFpBh0E";
inline constexpr std::string_view kLegacyGammaBlendGz11FeatherV1 =
    "gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU=";
inline constexpr std::string_view kLegacyGammaBlendV11UncompressedGuideFive =
    "000000000000000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

inline constexpr std::array kFrozenLegacyGammaBlendTuples{
    FrozenLegacyGammaBlendTuple{"9", kLegacyGammaBlendV9, 37U},
    FrozenLegacyGammaBlendTuple{"10", kLegacyGammaBlendGz14GuideOne, 37U},
    FrozenLegacyGammaBlendTuple{"11", kLegacyGammaBlendV11UncompressedGuideFive, 1U},
    FrozenLegacyGammaBlendTuple{"11", kLegacyGammaBlendGz14GuideOne, 19U},
    FrozenLegacyGammaBlendTuple{"11", kLegacyGammaBlendGz14GuideFive, 25U},
    FrozenLegacyGammaBlendTuple{"12", kLegacyGammaBlendGz12GuideFive, 2U},
    FrozenLegacyGammaBlendTuple{"12", kLegacyGammaBlendGz14GuideOne, 5U},
    FrozenLegacyGammaBlendTuple{"12", kLegacyGammaBlendGz14GuideFive, 4U},
    FrozenLegacyGammaBlendTuple{"13", kLegacyGammaBlendGz11FeatherV1, 14U},
    FrozenLegacyGammaBlendTuple{"13", kLegacyGammaBlendGz12GuideOne, 1U},
    FrozenLegacyGammaBlendTuple{"13", kLegacyGammaBlendGz12GuideFive, 6U},
    FrozenLegacyGammaBlendTuple{"14", kLegacyGammaBlendGz11FeatherV1, 7U},
};

struct LegacyGammaXmpOptions
{
    std::optional<std::string_view> version = "1";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = "0000000000000000";
    std::optional<std::string_view> blend_version = "9";
    std::optional<std::string_view> blend_parameters = kLegacyGammaBlendV9;
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_name_hand_edited;
    std::string_view extra_attributes;
    std::size_t instances = 1U;
};

[[nodiscard]] std::string legacy_gamma_xmp(const LegacyGammaXmpOptions &options = {})
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (std::size_t index = 0; index < options.instances; ++index)
    {
        document += R"(<rdf:li darktable:num=")";
        document += std::to_string(index + 40U);
        document += R"(" darktable:operation="gamma")";
        append_attribute("modversion", options.version);
        append_attribute("enabled", options.enabled);
        append_attribute("params", options.parameters);
        append_attribute("multi_name", options.multi_name);
        append_attribute("multi_priority", options.multi_priority);
        append_attribute("multi_name_hand_edited", options.multi_name_hand_edited);
        append_attribute("blendop_version", options.blend_version);
        append_attribute("blendop_params", options.blend_parameters);
        document += options.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

inline constexpr std::string_view kLegacyExposureV5ManualOne =
    "00000000000000000000803f00004842000080c0";
inline constexpr std::string_view kLegacyExposureV6ManualOneBias =
    "00000000000000000000803f00004842000080c001000000";
inline constexpr std::string_view kLegacyExposureV7ManualOneBothCompensations =
    "00000000000000000000803f00004842000080c00100000001000000";

struct LegacyExposureXmpOptions
{
    std::optional<std::string_view> history_position = "8";
    std::optional<std::string_view> version = "5";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kLegacyExposureV5ManualOne;
    std::optional<std::string_view> blend_version = "9";
    std::optional<std::string_view> blend_parameters = kLegacyGammaBlendV9;
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_name_hand_edited;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string legacy_exposure_xmp(const std::vector<LegacyExposureXmpOptions> &entries)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (const auto &entry : entries)
    {
        document += R"(<rdf:li darktable:operation="exposure")";
        append_attribute("num", entry.history_position);
        append_attribute("modversion", entry.version);
        append_attribute("enabled", entry.enabled);
        append_attribute("params", entry.parameters);
        append_attribute("multi_name", entry.multi_name);
        append_attribute("multi_priority", entry.multi_priority);
        append_attribute("multi_name_hand_edited", entry.multi_name_hand_edited);
        append_attribute("blendop_version", entry.blend_version);
        append_attribute("blendop_params", entry.blend_parameters);
        document += entry.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string legacy_exposure_xmp(const LegacyExposureXmpOptions &options = {})
{
    return legacy_exposure_xmp(std::vector<LegacyExposureXmpOptions>{options});
}

[[nodiscard]] std::optional<std::string> xml_attribute_value(const QXmlStreamAttributes &attributes,
                                                             const QStringView name)
{
    for (const auto &attribute : attributes)
    {
        if (attribute.name() == name)
        {
            return attribute.value().toString().toStdString();
        }
    }
    return std::nullopt;
}

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
    ProfileGammaParams profile_gamma;
    profile_gamma.mode = std::string(kProfileGammaModeGamma);
    profile_gamma.linear = 0.08;
    profile_gamma.gamma = 0.55;
    auto profile_gamma_parameters = profile_gamma_to_parameters(profile_gamma);
    ASSERT_TRUE(profile_gamma_parameters) << profile_gamma_parameters.error().message;
    recipe.operations.push_back({std::string(kProfileGammaOperationId),
                                 kProfileGammaOperationSchemaVersion, "profilegamma-1", true,
                                 std::move(profile_gamma_parameters).value(), std::nullopt});
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    PrimariesParams primaries;
    primaries.red_hue = 0.18;
    primaries.red_purity = 1.15;
    recipe.operations.push_back({std::string(kPrimariesOperationId), 1, "primaries-1", true,
                                 primaries_to_parameters(primaries), std::nullopt});
    ExposureParams exposure;
    exposure.mode = std::string(kExposureModeDeflicker);
    exposure.black = -0.01;
    exposure.compensate_exposure_bias = true;
    exposure.compensate_highlight_preservation = true;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-deflicker-1", true, exposure_to_parameters(exposure),
                                 std::nullopt});
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

TEST_F(CliTest, LegacyXmpGammaFixtureCensusPinsEveryFrozenMandatoryBoundary)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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

    std::array<std::size_t, kFrozenLegacyGammaBlendTuples.size()> tuple_counts{};
    std::size_t gamma_count = 0U;
    std::size_t missing_hand_edited_count = 0U;
    std::size_t zero_hand_edited_count = 0U;
    for (const auto &path : xmp_paths)
    {
        SCOPED_TRACE(path.generic_string());
        const auto path_u8 = path.generic_u8string();
        const std::string path_utf8(path_u8.begin(), path_u8.end());
        const auto content = read_utf8_text_file(path_utf8);
        ASSERT_TRUE(content) << content.error().message;
        const QByteArray bytes(content.value().data(),
                               static_cast<qsizetype>(content.value().size()));
        QXmlStreamReader reader(bytes);
        std::size_t file_gamma_count = 0U;
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "gamma")
            {
                continue;
            }

            ++file_gamma_count;
            ++gamma_count;
            const auto version = xml_attribute_value(reader.attributes(), u"modversion");
            const auto enabled = xml_attribute_value(reader.attributes(), u"enabled");
            const auto parameters = xml_attribute_value(reader.attributes(), u"params");
            const auto multi_priority = xml_attribute_value(reader.attributes(), u"multi_priority");
            const auto multi_name = xml_attribute_value(reader.attributes(), u"multi_name");
            const auto multi_name_hand_edited =
                xml_attribute_value(reader.attributes(), u"multi_name_hand_edited");
            const auto blend_version = xml_attribute_value(reader.attributes(), u"blendop_version");
            const auto blend_parameters =
                xml_attribute_value(reader.attributes(), u"blendop_params");
            const auto history_position = xml_attribute_value(reader.attributes(), u"num");

            ASSERT_TRUE(version);
            EXPECT_EQ(*version, "1");
            ASSERT_TRUE(enabled);
            EXPECT_EQ(*enabled, "1");
            ASSERT_TRUE(parameters);
            EXPECT_EQ(*parameters, "0000000000000000");
            ASSERT_TRUE(multi_priority);
            EXPECT_EQ(*multi_priority, "0");
            ASSERT_TRUE(multi_name);
            EXPECT_TRUE(multi_name->empty());
            ASSERT_TRUE(history_position);
            EXPECT_FALSE(history_position->empty());
            EXPECT_TRUE(std::all_of(history_position->begin(), history_position->end(),
                                    [](const char value) { return value >= '0' && value <= '9'; }));
            if (multi_name_hand_edited)
            {
                EXPECT_EQ(*multi_name_hand_edited, "0");
                ++zero_hand_edited_count;
            }
            else
            {
                ++missing_hand_edited_count;
            }

            for (const auto &attribute : reader.attributes())
            {
                const auto name = attribute.name();
                EXPECT_FALSE(name.contains(u"mask"));
                EXPECT_TRUE(name == u"num" || name == u"operation" || name == u"enabled" ||
                            name == u"modversion" || name == u"params" || name == u"multi_name" ||
                            name == u"multi_priority" || name == u"multi_name_hand_edited" ||
                            name == u"blendop_version" || name == u"blendop_params")
                    << name.toString().toStdString();
            }

            ASSERT_TRUE(blend_version);
            ASSERT_TRUE(blend_parameters);
            const auto tuple = std::find_if(kFrozenLegacyGammaBlendTuples.begin(),
                                            kFrozenLegacyGammaBlendTuples.end(),
                                            [&](const FrozenLegacyGammaBlendTuple &candidate)
                                            {
                                                return candidate.version == *blend_version &&
                                                       candidate.parameters == *blend_parameters;
                                            });
            ASSERT_NE(tuple, kFrozenLegacyGammaBlendTuples.end());
            ++tuple_counts[static_cast<std::size_t>(
                std::distance(kFrozenLegacyGammaBlendTuples.begin(), tuple))];
        }
        ASSERT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        EXPECT_EQ(file_gamma_count, 1U);
    }

    EXPECT_EQ(gamma_count, 158U);
    EXPECT_EQ(missing_hand_edited_count, 99U);
    EXPECT_EQ(zero_hand_edited_count, 59U);
    for (std::size_t index = 0; index < kFrozenLegacyGammaBlendTuples.size(); ++index)
    {
        SCOPED_TRACE(std::string(kFrozenLegacyGammaBlendTuples[index].version));
        EXPECT_EQ(tuple_counts[index], kFrozenLegacyGammaBlendTuples[index].fixture_count);
    }
}

TEST_F(CliTest, LegacyXmpGammaAbsorbsOnlyTheTwelveExactFrozenBlendTuples)
{
    for (std::size_t index = 0; index < kFrozenLegacyGammaBlendTuples.size(); ++index)
    {
        const auto &tuple = kFrozenLegacyGammaBlendTuples[index];
        SCOPED_TRACE(std::string(tuple.version) + ":" + std::to_string(index));
        LegacyGammaXmpOptions options;
        options.blend_version = tuple.version;
        options.blend_parameters = tuple.parameters;
        if ((index % 2U) == 0U)
        {
            options.multi_name_hand_edited = "0";
        }
        const auto imported = import_legacy_xmp(
            {legacy_gamma_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});

        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 2U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
        EXPECT_TRUE(std::none_of(imported.value().operations.begin(),
                                 imported.value().operations.end(),
                                 [](const OperationInstance &operation)
                                 {
                                     return operation.id == "ravo.core.gamma" ||
                                            operation.id == kProfileGammaOperationId;
                                 }));
        EXPECT_TRUE(imported.value().masks.empty());
    }
}

TEST_F(CliTest, LegacyXmpGammaRejectsEveryUnprovenHistoryState)
{
    const auto expect_rejected = [](const LegacyGammaXmpOptions &options,
                                    const ErrorCode expected_code,
                                    const std::string_view expected_reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_gamma_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, expected_code);
        EXPECT_EQ(imported.error().context.at("legacy_operation"), "gamma");
        EXPECT_EQ(imported.error().context.at("reason"), expected_reason);
    };

    LegacyGammaXmpOptions options;
    options.version = "2";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_version");

    options = {};
    options.version.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_version");

    options = {};
    options.enabled = "0";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_disabled");

    options = {};
    options.enabled.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_disabled");

    options = {};
    options.parameters = "0000000000000001";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_parameters");

    options = {};
    options.parameters.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_parameters");

    options = {};
    options.instances = 2U;
    expect_rejected(options, ErrorCode::kConflict, "duplicate_legacy_gamma");

    options = {};
    options.blend_version = "15";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_version.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_parameters = kLegacyGammaBlendGz14GuideOne;
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_parameters.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_mask");

    options = {};
    options.extra_attributes = R"( darktable:blendop_mask_id="42")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_mask");

    options = {};
    options.multi_priority = "1";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_priority.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name = "edited";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name_hand_edited = "1";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.extra_attributes = R"( darktable:unproven_state="1")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_attribute");
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

TEST_F(CliTest, LegacyXmpProfileGammaRejectsMissingFrozenPayloadEvidence)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="profile_gamma" darktable:modversion="2" darktable:enabled="1"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "profile_gamma");
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_profile_gamma_no_fixture");
}

TEST_F(CliTest, LegacyXmpMapsStrictExposureV5V6AndV7Payloads)
{
    struct Case
    {
        std::string_view version;
        std::string_view parameters;
        bool enabled;
        bool exposure_bias;
        bool highlight_preservation;
    };
    constexpr std::array cases{
        Case{"5", kLegacyExposureV5ManualOne, true, false, false},
        Case{"6", kLegacyExposureV6ManualOneBias, true, true, false},
        Case{"7", kLegacyExposureV7ManualOneBothCompensations, false, true, true},
    };
    for (const auto &test_case : cases)
    {
        LegacyExposureXmpOptions options;
        options.version = test_case.version;
        options.parameters = test_case.parameters;
        options.enabled = test_case.enabled ? "1" : "0";
        const auto xmp = legacy_exposure_xmp(options);
        const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

        const auto imported = import_legacy_xmp(request);

        ASSERT_TRUE(imported) << test_case.version << ": " << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 3U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        const auto &operation = imported.value().operations[1];
        EXPECT_EQ(operation.id, kExposureOperationId);
        EXPECT_EQ(operation.schema_version, kExposureOperationSchemaVersion);
        EXPECT_EQ(operation.instance_id, "legacy-exposure-8");
        EXPECT_EQ(operation.enabled, test_case.enabled);
        auto params = exposure_from_parameters(operation.parameters);
        ASSERT_TRUE(params) << params.error().message;
        EXPECT_EQ(params.value().mode, kExposureModeManual);
        EXPECT_DOUBLE_EQ(params.value().black, 0.0);
        EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);
        EXPECT_DOUBLE_EQ(params.value().deflicker_percentile, 50.0);
        EXPECT_DOUBLE_EQ(params.value().deflicker_target_ev, -4.0);
        EXPECT_EQ(params.value().compensate_exposure_bias, test_case.exposure_bias);
        EXPECT_EQ(params.value().compensate_highlight_preservation,
                  test_case.highlight_preservation);
    }
}

TEST_F(CliTest, LegacyXmpExposureFixtureCensusPinsRevisionsSingletonsMasksAndBlendStates)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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

    struct CensusEntry
    {
        std::uint64_t history_position = 0U;
        std::string version;
        std::string enabled;
        std::string multi_priority;
        std::string multi_name;
        std::string multi_name_hand_edited;
        std::string blend_version;
        std::string blend_parameters;
        bool frozen_blend = false;
    };
    std::map<std::string, std::size_t, std::less<>> versions;
    std::map<std::string, std::size_t, std::less<>> enabled_states;
    std::map<std::string, std::size_t, std::less<>> priorities;
    std::map<std::string, std::size_t, std::less<>> names;
    std::map<std::string, std::size_t, std::less<>> hand_edited_states;
    std::map<std::string, std::size_t, std::less<>> blend_versions;
    std::map<std::uint64_t, std::size_t> history_positions;
    std::array<std::size_t, 6> records_per_file{};
    std::set<std::pair<std::string, std::string>> distinct_blends;
    std::set<std::pair<std::string, std::string>> frozen_blends;
    std::size_t exposure_records = 0U;
    std::size_t frozen_blend_records = 0U;
    std::size_t explicit_mask_attributes = 0U;
    std::size_t strictly_increasing_revision_files = 0U;
    std::size_t final_singleton_frozen_files = 0U;
    std::map<std::string, std::size_t, std::less<>> final_versions;
    std::map<std::string, std::size_t, std::less<>> final_enabled_states;

    for (const auto &path : xmp_paths)
    {
        SCOPED_TRACE(path.generic_string());
        const auto content = read_utf8_text_file(path.generic_string());
        ASSERT_TRUE(content) << content.error().message;
        const QByteArray bytes(content.value().data(),
                               static_cast<qsizetype>(content.value().size()));
        QXmlStreamReader reader(bytes);
        std::vector<CensusEntry> file_entries;
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "exposure")
            {
                continue;
            }
            for (const auto &attribute : reader.attributes())
            {
                explicit_mask_attributes += attribute.name().contains(u"mask") ? 1U : 0U;
                const auto name = attribute.name();
                EXPECT_TRUE(name == u"num" || name == u"operation" || name == u"enabled" ||
                            name == u"modversion" || name == u"params" || name == u"multi_name" ||
                            name == u"multi_priority" || name == u"multi_name_hand_edited" ||
                            name == u"blendop_version" || name == u"blendop_params")
                    << name.toString().toStdString();
            }

            const auto position = xml_attribute_value(reader.attributes(), u"num");
            const auto version = xml_attribute_value(reader.attributes(), u"modversion");
            const auto enabled = xml_attribute_value(reader.attributes(), u"enabled");
            const auto priority = xml_attribute_value(reader.attributes(), u"multi_priority");
            const auto name = xml_attribute_value(reader.attributes(), u"multi_name");
            const auto hand_edited =
                xml_attribute_value(reader.attributes(), u"multi_name_hand_edited");
            const auto blend_version = xml_attribute_value(reader.attributes(), u"blendop_version");
            const auto blend_parameters =
                xml_attribute_value(reader.attributes(), u"blendop_params");
            ASSERT_TRUE(position);
            ASSERT_TRUE(version);
            ASSERT_TRUE(enabled);
            ASSERT_TRUE(priority);
            ASSERT_TRUE(name);
            ASSERT_TRUE(blend_version);
            ASSERT_TRUE(blend_parameters);
            const auto parsed_position = static_cast<std::uint64_t>(std::stoull(*position));

            LegacyExposureXmpOptions blend_probe;
            blend_probe.blend_version = *blend_version;
            blend_probe.blend_parameters = *blend_parameters;
            const auto probed =
                import_legacy_xmp({legacy_exposure_xmp(blend_probe),
                                   {"blend-census", "file:///fixture.raw", std::nullopt}});
            const bool frozen_blend = static_cast<bool>(probed);
            if (!frozen_blend)
            {
                EXPECT_EQ(probed.error().code, ErrorCode::kUnsupported);
                EXPECT_EQ(probed.error().context.at("reason"), "unsupported_legacy_exposure_blend");
            }

            file_entries.push_back({parsed_position, *version, *enabled, *priority, *name,
                                    hand_edited.value_or("<missing>"), *blend_version,
                                    *blend_parameters, frozen_blend});
            ++exposure_records;
            ++versions[*version];
            ++enabled_states[*enabled];
            ++priorities[*priority];
            ++names[*name];
            ++hand_edited_states[hand_edited.value_or("<missing>")];
            ++blend_versions[*blend_version];
            ++history_positions[parsed_position];
            distinct_blends.emplace(*blend_version, *blend_parameters);
            if (frozen_blend)
            {
                ++frozen_blend_records;
                frozen_blends.emplace(*blend_version, *blend_parameters);
            }
        }
        ASSERT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        ASSERT_LT(file_entries.size(), records_per_file.size());
        ++records_per_file[file_entries.size()];
        if (file_entries.empty())
        {
            continue;
        }
        const bool increasing =
            std::adjacent_find(file_entries.begin(), file_entries.end(),
                               [](const CensusEntry &left, const CensusEntry &right)
                               { return left.history_position >= right.history_position; }) ==
            file_entries.end();
        strictly_increasing_revision_files += increasing ? 1U : 0U;
        const auto final =
            std::max_element(file_entries.begin(), file_entries.end(),
                             [](const CensusEntry &left, const CensusEntry &right)
                             { return left.history_position < right.history_position; });
        ++final_versions[final->version];
        ++final_enabled_states[final->enabled];
        const bool singleton =
            final->multi_priority == "0" && final->multi_name.empty() &&
            (final->multi_name_hand_edited == "<missing>" || final->multi_name_hand_edited == "0");
        final_singleton_frozen_files += singleton && final->frozen_blend ? 1U : 0U;
    }

    const decltype(versions) expected_versions{{"5", 5U}, {"6", 102U}, {"7", 3U}};
    const decltype(enabled_states) expected_enabled{{"0", 1U}, {"1", 109U}};
    const decltype(priorities) expected_priorities{
        {"0", 77U}, {"1", 9U}, {"2", 8U}, {"3", 8U}, {"4", 8U}};
    const decltype(names) expected_names{{"", 47U},
                                         {"1", 9U},
                                         {"2", 8U},
                                         {"3", 8U},
                                         {"4", 8U},
                                         {"Défaut pour « relatif à la scène »", 16U},
                                         {"_builtin_scene-referred default", 7U},
                                         {"scene-referred default", 7U}};
    const decltype(hand_edited_states) expected_hand_edited{{"0", 38U}, {"<missing>", 72U}};
    const decltype(blend_versions) expected_blend_versions{{"9", 19U}, {"10", 4U},  {"11", 56U},
                                                           {"12", 6U}, {"13", 18U}, {"14", 7U}};
    const decltype(history_positions) expected_positions{{6U, 1U},  {7U, 6U},   {8U, 43U},
                                                         {9U, 24U}, {10U, 13U}, {11U, 10U},
                                                         {12U, 8U}, {13U, 3U},  {14U, 2U}};
    const decltype(final_versions) expected_final_versions{{"5", 4U}, {"6", 66U}, {"7", 3U}};
    const decltype(final_enabled_states) expected_final_enabled{{"0", 1U}, {"1", 72U}};
    EXPECT_EQ(exposure_records, 110U);
    EXPECT_EQ(records_per_file, (std::array<std::size_t, 6>{85U, 61U, 3U, 1U, 0U, 8U}));
    EXPECT_EQ(versions, expected_versions);
    EXPECT_EQ(enabled_states, expected_enabled);
    EXPECT_EQ(priorities, expected_priorities);
    EXPECT_EQ(names, expected_names);
    EXPECT_EQ(hand_edited_states, expected_hand_edited);
    EXPECT_EQ(blend_versions, expected_blend_versions);
    EXPECT_EQ(history_positions, expected_positions);
    EXPECT_EQ(final_versions, expected_final_versions);
    EXPECT_EQ(final_enabled_states, expected_final_enabled);
    EXPECT_EQ(distinct_blends.size(), 50U);
    EXPECT_EQ(frozen_blends.size(), 11U);
    EXPECT_EQ(frozen_blend_records, 69U);
    EXPECT_EQ(explicit_mask_attributes, 0U);
    EXPECT_EQ(strictly_increasing_revision_files, 73U);
    EXPECT_EQ(final_singleton_frozen_files, 29U);
}

TEST_F(CliTest, LegacyXmpImportsTheFrozenRealExposureFixture)
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0001-exposure" / "exposure.xmp";
    const auto source = read_utf8_text_file(path.string());
    ASSERT_TRUE(source) << source.error().message;

    const auto imported =
        import_legacy_xmp({source.value(), {"asset-1", "file:///fixture.raw", std::nullopt}});

    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.id, kExposureOperationId);
    EXPECT_EQ(operation.schema_version, kExposureOperationSchemaVersion);
    auto params = exposure_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value().mode, kExposureModeManual);
    EXPECT_DOUBLE_EQ(params.value().black, 0.0);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);
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
        output << legacy_exposure_xmp();
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

TEST_F(CliTest, LegacyXmpPreservesDeflickerAsAnExplicitDeferredContract)
{
    LegacyExposureXmpOptions options;
    options.parameters = "01000000000000000000803f00004842000080c0";
    const auto xmp = legacy_exposure_xmp(options);
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    auto params = exposure_from_parameters(imported.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value().mode, kExposureModeDeflicker);
    EXPECT_DOUBLE_EQ(params.value().deflicker_percentile, 50.0);
    EXPECT_DOUBLE_EQ(params.value().deflicker_target_ev, -4.0);
}

TEST_F(CliTest, LegacyXmpExposureRejectsMalformedVersionedPayloads)
{
    const auto expect_rejected = [&](const LegacyExposureXmpOptions &options, const ErrorCode code,
                                     const std::string_view reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_exposure_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyExposureXmpOptions unsupported_version;
    unsupported_version.version = "8";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_exposure_version");

    LegacyExposureXmpOptions wrong_length;
    wrong_length.parameters = "00000000";
    expect_rejected(wrong_length, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");

    LegacyExposureXmpOptions invalid_mode;
    invalid_mode.parameters = "02000000000000000000803f00004842000080c0";
    expect_rejected(invalid_mode, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");

    LegacyExposureXmpOptions invalid_boolean;
    invalid_boolean.version = "6";
    invalid_boolean.parameters = "00000000000000000000803f00004842000080c002000000";
    expect_rejected(invalid_boolean, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");

    LegacyExposureXmpOptions non_finite;
    non_finite.parameters = "00000000000000000000c07f00004842000080c0";
    expect_rejected(non_finite, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");
}

TEST_F(CliTest, LegacyXmpExposureAcceptsOnlyFrozenUnmaskedSingletonState)
{
    const auto expect_reason =
        [&](const LegacyExposureXmpOptions &options, const std::string_view reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_exposure_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyExposureXmpOptions blend;
    blend.blend_parameters = "legacy-blend";
    expect_reason(blend, "unsupported_legacy_exposure_blend");

    LegacyExposureXmpOptions mask;
    mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_reason(mask, "unsupported_legacy_exposure_mask");

    LegacyExposureXmpOptions priority;
    priority.multi_priority = "1";
    expect_reason(priority, "unsupported_legacy_exposure_multi_state");

    LegacyExposureXmpOptions noncanonical_priority;
    noncanonical_priority.multi_priority = "00";
    expect_reason(noncanonical_priority, "unsupported_legacy_exposure_multi_state");

    LegacyExposureXmpOptions named;
    named.multi_name = "second";
    expect_reason(named, "unsupported_legacy_exposure_multi_state");

    LegacyExposureXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_reason(unknown, "unsupported_legacy_exposure_attribute");
}

TEST_F(CliTest, LegacyXmpExposureFoldsRevisionsByNumWithoutUsingDocumentOrder)
{
    LegacyExposureXmpOptions final;
    final.history_position = "11";
    final.parameters = kLegacyExposureV5ManualOne;
    LegacyExposureXmpOptions superseded;
    superseded.history_position = "9";
    superseded.parameters = "00000000000000000000003f00004842000080c0";
    const auto xmp = legacy_exposure_xmp({final, superseded});
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.instance_id, "legacy-exposure-11");
    auto params = exposure_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);

    superseded.history_position = "11";
    const auto duplicate = import_legacy_xmp({legacy_exposure_xmp({final, superseded}),
                                              {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_legacy_exposure_revision");
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
                                                            "--set", "exposure=19", "--json"}),
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
                                                            "exposure=19", "--json"}),
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
