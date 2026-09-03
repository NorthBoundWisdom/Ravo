#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/json.h"

#include "cli_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string foreign_fixture_path(const std::string_view name)
{
    const auto path = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                       "foreign_catalog" / std::string(name))
                          .generic_u8string();
    return std::string(path.begin(), path.end());
}

[[nodiscard]] const JsonValue *find_path(const JsonValue &value,
                                         const std::vector<std::string_view> &keys)
{
    const JsonValue *current = &value;
    for (const auto key : keys)
    {
        if (current == nullptr)
            return nullptr;
        current = current->find(key);
    }
    return current;
}

[[nodiscard]] std::string string_at(const JsonValue &value,
                                    const std::vector<std::string_view> &keys)
{
    const auto *found = find_path(value, keys);
    if (found == nullptr || found->string_if() == nullptr)
        return {};
    return *found->string_if();
}

[[nodiscard]] std::string number_at(const JsonValue &value,
                                    const std::vector<std::string_view> &keys)
{
    const auto *found = find_path(value, keys);
    if (found == nullptr || found->number_if() == nullptr)
        return {};
    return found->number_if()->text;
}

} // namespace

TEST_F(CliTest, CatalogConvertForeignReportsMappedSkippedAndUnsupported)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-convert-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "converted.sqlite").string();
    const auto source = foreign_fixture_path("lightroom-classic-v1.json");

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
                  "catalog", "convert-foreign", "--catalog", catalog, "--foreign-source", source,
                  "--source-kind", "lightroom-classic", "--json"}),
              0)
        << stdout_stream.str();
    auto converted = parse_json(stdout_stream.str());
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(string_at(converted.value(), {"data", "schema"}), "ravo.foreign-catalog.fixture/v1");
    EXPECT_EQ(string_at(converted.value(), {"data", "source_kind"}), "lightroom-classic");
    EXPECT_EQ(string_at(converted.value(), {"data", "destination_catalog"}), catalog);
    EXPECT_EQ(number_at(converted.value(), {"data", "imported"}), "1");
    EXPECT_EQ(number_at(converted.value(), {"data", "skipped"}), "1");
    EXPECT_EQ(number_at(converted.value(), {"data", "failed"}), "0");
    const auto *originals_unchanged = find_path(converted.value(), {"data", "originals_unchanged"});
    ASSERT_NE(originals_unchanged, nullptr);
    ASSERT_NE(originals_unchanged->boolean_if(), nullptr);
    EXPECT_TRUE(*originals_unchanged->boolean_if());
    const auto *items = find_path(converted.value(), {"data", "items"});
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 2U);

    // The new catalog is the live authority and lists the converted asset.
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    const auto *assets = find_path(listed.value(), {"data", "assets"});
    ASSERT_NE(assets, nullptr);
    ASSERT_NE(assets->array_if(), nullptr);
    EXPECT_EQ(assets->array_if()->size(), 1U);

    // Converting again into a populated catalog fails closed.
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{"catalog", "convert-foreign",
                                                            "--catalog", catalog,
                                                            "--foreign-source", source, "--json"}),
              0);
    auto conflict = parse_json(stdout_stream.str());
    ASSERT_TRUE(conflict) << conflict.error().message;
    EXPECT_EQ(string_at(conflict.value(), {"error", "context", "reason"}),
              "destination_catalog_not_empty");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogConvertForeignRejectsVendorBinariesAndScopedFlags)
{
    const auto root = std::filesystem::temp_directory_path() /
                      ("ravo-cli-convert-reject-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "rejected.sqlite").string();

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{
                  "catalog", "convert-foreign", "--catalog", catalog, "--foreign-source",
                  foreign_fixture_path("not-a-catalog.lrcat"), "--json"}),
              0);
    auto vendor = parse_json(stdout_stream.str());
    ASSERT_TRUE(vendor) << vendor.error().message;
    EXPECT_EQ(string_at(vendor.value(), {"error", "context", "reason"}),
              "unsupported_source_schema");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{
                  "catalog", "convert-foreign", "--catalog", catalog, "--foreign-source",
                  foreign_fixture_path("unsupported-version.json"), "--json"}),
              0);
    auto version = parse_json(stdout_stream.str());
    ASSERT_TRUE(version) << version.error().message;
    EXPECT_EQ(string_at(version.value(), {"error", "context", "reason"}),
              "unsupported_source_version");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{"catalog", "convert-foreign",
                                                            "--catalog", catalog, "--json"}),
              0);

    // Conversion flags stay scoped to convert-foreign.
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{
                  "catalog", "list", "--catalog", catalog, "--foreign-source",
                  foreign_fixture_path("lightroom-classic-v1.json"), "--json"}),
              0);

    // Nothing was published by any rejected run.
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    const auto *assets = find_path(listed.value(), {"data", "assets"});
    ASSERT_NE(assets, nullptr);
    ASSERT_NE(assets->array_if(), nullptr);
    EXPECT_TRUE(assets->array_if()->empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace ravo
