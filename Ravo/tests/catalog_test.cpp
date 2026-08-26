#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <QBuffer>
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
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

#include "color_balance_fixture.h"

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

TEST(QtRasterDecoderTest, DecodeMemoryAppliesClockwiseQuarterTurnsWithoutExif)
{
    QImage source(32, 16, QImage::Format_RGB888);
    source.fill(QColor(12, 80, 200));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    ASSERT_TRUE(buffer.open(QIODevice::WriteOnly));
    ASSERT_TRUE(source.save(&buffer, "JPEG", 95));
    buffer.close();
    std::vector<std::uint8_t> bytes(encoded.cbegin(), encoded.cend());

    QtRasterDecoder decoder;
    auto identity = decoder.decode_memory(bytes, 64, CancellationToken{}, 0);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_EQ(identity.value().width, 32U);
    EXPECT_EQ(identity.value().height, 16U);

    auto rotated = decoder.decode_memory(bytes, 64, CancellationToken{}, 3);
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated.value().width, 16U);
    EXPECT_EQ(rotated.value().height, 32U);
}

TEST_F(CatalogServiceTest, RawImportCachesEmbeddedThumbnailSeparatelyFromProcessedPreview)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest browse;
    browse.asset_id = asset_id;
    browse.max_edge = kThumbnailMaxEdge;
    browse.prefer_embedded_preview = true;
    auto thumb = service->request_preview(browse);
    ASSERT_TRUE(thumb) << thumb.error().message;
    EXPECT_TRUE(std::filesystem::exists(thumb.value().cache_path));
    EXPECT_NE(thumb.value().cache_key.find(std::string(kEmbeddedBrowsePreviewDigest)),
              std::string::npos);
    auto listed_previews = service->list_previews();
    ASSERT_TRUE(listed_previews) << listed_previews.error().message;
    ASSERT_EQ(listed_previews.value().size(), 1U);
    EXPECT_EQ(listed_previews.value().front().asset_id, asset_id);
    EXPECT_EQ(listed_previews.value().front().state, kPreviewStateReady);
    ASSERT_TRUE(listed_previews.value().front().cache_relpath);
    EXPECT_FALSE(listed_previews.value().front().cache_relpath->empty());

    PreviewRequest processed;
    processed.asset_id = asset_id;
    processed.max_edge = kDefaultPreviewMaxEdge;
    auto full = service->request_preview(processed);
    ASSERT_TRUE(full) << full.error().message;
    EXPECT_TRUE(std::filesystem::exists(full.value().cache_path));
    EXPECT_EQ(full.value().cache_key.find(std::string(kEmbeddedBrowsePreviewDigest)),
              std::string::npos);
    EXPECT_NE(full.value().cache_path, thumb.value().cache_path);
    EXPECT_GT(std::max(full.value().width, full.value().height),
              std::max(thumb.value().width, thumb.value().height));
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

TEST_F(CatalogServiceTest, ImportProgressReportsEachImportedItemWithThumbnail)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    QImage image(24, 16, QImage::Format_RGB888);
    image.fill(QColor(20, 40, 80));
    const auto folder = root / "batch";
    std::filesystem::create_directories(folder);
    ASSERT_TRUE(image.save(QString::fromStdString((folder / "a.jpg").string()), "JPEG", 90));
    ASSERT_TRUE(image.save(QString::fromStdString((folder / "b.jpg").string()), "JPEG", 90));

    int imported_progress = 0;
    auto imported = service->import_inputs(
        {folder.string()}, CancellationToken{},
        [&](const std::size_t, const std::size_t, const ImportItemResult *item)
        {
            if (item != nullptr && item->status == ImportItemStatus::kImported)
            {
                ++imported_progress;
                ASSERT_TRUE(item->asset);
                ASSERT_TRUE(item->preview_cache_path);
                EXPECT_TRUE(std::filesystem::exists(*item->preview_cache_path));
            }
        });
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported_progress, 2);
    EXPECT_EQ(imported.value().size(), 2U);
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

TEST_F(CatalogServiceTest, RemoveOriginalAndCatalogDeletesTheFile)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(32, 32, QImage::Format_RGB888);
    image.fill(QColor(12, 34, 56));
    const auto photo = root / "delete-original.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(std::filesystem::exists(photo));

    auto removed = service->remove_original_and_catalog(asset_id);
    ASSERT_TRUE(removed) << removed.error().message;
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    EXPECT_FALSE(std::filesystem::exists(photo));
}

