#include <filesystem>
#include <fstream>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariant>
#include <QByteArray>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/adapters/lightroom_catalog.h"
#include "ravo/adapters/lightroom_develop.h"
#include "ravo/adapters/filesystem_recovery_store.h"
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
        ASSERT_TRUE(
            query.exec("INSERT INTO Adobe_imageDevelopSettings VALUES(1,'s={Exposure2012=2}')"));
        ASSERT_TRUE(query.exec("CREATE TABLE Adobe_AdditionalMetadata(image INTEGER,xmp TEXT)"));
        ASSERT_TRUE(query.exec(
            "INSERT INTO Adobe_AdditionalMetadata VALUES(1,'<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Master title</rdf:li></rdf:Alt></dc:title></rdf:Description></rdf:RDF>')"));
    }
    QSqlDatabase::removeDatabase(name);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.foreign_ids = {"0"};
    request.expected_source_sha256 = sha256_file_hex(source).value();
    auto missing_master = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(missing_master);
    EXPECT_EQ(missing_master.error().context.at("reason"), "foreign_selection_requires_master");
    EXPECT_TRUE(service->conversion().foreign_catalog_archives().value().empty());
    request.foreign_ids.clear();
    auto converted = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().schema, "ravo.lightroom-catalog-conversion/v1");
    EXPECT_EQ(converted.value().imported, 2U);
    EXPECT_EQ(converted.value().unsupported, 0U);
    EXPECT_EQ(converted.value().skipped, 2U);
    ASSERT_EQ(converted.value().items.size(), 4U);
    EXPECT_EQ(converted.value().items[2].reasons.front(), "duplicate_original");
    EXPECT_EQ(converted.value().items[3].foreign_id, "0");
    EXPECT_TRUE(converted.value().items[3].asset_id);
    auto assets = service->library().list_assets();
    ASSERT_TRUE(assets);
    ASSERT_EQ(assets.value().size(), 2U);
    for (const auto &asset : assets.value())
    {
        EXPECT_EQ(asset.review.rating, asset.source_asset_id ? 1 : 4);
        auto recipe = service->develop().load_recipe(asset.id);
        ASSERT_TRUE(recipe);
        EXPECT_DOUBLE_EQ(develop_from_recipe(recipe.value()).value().exposure_ev,
                         asset.source_asset_id ? 0 : 2);
        if (asset.source_asset_id)
        {
            EXPECT_TRUE(asset.tags.empty());
            EXPECT_EQ(asset.review.color_label, ColorLabel::kNone);
            EXPECT_FALSE(asset.metadata.title);
        }
        else
            EXPECT_EQ(asset.metadata.title, "Master title");
    }
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
    EXPECT_TRUE(result.value().items[0].unsupported_fields.empty());
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

