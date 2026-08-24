#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <gtest/gtest.h>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string repository_path(const std::filesystem::path &relative)
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / relative;
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string png_fixture_path()
{
    return repository_path(std::filesystem::path("legacy") / "tests" / "0000-nop" / "expected.png");
}

[[nodiscard]] std::string raw_fixture_path()
{
    return repository_path(std::filesystem::path("legacy") / "tests" / "images" / "mire1.cr2");
}

[[nodiscard]] QByteArray file_sha256(const std::string &path)
{
    QFile file(QString::fromStdString(path));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

[[nodiscard]] std::filesystem::path make_temp_root()
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-catalog-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    return root;
}

class CatalogServiceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto created = EngineFacade::create_phase1();
        ASSERT_TRUE(created) << created.error().message;
        engine = std::move(created).value();
        root = make_temp_root();
        database_path = (root / "library.sqlite").string();
    }

    void TearDown() override
    {
        service.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    Result<void> open_service(const bool create)
    {
        auto repository = create ? SqliteCatalogRepository::create(database_path) :
                                   SqliteCatalogRepository::open(database_path);
        if (!repository)
        {
            return repository.error();
        }
        auto cache = FilesystemPreviewCache::create(database_path + ".preview");
        if (!cache)
        {
            return cache.error();
        }
        service = std::make_unique<CatalogService>(engine, std::move(repository).value(),
                                                   std::make_unique<QtRasterDecoder>(),
                                                   std::move(cache).value());
        return {};
    }

    EngineFacade engine = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
    std::filesystem::path root;
    std::string database_path;
    std::unique_ptr<CatalogService> service;
};

TEST_F(CatalogServiceTest, CreateReopenAndRejectNewerSchema)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    ASSERT_TRUE(service->close());
    service.reset();

    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    ASSERT_TRUE(service->close());
    service.reset();

    {
        const auto connection = QStringLiteral("ravo_schema_hack");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(
            query.exec(QStringLiteral("UPDATE schema_info SET schema_version = 99 WHERE id = 1")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto newer = SqliteCatalogRepository::open(database_path);
    ASSERT_FALSE(newer);
    EXPECT_EQ(newer.error().code, ErrorCode::kUnsupported);
}

TEST_F(CatalogServiceTest, ImportPngAndRawThenReopenPreview)
{
    const auto png_path = png_fixture_path();
    const auto raw_path = raw_fixture_path();
    const auto png_hash = file_sha256(png_path);
    const auto raw_hash = file_sha256(raw_path);

    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    auto png = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(png) << png.error().message;
    EXPECT_EQ(png.value().status, ImportItemStatus::kImported);
    ASSERT_TRUE(png.value().asset);
    EXPECT_EQ(png.value().asset->media_type, kMediaTypePng);

    auto raw = service->import_one(raw_path, CancellationToken{});
    ASSERT_TRUE(raw) << raw.error().message;
    EXPECT_EQ(raw.value().status, ImportItemStatus::kImported);
    ASSERT_TRUE(raw.value().asset);
    EXPECT_EQ(raw.value().asset->media_type, kMediaTypeRaw);

    auto duplicate = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(duplicate) << duplicate.error().message;
    EXPECT_EQ(duplicate.value().status, ImportItemStatus::kDuplicate);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);

    PreviewRequest png_preview;
    png_preview.asset_id = png.value().asset->id;
    png_preview.max_edge = kDefaultPreviewMaxEdge;
    auto previewed_png = service->request_preview(png_preview);
    ASSERT_TRUE(previewed_png) << previewed_png.error().message;
    EXPECT_TRUE(std::filesystem::exists(previewed_png.value().cache_path));
    EXPECT_GT(previewed_png.value().width, 0U);

    PreviewRequest raw_preview;
    raw_preview.asset_id = raw.value().asset->id;
    raw_preview.max_edge = kDefaultPreviewMaxEdge;
    auto previewed_raw = service->request_preview(raw_preview);
    ASSERT_TRUE(previewed_raw) << previewed_raw.error().message;
    EXPECT_TRUE(std::filesystem::exists(previewed_raw.value().cache_path));
    EXPECT_GT(previewed_raw.value().width, 0U);

    EXPECT_EQ(file_sha256(png_path), png_hash);
    EXPECT_EQ(file_sha256(raw_path), raw_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);

    PreviewRequest reopen_preview;
    reopen_preview.asset_id = png.value().asset->id;
    reopen_preview.max_edge = kDefaultPreviewMaxEdge;
    auto previewed_again = service->request_preview(reopen_preview);
    ASSERT_TRUE(previewed_again) << previewed_again.error().message;
    EXPECT_TRUE(std::filesystem::exists(previewed_again.value().cache_path));
}

TEST_F(CatalogServiceTest, ImportJpegAndDirectorySkipsSidecars)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    const auto jpeg_path = (root / "probe.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.fill(QColor(12, 80, 200));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    const auto jpeg_hash = file_sha256(jpeg_path);

    auto jpeg = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(jpeg) << jpeg.error().message;
    EXPECT_EQ(jpeg.value().status, ImportItemStatus::kImported);
    ASSERT_TRUE(jpeg.value().asset);
    EXPECT_EQ(jpeg.value().asset->media_type, kMediaTypeJpeg);

    PreviewRequest preview;
    preview.asset_id = jpeg.value().asset->id;
    auto previewed = service->request_preview(preview);
    ASSERT_TRUE(previewed) << previewed.error().message;
    EXPECT_TRUE(std::filesystem::exists(previewed.value().cache_path));
    EXPECT_EQ(file_sha256(jpeg_path), jpeg_hash);

    const auto folder = root / "trip";
    std::filesystem::create_directories(folder / "100MSDCF");
    ASSERT_TRUE(image.save(QString::fromStdString((folder / "100MSDCF" / "frame.jpg").string()),
                           "JPEG", 90));
    {
        std::ofstream sidecar(folder / "100MSDCF" / "frame.jpg.xmp");
        sidecar << "<x:xmpmeta/>";
        std::ofstream notes(folder / "readme.txt");
        notes << "not an image";
    }

    auto imported = service->import_inputs({folder.string()}, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().size(), 1U);
    EXPECT_EQ(imported.value().front().status, ImportItemStatus::kImported);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_EQ(listed.value().size(), 2U);
}

TEST_F(CatalogServiceTest, MissingAndUnsupportedInputsDoNotCreateReadyAssets)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    auto missing = service->import_one((root / "missing.png").string(), CancellationToken{});
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_EQ(missing.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(missing.value().asset);

    const auto text_path = (root / "notes.txt").string();
    {
        std::ofstream output(text_path);
        output << "not an image";
    }
    auto unsupported = service->import_one(text_path, CancellationToken{});
    ASSERT_TRUE(unsupported) << unsupported.error().message;
    EXPECT_EQ(unsupported.value().status, ImportItemStatus::kUnsupported);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
}

TEST_F(CatalogServiceTest, MissingOriginalIsAssetStateNotPreviewFailureWhenCacheExists)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    const auto jpeg_path = (root / "gone.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(QColor(40, 80, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(std::filesystem::remove(jpeg_path));

    PreviewRequest preview;
    preview.asset_id = asset_id;
    auto previewed = service->request_preview(preview);
    ASSERT_TRUE(previewed) << previewed.error().message;
    EXPECT_TRUE(previewed.value().original_missing);
    EXPECT_TRUE(std::filesystem::exists(previewed.value().cache_path));

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().import_state, kImportStateMissing);
}