TEST_F(CatalogServiceTest, RemoveOriginalAndCatalogFailsWhenFileIsMissing)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(QColor(90, 12, 12));
    const auto photo = root / "already-gone.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(std::filesystem::remove(photo));

    auto removed = service->remove_original_and_catalog(asset_id);
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, ErrorCode::kNotFound);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, asset_id);
}

TEST_F(CatalogServiceTest, ReviewStatePersistsThroughReopenAndFilters)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);

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
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
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
    params.vignette = 0.35;
    params.flip_horizontal = 1;
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
    EXPECT_NEAR(roundtrip.value().vignette, 0.35, 1e-6);
    EXPECT_EQ(roundtrip.value().flip_horizontal, 1);
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

    DevelopParams cropped_params = roundtrip.value();
    cropped_params.crop_x = 0.25;
    cropped_params.crop_y = 0.25;
    cropped_params.crop_width = 0.5;
    cropped_params.crop_height = 0.5;
    ASSERT_TRUE(service->save_develop(asset_id, cropped_params));
    PreviewRequest guides;
    guides.asset_id = asset_id;
    guides.ignore_crop = true;
    auto uncropped = service->request_preview(guides);
    ASSERT_TRUE(uncropped) << uncropped.error().message;
    PreviewRequest applied;
    applied.asset_id = asset_id;
    auto cropped_again = service->request_preview(applied);
    ASSERT_TRUE(cropped_again) << cropped_again.error().message;
    EXPECT_GT(uncropped.value().width, cropped_again.value().width);
    EXPECT_GT(uncropped.value().height, cropped_again.value().height);

    auto reset = service->reset_recipe(asset_id);
    ASSERT_TRUE(reset) << reset.error().message;
    EXPECT_FALSE(reset.value().has_edits);
    EXPECT_EQ(reset.value().review.rating, 3);
}

TEST_F(CatalogServiceTest, TagsMetadataAndHistoryPersistThroughReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "meta.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(QColor(12, 34, 56));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    auto tagged = service->set_tags(asset_id, {"风景", "  风景  ", "archive"});
    ASSERT_TRUE(tagged) << tagged.error().message;
    ASSERT_EQ(tagged.value().tags.size(), 2U);
    EXPECT_EQ(tagged.value().tags.front(), "风景");
    EXPECT_EQ(tagged.value().tags.back(), "archive");

    WritableMetadata metadata;
    metadata.title = "标题";
    metadata.creator = "Ravo";
    auto written = service->set_writable_metadata(asset_id, metadata);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written.value().metadata.title, "标题");

    DevelopParams params;
    params.exposure_ev = 0.4;
    params.graduated_density = 0.6;
    ASSERT_TRUE(service->save_develop(asset_id, params));
    auto snapshot = service->create_recipe_snapshot(asset_id, "keep");
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    DevelopParams next;
    next.exposure_ev = -0.5;
    ASSERT_TRUE(service->save_develop(asset_id, next));

    LibraryQuery query;
    query.tag = "风景";
    auto filtered = service->list_assets(query);
    ASSERT_TRUE(filtered) << filtered.error().message;
    ASSERT_EQ(filtered.value().size(), 1U);

    auto empty = service->set_tags(asset_id, {""});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kValidation);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().tags.size(), 2U);
    EXPECT_EQ(listed.value().front().metadata.title, "标题");
    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_FALSE(history.value().empty());
    std::int64_t snapshot_id = 0;
    for (const auto &entry : history.value())
    {
        if (entry.kind == kRecipeHistoryKindSnapshot)
        {
            snapshot_id = entry.id;
            break;
        }
    }
    ASSERT_NE(snapshot_id, 0);
    auto restored = service->restore_recipe_history(asset_id, snapshot_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_NEAR(develop.value().exposure_ev, 0.4, 1e-6);
    EXPECT_NEAR(develop.value().graduated_density, 0.6, 1e-6);
}

TEST_F(CatalogServiceTest, RecipeTransactionFailurePreservesCurrentRecipeAndRevision)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-transaction.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.fill(QColor(90, 45, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams accepted;
    accepted.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, accepted));
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    auto history_before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;

    {
        const auto connection = QStringLiteral("ravo_recipe_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_recipe_history BEFORE INSERT ON asset_recipe_history "
            "BEGIN SELECT RAISE(ABORT, 'forced recipe history failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    DevelopParams rejected = accepted;
    rejected.exposure_ev = -0.75;
    auto failed = service->save_develop(asset_id, rejected);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);

    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_ev, accepted.exposure_ev, 1e-9);
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
    auto history_after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    EXPECT_EQ(history_after.value().size(), history_before.value().size());

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_params = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_params) << restored_params.error().message;
    EXPECT_NEAR(restored_params.value().exposure_ev, accepted.exposure_ev, 1e-9);
}