TEST(LightroomDevelop, ParsesDataWithoutExecutionAndReportsIndependentUnsupportedGroups)
{
    const AssetDescriptor asset{"test", "file:///test.png", {}};
    auto converted = import_lightroom_develop(
        "s = { ProcessVersion = \"11.0\", Exposure2012 = 1.5, WhiteBalance = \"Custom\", "
        "Temperature = 5200, Tint = 10, UnknownFuture = { name = \"a,b\", child = { 1, 2 } }, "
        "CropLeft=0.1, CropTop=0.2, CropRight=0.9, CropBottom=0.8, CropAngle=2, Texture=45, "
        "ToneCurvePV2012 = {0, 0, 128, 145, 255, 255}, }",
        asset);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_DOUBLE_EQ(converted.value().look.exposure_ev, 1.5);
    EXPECT_TRUE(converted.value().mask.exposure);
    EXPECT_TRUE(converted.value().mask.rgb_curve);
    EXPECT_FALSE(converted.value().mask.white_balance);
    EXPECT_TRUE(converted.value().geometry);
    EXPECT_TRUE(converted.value().texture);
    DevelopParams applied;
    apply_lightroom_develop(applied, converted.value());
    EXPECT_DOUBLE_EQ(applied.crop_x, 0.1);
    EXPECT_DOUBLE_EQ(applied.crop_width, 0.8);
    EXPECT_NEAR(applied.crop_height, 0.6, 1e-12);
    EXPECT_DOUBLE_EQ(applied.straighten_degrees, 2);
    EXPECT_DOUBLE_EQ(applied.texture.strength, 0.45);
    EXPECT_GE(converted.value().omitted.size(), 4U);
    for (const auto text : {"s={Exposure2012=1, Exposure2012=2}", "s={x={1,2}",
                            "s={x=\"unterminated}", "return os.execute('bad')"})
        EXPECT_FALSE(parse_lightroom_develop_fields(text)) << text;
    auto executable = import_lightroom_develop("s={ Exposure2012=os.execute(\"bad\") }", asset);
    ASSERT_TRUE(executable);
    EXPECT_FALSE(executable.value().mask.exposure);
    EXPECT_FALSE(executable.value().omitted.empty());
    auto legacy = import_lightroom_develop(
        "s={ProcessVersion=\"5.7\",Exposure=1.25,Contrast=40,Brightness=50,Shadows=5}", asset);
    ASSERT_TRUE(legacy);
    EXPECT_DOUBLE_EQ(legacy.value().look.exposure_ev, 1.25);
    ASSERT_EQ(legacy.value().omitted.size(), 1U);
    EXPECT_EQ(legacy.value().omitted.front().key, "Contrast");
    auto modern = import_lightroom_develop(
        "s={ProcessVersion=\"11.0\",Exposure2012=1.5,Exposure=9,Contrast=100,Brightness=-100}",
        asset);
    ASSERT_TRUE(modern);
    EXPECT_DOUBLE_EQ(modern.value().look.exposure_ev, 1.5);
    EXPECT_TRUE(modern.value().omitted.empty());
    auto neutral = import_lightroom_develop(
        "s={Exposure2012=0,IncrementalTemperature=0,IncrementalTint=0,"
        "LensProfileEnable=0,LensProfileSetup=\"LensDefaults\",PerspectiveUpright=0,"
        "UprightTransformCount=6,Look={},RetouchInfo={ },RedEyeInfo={}}",
        asset);
    ASSERT_TRUE(neutral);
    EXPECT_TRUE(neutral.value().omitted.empty());
    auto active = import_lightroom_develop("s={Exposure2012=0,RetouchInfo={x=1}}", asset);
    ASSERT_TRUE(active);
    ASSERT_EQ(active.value().omitted.size(), 1U);
    EXPECT_EQ(active.value().omitted.front().reason, "unsupported_lightroom_nested_field");
}

TEST(LightroomDevelop, NumericFieldsUseBoundedPortableAsciiParsing)
{
    const AssetDescriptor asset{"numbers", "file:///numbers.png", {}};
    auto valid = import_lightroom_develop(
        "s={Texture=4.5e1,CropLeft=1e-1,CropRight=9e-1,CropAngle=-2.5}", asset);
    ASSERT_TRUE(valid);
    EXPECT_DOUBLE_EQ(valid.value().look.texture.strength, 0.45);
    EXPECT_DOUBLE_EQ(valid.value().look.crop_x, 0.1);
    EXPECT_DOUBLE_EQ(valid.value().look.straighten_degrees, -2.5);
    for (const auto &token : {"45junk", "nan", "inf", "1e999", "0x10", "+45"})
    {
        auto result = import_lightroom_develop("s={Texture=" + std::string(token) + "}", asset);
        ASSERT_TRUE(result);
        EXPECT_FALSE(result.value().texture) << token;
    }
    auto overlong = import_lightroom_develop("s={Texture=" + std::string(65, '0') + "}", asset);
    ASSERT_TRUE(overlong);
    EXPECT_FALSE(overlong.value().texture);
    EXPECT_FALSE(parse_lightroom_develop_fields("s={Texture=1,5}"));
}

