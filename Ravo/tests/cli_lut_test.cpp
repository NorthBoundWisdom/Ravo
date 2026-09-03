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

TEST_F(CliTest, CatalogDevelopProbeIsReadOnlyAndReportsDeterministicPixelStatistics)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-probe-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "0000-nop" / "expected.png")
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
    auto texture = run_probe("texture=0.75", true);
    ASSERT_TRUE(texture) << texture.error().message;
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
    const auto *gpu_backend = minus_data->find("gpu_backend");
    ASSERT_NE(gpu_backend, nullptr);
    ASSERT_NE(gpu_backend->string_if(), nullptr);
    if (engine.gpu_backend() != "unavailable")
    {
        EXPECT_EQ(*gpu_backend->string_if(), std::string(engine.gpu_backend()));
    }
    else
    {
        EXPECT_EQ(*gpu_backend->string_if(), "cpu");
    }

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "develop", "--catalog", catalog, "--asset-id", id, "--set",
                  "exposure=1", "--set", "demosaicModeIndex=1", "--json"}),
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
    ASSERT_EQ(application.run(std::vector<std::string_view>{"develop-fields", "--json"}), 0)
        << stdout_stream.str();
    auto field_list = parse_json(stdout_stream.str());
    ASSERT_TRUE(field_list) << field_list.error().message;
    const auto *field_data = field_list.value().find("data");
    ASSERT_NE(field_data, nullptr);
    const auto *listed = field_data->find("fields");
    ASSERT_NE(listed, nullptr);
    ASSERT_NE(listed->array_if(), nullptr);
    EXPECT_GE(listed->array_if()->size(), 50U);
    bool listed_exposure = false;
    bool listed_demosaic_mode = false;
    bool listed_raw_denoise = false;
    bool listed_texture = false;
    bool listed_texture_iterations = false;
    for (const auto &field : *listed->array_if())
    {
        const auto *name = field.find("name");
        if (name != nullptr && name->string_if() != nullptr && *name->string_if() == "exposure")
        {
            listed_exposure = true;
        }
        if (name != nullptr && name->string_if() != nullptr &&
            *name->string_if() == "demosaicModeIndex")
        {
            listed_demosaic_mode = true;
            const auto *kind = field.find("kind");
            ASSERT_NE(kind, nullptr);
            ASSERT_NE(kind->string_if(), nullptr);
            EXPECT_EQ(*kind->string_if(), "integer");
            const auto *minimum = field.find("minimum");
            const auto *maximum = field.find("maximum");
            ASSERT_NE(minimum, nullptr);
            ASSERT_NE(maximum, nullptr);
            EXPECT_DOUBLE_EQ(std::stod(minimum->number_if()->text), 0.0);
            EXPECT_DOUBLE_EQ(std::stod(maximum->number_if()->text), 3.0);
        }
        if (name != nullptr && name->string_if() != nullptr &&
            *name->string_if() == "rawDenoiseThreshold")
        {
            listed_raw_denoise = true;
            const auto *minimum = field.find("minimum");
            const auto *maximum = field.find("maximum");
            ASSERT_NE(minimum, nullptr);
            ASSERT_NE(maximum, nullptr);
            EXPECT_DOUBLE_EQ(std::stod(minimum->number_if()->text), 0.0);
            EXPECT_DOUBLE_EQ(std::stod(maximum->number_if()->text), 1.0);
        }
        if (name != nullptr && name->string_if() != nullptr && *name->string_if() == "texture")
        {
            listed_texture = true;
            EXPECT_DOUBLE_EQ(std::stod(field.find("minimum")->number_if()->text), -2.0);
            EXPECT_DOUBLE_EQ(std::stod(field.find("maximum")->number_if()->text), 2.0);
        }
        if (name != nullptr && name->string_if() != nullptr &&
            *name->string_if() == "textureIterations")
        {
            listed_texture_iterations = true;
            ASSERT_NE(field.find("kind"), nullptr);
            EXPECT_EQ(*field.find("kind")->string_if(), "integer");
            EXPECT_DOUBLE_EQ(std::stod(field.find("minimum")->number_if()->text), 1.0);
            EXPECT_DOUBLE_EQ(std::stod(field.find("maximum")->number_if()->text), 5.0);
        }
    }
    EXPECT_TRUE(listed_exposure);
    EXPECT_TRUE(listed_demosaic_mode);
    EXPECT_TRUE(listed_raw_denoise);
    EXPECT_TRUE(listed_texture);
    EXPECT_TRUE(listed_texture_iterations);
    const auto *prefixes = field_data->find("prefixes");
    ASSERT_NE(prefixes, nullptr);
    ASSERT_NE(prefixes->array_if(), nullptr);
    EXPECT_GE(prefixes->array_if()->size(), 2U);

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "fields", "--json"}), 0)
        << stdout_stream.str();
    auto catalog_fields = parse_json(stdout_stream.str());
    ASSERT_TRUE(catalog_fields) << catalog_fields.error().message;
    const auto *catalog_field_data = catalog_fields.value().find("data");
    ASSERT_NE(catalog_field_data, nullptr);
    const auto *catalog_listed = catalog_field_data->find("fields");
    ASSERT_NE(catalog_listed, nullptr);
    ASSERT_NE(catalog_listed->array_if(), nullptr);
    EXPECT_EQ(catalog_listed->array_if()->size(), listed->array_if()->size());

    const auto probe_png = (root / "probe.png").generic_string();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "probe", "--catalog", catalog, "--asset-id", id, "--baseline", "--set",
                  "exposure=1", "--max-edge", "64", "--output", probe_png, "--json"}),
              0)
        << stdout_stream.str();
    auto probed_file = parse_json(stdout_stream.str());
    ASSERT_TRUE(probed_file) << probed_file.error().message;
    const auto *probe_data = probed_file.value().find("data");
    ASSERT_NE(probe_data, nullptr);
    const auto *output = probe_data->find("output");
    ASSERT_NE(output, nullptr);
    ASSERT_NE(output->string_if(), nullptr);
    EXPECT_EQ(*output->string_if(), probe_png);
    EXPECT_TRUE(std::filesystem::exists(probe_png));
    EXPECT_GT(std::filesystem::file_size(probe_png), 8U);
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "probe", "--catalog", catalog, "--asset-id", id, "--output",
                  (root / "probe.jpg").generic_string(), "--json"}),
              2);
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "probe", "--catalog",
                                                            catalog, "--asset-id", id, "--output",
                                                            probe_png, "--json"}),
              6);

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

