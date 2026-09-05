#include <filesystem>
#include <QColorSpace>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <gtest/gtest.h>
#include "catalog_test_support.h"
#include "catalog_repository_test_control.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{
bool write_photo(const std::filesystem::path &path, const QColor &color)
{
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(QString::fromStdString(path.string()), "PNG");
}
} // namespace

TEST_F(CatalogServiceTest, ImportScanFindsRenamedCatalogAndBatchContentWithoutPublishing)
{
    ASSERT_TRUE(open_service(true));
    const auto input = root / "source";
    const auto destination = root / "destination";
    std::filesystem::create_directories(input);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_photo(input / "a.png", Qt::red));
    ASSERT_TRUE(std::filesystem::copy_file(input / "a.png", input / "renamed.png"));
    ASSERT_TRUE(write_photo(input / "new.png", Qt::blue));
    const auto original_hash = file_sha256((input / "a.png").string());
    auto first = service->scan_import_candidates({input.string()}, input.string(), true, {});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first.value().schema, "ravo-import-scan/v1");
    EXPECT_EQ(first.value().duplicates, 1U);
    EXPECT_TRUE(service->list_assets().value().empty());
    ImportRequest request;
    request.inputs = {(input / "a.png").string()};
    request.source_root = input.string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.skip_existing = true;
    request.defer_previews = true;
    auto copied = service->execute_import(request);
    ASSERT_TRUE(copied) << copied.error().message;
    ASSERT_EQ(copied.value().imported, 1U);
    const auto revision = service->snapshot().value().revision;
    auto scan = service->scan_import_candidates({input.string()}, input.string(), true, {});
    ASSERT_TRUE(scan) << scan.error().message;
    EXPECT_EQ(scan.value().duplicates, 2U);
    EXPECT_EQ(scan.value().unavailable, 0U);
    EXPECT_EQ(service->snapshot().value().revision, revision);
    EXPECT_EQ(file_sha256((input / "a.png").string()), original_hash);
    EXPECT_EQ(file_sha256((destination / "a.png").string()), original_hash);
    service.reset();
    ASSERT_TRUE(open_service(false));
    std::filesystem::remove(destination / "a.png");
    auto offline = service->scan_import_candidates({input.string()}, input.string(), true, {});
    ASSERT_TRUE(offline) << offline.error().message;
    EXPECT_EQ(offline.value().duplicates, 2U);
}

TEST_F(CatalogServiceTest, ImportScanSameSizeAndMtimeDoNotMeanSameContent)
{
    ASSERT_TRUE(open_service(true));
    const auto a = root / "a.png";
    const auto b = root / "b.png";
    // Equal size and mtime never substitute for byte identity, even for corrupt candidates.
    ASSERT_TRUE(write_utf8_text_file_atomically(a.string(), "AAAA"));
    ASSERT_TRUE(write_utf8_text_file_atomically(b.string(), "BBBB"));
    std::filesystem::last_write_time(b, std::filesystem::last_write_time(a));
    auto scan = service->scan_import_candidates({a.string(), b.string()}, root.string(), false, {});
    ASSERT_TRUE(scan) << scan.error().message;
    ASSERT_EQ(scan.value().candidates.size(), 2U);
    EXPECT_EQ(scan.value().duplicates, 0U);
    EXPECT_NE(scan.value().candidates[0].content_sha256, scan.value().candidates[1].content_sha256);
}

TEST_F(CatalogServiceTest, ImportScanCancelsAndRejectsConcurrentRevision)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "photo.png";
    ASSERT_TRUE(write_photo(photo, Qt::red));
    CancellationSource cancel;
    ASSERT_TRUE(cancel.cancel("test"));
    auto stopped =
        service->scan_import_candidates({photo.string()}, root.string(), false, cancel.token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
    auto stale = service->scan_import_candidates(
        {photo.string()}, root.string(), false, {},
        [&](auto, auto, const auto &) { ASSERT_TRUE(service->import_one(photo.string(), {})); });
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().context.at("reason"), "import_scan_stale");
}