TEST_F(CatalogServiceTest, RawSigmoidBaselinePersistsOnlyUserOverrides)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    EXPECT_FALSE(imported.value().asset->has_edits);

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_EQ(baseline.value().operations.size(), 1U);
    EXPECT_EQ(baseline.value().operations.front().id, "ravo.display.sigmoid");
    auto baseline_params = develop_from_recipe(baseline.value());
    ASSERT_TRUE(baseline_params) << baseline_params.error().message;
    EXPECT_TRUE(baseline_params.value().sigmoid_enabled);
    EXPECT_NEAR(baseline_params.value().sigmoid_contrast, kSigmoidContrastDefault, 1e-9);
    auto baseline_has_edits = service->asset_has_edits(asset_id);
    ASSERT_TRUE(baseline_has_edits) << baseline_has_edits.error().message;
    EXPECT_FALSE(baseline_has_edits.value());

    auto adjusted = baseline_params.value();
    adjusted.sigmoid_skew = -0.35;
    auto saved = service->save_develop(asset_id, adjusted);
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_TRUE(saved.value().has_edits);
    ASSERT_TRUE(service->close());
    service.reset();

    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_params = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_params) << restored_params.error().message;
    EXPECT_NEAR(restored_params.value().sigmoid_skew, -0.35, 1e-9);

    auto reset = service->reset_recipe(asset_id);
    ASSERT_TRUE(reset) << reset.error().message;
    EXPECT_FALSE(reset.value().has_edits);
    auto reset_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(reset_recipe) << reset_recipe.error().message;
    auto reset_params = develop_from_recipe(reset_recipe.value());
    ASSERT_TRUE(reset_params) << reset_params.error().message;
    EXPECT_TRUE(reset_params.value().sigmoid_enabled);
    EXPECT_NEAR(reset_params.value().sigmoid_skew, kSigmoidSkewDefault, 1e-9);
}

TEST_F(CatalogServiceTest, LiveDevelopPreviewAppliesWithoutSavingRecipe)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "live.jpg").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.fill(QColor(200, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams live;
    live.exposure_ev = 1.25;
    live.temperature = 4500.0;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kInteractivePreviewMaxEdge;
    request.persist_preview_record = false;
    auto first = service->request_preview(request, live);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().cache_path.empty());
    EXPECT_FALSE(first.value().srgb.empty());
    EXPECT_EQ(first.value().srgb.size(),
              static_cast<std::size_t>(first.value().width) * first.value().height * 3U);
    EXPECT_LE(std::max(first.value().width, first.value().height), kInteractivePreviewMaxEdge);

    auto stored = service->load_recipe(asset_id);
    ASSERT_TRUE(stored) << stored.error().message;
    auto stored_params = develop_from_recipe(stored.value());
    ASSERT_TRUE(stored_params) << stored_params.error().message;
    EXPECT_TRUE(stored_params.value().is_identity());
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_FALSE(listed.value().front().has_edits);

    const auto first_pixels = first.value().srgb;
    live.exposure_ev = -0.75;
    auto second = service->request_preview(request, live);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().cache_path.empty());
    EXPECT_NE(second.value().srgb, first_pixels);
}

