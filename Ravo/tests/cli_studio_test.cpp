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

TEST_F(CliTest, CatalogSidecarAndBackupCommandsExposeVersionedJsonArtifacts)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-backup-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto fixture = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                         "fixtures" / "frozen" / "0000-nop" / "expected.png";
    const auto source = root / "source.png";
    std::filesystem::copy_file(fixture, source);
    const auto source_before = source_file_snapshot(source.string());
    ASSERT_TRUE(source_before);

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "create", "--catalog",
                                                            catalog, "--json"}),
              0)
        << stdout_stream.str();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", source.string(), "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *items = imported.value().find("data")->find("items")->array_if();
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size(), 1U);
    const auto *id_value = items->front().find("asset")->find("id")->string_if();
    ASSERT_NE(id_value, nullptr);
    const std::string asset_id = *id_value;

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "sidecar-status", "--catalog",
                                                      catalog, "--asset-id", asset_id, "--json"}),
        0)
        << stdout_stream.str();
    auto status = parse_json(stdout_stream.str());
    ASSERT_TRUE(status) << status.error().message;
    const auto *pending = status.value().find("data")->find("pending")->number_if();
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(pending->text, "0");
    const auto *states = status.value().find("data")->find("states")->array_if();
    ASSERT_NE(states, nullptr);
    ASSERT_EQ(states->size(), 1U);
    ASSERT_NE(states->front().find("pending")->boolean_if(), nullptr);
    EXPECT_FALSE(*states->front().find("pending")->boolean_if());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "sidecar-sync", "--catalog",
                                                      catalog, "--asset-id", asset_id, "--json"}),
        0)
        << stdout_stream.str();
    auto synchronized = parse_json(stdout_stream.str());
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    const auto *artifacts = synchronized.value().find("data")->find("artifacts")->array_if();
    ASSERT_NE(artifacts, nullptr);
    ASSERT_EQ(artifacts->size(), 1U);
    const auto *sidecar_sha = artifacts->front().find("sha256")->string_if();
    ASSERT_NE(sidecar_sha, nullptr);
    EXPECT_EQ(sidecar_sha->size(), 64U);

    const auto backup = (root / "backup").string();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "backup", "--catalog",
                                                            catalog, "--backup", backup, "--json"}),
              0)
        << stdout_stream.str();
    auto created = parse_json(stdout_stream.str());
    ASSERT_TRUE(created) << created.error().message;
    const auto *format = created.value().find("data")->find("format_version")->number_if();
    ASSERT_NE(format, nullptr);
    EXPECT_EQ(format->text, std::to_string(kCatalogBackupFormatVersion));

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "backup-verify", "--backup",
                                                            backup, "--json"}),
              0)
        << stdout_stream.str();
    auto verified = parse_json(stdout_stream.str());
    ASSERT_TRUE(verified) << verified.error().message;
    ASSERT_NE(verified.value().find("data")->find("verified")->boolean_if(), nullptr);
    EXPECT_TRUE(*verified.value().find("data")->find("verified")->boolean_if());

    const auto restored_catalog = (root / "restored.sqlite").string();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "backup-restore", "--backup",
                                                            backup, "--output", restored_catalog,
                                                            "--json"}),
              0)
        << stdout_stream.str();
    auto restored = parse_json(stdout_stream.str());
    ASSERT_TRUE(restored) << restored.error().message;
    const auto *restore_data = restored.value().find("data");
    ASSERT_NE(restore_data, nullptr);
    ASSERT_NE(restore_data->find("published")->boolean_if(), nullptr);
    EXPECT_TRUE(*restore_data->find("published")->boolean_if());
    ASSERT_NE(restore_data->find("previews_rebuild_required")->boolean_if(), nullptr);
    EXPECT_TRUE(*restore_data->find("previews_rebuild_required")->boolean_if());
    ASSERT_NE(restore_data->find("catalog")->find("path")->string_if(), nullptr);
    EXPECT_EQ(*restore_data->find("catalog")->find("path")->string_if(), restored_catalog);
    EXPECT_TRUE(std::filesystem::is_regular_file(restored_catalog));
    EXPECT_TRUE(std::filesystem::is_directory(restored_catalog + ".ravo/sidecars"));

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "list", "--catalog",
                                                            restored_catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto restored_list = parse_json(stdout_stream.str());
    ASSERT_TRUE(restored_list) << restored_list.error().message;
    const auto *restored_items = restored_list.value().find("data")->find("assets")->array_if();
    ASSERT_NE(restored_items, nullptr);
    ASSERT_EQ(restored_items->size(), 1U);
    EXPECT_EQ(*restored_items->front().find("id")->string_if(), asset_id);

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "preview-rebuild",
                                                            "--catalog", restored_catalog,
                                                            "--asset-id", asset_id, "--json"}),
              0)
        << stdout_stream.str();
    auto rebuilt = parse_json(stdout_stream.str());
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    const auto *rebuild_data = rebuilt.value().find("data");
    ASSERT_NE(rebuild_data, nullptr);
    const auto *rebuild_succeeded = rebuild_data->find("succeeded")->number_if();
    ASSERT_NE(rebuild_succeeded, nullptr);
    EXPECT_EQ(rebuild_succeeded->text, "1");
    const auto *rebuild_items = rebuild_data->find("items")->array_if();
    ASSERT_NE(rebuild_items, nullptr);
    ASSERT_EQ(rebuild_items->size(), 1U);
    ASSERT_NE(rebuild_items->front().find("browse_cache_path")->string_if(), nullptr);
    ASSERT_NE(rebuild_items->front().find("develop_cache_path")->string_if(), nullptr);
    EXPECT_TRUE(std::filesystem::is_regular_file(
        *rebuild_items->front().find("browse_cache_path")->string_if()));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        *rebuild_items->front().find("develop_cache_path")->string_if()));

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "backup-restore", "--backup",
                                                            backup, "--output", restored_catalog,
                                                            "--json"}),
              6)
        << stdout_stream.str();
    auto restore_conflict = parse_json(stdout_stream.str());
    ASSERT_TRUE(restore_conflict) << restore_conflict.error().message;
    ASSERT_NE(restore_conflict.value().find("error"), nullptr);
    EXPECT_EQ(*restore_conflict.value().find("error")->find("code")->string_if(), "conflict");

    const auto schedule_directory = (root / "scheduled").string();
    ASSERT_TRUE(std::filesystem::create_directory(schedule_directory));
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{
            "catalog", "backup-policy", "--catalog", catalog, "--schedule-dir", schedule_directory,
            "--interval-minutes", "15", "--retention-count", "1", "--enabled", "true", "--json"}),
        0)
        << stdout_stream.str();
    auto policy = parse_json(stdout_stream.str());
    ASSERT_TRUE(policy) << policy.error().message;
    ASSERT_NE(policy.value().find("data")->find("enabled")->boolean_if(), nullptr);
    EXPECT_TRUE(*policy.value().find("data")->find("enabled")->boolean_if());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "backup-run", "--catalog",
                                                            catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto scheduled = parse_json(stdout_stream.str());
    ASSERT_TRUE(scheduled) << scheduled.error().message;
    ASSERT_NE(scheduled.value().find("data")->find("ran")->boolean_if(), nullptr);
    EXPECT_TRUE(*scheduled.value().find("data")->find("ran")->boolean_if());
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(schedule_directory),
                            std::filesystem::directory_iterator()),
              1);
    EXPECT_EQ(source_file_snapshot(source.string()), source_before);
    EXPECT_TRUE(stderr_stream.str().empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogFoldersExposeStableMissingIdentityAndRelinkExplicitly)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-relink-" + generate_catalog_id());
    const auto original = root / "original";
    const auto replacement = root / "replacement";
    std::filesystem::create_directories(original);
    const auto catalog = (root / "library.sqlite").string();
    const auto fixture = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                         "fixtures" / "frozen" / "0000-nop" / "expected.png";
    const auto source = original / "source.png";
    std::filesystem::copy_file(fixture, source);
    const auto source_before = source_file_snapshot(source.string());
    ASSERT_TRUE(source_before);

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "create", "--catalog",
                                                            catalog, "--json"}),
              0)
        << stdout_stream.str();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", source.string(), "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "folders", "--catalog",
                                                            catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    const auto *folders = listed.value().find("data")->find("folders")->array_if();
    ASSERT_NE(folders, nullptr);
    std::string folder_id;
    for (const auto &folder : *folders)
    {
        const auto *name = folder.find("display_name");
        if (name == nullptr || name->string_if() == nullptr || *name->string_if() != "original")
            continue;
        const auto *id = folder.find("folder_id");
        ASSERT_NE(id, nullptr);
        ASSERT_NE(id->string_if(), nullptr);
        folder_id = *id->string_if();
        ASSERT_NE(folder.find("missing")->boolean_if(), nullptr);
        EXPECT_FALSE(*folder.find("missing")->boolean_if());
    }
    ASSERT_FALSE(folder_id.empty());

    std::filesystem::rename(original, replacement);
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "folders", "--catalog",
                                                            catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto missing = parse_json(stdout_stream.str());
    ASSERT_TRUE(missing) << missing.error().message;
    const auto *missing_folders = missing.value().find("data")->find("folders")->array_if();
    ASSERT_NE(missing_folders, nullptr);
    const auto missing_folder = std::find_if(missing_folders->begin(), missing_folders->end(),
                                             [&](const JsonValue &folder)
                                             {
                                                 const auto *id = folder.find("folder_id");
                                                 return id != nullptr &&
                                                        id->string_if() != nullptr &&
                                                        *id->string_if() == folder_id;
                                             });
    ASSERT_NE(missing_folder, missing_folders->end());
    ASSERT_NE(missing_folder->find("missing")->boolean_if(), nullptr);
    EXPECT_TRUE(*missing_folder->find("missing")->boolean_if());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "folder-relink", "--catalog", catalog, "--folder-id", folder_id,
                  "--replacement", replacement.string(), "--json"}),
              0)
        << stdout_stream.str();
    auto relinked = parse_json(stdout_stream.str());
    ASSERT_TRUE(relinked) << relinked.error().message;
    const auto *data = relinked.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("folder_id")->string_if(), nullptr);
    EXPECT_EQ(*data->find("folder_id")->string_if(), folder_id);
    ASSERT_NE(data->find("asset_count")->number_if(), nullptr);
    EXPECT_EQ(data->find("asset_count")->number_if()->text, "1");
    ASSERT_NE(data->find("recovery_pending")->number_if(), nullptr);
    EXPECT_EQ(data->find("recovery_pending")->number_if()->text, "1");
    EXPECT_EQ(source_file_snapshot((replacement / "source.png").string()), source_before);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogNamedLibrarySetsCreateListAndFilter)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-sets-" + generate_catalog_id());
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
    ASSERT_TRUE(imported);
    const auto id = *imported.value()
                         .find("data")
                         ->find("items")
                         ->array_if()
                         ->front()
                         .find("asset")
                         ->find("id")
                         ->string_if();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "set-create", "--catalog", catalog,
                                                      "--name", "Job", "--asset-id", id, "--json"}),
        0)
        << stdout_stream.str();
    auto created = parse_json(stdout_stream.str());
    ASSERT_TRUE(created) << created.error().message;
    const auto set_id = *created.value().find("data")->find("set")->find("id")->string_if();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "sets", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().find("data")->find("sets")->array_if()->size(), 1U);
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "list", "--catalog", catalog,
                                                            "--set-id", set_id, "--json"}),
              0)
        << stdout_stream.str();
    auto assets = parse_json(stdout_stream.str());
    ASSERT_TRUE(assets);
    ASSERT_EQ(assets.value().find("data")->find("assets")->array_if()->size(), 1U);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogVersionsStacksAndCollapsedList)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-versions-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto fixture = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                         "fixtures" / "frozen" / "0000-nop" / "expected.png";
    const auto first_source = root / "first.png";
    const auto second_source = root / "second.png";
    std::filesystem::copy_file(fixture, first_source);
    std::filesystem::copy_file(fixture, second_source);
    const auto first_text = first_source.string();
    const auto second_text = second_source.string();
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
                                                            catalog, "--input", first_text,
                                                            "--input", second_text, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported);
    const auto *items = imported.value().find("data")->find("items")->array_if();
    ASSERT_NE(items, nullptr);
    ASSERT_EQ(items->size(), 2U);
    const auto first_id = *items->front().find("asset")->find("id")->string_if();
    const auto second_id = *items->back().find("asset")->find("id")->string_if();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "version-create", "--catalog",
                                                      catalog, "--asset-id", first_id, "--json"}),
        0)
        << stdout_stream.str();
    auto versioned = parse_json(stdout_stream.str());
    ASSERT_TRUE(versioned) << versioned.error().message;
    const auto *ordinal = versioned.value().find("data")->find("asset")->find("version_ordinal");
    ASSERT_NE(ordinal, nullptr);
    ASSERT_NE(ordinal->number_if(), nullptr);
    EXPECT_EQ(ordinal->number_if()->text, "1");
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "stack", "--catalog", catalog, "--asset-id", first_id, "--asset-id",
                  second_id, "--pick-id", first_id, "--json"}),
              0)
        << stdout_stream.str();
    auto stacked = parse_json(stdout_stream.str());
    ASSERT_TRUE(stacked) << stacked.error().message;
    const auto stack_id = *stacked.value().find("data")->find("stack")->find("id")->string_if();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto collapsed = parse_json(stdout_stream.str());
    ASSERT_TRUE(collapsed);
    EXPECT_EQ(collapsed.value().find("data")->find("assets")->array_if()->size(), 2U);
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "list", "--catalog", catalog,
                                                            "--stack-expanded", "--json"}),
              0)
        << stdout_stream.str();
    auto expanded = parse_json(stdout_stream.str());
    ASSERT_TRUE(expanded);
    EXPECT_EQ(expanded.value().find("data")->find("assets")->array_if()->size(), 3U);
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "unstack", "--catalog", catalog, "--stack-id", stack_id, "--json"}),
              0)
        << stdout_stream.str();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogBatchExportUsesStrictTemplateAndSharedTypedOptions)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-batch-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto fixture = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" /
                         "fixtures" / "frozen" / "0000-nop" / "expected.png";
    const auto first_source = root / "first.png";
    const auto second_source = root / "second.png";
    std::filesystem::copy_file(fixture, first_source);
    std::filesystem::copy_file(fixture, second_source);
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();
    stdout_stream.str({});
    stdout_stream.clear();
    const auto first_text = first_source.string();
    const auto second_text = second_source.string();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "import", "--catalog",
                                                            catalog, "--input", first_text,
                                                            "--input", second_text, "--json"}),
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
    std::vector<std::string> asset_ids;
    for (const auto &item : *items->array_if())
    {
        const auto *asset = item.find("asset");
        ASSERT_NE(asset, nullptr);
        const auto *asset_id = asset->find("id");
        ASSERT_NE(asset_id, nullptr);
        ASSERT_NE(asset_id->string_if(), nullptr);
        asset_ids.push_back(*asset_id->string_if());
    }
    const auto output_directory = root / "delivery";
    std::filesystem::create_directory(output_directory);
    const auto output_text = output_directory.string();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export-batch", "--catalog", catalog, "--asset-id", asset_ids[0],
                  "--asset-id", asset_ids[1], "--output-dir", output_text, "--filename-template",
                  "{sequence}-{stem}{ext}", "--format", "jpeg", "--quality", "80", "--metadata",
                  "none", "--json"}),
              0)
        << stdout_stream.str();
    auto exported = parse_json(stdout_stream.str());
    ASSERT_TRUE(exported) << exported.error().message;
    data = exported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *count = data->find("exported");
    ASSERT_NE(count, nullptr);
    ASSERT_NE(count->number_if(), nullptr);
    EXPECT_EQ(count->number_if()->text, "2");
    const auto *metadata_mode = data->find("metadata_mode");
    ASSERT_NE(metadata_mode, nullptr);
    ASSERT_NE(metadata_mode->string_if(), nullptr);
    EXPECT_EQ(*metadata_mode->string_if(), "none");
    EXPECT_TRUE(std::filesystem::exists(output_directory / "0001-first.jpg"));
    EXPECT_TRUE(std::filesystem::exists(output_directory / "0002-second.jpg"));

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export-batch", "--catalog", catalog, "--asset-id", asset_ids[0],
                  "--asset-id", asset_ids[1], "--output-dir", output_text, "--filename-template",
                  "{sequence}-{stem}{ext}", "--format", "jpeg", "--json"}),
              6);
    auto conflict = parse_json(stdout_stream.str());
    ASSERT_TRUE(conflict) << conflict.error().message;
    const auto *error = conflict.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *context = error->find("context");
    ASSERT_NE(context, nullptr);
    const auto *completed = context->find("completed_count");
    ASSERT_NE(completed, nullptr);
    ASSERT_NE(completed->string_if(), nullptr);
    EXPECT_EQ(*completed->string_if(), "0");
    EXPECT_TRUE(stderr_stream.str().empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogListJsonIncludesCapturedAtAndGps)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-capture-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto raw = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "images" / "mire1.cr2")
                         .generic_u8string();
    const std::string raw_path(raw.begin(), raw.end());
    const auto located_path = root / "located.tif";
    const auto below_zero_path = root / "below-zero.tif";
    {
        const auto bytes = test_support::make_capture_exif_tiff();
        std::ofstream output(located_path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    {
        test_support::CaptureExifProfile profile;
        profile.latitude_ref = 'S';
        profile.longitude_ref = 'W';
        profile.altitude_ref = 1U;
        profile.altitude = {0U, 1U};
        const auto bytes = test_support::make_capture_exif_tiff(profile);
        std::ofstream output(below_zero_path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0);
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", raw_path, "--json"}),
              0)
        << stdout_stream.str();
    for (const auto &path : {located_path, below_zero_path})
    {
        stdout_stream.str({});
        stdout_stream.clear();
        const auto text = path.string();
        EXPECT_EQ(application.run(std::vector<std::string_view>{
                      "catalog", "import", "--catalog", catalog, "--input", text, "--json"}),
                  0)
            << stdout_stream.str();
    }
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    const std::string first_list = stdout_stream.str();
    auto listed = parse_json(first_list);
    ASSERT_TRUE(listed) << listed.error().message;
    const auto *data = listed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *assets = data->find("assets");
    ASSERT_NE(assets, nullptr);
    ASSERT_EQ(assets->array_if()->size(), 3U);
    const auto find_asset = [&](const std::string_view filename) -> const JsonValue *
    {
        for (const auto &asset : *assets->array_if())
        {
            const auto *uri = asset.find("uri");
            if (uri != nullptr && uri->string_if() != nullptr &&
                uri->string_if()->find(filename) != std::string::npos)
            {
                return &asset;
            }
        }
        return nullptr;
    };
    const auto *raw_asset = find_asset("mire1.cr2");
    ASSERT_NE(raw_asset, nullptr);
    const auto *capture = raw_asset->find("capture");
    ASSERT_NE(capture, nullptr);
    const auto *captured_at = capture->find("captured_at");
    ASSERT_NE(captured_at, nullptr);
    ASSERT_NE(captured_at->string_if(), nullptr);
    EXPECT_EQ(*captured_at->string_if(), "2007-09-11T13:53:33.18");
    const auto *gps = capture->find("gps");
    ASSERT_NE(gps, nullptr);
    EXPECT_TRUE(gps->is_null());

    const auto verify_gps = [](const JsonValue &asset, const std::string_view latitude,
                               const std::string_view longitude, const std::string_view altitude)
    {
        const auto *capture_value = asset.find("capture");
        ASSERT_NE(capture_value, nullptr);
        const auto *captured_at_value = capture_value->find("captured_at");
        ASSERT_NE(captured_at_value, nullptr);
        ASSERT_NE(captured_at_value->string_if(), nullptr);
        EXPECT_EQ(*captured_at_value->string_if(), "2007-09-11T13:53:33.18+02:00");
        const auto *gps_value = capture_value->find("gps");
        ASSERT_NE(gps_value, nullptr);
        const auto *lat = gps_value->find("latitude");
        const auto *lon = gps_value->find("longitude");
        const auto *alt = gps_value->find("altitude_m");
        ASSERT_NE(lat, nullptr);
        ASSERT_NE(lon, nullptr);
        ASSERT_NE(alt, nullptr);
        ASSERT_NE(lat->number_if(), nullptr);
        ASSERT_NE(lon->number_if(), nullptr);
        ASSERT_NE(alt->number_if(), nullptr);
        EXPECT_EQ(lat->number_if()->text, latitude);
        EXPECT_EQ(lon->number_if()->text, longitude);
        EXPECT_EQ(alt->number_if()->text, altitude);
    };
    const auto *located = find_asset("located.tif");
    const auto *below_zero = find_asset("below-zero.tif");
    ASSERT_NE(located, nullptr);
    ASSERT_NE(below_zero, nullptr);
    verify_gps(*located, "49.253239", "3.050766", "123.456");
    verify_gps(*below_zero, "-49.253239", "-3.050766", "0");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0);
    EXPECT_EQ(stdout_stream.str(), first_list);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace
} // namespace ravo
