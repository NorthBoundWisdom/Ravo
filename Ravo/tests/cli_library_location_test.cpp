#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "cli_test_support.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/foundation/json.h"

namespace ravo
{
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
