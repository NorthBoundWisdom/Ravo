#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <set>

#include "cli_test_support.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/foundation/json.h"

namespace ravo
{
TEST_F(CliTest, RealCatalogListPagesAreBoundedRevisionCheckedAndStrict)
{
    QTemporaryDir directory;
    const auto path = directory.filePath("library.sqlite");
    auto repository = SqliteCatalogRepository::create(path.toStdString());
    ASSERT_TRUE(repository);
    for (int i = 0; i < 250; ++i)
    {
        AssetRecord record;
        record.id = "ast_page_" + std::to_string(i);
        record.normalized_uri = "file:///page/" + std::to_string(i) + ".png";
        record.media_type = std::string(kMediaTypePng);
        record.created_unix_ms = i;
        ASSERT_TRUE(repository.value()->commit_imported_asset(record));
    }
    auto set = repository.value()->create_library_set(LibrarySetKind::kManual, "Selected", {},
                                                      {"ast_page_0", "ast_page_249"}, {});
    ASSERT_TRUE(set);
    const auto set_id = QString::fromStdString(set.value().set.id);
    const auto revision = repository.value()->snapshot().value().revision;
    repository.value().reset();
    const auto run = [&](const QStringList &extra, const bool success)
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE),
                      QStringList{"catalog", "list", "--catalog", path, "--json"} + extra);
        EXPECT_TRUE(process.waitForFinished(30000));
        const auto output = process.readAllStandardOutput().toStdString();
        EXPECT_EQ(process.exitCode() == 0, success) << output;
        return parse_json(output);
    };
    std::set<std::string> seen;
    for (const int offset : {0, 100, 200, 300})
    {
        auto response = run({"--offset", QString::number(offset), "--limit", "100", "--revision",
                             QString::number(revision)},
                            true);
        ASSERT_TRUE(response);
        const auto *data = response.value().find("data");
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(*data->find("schema")->string_if(), "ravo.library.page/v1");
        ASSERT_NE(data->find("query"), nullptr);
        EXPECT_TRUE(parse_library_query_document(serialize_json(*data->find("query"))));
        EXPECT_EQ(data->find("total")->number_if()->text, "250");
        const auto &assets = *data->find("assets")->array_if();
        EXPECT_EQ(assets.size(), offset < 200 ? 100U : offset == 200 ? 50U : 0U);
        EXPECT_EQ(*data->find("has_more")->boolean_if(), offset < 200);
        EXPECT_EQ(data->find("materialized_rows")->number_if()->text,
                  std::to_string(assets.size()));
        for (const auto &asset : assets)
            EXPECT_TRUE(seen.insert(*asset.find("id")->string_if()).second);
    }
    EXPECT_EQ(seen.size(), 250U);
    auto subset = run({"--limit", "100", "--set-id", set_id}, true);
    ASSERT_TRUE(subset);
    ASSERT_NE(subset.value().find("data"), nullptr);
    EXPECT_EQ(subset.value().find("data")->find("assets")->array_if()->size(), 2U);
    EXPECT_EQ(*subset.value().find("data")->find("set_id")->string_if(), set_id.toStdString());
    LibraryQuery query;
    query.media_types = {std::string(kMediaTypeJpeg)};
    auto query_document = serialize_library_query_document(query);
    ASSERT_TRUE(query_document);
    auto filtered =
        run({"--limit", "100", "--query", QString::fromStdString(query_document.value())}, true);
    ASSERT_TRUE(filtered);
    ASSERT_NE(filtered.value().find("data"), nullptr);
    EXPECT_TRUE(filtered.value().find("data")->find("assets")->array_if()->empty());
    for (const auto &extra : std::vector<QStringList>{
             {"--limit", "0"},
             {"--limit", "513"},
             {"--offset", "1"},
             {"--limit", "1.5"},
             {"--limit", "1", "--offset", "-1"},
             {"--limit", "1", "--offset", "9007199254740992"},
             {"--limit", "1", "--limit", "2"},
             {"--limit", "1", "--query", R"({"schema_version":999})"},
             {"--limit", "1", "--query", R"({"schema_version":5,"unknown":true})"}})
    {
        auto response = run(extra, false);
        ASSERT_TRUE(response);
        EXPECT_NE(response.value().find("error"), nullptr);
    }
    auto stale = run({"--limit", "1", "--revision", QString::number(revision - 1)}, false);
    ASSERT_TRUE(stale);
    EXPECT_EQ(*stale.value().find("error")->find("code")->string_if(), "conflict");
    auto reopened = SqliteCatalogRepository::open(path.toStdString());
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened.value()->snapshot().value().revision, revision);
}

TEST_F(CliTest, RealCatalogLocateIsBoundedStableAndRejectsMissingIdentity)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath("library.sqlite");
    auto repository = SqliteCatalogRepository::create(path.toStdString());
    ASSERT_TRUE(repository);
    for (int i = 0; i < 250; ++i)
    {
        AssetRecord record;
        record.id = "ast_locate_" + std::to_string(i);
        record.normalized_uri = "file:///locate/" + std::to_string(i) + ".png";
        record.media_type = std::string(kMediaTypePng);
        record.created_unix_ms = i;
        ASSERT_TRUE(repository.value()->commit_imported_asset(record));
    }
    const auto revision = repository.value()->snapshot().value().revision;
    repository.value().reset();
    QProcess process;
    process.start(QStringLiteral(RAVO_CLI_EXECUTABLE),
                  {"catalog", "locate", "--catalog", path, "--asset-id", "ast_locate_0", "--json"});
    ASSERT_TRUE(process.waitForFinished(30000));
    ASSERT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
    auto response = parse_json(process.readAllStandardOutput().toStdString());
    ASSERT_TRUE(response);
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(*data->find("schema")->string_if(), "ravo.library.location/v1");
    EXPECT_EQ(data->find("index")->number_if()->text, "249");
    EXPECT_EQ(data->find("offset")->number_if()->text, "200");
    EXPECT_EQ(data->find("materialized_rows")->number_if()->text, "50");
    EXPECT_EQ(data->find("assets")->array_if()->size(), 50U);
    process.start(QStringLiteral(RAVO_CLI_EXECUTABLE),
                  {"catalog", "locate", "--catalog", path, "--asset-id", "absent", "--json"});
    ASSERT_TRUE(process.waitForFinished(30000));
    EXPECT_NE(process.exitCode(), 0);
    auto missing = parse_json(process.readAllStandardOutput().toStdString());
    ASSERT_TRUE(missing);
    EXPECT_EQ(*missing.value().find("error")->find("code")->string_if(), "not_found");
    auto reopened = SqliteCatalogRepository::open(path.toStdString());
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened.value()->snapshot().value().revision, revision);
}
} // namespace ravo