TEST_F(CatalogServiceTest, LightroomAuditCancellationAndFailureRetainCommittedReceipts)
{
    ASSERT_TRUE(open_service(true));
    const auto original = (root / "owned.png").string();
    std::filesystem::copy_file(png_fixture_path(), original);
    const auto source = (root / "audit.lrcat").string();
    make_lightroom(source, original);
    CancellationSource cancellation;
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.cancellation = cancellation.token();
    request.progress = [&](std::string_view stage, std::size_t, std::size_t)
    {
        if (stage == "source_audit")
            ASSERT_TRUE(cancellation.cancel());
    };
    auto result = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().imported, 1U);
    EXPECT_TRUE(result.value().items.front().asset_id);
    EXPECT_TRUE(result.value().cancelled);
    EXPECT_FALSE(result.value().originals_unchanged);
    EXPECT_FALSE(result.value().source_audit_complete);
    ASSERT_EQ(result.value().source_audits.size(), 1U);
    EXPECT_EQ(result.value().source_audits.front().status, "cancelled");
    EXPECT_FALSE(result.value().source_audits.front().after);
    EXPECT_EQ(sha256_file_hex(original).value(), result.value().source_originals.front().sha256);
    const auto id = result.value().conversion_id;
    const auto asset = *result.value().items.front().asset_id;
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto journal = service->conversion().foreign_conversion_status(id);
    ASSERT_TRUE(journal);
    ASSERT_TRUE(journal.value());
    ASSERT_EQ(journal.value()->records.size(), 1U);
    EXPECT_TRUE(journal.value()->records.front().complete);
    EXPECT_EQ(journal.value()->records.front().asset_id, asset);
    const auto history = service->develop().list_recipe_history(asset).value();
    request.resume = true;
    request.expected_source_sha256 = sha256_file_hex(source).value();
    request.cancellation = {};
    request.progress = [&](std::string_view stage, std::size_t, std::size_t)
    {
        if (stage == "source_audit")
            std::filesystem::remove(original);
    };
    auto resumed = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed.value().imported, 1U);
    EXPECT_TRUE(resumed.value().items.front().resumed);
    EXPECT_EQ(resumed.value().items.front().asset_id, asset);
    EXPECT_EQ(resumed.value().source_audits.front().status, "failed");
    EXPECT_FALSE(resumed.value().originals_unchanged);
    EXPECT_EQ(service->develop().list_recipe_history(asset).value().size(), history.size());
    EXPECT_EQ(service->library().list_assets().value().size(), 1U);
}

TEST_F(CatalogServiceTest, LightroomPartialItemIsJournaledAndNeverReplayedOnResume)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "partial.lrcat").string();
    make_lightroom(source, png_fixture_path());
    CancellationSource cancellation;
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.cancellation = cancellation.token();
    request.progress = [&](std::string_view stage, std::size_t, std::size_t)
    {
        if (stage == "rating")
            ASSERT_TRUE(cancellation.cancel());
    };
    auto converted = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(converted);
    ASSERT_TRUE(converted.value().items.front().asset_id);
    const auto asset = *converted.value().items.front().asset_id;
    EXPECT_EQ(converted.value().items.front().phase, "rating");
    auto journal =
        service->conversion().foreign_conversion_status(converted.value().conversion_id).value();
    ASSERT_TRUE(journal);
    ASSERT_EQ(journal->records.size(), 1U);
    EXPECT_FALSE(journal->records.front().complete);
    EXPECT_EQ(journal->records.front().asset_id, asset);
    service.reset();
    ASSERT_TRUE(open_service(false));
    request.progress = {};
    request.cancellation = {};
    request.resume = true;
    request.expected_source_sha256 = sha256_file_hex(source).value();
    auto resumed = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed.value().failed, 1U);
    EXPECT_EQ(resumed.value().items.front().reasons.back(),
              "incomplete_conversion_requires_resolution");
    EXPECT_EQ(service->library().list_assets().value().size(), 1U);
    ASSERT_TRUE(service->library().set_rating(asset, 5));
    auto conflict = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().context.at("reason"), "foreign_conversion_revision_conflict");
}