TEST_F(CatalogServiceTest, ListsImportedFoldersAndFiltersByFolderUri)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto trip = root / "2024" / "Trip";
    const auto home = root / "2024" / "Home";
    std::filesystem::create_directories(trip);
    std::filesystem::create_directories(home);
    QImage image(8, 8, QImage::Format_RGB888);
    image.fill(QColor(12, 24, 48));
    ASSERT_TRUE(image.save(QString::fromStdString((trip / "a.jpg").string()), "JPEG", 90));
    ASSERT_TRUE(image.save(QString::fromStdString((home / "b.jpg").string()), "JPEG", 90));
    auto imported = service->import_inputs({root.string()}, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;

    auto folders = service->list_folders();
    ASSERT_TRUE(folders) << folders.error().message;
    ASSERT_FALSE(folders.value().empty());
    EXPECT_TRUE(folders.value().front().uri.empty());
    EXPECT_EQ(folders.value().front().asset_count, 2);

    std::string trip_uri;
    for (const auto &folder : folders.value())
    {
        if (folder.display_name == "Trip")
        {
            trip_uri = folder.uri;
            EXPECT_EQ(folder.asset_count, 1);
        }
    }
    ASSERT_FALSE(trip_uri.empty());
    LibraryQuery query;
    query.folder_uri = trip_uri;
    auto listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_NE(listed.value().front().normalized_uri.find("Trip"), std::string::npos);
}

TEST_F(CatalogServiceTest, RemoveFromCatalogLeavesTheOriginalFile)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(400, 400, QImage::Format_RGB888);
    image.fill(QColor(32, 64, 96));
    const auto photo = root / "keep-original.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto original_hash = file_sha256(photo.string());

    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    PreviewRequest thumb;
    thumb.asset_id = asset_id;
    thumb.max_edge = kThumbnailMaxEdge;
    auto thumb_preview = service->request_preview(thumb);
    ASSERT_TRUE(thumb_preview) << thumb_preview.error().message;
    PreviewRequest full;
    full.asset_id = asset_id;
    full.max_edge = kDefaultPreviewMaxEdge;
    auto full_preview = service->request_preview(full);
    ASSERT_TRUE(full_preview) << full_preview.error().message;
    EXPECT_NE(thumb_preview.value().cache_path, full_preview.value().cache_path);
    EXPECT_TRUE(std::filesystem::exists(thumb_preview.value().cache_path));
    EXPECT_TRUE(std::filesystem::exists(full_preview.value().cache_path));

    auto removed = service->remove_from_catalog(asset_id);
    ASSERT_TRUE(removed) << removed.error().message;
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    EXPECT_TRUE(std::filesystem::exists(photo));
    EXPECT_EQ(file_sha256(photo.string()), original_hash);
    EXPECT_FALSE(std::filesystem::exists(thumb_preview.value().cache_path));
    EXPECT_FALSE(std::filesystem::exists(full_preview.value().cache_path));

    auto missing = service->remove_from_catalog(asset_id);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
}