TEST_F(CliTest, RealCliColorHarmonizerDevelopSetPersistsAndRejectsInvalidInput)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-harmonizer-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "0000-nop" / "expected.png")
                         .generic_u8string();
    const QString catalog_q = QString::fromStdString(catalog);
    const QString png_q = QString::fromStdString(std::string(png.begin(), png.end()));
    struct CliRun
    {
        int exit_code = 1;
        QByteArray stdout_bytes;
        QByteArray stderr_bytes;
    };
    const auto run = [&](const QStringList &arguments) -> CliRun
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForStarted());
        EXPECT_TRUE(process.waitForFinished());
        return {process.exitCode(), process.readAllStandardOutput(),
                process.readAllStandardError()};
    };
    const auto recipe_json = [&](const QByteArray &stdout_bytes) -> Result<JsonValue>
    {
        auto parsed = parse_json(stdout_bytes.toStdString());
        if (!parsed)
        {
            return parsed.error();
        }
        const auto *data = parsed.value().find("data");
        const auto *recipe = data == nullptr ? nullptr : data->find("recipe");
        if (recipe == nullptr)
        {
            return make_error(ErrorCode::kValidation, "CLI recipe payload is missing");
        }
        return *recipe;
    };

    const auto created = run({QStringLiteral("catalog"), QStringLiteral("create"),
                              QStringLiteral("--path"), catalog_q, QStringLiteral("--json")});
    ASSERT_EQ(created.exit_code, 0) << created.stdout_bytes.constData();
    EXPECT_TRUE(created.stderr_bytes.isEmpty());
    const auto imported =
        run({QStringLiteral("catalog"), QStringLiteral("import"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--input"), png_q, QStringLiteral("--json")});
    ASSERT_EQ(imported.exit_code, 0) << imported.stdout_bytes.constData();
    const auto imported_json = parse_json(imported.stdout_bytes.toStdString());
    ASSERT_TRUE(imported_json) << imported_json.error().message;
    const auto *data = imported_json.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const QString id = QString::fromStdString(*asset_id->string_if());

    const auto baseline =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(baseline.exit_code, 0) << baseline.stdout_bytes.constData();
    auto baseline_recipe = recipe_json(baseline.stdout_bytes);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    const auto serialized_baseline = serialize_json(baseline_recipe.value());

    const auto noop =
        run({QStringLiteral("catalog"), QStringLiteral("develop"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(noop.exit_code, 0) << noop.stdout_bytes.constData();
    const auto after_noop =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(after_noop.exit_code, 0) << after_noop.stdout_bytes.constData();
    auto after_noop_recipe = recipe_json(after_noop.stdout_bytes);
    ASSERT_TRUE(after_noop_recipe) << after_noop_recipe.error().message;
    EXPECT_EQ(serialize_json(after_noop_recipe.value()), serialized_baseline);

    const auto enabled =
        run({QStringLiteral("catalog"), QStringLiteral("develop"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--set"),
             QStringLiteral("colorHarmonizerEnabled=1"), QStringLiteral("--json")});
    ASSERT_EQ(enabled.exit_code, 0) << enabled.stdout_bytes.constData();
    const auto enabled_recipe_run =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(enabled_recipe_run.exit_code, 0) << enabled_recipe_run.stdout_bytes.constData();
    auto enabled_recipe = recipe_json(enabled_recipe_run.stdout_bytes);
    ASSERT_TRUE(enabled_recipe) << enabled_recipe.error().message;
    const auto enabled_text = serialize_json(enabled_recipe.value());
    EXPECT_NE(enabled_text.find("ravo.color.colorharmonizer"), std::string::npos);
    EXPECT_NE(enabled_text.find("\"pull_strength\""), std::string::npos);
    const auto enabled_again =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    EXPECT_EQ(enabled_again.stdout_bytes, enabled_recipe_run.stdout_bytes);

    const auto edited =
        run({QStringLiteral("catalog"),    QStringLiteral("develop"),
             QStringLiteral("--catalog"),  catalog_q,
             QStringLiteral("--asset-id"), id,
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerRuleIndex=4"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerAnchorHueDegrees=198"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerPullStrength=0.82"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerPullWidth=1.84"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerSmoothing=0.5"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerNodeSaturation0=1.26"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerNodeSaturation1=0.18"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerNodeSaturation2=1.52"),
             QStringLiteral("--json")});
    ASSERT_EQ(edited.exit_code, 0) << edited.stdout_bytes.constData();

    const auto expect_fail = [&](const QStringList &extra)
    {
        QStringList arguments{QStringLiteral("catalog"),    QStringLiteral("develop"),
                              QStringLiteral("--catalog"),  catalog_q,
                              QStringLiteral("--asset-id"), id};
        arguments.append(extra);
        arguments.push_back(QStringLiteral("--json"));
        const auto failed = run(arguments);
        EXPECT_NE(failed.exit_code, 0) << extra.join(' ').toStdString();
        EXPECT_TRUE(failed.stderr_bytes.isEmpty()) << failed.stderr_bytes.constData();
        const auto parsed = parse_json(failed.stdout_bytes.toStdString());
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto *error = parsed.value().find("error");
        ASSERT_NE(error, nullptr);
        const auto *code = error->find("code");
        ASSERT_NE(code, nullptr);
        ASSERT_NE(code->string_if(), nullptr);
        EXPECT_EQ(*code->string_if(), "invalid_argument");
    };
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerEnabled=1"),
                 QStringLiteral("--set"), QStringLiteral("colorHarmonizerEnabled=1")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerRuleIndex=3.5")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerEnabled=0.5")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerPullStrength=nan")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerPullStrength=inf")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerPullStrength=1.5")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerSmoothing=-0.01")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerSmoothing=2.01")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("unknownHarmonizer=1")});

    const auto after_fail =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(after_fail.exit_code, 0) << after_fail.stdout_bytes.constData();
    auto after_fail_recipe = recipe_json(after_fail.stdout_bytes);
    ASSERT_TRUE(after_fail_recipe) << after_fail_recipe.error().message;
    EXPECT_NE(serialize_json(after_fail_recipe.value()).find("split_complementary"),
              std::string::npos);
    EXPECT_NE(serialize_json(after_fail_recipe.value()).find("\"smoothing\":0.5"),
              std::string::npos);

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
}

TEST_F(CliTest, RealCliLut3dPersistsProbesExportsAndRejectsChangedCorruptSource)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-lut3d-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = root / "library.sqlite";
    const auto cube = root / "red-compression.cube";
    const auto source = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                        "fixtures" / "frozen" / "0000-nop" / "expected.png";
    const auto source_before = source_file_snapshot(source.string());
    ASSERT_TRUE(source_before);
    {
        std::ofstream output(cube, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output << "TITLE \"red compression\"\n"
                  "LUT_3D_SIZE 2\n"
                  "DOMAIN_MIN 0 0 0\n"
                  "DOMAIN_MAX 1 1 1\n"
                  "0 0 0\n0.2 0 0\n0 1 0\n0.2 1 0\n"
                  "0 0 1\n0.2 0 1\n0 1 1\n0.2 1 1\n";
        ASSERT_TRUE(output);
    }

    struct CliRun
    {
        int exit_code = 1;
        QByteArray stdout_bytes;
        QByteArray stderr_bytes;
    };
    const auto run = [&](const QStringList &arguments) -> CliRun
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForStarted());
        EXPECT_TRUE(process.waitForFinished());
        return {process.exitCode(), process.readAllStandardOutput(),
                process.readAllStandardError()};
    };
    const auto parse_data = [](const QByteArray &stdout_bytes) -> Result<JsonValue>
    {
        auto response = parse_json(stdout_bytes.toStdString());
        if (!response)
            return response.error();
        const auto *data = response.value().find("data");
        if (data == nullptr)
            return make_error(ErrorCode::kValidation, "CLI response is missing data");
        return *data;
    };
    const auto recipe_text = [&](const QString &asset_id) -> Result<std::string>
    {
        const auto response =
            run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
                 QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), asset_id,
                 QStringLiteral("--json")});
        if (response.exit_code != 0)
            return make_error(ErrorCode::kIo, "CLI recipe command failed");
        auto data = parse_data(response.stdout_bytes);
        if (!data)
            return data.error();
        const auto *recipe = data.value().find("recipe");
        if (recipe == nullptr)
            return make_error(ErrorCode::kValidation, "CLI recipe payload is missing recipe");
        return serialize_json(*recipe);
    };
    const auto probe_luma = [&](const QStringList &extra) -> Result<double>
    {
        QStringList arguments{QStringLiteral("catalog"), QStringLiteral("probe"),
                              QStringLiteral("--catalog"), QString::fromStdString(catalog.string()),
                              QStringLiteral("--asset-id")};
        arguments.append(extra);
        arguments.append(
            {QStringLiteral("--max-edge"), QStringLiteral("64"), QStringLiteral("--json")});
        const auto response = run(arguments);
        if (response.exit_code != 0)
            return make_error(ErrorCode::kIo, "CLI probe command failed",
                              {{"stdout", response.stdout_bytes.toStdString()}});
        auto data = parse_data(response.stdout_bytes);
        if (!data)
            return data.error();
        const auto *statistics = data.value().find("statistics");
        const auto *luma = statistics == nullptr ? nullptr : statistics->find("display_luma_mean");
        if (luma == nullptr || luma->number_if() == nullptr)
            return make_error(ErrorCode::kValidation, "CLI probe payload is missing luma");
        return std::stod(luma->number_if()->text);
    };

    const auto created =
        run({QStringLiteral("catalog"), QStringLiteral("create"), QStringLiteral("--path"),
             QString::fromStdString(catalog.string()), QStringLiteral("--json")});
    ASSERT_EQ(created.exit_code, 0) << created.stdout_bytes.constData();
    const auto imported =
        run({QStringLiteral("catalog"), QStringLiteral("import"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--input"),
             QString::fromStdString(source.string()), QStringLiteral("--json")});
    ASSERT_EQ(imported.exit_code, 0) << imported.stdout_bytes.constData();
    auto import_data = parse_data(imported.stdout_bytes);
    ASSERT_TRUE(import_data) << import_data.error().message;
    const auto *items = import_data.value().find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 1U);
    const auto *asset = items->array_if()->front().find("asset");
    const auto *asset_id = asset == nullptr ? nullptr : asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const QString id = QString::fromStdString(*asset_id->string_if());

    auto baseline_luma = probe_luma({id, QStringLiteral("--baseline")});
    ASSERT_TRUE(baseline_luma) << baseline_luma.error().message;
    const auto developed =
        run({QStringLiteral("catalog"), QStringLiteral("develop"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), id,
             QStringLiteral("--set-text"),
             QStringLiteral("lut3dFile=") + QString::fromStdString(cube.string()),
             QStringLiteral("--set"), QStringLiteral("lut3dInputSpaceIndex=3"),
             QStringLiteral("--set"), QStringLiteral("lut3dOutputSpaceIndex=3"),
             QStringLiteral("--set"), QStringLiteral("lut3dInterpolationIndex=0"),
             QStringLiteral("--set"), QStringLiteral("lut3dStrength=1"), QStringLiteral("--json")});
    ASSERT_EQ(developed.exit_code, 0) << developed.stdout_bytes.constData();
    EXPECT_TRUE(developed.stderr_bytes.isEmpty());

    auto stored = recipe_text(id);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_NE(stored.value().find("ravo.color.lut3d"), std::string::npos);
    auto stored_recipe = parse_recipe_json(stored.value());
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    auto stored_develop = develop_from_recipe(stored_recipe.value());
    ASSERT_TRUE(stored_develop) << stored_develop.error().message;
    EXPECT_EQ(stored_develop.value().lut3d.file_path, cube.string());
    EXPECT_NE(stored.value().find("linear_rec709"), std::string::npos);
    auto developed_luma = probe_luma({id});
    ASSERT_TRUE(developed_luma) << developed_luma.error().message;
    EXPECT_LT(developed_luma.value(), baseline_luma.value());

    auto zero_strength_luma =
        probe_luma({id, QStringLiteral("--set"), QStringLiteral("lut3dStrength=0")});
    ASSERT_TRUE(zero_strength_luma) << zero_strength_luma.error().message;
    EXPECT_NEAR(zero_strength_luma.value(), baseline_luma.value(), 0.01);
    auto after_probe = recipe_text(id);
    ASSERT_TRUE(after_probe) << after_probe.error().message;
    EXPECT_EQ(after_probe.value(), stored.value());

    const auto exported_path = root / "lut-export.png";
    const auto exported =
        run({QStringLiteral("catalog"), QStringLiteral("export"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), id,
             QStringLiteral("--output"), QString::fromStdString(exported_path.string()),
             QStringLiteral("--format"), QStringLiteral("png"), QStringLiteral("--json")});
    ASSERT_EQ(exported.exit_code, 0) << exported.stdout_bytes.constData();
    EXPECT_TRUE(std::filesystem::exists(exported_path));
    EXPECT_GT(std::filesystem::file_size(exported_path), 8U);

    {
        std::ofstream output(cube, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output << "LUT_3D_SIZE 2\n0 0 0\n";
    }
    const auto corrupt =
        run({QStringLiteral("catalog"), QStringLiteral("probe"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), id,
             QStringLiteral("--max-edge"), QStringLiteral("64"), QStringLiteral("--json")});
    EXPECT_NE(corrupt.exit_code, 0);
    auto corrupt_json = parse_json(corrupt.stdout_bytes.toStdString());
    ASSERT_TRUE(corrupt_json) << corrupt_json.error().message;
    EXPECT_NE(corrupt_json.value().find("error"), nullptr);
    auto after_corrupt = recipe_text(id);
    ASSERT_TRUE(after_corrupt) << after_corrupt.error().message;
    EXPECT_EQ(after_corrupt.value(), stored.value());
    EXPECT_EQ(source_file_snapshot(source.string()), source_before);

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
}

} // namespace
} // namespace ravo
