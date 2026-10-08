#include <filesystem>
#include <fstream>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/adapters/lightroom_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/services/foreign_catalog.h"

namespace ravo
{
namespace
{
void make_lightroom(const std::string &path, const std::string &original, const bool broken = false)
{
    const QString name = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(QString::fromStdString(path));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        for (
            const auto *sql :
            {"CREATE TABLE AgLibraryRootFolder(id_local INTEGER PRIMARY KEY,absolutePath TEXT)",
             "CREATE TABLE AgLibraryFolder(id_local INTEGER PRIMARY KEY,rootFolder INTEGER,pathFromRoot TEXT)",
             "CREATE TABLE AgLibraryFile(id_local INTEGER PRIMARY KEY,folder INTEGER,idx_filename TEXT,baseName TEXT,extension TEXT)",
             "CREATE TABLE Adobe_images(id_local INTEGER PRIMARY KEY,rootFile INTEGER,rating INTEGER,pick INTEGER,colorLabels TEXT,masterImage INTEGER)",
             "CREATE TABLE AgLibraryKeyword(id_local INTEGER PRIMARY KEY,name TEXT,parent INTEGER)",
             "CREATE TABLE AgLibraryKeywordImage(image INTEGER,tag INTEGER)",
             "CREATE TABLE Adobe_imageDevelopSettings(image INTEGER,text TEXT)",
             "INSERT INTO AgLibraryFolder VALUES(1,1,'')",
             "INSERT INTO Adobe_images VALUES(1,1,4,-1,'Red',NULL)",
             "INSERT INTO Adobe_images VALUES(2,2,0,0,'',NULL)",
             "INSERT INTO AgLibraryFile VALUES(2,1,'missing.png','missing','png')",
             "INSERT INTO AgLibraryKeyword VALUES(1,'Travel',NULL),(2,'Japan',1)",
             "INSERT INTO AgLibraryKeywordImage VALUES(1,2)"})
            ASSERT_TRUE(q.exec(QString::fromLatin1(sql))) << sql;
        ASSERT_TRUE(q.prepare("INSERT INTO AgLibraryRootFolder VALUES(1,?)"));
        q.addBindValue(
            QString::fromStdString(std::filesystem::path(original).parent_path().string()));
        ASSERT_TRUE(q.exec());
        ASSERT_TRUE(q.prepare("INSERT INTO AgLibraryFile VALUES(1,1,?,'','png')"));
        q.addBindValue(QString::fromStdString(std::filesystem::path(original).filename().string()));
        ASSERT_TRUE(q.exec());
        if (broken)
            ASSERT_TRUE(q.exec("UPDATE AgLibraryFile SET folder=99 WHERE id_local=1"));
    }
    QSqlDatabase::removeDatabase(name);
}
} // namespace

TEST_F(CatalogServiceTest, LightroomVirtualCopiesDoNotConsumeOriginalIdentity)
{
    ASSERT_TRUE(open_service(true));
    const auto source =
        (std::filesystem::path(database_path).parent_path() / "copies.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString name = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        ASSERT_TRUE(query.exec("INSERT INTO Adobe_images VALUES(0,1,1,0,'',1)"));
        ASSERT_TRUE(query.exec("INSERT INTO Adobe_images VALUES(3,1,2,0,'',NULL)"));
    }
    QSqlDatabase::removeDatabase(name);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto converted = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().schema, "ravo.lightroom-catalog-conversion/v1");
    EXPECT_EQ(converted.value().imported, 1U);
    EXPECT_EQ(converted.value().unsupported, 1U);
    EXPECT_EQ(converted.value().skipped, 2U);
    ASSERT_EQ(converted.value().items.size(), 4U);
    EXPECT_EQ(converted.value().items[0].reasons.front(), "lightroom_virtual_copy_unsupported");
    EXPECT_EQ(converted.value().items[3].reasons.front(), "duplicate_original");
    auto assets = service->library().list_assets();
    ASSERT_TRUE(assets);
    ASSERT_EQ(assets.value().size(), 1U);
    EXPECT_EQ(assets.value()[0].review.rating, 4);
}

TEST_F(CatalogServiceTest, LightroomKeywordCycleFailsBeforeDestinationWrites)
{
    ASSERT_TRUE(open_service(true));
    const auto source =
        (std::filesystem::path(database_path).parent_path() / "cycle.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString name = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        ASSERT_TRUE(query.exec("UPDATE AgLibraryKeyword SET parent=2 WHERE id_local=1"));
    }
    QSqlDatabase::removeDatabase(name);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto converted = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(converted);
    EXPECT_EQ(converted.error().context.at("reason"), "unsupported_source_schema");
    auto assets = service->library().list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
}

TEST_F(CatalogServiceTest, LightroomSqliteImportPreservesSourcesAndReopens)
{
    ASSERT_TRUE(open_service(true));
    const auto source =
        (std::filesystem::path(database_path).parent_path() / "source.lrcat").string();
    const auto original = png_fixture_path();
    make_lightroom(source, original);
    const auto source_hash = sha256_file_hex(source);
    const auto original_hash = sha256_file_hex(original);
    ASSERT_TRUE(source_hash);
    ASSERT_TRUE(original_hash);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto result = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().imported, 1U);
    EXPECT_EQ(result.value().skipped, 1U);
    ASSERT_EQ(result.value().items.size(), 2U);
    EXPECT_EQ(result.value().items[1].reasons.front(), "missing_original");
    EXPECT_FALSE(result.value().items[0].unsupported_fields.empty());
    EXPECT_EQ(sha256_file_hex(source).value(), source_hash.value());
    EXPECT_EQ(sha256_file_hex(original).value(), original_hash.value());
    EXPECT_FALSE(std::filesystem::exists(source + "-journal"));
    EXPECT_FALSE(std::filesystem::exists(source + "-wal"));
    auto duplicate = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    service.reset();
    ASSERT_TRUE(open_service(false));
    const auto assets = service->library().list_assets();
    ASSERT_TRUE(assets);
    ASSERT_EQ(assets.value().size(), 1U);
    EXPECT_EQ(assets.value()[0].review.rating, 4);
    EXPECT_TRUE(assets.value()[0].review.rejected);
    EXPECT_EQ(assets.value()[0].review.color_label, ColorLabel::kRed);
    EXPECT_EQ(assets.value()[0].tags, (std::vector<std::string>{"Travel|Japan"}));
}

TEST_F(CatalogServiceTest, LightroomRejectsBrokenReferencesActiveJournalAndCancellation)
{
    ASSERT_TRUE(open_service(true));
    const auto source =
        (std::filesystem::path(database_path).parent_path() / "broken.lrcat").string();
    make_lightroom(source, png_fixture_path(), true);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto broken = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(broken);
    EXPECT_EQ(broken.error().context.at("reason"), "unsupported_source_schema");
    {
        std::ofstream journal(source + "-wal");
        journal << "active";
    }
    auto active = read_lightroom_catalog(source);
    ASSERT_FALSE(active);
    EXPECT_EQ(active.error().context.at("reason"), "lightroom_catalog_active");
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    auto cancelled = read_lightroom_catalog(source, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    const auto assets = service->library().list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
}
} // namespace ravo