TEST_F(CatalogServiceTest, LightroomResumesUntouchedCopyAndCollectionsWithoutReplayingMaster)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "resume-copy.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        for (
            const auto *sql :
            {"INSERT INTO Adobe_images VALUES(0,1,2,0,'Blue',1)",
             "INSERT INTO Adobe_imageDevelopSettings VALUES(1,'s={Exposure2012=1}')",
             "INSERT INTO Adobe_imageDevelopSettings VALUES(0,'s={Exposure2012=-1}')",
             "CREATE TABLE AgLibraryCollection(id_local INTEGER,name TEXT,parent INTEGER,creationId TEXT)",
             "INSERT INTO AgLibraryCollection VALUES(10,'Copies',NULL,'collection')",
             "CREATE TABLE AgLibraryCollectionImage(collection INTEGER,image INTEGER,positionInCollection TEXT)",
             "INSERT INTO AgLibraryCollectionImage VALUES(10,1,'a'),(10,0,'b')"})
            ASSERT_TRUE(query.exec(sql));
    }
    QSqlDatabase::removeDatabase(connection);
    CancellationSource cancellation;
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.cancellation = cancellation.token();
    request.progress = [&](std::string_view stage, std::size_t completed, std::size_t)
    {
        if (stage == "conversion" && completed == 1)
            static_cast<void>(cancellation.cancel());
    };
    auto first = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(first);
    ASSERT_EQ(first.value().imported, 1U);
    EXPECT_EQ(first.value().source_originals.size(), 1U);
    const auto master = *first.value().items.front().asset_id;
    const auto history_count = service->develop().list_recipe_history(master).value().size();
    service.reset();
    ASSERT_TRUE(open_service(false));
    request.cancellation = {};
    request.progress = {};
    request.resume = true;
    request.expected_source_sha256 = sha256_file_hex(source).value();
    auto second = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(second.value().imported, 2U);
    EXPECT_EQ(service->library().list_assets().value().size(), 2U);
    EXPECT_EQ(service->develop().list_recipe_history(master).value().size(), history_count);
    ASSERT_EQ(second.value().collections.size(), 1U);
    EXPECT_EQ(second.value().collections.front().imported_members, 2U);
    auto third = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(third);
    EXPECT_EQ(service->library().list_assets().value().size(), 2U);
    EXPECT_EQ(service->library().list_library_sets().value().size(), 1U);
    EXPECT_EQ(third.value().collections.front().set_id, second.value().collections.front().set_id);
    const auto assets = service->library().list_assets().value();
    for (const auto &asset : assets)
    {
        auto params = develop_from_recipe(service->develop().load_recipe(asset.id).value());
        ASSERT_TRUE(params);
        EXPECT_DOUBLE_EQ(params.value().exposure_ev, asset.source_asset_id ? -1 : 1);
    }
}

TEST_F(CatalogServiceTest, LightroomObservedSourceChangeRetainsBeforeAfterAndTarget)
{
    ASSERT_TRUE(open_service(true));
    const auto original = (root / "owned.png").string();
    std::filesystem::copy_file(png_fixture_path(), original);
    const auto source = (root / "changed.lrcat").string();
    make_lightroom(source, original);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.progress = [&](std::string_view stage, std::size_t, std::size_t)
    {
        if (stage == "source_audit")
        {
            std::ofstream changed(original, std::ios::app);
            changed << 'x';
        }
    };
    auto result = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(result);
    ASSERT_EQ(result.value().imported, 1U);
    ASSERT_TRUE(result.value().items.front().asset_id);
    EXPECT_TRUE(result.value().source_audit_complete);
    EXPECT_FALSE(result.value().originals_unchanged);
    const auto &audit = result.value().source_audits.front();
    EXPECT_EQ(audit.status, "changed");
    ASSERT_TRUE(audit.after);
    EXPECT_NE(audit.before.sha256, audit.after->sha256);
    EXPECT_EQ(audit.error->context.at("reason"), "source_changed_during_conversion");
    request.resume = true;
    request.progress = {};
    request.expected_source_sha256 = sha256_file_hex(source).value();
    auto resumed = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(resumed);
    ASSERT_FALSE(resumed.value().issues.empty());
    EXPECT_EQ(resumed.value().issues.front().context.at("reason"),
              "foreign_conversion_source_conflict");
    EXPECT_EQ(resumed.value().imported, 1U);
    EXPECT_EQ(service->library().list_assets().value().size(), 1U);
}

TEST_F(CatalogServiceTest, LightroomCheckpointFailureRetainsAssetAndStopsNewMutations)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "checkpoint-fault.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString source_connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", source_connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        ASSERT_TRUE(query.exec(
            "CREATE TABLE AgLibraryCollection(id_local INTEGER,name TEXT,parent INTEGER,creationId TEXT)"));
        ASSERT_TRUE(
            query.exec("INSERT INTO AgLibraryCollection VALUES(10,'Pending',NULL,'collection')"));
    }
    QSqlDatabase::removeDatabase(source_connection);
    service.reset();
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        ASSERT_TRUE(query.exec(
            "CREATE TRIGGER fail_conversion_complete BEFORE INSERT ON foreign_conversion_record "
            "WHEN NEW.phase='complete' BEGIN SELECT RAISE(ABORT,'injected checkpoint failure'); END"));
    }
    QSqlDatabase::removeDatabase(connection);
    ASSERT_TRUE(open_service(false));
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto result = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().items.front().asset_id);
    ASSERT_FALSE(result.value().issues.empty());
    EXPECT_FALSE(result.value().cancelled);
    ASSERT_EQ(result.value().collections.size(), 1U);
    EXPECT_EQ(result.value().collections.front().reasons.front(),
              "conversion_checkpoint_unavailable");
    EXPECT_FALSE(result.value().collections.front().set_id);
    EXPECT_EQ(result.value().items.front().reasons.back(), "checkpoint_write_failed");
    auto journal =
        service->conversion().foreign_conversion_status(result.value().conversion_id).value();
    ASSERT_TRUE(journal);
    ASSERT_EQ(journal->records.size(), 1U);
    EXPECT_FALSE(journal->records.front().complete);
    EXPECT_EQ(journal->records.front().asset_id, result.value().items.front().asset_id);
}