TEST_F(CatalogServiceTest, ReviewStatePersistsThroughReopenAndFilters)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, 3);

    const auto jpeg_path = (root / "keep.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(QColor(20, 40, 80));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto rated = service->set_rating(asset_id, 4);
    ASSERT_TRUE(rated) << rated.error().message;
    EXPECT_EQ(rated.value().review.rating, 4);
    auto labeled = service->set_color_label(asset_id, ColorLabel::kGreen);
    ASSERT_TRUE(labeled) << labeled.error().message;
    EXPECT_EQ(labeled.value().review.color_label, ColorLabel::kGreen);
    auto rejected = service->set_rejected(asset_id, true);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_TRUE(rejected.value().review.rejected);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().review.rating, 4);
    EXPECT_EQ(listed.value().front().review.color_label, ColorLabel::kGreen);
    EXPECT_TRUE(listed.value().front().review.rejected);

    LibraryQuery exclude_rejected;
    exclude_rejected.reject_filter = RejectFilter::kExclude;
    auto filtered = service->list_assets(exclude_rejected);
    ASSERT_TRUE(filtered) << filtered.error().message;
    EXPECT_TRUE(filtered.value().empty());
}

TEST_F(CatalogServiceTest, MigratesV1CatalogToReviewSchema)
{
    {
        const auto connection = QStringLiteral("ravo_v1_seed");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA foreign_keys = ON")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
            "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
            "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset (id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "media_type TEXT NOT NULL, size_bytes INTEGER NOT NULL, mtime_unix_ms INTEGER NOT NULL, "
            "content_fingerprint TEXT, width INTEGER, height INTEGER, import_state TEXT NOT NULL, "
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 1, 'cat_legacy', 0, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, media_type, size_bytes, mtime_unix_ms, "
            "import_state, created_unix_ms) VALUES ('ast_old', 'file:///tmp/old.png', "
            "'image/png', 12, 1, 'imported', 1)")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto opened = open_service(false);
    ASSERT_TRUE(opened) << opened.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, 3);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().review.rating, 0);
    EXPECT_EQ(listed.value().front().review.color_label, ColorLabel::kNone);
    EXPECT_FALSE(listed.value().front().review.rejected);
    EXPECT_FALSE(listed.value().front().has_edits);
}

TEST_F(CatalogServiceTest, DevelopRecipePersistsIndependentlyOfReview)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "edit.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.fill(QColor(180, 40, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto rated = service->set_rating(asset_id, 3);
    ASSERT_TRUE(rated) << rated.error().message;

    DevelopParams params;
    params.exposure_ev = 0.75;
    params.saturation = -0.2;
    auto saved = service->save_develop(asset_id, params);
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_TRUE(saved.value().has_edits);
    EXPECT_EQ(saved.value().review.rating, 3);

    const auto original_hash = file_sha256(jpeg_path);
    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto loaded = service->load_recipe(asset_id);
    ASSERT_TRUE(loaded) << loaded.error().message;
    auto roundtrip = develop_from_recipe(loaded.value());
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    EXPECT_NEAR(roundtrip.value().exposure_ev, 0.75, 1e-6);
    EXPECT_NEAR(roundtrip.value().saturation, -0.2, 1e-6);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_TRUE(listed.value().front().has_edits);
    EXPECT_EQ(listed.value().front().review.rating, 3);
    EXPECT_EQ(file_sha256(jpeg_path), original_hash);

    PreviewRequest preview;
    preview.asset_id = asset_id;
    auto previewed = service->request_preview(preview);
    ASSERT_TRUE(previewed) << previewed.error().message;
    EXPECT_FALSE(previewed.value().original_missing);

    auto reset = service->reset_recipe(asset_id);
    ASSERT_TRUE(reset) << reset.error().message;
    EXPECT_FALSE(reset.value().has_edits);
    EXPECT_EQ(reset.value().review.rating, 3);
}

TEST_F(CatalogServiceTest, InvalidStoredRecipeFailsStructuredWithoutTouchingReview)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "bad-recipe.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(QColor(10, 80, 10));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->set_rating(asset_id, 2));

    auto repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    ASSERT_TRUE(repository.value()->save_recipe_json(asset_id, 1, R"({"not":"a-recipe")"));
    ASSERT_TRUE(repository.value()->close());

    auto loaded = service->load_recipe(asset_id);
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, ErrorCode::kValidation);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().review.rating, 2);
    EXPECT_TRUE(listed.value().front().has_edits);
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-catalog-tests");
    testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
