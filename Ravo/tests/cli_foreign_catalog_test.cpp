#include <filesystem>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <QProcess>
#include "ravo/adapters/text_file.h"

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

TEST_F(CliTest, NativeLightroomConvertListsAndExportsVerifiedArchive)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-native-lr-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto source = (root / "source.lrcat").string();
    const auto catalog = (root / "library.sqlite").string();
    const auto output = (root / "exported.lrcat").string();
    const auto original = std::filesystem::path(RAVO_REPOSITORY_ROOT) /
                          "Ravo/tests/fixtures/frozen/0000-nop/expected.png";
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        for (
            const auto *sql :
            {"CREATE TABLE AgLibraryRootFolder(id_local INTEGER,absolutePath TEXT)",
             "CREATE TABLE AgLibraryFolder(id_local INTEGER,rootFolder INTEGER,pathFromRoot TEXT)",
             "CREATE TABLE AgLibraryFile(id_local INTEGER,folder INTEGER,idx_filename TEXT,baseName TEXT,extension TEXT)",
             "CREATE TABLE Adobe_images(id_local INTEGER,rootFile INTEGER,rating INTEGER,pick INTEGER,colorLabels TEXT,masterImage INTEGER)",
             "CREATE TABLE Adobe_imageDevelopSettings(image INTEGER,text TEXT)",
             "INSERT INTO AgLibraryFolder VALUES(1,1,'')",
             "INSERT INTO AgLibraryFile VALUES(1,1,'expected.png','expected','png')",
             "INSERT INTO Adobe_images VALUES(1,1,4,1,'Red',NULL)",
             "INSERT INTO Adobe_imageDevelopSettings VALUES(1,'s={Exposure2012=0.5}')"})
            ASSERT_TRUE(q.exec(sql)) << sql;
        ASSERT_TRUE(q.prepare("INSERT INTO AgLibraryRootFolder VALUES(1,?)"));
        q.addBindValue(QString::fromStdString(original.parent_path().string()));
        ASSERT_TRUE(q.exec());
    }
    QSqlDatabase::removeDatabase(connection);
    const auto source_hash = sha256_file_hex(source).value();
    const auto original_hash = sha256_file_hex(original.string()).value();
    const auto run = [&](const QStringList &arguments, const bool success = true) -> std::string
    {
        QProcess process;
        process.start(QString::fromUtf8(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForFinished(30000));
        const auto bytes = process.readAllStandardOutput();
        if (success)
            EXPECT_EQ(process.exitCode(), 0)
                << bytes.toStdString() << process.readAllStandardError().toStdString();
        else
            EXPECT_NE(process.exitCode(), 0);
        return bytes.toStdString();
    };
    auto inspected = parse_json(run({"catalog", "inspect-foreign", "--foreign-source",
                                     QString::fromStdString(source), "--json"}));
    ASSERT_TRUE(inspected);
    EXPECT_EQ(string_at(inspected.value(), {"data", "schema"}),
              "ravo.lightroom-catalog-inspection/v1");
    EXPECT_EQ(number_at(inspected.value(), {"data", "current_edits"}), "1");
    EXPECT_EQ(string_at(inspected.value(), {"data", "source_sha256"}), source_hash);
    EXPECT_FALSE(std::filesystem::exists(catalog));
    run({"catalog", "create", "--path", QString::fromStdString(catalog), "--json"});
    auto converted =
        parse_json(run({"catalog", "convert-foreign", "--catalog", QString::fromStdString(catalog),
                        "--foreign-source", QString::fromStdString(source), "--foreign-id", "1",
                        "--expect-source-sha256", QString::fromStdString(source_hash), "--json"}));
    ASSERT_TRUE(converted);
    EXPECT_EQ(number_at(converted.value(), {"data", "imported"}), "1");
    EXPECT_EQ(string_at(converted.value(), {"data", "source_archive", "sha256"}), source_hash);
    auto listed = parse_json(run(
        {"catalog", "foreign-sources", "--catalog", QString::fromStdString(catalog), "--json"}));
    ASSERT_TRUE(listed);
    EXPECT_EQ(string_at(listed.value(), {"data", "schema"}), "ravo.foreign-catalog-sources/v1");
    const QStringList export_args{"catalog",     "foreign-source-export",
                                  "--catalog",   QString::fromStdString(catalog),
                                  "--source-id", QString::fromStdString(source_hash),
                                  "--output",    QString::fromStdString(output),
                                  "--json"};
    auto exported = parse_json(run(export_args));
    ASSERT_TRUE(exported);
    EXPECT_EQ(sha256_file_hex(output).value(), source_hash);
    auto conflict = parse_json(run(export_args, false));
    ASSERT_TRUE(conflict);
    EXPECT_EQ(string_at(conflict.value(), {"error", "code"}), "conflict");
    EXPECT_EQ(sha256_file_hex(source).value(), source_hash);
    EXPECT_EQ(sha256_file_hex(original.string()).value(), original_hash);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

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