TEST_F(CatalogServiceTest, LightroomImportsDatabaseEditsHistoryMetadataCopiesAndCollections)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "complete.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        for (
            const auto *sql :
            {"UPDATE Adobe_images SET pick=1 WHERE id_local=1",
             "INSERT INTO Adobe_images VALUES(0,1,2,0,'Blue',1)",
             "INSERT INTO Adobe_imageDevelopSettings VALUES(1,'s = { Exposure2012 = 1.5, Texture = 45, FutureAdjustment = 42 }')",
             "INSERT INTO Adobe_imageDevelopSettings VALUES(0,'s = { Exposure2012 = -0.5 }')",
             "INSERT INTO Adobe_imageDevelopSettings VALUES(2,'')",
             "CREATE TABLE Adobe_libraryImageDevelopHistoryStep(id_local INTEGER,image INTEGER,name TEXT,dateCreated REAL,text BLOB)",
             "INSERT INTO Adobe_libraryImageDevelopHistoryStep VALUES(1,1,'Exposure',1,'s = { Exposure2012 = 0.75 }')",
             "CREATE TABLE Adobe_libraryImageDevelopSnapshot(id_local INTEGER,image INTEGER,name TEXT,text BLOB)",
             "INSERT INTO Adobe_libraryImageDevelopSnapshot VALUES(1,1,'Saved', 's = { Exposure2012 = 0.25 }')",
             "CREATE TABLE Adobe_AdditionalMetadata(image INTEGER,xmp TEXT)",
             "CREATE TABLE VendorFutureTable(payload TEXT)",
             "INSERT INTO VendorFutureTable VALUES('preserve this unknown vendor state')",
             "CREATE TABLE AgLibraryCollection(id_local INTEGER,name TEXT,parent INTEGER,creationId TEXT)",
             "INSERT INTO AgLibraryCollection VALUES(10,'Trips',NULL,'com.adobe.ag.library.collection_set'),(11,'Japan',10,'com.adobe.ag.library.collection')",
             "CREATE TABLE AgLibraryCollectionImage(collection INTEGER,image INTEGER,positionInCollection TEXT)",
             "INSERT INTO AgLibraryCollectionImage VALUES(11,1,'a'),(11,0,'b')"})
            ASSERT_TRUE(q.exec(sql)) << sql;
        ASSERT_TRUE(q.prepare("INSERT INTO Adobe_AdditionalMetadata VALUES(1,?)"));
        const QByteArray metadata(
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">Catalog title</rdf:li></rdf:Alt></dc:title></rdf:Description></rdf:RDF></x:xmpmeta>");
        q.addBindValue(QByteArray("\xef\xbb\xbf") + metadata);
        ASSERT_TRUE(q.exec());
        ASSERT_TRUE(q.prepare("INSERT INTO Adobe_AdditionalMetadata VALUES(0,?)"));
        q.addBindValue(qCompress(QByteArray("\xef\xbb\xbf") + metadata));
        ASSERT_TRUE(q.exec());
    }
    QSqlDatabase::removeDatabase(connection);
    const auto before = sha256_file_hex(source).value();
    auto inspected = ConversionService::inspect_lightroom_catalog(source);
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_EQ(inspected.value().photos, 3U);
    EXPECT_EQ(inspected.value().virtual_copies, 1U);
    EXPECT_EQ(inspected.value().current_edits, 2U);
    EXPECT_EQ(inspected.value().malformed_edits, 0U);
    EXPECT_EQ(inspected.value().metadata_photos, 2U);
    EXPECT_EQ(inspected.value().history_steps, 1U);
    EXPECT_EQ(inspected.value().snapshots, 1U);
    EXPECT_EQ(inspected.value().develop_fields.at("Exposure2012"), 2U);
    EXPECT_EQ(inspected.value().editing_samples.at("nonzero_exposure").foreign_id, "1");
    EXPECT_EQ(inspected.value().source_sha256, before);
    EXPECT_TRUE(service->library().list_assets().value().empty());
    EXPECT_TRUE(service->conversion().foreign_catalog_archives().value().empty());
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto result = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result.value().imported, 2U);
    EXPECT_EQ(result.value().skipped, 1U);
    EXPECT_EQ(result.value().unsupported_fields, 1U);
    ASSERT_TRUE(result.value().source_archive);
    EXPECT_EQ(result.value().archived_only_tables, std::vector<std::string>{"VendorFutureTable"});
    EXPECT_EQ(result.value().source_archive->sha256, before);
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto assets = service->library().list_assets();
    ASSERT_TRUE(assets);
    ASSERT_EQ(assets.value().size(), 2U);
    std::string master;
    for (const auto &asset : assets.value())
    {
        auto recipe = service->develop().load_recipe(asset.id);
        ASSERT_TRUE(recipe);
        auto params = develop_from_recipe(recipe.value());
        ASSERT_TRUE(params);
        EXPECT_DOUBLE_EQ(params.value().exposure_ev, asset.source_asset_id ? -0.5 : 1.5);
        EXPECT_DOUBLE_EQ(params.value().texture.strength, asset.source_asset_id ? 0 : 0.45);
        if (!asset.source_asset_id)
        {
            master = asset.id;
            EXPECT_TRUE(asset.review.picked);
            EXPECT_EQ(asset.metadata.title, "Catalog title");
        }
    }
    auto history = service->develop().list_recipe_history(master);
    ASSERT_TRUE(history);
    bool snapshot_found = false, step_found = false;
    for (const auto &entry : history.value())
    {
        snapshot_found |= entry.label == "Lightroom: Saved";
        step_found |= entry.label == "Lightroom: Exposure";
    }
    EXPECT_TRUE(snapshot_found);
    EXPECT_TRUE(step_found);
    auto sets = service->library().list_library_sets();
    ASSERT_TRUE(sets);
    ASSERT_EQ(sets.value().size(), 2U);
    bool members_found = false;
    for (const auto &set : sets.value())
        members_found |= set.name == "Trips / Japan" && set.asset_count == 2;
    EXPECT_TRUE(members_found);
    const auto output = (root / "preserved.lrcat").string();
    ASSERT_TRUE(service->conversion().export_foreign_catalog_archive(before, output));
    EXPECT_EQ(sha256_file_hex(output).value(), before);
    auto conflict = service->conversion().export_foreign_catalog_archive(before, output);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_EQ(sha256_file_hex(source).value(), before);
}