TEST_F(CatalogServiceTest, RawLivePreviewReusesLinearWorkingWithoutSaving)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams live;
    live.sigmoid_enabled = true;
    live.exposure_ev = 0.75;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kInteractivePreviewMaxEdge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = true;
    auto first = service->request_preview(request, live);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().cache_path.empty());
    EXPECT_FALSE(first.value().srgb.empty());
    EXPECT_LE(std::max(first.value().width, first.value().height), kInteractivePreviewMaxEdge);

    auto direct_recipe = recipe_from_develop(
        {asset_id, raw_fixture_path(), imported.value().asset->content_fingerprint}, live);
    ASSERT_TRUE(direct_recipe) << direct_recipe.error().message;
    RenderRequest direct_request;
    direct_request.asset = direct_recipe.value().asset;
    direct_request.recipe = direct_recipe.value();
    direct_request.output_width = first.value().width;
    direct_request.output_height = first.value().height;
    auto direct = engine.render_to_image(direct_request);
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(first.value().srgb, direct.value().rgb);

    auto stored = service->load_recipe(asset_id);
    ASSERT_TRUE(stored) << stored.error().message;
    auto stored_params = develop_from_recipe(stored.value());
    ASSERT_TRUE(stored_params) << stored_params.error().message;
    EXPECT_NEAR(stored_params.value().exposure_ev, 0.0, 1e-9);
    EXPECT_TRUE(stored_params.value().sigmoid_enabled);

    live.exposure_ev = -0.5;
    auto second = service->request_preview(request, live);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().cache_path.empty());
    EXPECT_NE(second.value().srgb, first.value().srgb);

    live.exposure_ev = 0.75;
    auto third = service->request_preview(request, live);
    ASSERT_TRUE(third) << third.error().message;
    EXPECT_EQ(third.value().srgb, first.value().srgb);

    live.raw_highlights = 1.0;
    auto highlighted = service->request_preview(request, live);
    ASSERT_TRUE(highlighted) << highlighted.error().message;
    EXPECT_EQ(highlighted.value().width, first.value().width);
    EXPECT_EQ(highlighted.value().height, first.value().height);
}

TEST_F(CatalogServiceTest, MigratedDevelopControlsPersistAndReproducePixelsAfterReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto edited = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(edited) << edited.error().message;
    edited.value().raw_highlights = 0.35;
    edited.value().hot_pixels_strength = 0.25;
    edited.value().hot_pixels_threshold = 0.05;
    edited.value().hot_pixels_permissive = true;
    edited.value().raw_ca_iterations = 1;
    edited.value().raw_ca_avoid_shift = false;
    edited.value().raw_highlights_mode = std::string(kRawHighlightsModeOpposed);
    edited.value().denoise = 0.2;
    edited.value().denoise_chroma = 0.7;
    edited.value().denoise_radius = 1.5;
    edited.value().lens_mode = std::string(kLensModeManual);
    edited.value().lens_k1 = -0.04;
    edited.value().lens_vignetting = 0.15;
    // The selected Inspector band is transient presentation state; all eight algorithm bands
    // are canonical recipe data.
    edited.value().color_eq_band = 0;
    edited.value().color_eq_hue[2] = 0.1;
    edited.value().color_eq_sat[2] = 0.25;
    edited.value().color_eq_light[2] = -0.15;
    edited.value().graduated_density = 0.4;
    edited.value().graduated_hardness = 0.65;
    edited.value().graduated_rotation = 15.0;
    edited.value().graduated_offset = -0.1;
    edited.value().tone_eq_blacks = -0.2;
    edited.value().tone_eq_shadows = 0.15;
    edited.value().tone_eq_midtones = 0.25;
    edited.value().tone_eq_highlights = -0.1;
    edited.value().tone_eq_whites = -0.3;
    edited.value().channel_mixer.red = {0.95, 0.05, 0.0};
    edited.value().channel_mixer.green = {0.02, 0.96, 0.02};
    edited.value().channel_mixer.blue = {0.0, 0.08, 0.92};
    edited.value().color_balance_rgb = test::color_balance_0093_params();
    clamp_develop(edited.value());

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 96;
    preview.persist_preview_record = false;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().srgb.empty());

    auto saved = service->save_develop(asset_id, edited.value());
    ASSERT_TRUE(saved) << saved.error().message;
    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().srgb.empty());
    EXPECT_NE(before_reopen.value().srgb, baseline.value().srgb);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value(), edited.value());

    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().srgb, before_reopen.value().srgb);
}