TEST_F(CatalogServiceTest, ImportPreflightRejectsChangedSelectionAndDifferentDestinationContent)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source";
    const auto destination = root / "destination";
    std::filesystem::create_directories(source);
    std::filesystem::create_directories(destination);
    ASSERT_TRUE(write_photo(source / "same.png", Qt::red));
    ASSERT_TRUE(write_photo(destination / "same.png", Qt::blue));
    ASSERT_TRUE(service->import_one((destination / "same.png").string(), {}));
    const auto existing_hash = file_sha256((destination / "same.png").string());
    ImportRequest request;
    request.inputs = {source.string()};
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.skip_existing = true;
    auto conflict = service->preflight_import(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().context.at("reason"), "import_destination_conflict");
    EXPECT_EQ(file_sha256((destination / "same.png").string()), existing_hash);
    request.destination_directory = (root / "other").string();
    std::filesystem::create_directory(request.destination_directory);
    request.expected_content_hashes = {{(source / "same.png").string(), std::string(64, '0')}};
    auto changed = service->preflight_import(request);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().context.at("reason"), "import_content_source_changed");
    EXPECT_TRUE(std::filesystem::is_empty(request.destination_directory));
}

TEST_F(CatalogServiceTest, ImportContentTransactionRejectsInvalidHashAndConcurrentDuplicate)
{
    ASSERT_TRUE(open_service(true));
    const auto source = root / "source.png";
    const auto other = root / "other.png";
    ASSERT_TRUE(write_photo(source, Qt::red));
    ASSERT_TRUE(std::filesystem::copy_file(source, other));
    auto imported = service->import_one(source.string(), {});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto revision = service->snapshot().value().revision;
    auto candidate = *imported.value().asset;
    candidate.id = generate_asset_id();
    candidate.normalized_uri = normalize_local_input(other.string()).value().uri;
    auto invalid = sqlite_repository->commit_imported_asset(candidate, "not-a-hash");
    ASSERT_FALSE(invalid);
    EXPECT_FALSE(sqlite_repository->find_asset_by_id(candidate.id).value());
    EXPECT_EQ(service->snapshot().value().revision, revision);
    const auto digest = sha256_file_hex(source.string()).value();
    auto duplicate = sqlite_repository->commit_imported_asset(candidate, digest, true);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "import_duplicate_content");
    EXPECT_FALSE(sqlite_repository->find_asset_by_id(candidate.id).value());
    EXPECT_EQ(service->snapshot().value().revision, revision);
    auto changed = service->import_one(other.string(), {}, ImportPreviewPolicy::kMinimal, true,
                                       true, std::string(64, '0'));
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed.value().status, ImportItemStatus::kFailed);
    EXPECT_EQ(changed.value().error->context.at("reason"), "import_content_source_changed");
    EXPECT_EQ(service->list_assets().value().size(), 1U);
}

TEST_F(CatalogServiceTest, ImportContentIndexMigratesV16AndBackfillsWithoutRevisionChange)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "original.png";
    const auto duplicate = root / "renamed.png";
    ASSERT_TRUE(write_photo(photo, Qt::green));
    ASSERT_TRUE(std::filesystem::copy_file(photo, duplicate));
    auto item = service->import_one(photo.string(), {});
    ASSERT_TRUE(item);
    const auto revision = service->snapshot().value().revision;
    service.reset();
    // v16 is the complete current schema without the derived v17 table.
    const auto connection = QStringLiteral("import-scan-v16");
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(db.open());
        QSqlQuery sql(db);
        ASSERT_TRUE(sql.exec("DROP TABLE asset_content_hash"));
        ASSERT_TRUE(sql.exec("UPDATE schema_info SET schema_version = 16"));
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    ASSERT_TRUE(open_service(false));
    EXPECT_EQ(service->snapshot().value().schema_version, 17);
    auto scan = service->scan_import_candidates({duplicate.string()}, root.string(), false, {});
    ASSERT_TRUE(scan) << scan.error().message;
    EXPECT_EQ(scan.value().duplicates, 1U);
    EXPECT_EQ(service->snapshot().value().revision, revision);
    auto indexed = sqlite_repository->import_content_sources(std::filesystem::file_size(photo), "");
    ASSERT_TRUE(indexed);
    ASSERT_EQ(indexed.value().size(), 1U);
    EXPECT_TRUE(indexed.value()[0].sha256.has_value());
}
} // namespace ravo