TEST_F(CatalogServiceTest, LightroomArchiveRollsBackChangedSourceAndCancelledOutput)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "archive.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const auto before_revision = service->library().snapshot().value().revision;
    auto wrong = sqlite_repository->archive_foreign_catalog(source, std::string(64, '0'), {});
    ASSERT_FALSE(wrong);
    EXPECT_EQ(wrong.error().code, ErrorCode::kConflict);
    EXPECT_TRUE(sqlite_repository->list_foreign_catalog_archives().value().empty());
    EXPECT_EQ(service->library().snapshot().value().revision, before_revision);
    const auto hash = sha256_file_hex(source).value();
    ASSERT_TRUE(sqlite_repository->archive_foreign_catalog(source, hash, {}));
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel());
    const auto output = (root / "cancelled.lrcat").string();
    auto cancelled =
        service->conversion().export_foreign_catalog_archive(hash, output, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(CatalogServiceTest, LightroomCorruptArchiveNeverPublishesOutput)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "corrupt-source.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const auto hash = sha256_file_hex(source).value();
    ASSERT_TRUE(sqlite_repository->archive_foreign_catalog(source, hash, {}));
    service.reset();
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        ASSERT_TRUE(q.exec("UPDATE foreign_catalog_chunk SET bytes=x'00' WHERE ordinal=0"));
    }
    QSqlDatabase::removeDatabase(connection);
    ASSERT_TRUE(open_service(false));
    const auto output = (root / "must-not-publish.lrcat").string();
    auto exported = service->conversion().export_foreign_catalog_archive(hash, output);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kValidation);
    EXPECT_FALSE(std::filesystem::exists(output));
    for (const auto &entry : std::filesystem::directory_iterator(root))
        EXPECT_FALSE(entry.path().filename().string().starts_with(".ravo-foreign-"));
}