TEST_F(CatalogServiceTest, IgnoreStraightenKeepsWorkingImageCorners)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "tilt.jpg").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.fill(QColor(200, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams tilted;
    tilted.straighten_degrees = 20.0;
    PreviewRequest baked;
    baked.asset_id = asset_id;
    baked.max_edge = kInteractivePreviewMaxEdge;
    baked.persist_preview_record = false;
    auto straightened = service->request_preview(baked, tilted);
    ASSERT_TRUE(straightened) << straightened.error().message;
    ASSERT_FALSE(straightened.value().srgb.empty());
    PreviewRequest guide = baked;
    guide.ignore_straighten = true;
    auto unstraightened = service->request_preview(guide, tilted);
    ASSERT_TRUE(unstraightened) << unstraightened.error().message;
    ASSERT_FALSE(unstraightened.value().srgb.empty());
    ASSERT_EQ(unstraightened.value().srgb.size(), straightened.value().srgb.size());
    EXPECT_LT(straightened.value().srgb[0] + straightened.value().srgb[1] +
                  straightened.value().srgb[2],
              unstraightened.value().srgb[0] + unstraightened.value().srgb[1] +
                  unstraightened.value().srgb[2]);
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

TEST_F(CatalogServiceTest, ExportJpegPngOriginalCopyConflictAndCancel)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "source.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.fill(QColor(40, 120, 200));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    const auto original_hash = file_sha256(jpeg_path);
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    DevelopParams params;
    params.exposure_ev = 0.5;
    ASSERT_TRUE(service->save_develop(asset_id, params));

    const auto png_out = (root / "out.png").string();
    ExportRequest png;
    png.asset_id = asset_id;
    png.output_path = png_out;
    png.format = ExportFormat::kPng;
    auto exported_png = service->export_asset(png);
    ASSERT_TRUE(exported_png) << exported_png.error().message;
    EXPECT_EQ(exported_png.value().format, ExportFormat::kPng);
    EXPECT_GT(exported_png.value().bytes_written, 0U);
    EXPECT_TRUE(std::filesystem::exists(png_out));
    QImage read_png(QString::fromStdString(png_out));
    ASSERT_FALSE(read_png.isNull());
    EXPECT_EQ(read_png.width(), static_cast<int>(exported_png.value().width));
    EXPECT_EQ(read_png.height(), static_cast<int>(exported_png.value().height));
    EXPECT_EQ(file_sha256(jpeg_path), original_hash);

    auto conflict = service->export_asset(png);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_TRUE(std::filesystem::exists(png_out));
    const auto after_conflict = file_sha256(png_out);

    const auto jpeg_out = (root / "out.jpg").string();
    ExportRequest jpeg;
    jpeg.asset_id = asset_id;
    jpeg.output_path = jpeg_out;
    jpeg.format = ExportFormat::kJpeg;
    jpeg.jpeg_quality = 85;
    auto exported_jpeg = service->export_asset(jpeg);
    ASSERT_TRUE(exported_jpeg) << exported_jpeg.error().message;
    EXPECT_TRUE(std::filesystem::exists(jpeg_out));
    EXPECT_FALSE(QImage(QString::fromStdString(jpeg_out)).isNull());

    const auto tiff_out = (root / "out.tif").string();
    ExportRequest tiff;
    tiff.asset_id = asset_id;
    tiff.output_path = tiff_out;
    tiff.format = ExportFormat::kTiff;
    auto exported_tiff = service->export_asset(tiff);
    if (exported_tiff)
    {
        EXPECT_TRUE(std::filesystem::exists(tiff_out));
        EXPECT_FALSE(QImage(QString::fromStdString(tiff_out)).isNull());
    }
    else
    {
        EXPECT_EQ(exported_tiff.error().code, ErrorCode::kUnsupported);
        EXPECT_FALSE(std::filesystem::exists(tiff_out));
    }

    const auto copy_out = (root / "original-copy.jpg").string();
    ExportRequest copy;
    copy.asset_id = asset_id;
    copy.output_path = copy_out;
    copy.format = ExportFormat::kOriginalCopy;
    auto exported_copy = service->export_asset(copy);
    ASSERT_TRUE(exported_copy) << exported_copy.error().message;
    EXPECT_EQ(file_sha256(copy_out), original_hash);
    EXPECT_EQ(file_sha256(jpeg_path), original_hash);
    EXPECT_EQ(file_sha256(png_out), after_conflict);

    const auto cancelled_out = (root / "cancelled.png").string();
    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("test"));
    ExportRequest cancel;
    cancel.asset_id = asset_id;
    cancel.output_path = cancelled_out;
    cancel.format = ExportFormat::kPng;
    cancel.cancellation = cancelled.token();
    auto exported_cancel = service->export_asset(cancel);
    ASSERT_FALSE(exported_cancel);
    EXPECT_EQ(exported_cancel.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(cancelled_out));
    EXPECT_FALSE(std::filesystem::exists(cancelled_out + ".ravo-export-tmp"));

    ExportRequest bad_quality = jpeg;
    bad_quality.output_path = (root / "bad-quality.jpg").string();
    bad_quality.jpeg_quality = 0;
    auto invalid_quality = service->export_asset(bad_quality);
    ASSERT_FALSE(invalid_quality);
    EXPECT_EQ(invalid_quality.error().code, ErrorCode::kValidation);
    EXPECT_FALSE(std::filesystem::exists(bad_quality.output_path));
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
