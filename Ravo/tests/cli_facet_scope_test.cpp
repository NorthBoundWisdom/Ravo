#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
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

[[nodiscard]] std::size_t array_size_at(const JsonValue &value,
                                        const std::vector<std::string_view> &keys)
{
    const auto *found = find_path(value, keys);
    if (found == nullptr || found->array_if() == nullptr)
        return 0U;
    return found->array_if()->size();
}

[[nodiscard]] bool boolean_at(const JsonValue &value, const std::vector<std::string_view> &keys)
{
    const auto *found = find_path(value, keys);
    if (found == nullptr || found->boolean_if() == nullptr)
        return false;
    return *found->boolean_if();
}

[[nodiscard]] std::string first_entry_count(const JsonValue &value, const std::string_view facet)
{
    const auto *found = find_path(value, {"data", facet});
    if (found == nullptr || found->array_if() == nullptr || found->array_if()->empty())
        return {};
    const auto *count = found->array_if()->front().find("count");
    if (count == nullptr || count->number_if() == nullptr)
        return {};
    return count->number_if()->text;
}

[[nodiscard]] std::string first_entry_key(const JsonValue &value, const std::string_view facet)
{
    const auto *found = find_path(value, {"data", facet});
    if (found == nullptr || found->array_if() == nullptr || found->array_if()->empty())
        return {};
    const auto *key = found->array_if()->front().find("key");
    if (key == nullptr || key->string_if() == nullptr)
        return {};
    return *key->string_if();
}

[[nodiscard]] std::string asset_id_at(const JsonValue &listed, const std::size_t index)
{
    const auto *assets = find_path(listed, {"data", "assets"});
    if (assets == nullptr || assets->array_if() == nullptr || assets->array_if()->size() <= index)
        return {};
    const auto *id = assets->array_if()->at(index).find("id");
    if (id == nullptr || id->string_if() == nullptr)
        return {};
    return *id->string_if();
}

} // namespace

TEST_F(CliTest, CatalogFacetsScopeCountsToTheActiveFilter)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-facet-scope-" + generate_catalog_id());
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
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "import", "--catalog",
                                                            catalog, "--input", png_path, "--input",
                                                            mire1_path(), "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(array_size_at(listed.value(), {"data", "assets"}), 2U);
    const auto first_id = asset_id_at(listed.value(), 0U);
    const auto second_id = asset_id_at(listed.value(), 1U);
    ASSERT_FALSE(first_id.empty());
    ASSERT_FALSE(second_id.empty());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "metadata", "--catalog", catalog, "--asset-id", first_id, "--country",
                  "China", "--city", "Shanghai", "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "metadata", "--catalog", catalog, "--asset-id", second_id, "--country",
                  "Japan", "--city", "Kyoto", "--json"}),
              0)
        << stdout_stream.str();

    // Whole-catalog counts stay the default and report themselves as unscoped.
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "facets", "--catalog",
                                                            catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto unscoped = parse_json(stdout_stream.str());
    ASSERT_TRUE(unscoped) << unscoped.error().message;
    EXPECT_FALSE(boolean_at(unscoped.value(), {"data", "scoped"}));
    EXPECT_EQ(array_size_at(unscoped.value(), {"data", "countries"}), 2U);
    EXPECT_EQ(array_size_at(unscoped.value(), {"data", "cities"}), 2U);
    EXPECT_EQ(first_entry_key(unscoped.value(), "countries"), "China");
    EXPECT_EQ(first_entry_count(unscoped.value(), "countries"), "1");

    // A location filter narrows the counts and echoes the applied scope.
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "facets", "--catalog", catalog, "--country", "China", "--json"}),
              0)
        << stdout_stream.str();
    auto scoped = parse_json(stdout_stream.str());
    ASSERT_TRUE(scoped) << scoped.error().message;
    EXPECT_TRUE(boolean_at(scoped.value(), {"data", "scoped"}));
    EXPECT_EQ(string_at(scoped.value(), {"data", "scope", "country"}), "China");
    ASSERT_EQ(array_size_at(scoped.value(), {"data", "countries"}), 1U);
    EXPECT_EQ(first_entry_count(scoped.value(), "countries"), "1");
    ASSERT_EQ(array_size_at(scoped.value(), {"data", "cities"}), 1U);
    EXPECT_EQ(first_entry_key(scoped.value(), "cities"), "Shanghai");

    // A raw query document scopes the same way as the shorthand flags.
    LibraryQuery document_scope;
    document_scope.city_equals = "Kyoto";
    auto serialized = serialize_library_query_document(document_scope);
    ASSERT_TRUE(serialized) << serialized.error().message;
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "facets", "--catalog", catalog,
                                                      "--query", serialized.value(), "--json"}),
        0)
        << stdout_stream.str();
    auto by_document = parse_json(stdout_stream.str());
    ASSERT_TRUE(by_document) << by_document.error().message;
    EXPECT_TRUE(boolean_at(by_document.value(), {"data", "scoped"}));
    EXPECT_EQ(string_at(by_document.value(), {"data", "scope", "city"}), "Kyoto");
    ASSERT_EQ(array_size_at(by_document.value(), {"data", "countries"}), 1U);
    EXPECT_EQ(first_entry_key(by_document.value(), "countries"), "Japan");
    EXPECT_EQ(first_entry_count(by_document.value(), "countries"), "1");

    // A scope that selects nothing reports empty facets rather than catalog totals.
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "facets", "--catalog", catalog, "--city", "Nowhere", "--json"}),
              0)
        << stdout_stream.str();
    auto empty_scope = parse_json(stdout_stream.str());
    ASSERT_TRUE(empty_scope) << empty_scope.error().message;
    EXPECT_TRUE(boolean_at(empty_scope.value(), {"data", "scoped"}));
    EXPECT_EQ(array_size_at(empty_scope.value(), {"data", "countries"}), 0U);
    EXPECT_EQ(array_size_at(empty_scope.value(), {"data", "cities"}), 0U);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogFacetsRejectIncompleteAndMisplacedScopes)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-facet-reject-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    // `--camera-make` alone keeps catalog list semantics: the model selector
    // becomes an explicit "empty model" match rather than an error, and the
    // empty catalog yields no facet values.
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "facets", "--catalog", catalog, "--camera-make", "RavoCam", "--json"}),
              0)
        << stdout_stream.str();
    auto make_only = parse_json(stdout_stream.str());
    ASSERT_TRUE(make_only) << make_only.error().message;
    EXPECT_TRUE(boolean_at(make_only.value(), {"data", "scoped"}));
    EXPECT_EQ(string_at(make_only.value(), {"data", "scope", "camera_make"}), "RavoCam");
    EXPECT_EQ(string_at(make_only.value(), {"data", "scope", "camera_model"}), "");
    EXPECT_EQ(array_size_at(make_only.value(), {"data", "cameras"}), 0U);

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{"catalog", "facets", "--catalog",
                                                            catalog, "--captured-local-date",
                                                            "2024-05-01", "--json"}),
              0);
    auto bad_date = parse_json(stdout_stream.str());
    ASSERT_TRUE(bad_date) << bad_date.error().message;
    EXPECT_EQ(string_at(bad_date.value(), {"error", "context", "reason"}),
              "invalid_library_capture_date_facet");

    // Scope flags stay rejected on subcommands that do not filter.
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_NE(application.run(std::vector<std::string_view>{"catalog", "sets", "--catalog", catalog,
                                                            "--country", "China", "--json"}),
              0);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

} // namespace ravo