TEST_F(CatalogServiceTest, LightroomReadsBoundedCompressedHistoryAndRejectsCorruption)
{
    const auto source = (root / "compressed.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QByteArray settings("s = { Exposure2012 = 0.5 }");
    const auto compressed = qCompress(settings);
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        ASSERT_TRUE(q.exec(
            "CREATE TABLE Adobe_libraryImageDevelopHistoryStep(id_local INTEGER,image INTEGER,name TEXT,dateCreated REAL,text BLOB)"));
        ASSERT_TRUE(q.prepare(
            "INSERT INTO Adobe_libraryImageDevelopHistoryStep VALUES(1,1,'Compressed',1,?)"));
        q.addBindValue(compressed);
        ASSERT_TRUE(q.exec());
    }
    QSqlDatabase::removeDatabase(connection);
    auto read = read_lightroom_catalog(source);
    ASSERT_TRUE(read) << read.error().message;
    ASSERT_EQ(read.value().photos.front().develop_states.size(), 1U);
    EXPECT_EQ(read.value().photos.front().develop_states.front().settings, settings.toStdString());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        ASSERT_TRUE(q.prepare("UPDATE Adobe_libraryImageDevelopHistoryStep SET text=?"));
        auto corrupt = compressed;
        corrupt[0] = '\x7f';
        q.addBindValue(corrupt);
        ASSERT_TRUE(q.exec());
    }
    QSqlDatabase::removeDatabase(connection);
    auto corrupt = read_lightroom_catalog(source);
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().context.at("reason"), "unsupported_source_schema");
}

TEST_F(CatalogServiceTest, LightroomMapsForeignVolumesExplicitlyWithoutFallback)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "foreign-path.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        ASSERT_TRUE(q.exec("UPDATE AgLibraryRootFolder SET absolutePath='Z:/photos'"));
    }
    QSqlDatabase::removeDatabase(connection);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.path_mappings = {
        {"Z:/photos", std::filesystem::path(png_fixture_path()).parent_path().string()}};
    request.foreign_ids = {"1"};
    auto missing_hash = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(missing_hash);
    EXPECT_EQ(missing_hash.error().context.at("reason"), "foreign_selection_requires_source_hash");
    request.expected_source_sha256 = std::string(64, '0');
    auto stale_hash = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(stale_hash);
    EXPECT_EQ(stale_hash.error().code, ErrorCode::kConflict);
    EXPECT_TRUE(service->conversion().foreign_catalog_archives().value().empty());
    request.expected_source_sha256 = sha256_file_hex(source).value();
    request.foreign_ids = {"missing"};
    auto unknown = service->conversion().convert_foreign_catalog(request);
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::kNotFound);
    EXPECT_TRUE(service->conversion().foreign_catalog_archives().value().empty());
    request.foreign_ids = {"1", "1"};
    EXPECT_FALSE(service->conversion().convert_foreign_catalog(request));
    request.foreign_ids = {"1"};
    auto converted = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().imported, 1U);
    EXPECT_EQ(converted.value().skipped, 0U);
    EXPECT_EQ(converted.value().unsupported, 0U);
    EXPECT_EQ(converted.value().source_photo_count, 2U);
    EXPECT_EQ(converted.value().selected_photo_count, 1U);
}

TEST_F(CatalogServiceTest, LightroomSchema18UpgradesAndPreservesArchiveThroughBackup)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository);
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(db.open());
        QSqlQuery q(db);
        ASSERT_TRUE(q.exec("DROP TABLE foreign_conversion_record"));
        ASSERT_TRUE(q.exec("DROP TABLE foreign_conversion"));
        ASSERT_TRUE(q.exec("DROP TABLE foreign_catalog_chunk"));
        ASSERT_TRUE(q.exec("DROP TABLE foreign_catalog_source"));
        ASSERT_TRUE(q.exec("UPDATE schema_info SET schema_version=18"));
    }
    QSqlDatabase::removeDatabase(connection);
    ASSERT_TRUE(open_service(false));
    EXPECT_EQ(service->library().snapshot().value().schema_version, kCatalogSchemaVersion);
    const auto source = (root / "backup-source.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const auto hash = sha256_file_hex(source).value();
    ASSERT_TRUE(sqlite_repository->archive_foreign_catalog(source, hash, {}));
    const auto backup = (root / "archive-backup.sqlite").string();
    const auto revision = sqlite_repository->snapshot().value().revision;
    ASSERT_TRUE(sqlite_repository->begin_foreign_conversion(hash, hash, revision));
    ForeignCatalogItemReport receipt;
    receipt.foreign_id = "1";
    receipt.phase = "complete";
    receipt.status = ForeignCatalogItemStatus::kImported;
    ForeignConversionCheckpoint record{
        "1", "complete", {}, true, serialize_json(foreign_catalog_item_to_json(receipt))};
    const auto begun_revision = sqlite_repository->snapshot().value().revision;
    auto stale =
        sqlite_repository->save_foreign_conversion_checkpoint(hash, record, begun_revision - 1);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    EXPECT_EQ(sqlite_repository->snapshot().value().revision, begun_revision);
    EXPECT_TRUE(sqlite_repository->load_foreign_conversion(hash).value()->records.empty());
    ASSERT_TRUE(
        sqlite_repository->save_foreign_conversion_checkpoint(hash, record, begun_revision));
    auto snapshot = sqlite_repository->create_backup_database(backup, {});
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto reopened = SqliteCatalogRepository::open(backup);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto journal = reopened.value()->load_foreign_conversion(hash);
    ASSERT_TRUE(journal);
    ASSERT_TRUE(journal.value());
    ASSERT_EQ(journal.value()->records.size(), 1U);
    EXPECT_EQ(journal.value()->records.front().receipt_json, record.receipt_json);
    const auto output = (root / "backup-export.lrcat").string();
    ASSERT_TRUE(reopened.value()->export_foreign_catalog_archive(hash, output, {}));
    EXPECT_EQ(sha256_file_hex(output).value(), hash);
}

TEST_F(CatalogServiceTest, LightroomHistoryWithoutCurrentEditPublishesRecoveryAndRestores)
{
    ASSERT_TRUE(open_service(true));
    const auto source = (root / "history-only.lrcat").string();
    make_lightroom(source, png_fixture_path());
    const QString connection = QString::fromStdString(generate_catalog_id());
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(source));
        ASSERT_TRUE(db.open());
        QSqlQuery query(db);
        ASSERT_TRUE(query.exec(
            "CREATE TABLE Adobe_libraryImageDevelopHistoryStep(id_local INTEGER,image INTEGER,name TEXT,dateCreated REAL,text BLOB)"));
        ASSERT_TRUE(query.exec(
            "INSERT INTO Adobe_libraryImageDevelopHistoryStep VALUES(1,1,'Only history',1,'s={Exposure2012=1.25}')"));
    }
    QSqlDatabase::removeDatabase(connection);
    ForeignCatalogConversionRequest request;
    request.source_path = source;
    request.foreign_ids = {"1"};
    request.expected_source_sha256 = sha256_file_hex(source).value();
    auto converted = service->conversion().convert_foreign_catalog(request);
    ASSERT_TRUE(converted) << converted.error().message;
    ASSERT_EQ(converted.value().imported, 1U);
    const auto asset_id = *converted.value().items.front().asset_id;
    auto current = service->develop().load_recipe(asset_id);
    ASSERT_TRUE(current);
    EXPECT_DOUBLE_EQ(develop_from_recipe(current.value()).value().exposure_ev, 0);
    auto state = service->recovery().recovery_state(asset_id);
    ASSERT_TRUE(state);
    EXPECT_FALSE(state.value().pending());
    auto store = FilesystemRecoveryStore::open_existing(
        FilesystemRecoveryStore::default_root_for_catalog(database_path));
    ASSERT_TRUE(store);
    auto artifact = store.value()->verify(asset_id, state.value().generation, {});
    ASSERT_TRUE(artifact) << artifact.error().message;
    auto document = read_utf8_text_file(artifact.value().path);
    ASSERT_TRUE(document);
    EXPECT_NE(document.value().find("Lightroom: Only history"), std::string::npos);
    auto history = service->develop().list_recipe_history(asset_id);
    ASSERT_TRUE(history);
    std::int64_t imported_id = 0;
    for (const auto &entry : history.value())
        if (entry.label == "Lightroom: Only history")
            imported_id = entry.id;
    ASSERT_NE(imported_id, 0);
    ASSERT_TRUE(service->develop().restore_recipe_history(asset_id, imported_id));
    EXPECT_DOUBLE_EQ(
        develop_from_recipe(service->develop().load_recipe(asset_id).value()).value().exposure_ev,
        1.25);
    const auto revision = service->library().snapshot().value().revision;
    current.value().asset.id = "different-asset";
    auto wrong_owner =
        service->develop().create_recipe_snapshot(asset_id, current.value(), "wrong owner");
    ASSERT_FALSE(wrong_owner);
    EXPECT_EQ(wrong_owner.error().code, ErrorCode::kConflict);
    EXPECT_EQ(service->library().snapshot().value().revision, revision);
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
