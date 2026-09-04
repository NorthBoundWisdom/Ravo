#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <zlib.h>

#include <QBuffer>
#include <QByteArray>
#include <QByteArrayView>
#include <QColor>
#include <QColorSpace>
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
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/color_reconstruction.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/dehaze.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/sharpen.h"
#include "ravo/recipe/texture.h"
#include "ravo/services/catalog_service.h"

#include "capture_metadata_test_support.h"
#include "catalog_test_support.h"
#include "catalog_service_test_support.h"
#include "color_balance_fixture.h"
#include "catalog_repository_test_control.h"
#include "recovery_publication_internal.h"
#include "temperature_fixture.h"

namespace ravo
{
namespace
{

TEST_F(CatalogServiceTest, StableFolderIdentityRelinksMissingRootAndSurvivesReopen)
{
    ASSERT_TRUE(open_service(true));
    const auto original = root / "original-root";
    const auto replacement = root / "replacement-root";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    QImage first_image(16, 12, QImage::Format_RGB888);
    first_image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    first_image.fill(QColor(30, 90, 150));
    QImage second_image(12, 16, QImage::Format_RGB888);
    second_image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    second_image.fill(QColor(150, 80, 30));
    const auto first = original / "first.png";
    const auto second = original / "second.png";
    ASSERT_TRUE(first_image.save(QString::fromStdString(first.string()), "PNG"));
    ASSERT_TRUE(second_image.save(QString::fromStdString(second.string()), "PNG"));
    const auto first_hash = file_sha256(first.string());
    const auto second_hash = file_sha256(second.string());
    auto imported = service->import_inputs({original.string()}, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().size(), 2U);
    ASSERT_TRUE(imported.value().front().asset);
    DevelopParams edited;
    edited.exposure_ev = 0.35;
    ASSERT_TRUE(service->save_develop(imported.value().front().asset->id, edited));

    auto before_folders = service->list_folders();
    ASSERT_TRUE(before_folders) << before_folders.error().message;
    const auto before = std::find_if(before_folders.value().begin(), before_folders.value().end(),
                                     [](const FolderRecord &folder)
                                     { return folder.display_name == "original-root"; });
    ASSERT_NE(before, before_folders.value().end());
    ASSERT_FALSE(before->id.empty());
    EXPECT_FALSE(before->missing);
    const auto folder_id = before->id;
    const auto previous_uri = before->uri;

    std::filesystem::rename(original, replacement);
    auto missing_folders = service->list_folders();
    ASSERT_TRUE(missing_folders) << missing_folders.error().message;
    const auto missing =
        std::find_if(missing_folders.value().begin(), missing_folders.value().end(),
                     [&](const FolderRecord &folder) { return folder.id == folder_id; });
    ASSERT_NE(missing, missing_folders.value().end());
    EXPECT_TRUE(missing->missing);
    EXPECT_EQ(missing->uri, previous_uri);

    auto relinked = service->relink_folder(folder_id, replacement.string());
    ASSERT_TRUE(relinked) << relinked.error().message;
    EXPECT_EQ(relinked.value().folder_id, folder_id);
    EXPECT_EQ(relinked.value().previous_uri, previous_uri);
    EXPECT_EQ(relinked.value().asset_count, 2U);
    EXPECT_EQ(relinked.value().recovery_pending, 2U);
    auto pending = service->pending_recovery();
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_EQ(pending.value().size(), 2U);
    auto synchronized = service->sync_recovery(std::nullopt);
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    EXPECT_EQ(synchronized.value().pending_after, 0U);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 2U);
    for (const auto &asset : assets.value())
        EXPECT_NE(asset.normalized_uri.find("replacement-root"), std::string::npos);
    EXPECT_EQ(file_sha256((replacement / "first.png").string()), first_hash);
    EXPECT_EQ(file_sha256((replacement / "second.png").string()), second_hash);

    service.reset();
    sqlite_repository = nullptr;
    ASSERT_TRUE(open_service(false));
    auto reopened_folders = service->list_folders();
    ASSERT_TRUE(reopened_folders) << reopened_folders.error().message;
    const auto reopened =
        std::find_if(reopened_folders.value().begin(), reopened_folders.value().end(),
                     [&](const FolderRecord &folder) { return folder.id == folder_id; });
    ASSERT_NE(reopened, reopened_folders.value().end());
    EXPECT_FALSE(reopened->missing);
    EXPECT_EQ(reopened->display_name, "replacement-root");
    auto reopened_recipe = service->load_recipe(imported.value().front().asset->id);
    ASSERT_TRUE(reopened_recipe) << reopened_recipe.error().message;
    auto reopened_develop = develop_from_recipe(reopened_recipe.value());
    ASSERT_TRUE(reopened_develop) << reopened_develop.error().message;
    EXPECT_NEAR(reopened_develop.value().exposure_ev, edited.exposure_ev, 1e-12);
}

TEST_F(CatalogServiceTest, FolderRelinkRejectsCancellationAndIdentityMismatchWithoutMutation)
{
    ASSERT_TRUE(open_service(true));
    const auto original = root / "missing-root";
    const auto replacement = root / "candidate-root";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    QImage source(14, 10, QImage::Format_RGB888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    source.fill(QColor(20, 70, 120));
    const auto photo = original / "asset.png";
    ASSERT_TRUE(source.save(QString::fromStdString(photo.string()), "PNG"));
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    const auto original_uri = imported.value().asset->normalized_uri;
    auto folders = service->list_folders();
    ASSERT_TRUE(folders) << folders.error().message;
    const auto folder =
        std::find_if(folders.value().begin(), folders.value().end(),
                     [](const FolderRecord &item) { return item.display_name == "missing-root"; });
    ASSERT_NE(folder, folders.value().end());
    const auto folder_id = folder->id;
    std::filesystem::rename(original, replacement);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("test_cancel"));
    auto cancelled_result =
        service->relink_folder(folder_id, replacement.string(), cancelled.token());
    ASSERT_FALSE(cancelled_result);
    EXPECT_EQ(cancelled_result.error().code, ErrorCode::kCancelled);
    auto unchanged = service->list_assets();
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    ASSERT_EQ(unchanged.value().size(), 1U);
    EXPECT_EQ(unchanged.value().front().normalized_uri, original_uri);

    QImage wrong(14, 10, QImage::Format_RGB888);
    wrong.setColorSpace(QColorSpace(QColorSpace::SRgb));
    wrong.fill(QColor(220, 40, 20));
    ASSERT_TRUE(wrong.save(QString::fromStdString((replacement / "asset.png").string()), "PNG"));
    auto mismatched = service->relink_folder(folder_id, replacement.string());
    ASSERT_FALSE(mismatched);
    EXPECT_EQ(mismatched.error().code, ErrorCode::kConflict);
    EXPECT_EQ(mismatched.error().context.at("reason"), "replacement_asset_identity_mismatch");
    unchanged = service->list_assets();
    ASSERT_TRUE(unchanged) << unchanged.error().message;
    ASSERT_EQ(unchanged.value().size(), 1U);
    EXPECT_EQ(unchanged.value().front().id, asset_id);
    EXPECT_EQ(unchanged.value().front().normalized_uri, original_uri);
    auto still_missing = service->list_folders();
    ASSERT_TRUE(still_missing) << still_missing.error().message;
    const auto same_folder =
        std::find_if(still_missing.value().begin(), still_missing.value().end(),
                     [&](const FolderRecord &item) { return item.id == folder_id; });
    ASSERT_NE(same_folder, still_missing.value().end());
    EXPECT_TRUE(same_folder->missing);
}

TEST_F(CatalogServiceTest, FolderRelinkTransactionFailuresRollBackFolderAssetsAndRevision)
{
    ASSERT_TRUE(open_service(true));
    const auto original = root / "transaction-old";
    const auto replacement = root / "transaction-new";
    ASSERT_TRUE(std::filesystem::create_directory(original));
    QImage image(10, 10, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(45, 95, 145));
    ASSERT_TRUE(image.save(QString::fromStdString((original / "a.png").string()), "PNG"));
    image.fill(QColor(145, 95, 45));
    ASSERT_TRUE(image.save(QString::fromStdString((original / "b.png").string()), "PNG"));
    auto imported = service->import_inputs({original.string()}, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    auto folders = service->list_folders();
    ASSERT_TRUE(folders) << folders.error().message;
    const auto folder =
        std::find_if(folders.value().begin(), folders.value().end(), [](const FolderRecord &item)
                     { return item.display_name == "transaction-old"; });
    ASSERT_NE(folder, folders.value().end());
    const auto folder_id = folder->id;
    const auto folder_uri = folder->uri;
    auto before_assets = service->list_assets();
    ASSERT_TRUE(before_assets) << before_assets.error().message;
    auto before_snapshot = service->snapshot();
    ASSERT_TRUE(before_snapshot) << before_snapshot.error().message;
    std::filesystem::rename(original, replacement);
    ASSERT_NE(sqlite_repository, nullptr);

    const std::array failures{
        testing::SqliteFolderRelinkFailure::kAfterFolderUpdate,
        testing::SqliteFolderRelinkFailure::kAfterFirstAssetUpdate,
        testing::SqliteFolderRelinkFailure::kBeforeCommit,
    };
    for (const auto failure : failures)
    {
        testing::SqliteCatalogTestControl::inject_folder_relink(*sqlite_repository, failure);
        auto relinked = service->relink_folder(folder_id, replacement.string());
        ASSERT_FALSE(relinked);
        EXPECT_EQ(relinked.error().code, ErrorCode::kIo);
        auto after_assets = service->list_assets();
        ASSERT_TRUE(after_assets) << after_assets.error().message;
        ASSERT_EQ(after_assets.value().size(), before_assets.value().size());
        for (std::size_t index = 0; index < before_assets.value().size(); ++index)
        {
            EXPECT_EQ(after_assets.value()[index].id, before_assets.value()[index].id);
            EXPECT_EQ(after_assets.value()[index].normalized_uri,
                      before_assets.value()[index].normalized_uri);
        }
        auto after_folder = sqlite_repository->find_folder_by_id(folder_id);
        ASSERT_TRUE(after_folder) << after_folder.error().message;
        ASSERT_TRUE(after_folder.value());
        EXPECT_EQ(after_folder.value()->uri, folder_uri);
        auto after_snapshot = service->snapshot();
        ASSERT_TRUE(after_snapshot) << after_snapshot.error().message;
        EXPECT_EQ(after_snapshot.value().revision, before_snapshot.value().revision);
        auto pending = service->pending_recovery();
        ASSERT_TRUE(pending) << pending.error().message;
        EXPECT_TRUE(pending.value().empty());
    }

    service.reset();
    sqlite_repository = nullptr;
    ASSERT_TRUE(open_service(false));
    auto reopened = service->list_assets();
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened.value().size(), before_assets.value().size());
    for (std::size_t index = 0; index < before_assets.value().size(); ++index)
    {
        EXPECT_EQ(reopened.value()[index].id, before_assets.value()[index].id);
        EXPECT_EQ(reopened.value()[index].normalized_uri,
                  before_assets.value()[index].normalized_uri);
    }
}

TEST_F(CatalogServiceTest, LibraryQueryValidationFailsBeforeFiltering)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    LibraryQuery query;
    query.rating_value = 9;
    auto rejected = service->list_assets(query);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_library_rating_filter");

    query = {};
    query.aperture = {8.0, 2.8};
    rejected = service->list_assets(query);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("field"), "aperture");
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_library_filter_range");
}

TEST_F(CatalogServiceTest, LibraryQueryFiltersMediaTextAndEditStateThroughService)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "Golden-JPEG.jpg").string();
    const auto png_path = (root / "Blue-PNG.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 80, 140));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    ASSERT_TRUE(image.save(QString::fromStdString(png_path), "PNG"));
    auto jpeg = service->import_one(jpeg_path, CancellationToken{});
    auto png = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(jpeg) << jpeg.error().message;
    ASSERT_TRUE(png) << png.error().message;
    ASSERT_TRUE(jpeg.value().asset);
    ASSERT_TRUE(png.value().asset);

    DevelopParams edited;
    edited.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(jpeg.value().asset->id, edited));

    LibraryQuery query;
    query.media_types = {std::string(kMediaTypePng)};
    auto listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, png.value().asset->id);

    query = {};
    query.text = "golden-jpeg";
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, jpeg.value().asset->id);

    query = {};
    query.edit_filter = EditFilter::kEdited;
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, jpeg.value().asset->id);
}

TEST_F(CatalogServiceTest, CaptureFacetsEnumerateAndFilterThroughService)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    for (int index = 0; index < 6; ++index)
    {
        AssetRecord asset;
        asset.id = "ast_facet_" + std::to_string(index);
        asset.normalized_uri = "file:///library/facet/photo-" + std::to_string(index) + ".jpg";
        asset.media_type = std::string(kMediaTypeJpeg);
        asset.size_bytes = 1000U + static_cast<std::uint64_t>(index);
        asset.mtime_unix_ms = 10'000 + index;
        asset.width = 100;
        asset.height = 80;
        asset.created_unix_ms = 20'000 + index;
        if (index < 4)
        {
            asset.capture.camera_make = index < 2 ? "RavoCam" : "OtherCam";
            asset.capture.camera_model = index % 2 == 0 ? "Alpha" : "Beta";
            asset.capture.focal_length_mm = index < 2 ? 35.0 : 85.0;
            asset.capture.lens_make = index < 2 ? "RavoOptics" : "OtherOptics";
            asset.capture.lens_model = index % 2 == 0 ? "Prime 35" : "Zoom 85";
            CaptureDateTime when;
            when.local_exif = (index < 3 ? "2024:05:01" : "2024:05:02") + std::string(" 12:00:00");
            asset.capture.captured_datetime = when;
            asset.capture.captured_unix_s = 1'700'000'000 + index;
        }
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    }
    repository.value().reset();

    auto service_opened = open_service(false);
    ASSERT_TRUE(service_opened) << service_opened.error().message;

    auto facets = service->list_capture_facets();
    ASSERT_TRUE(facets) << facets.error().message;
    EXPECT_FALSE(facets.value().truncated);
    ASSERT_EQ(facets.value().cameras.size(), 4U);
    EXPECT_EQ(facets.value().cameras[0].camera_make, "OtherCam");
    EXPECT_EQ(facets.value().lenses.size(), 2U);
    ASSERT_EQ(facets.value().lens_names.size(), 4U);
    EXPECT_EQ(facets.value().lens_names[0].label, "OtherOptics Prime 35");
    ASSERT_EQ(facets.value().capture_dates.size(), 2U);
    EXPECT_EQ(facets.value().capture_dates.front().captured_local_date, "2024:05:02");

    LibraryQuery query;
    query.camera_make_equals = "RavoCam";
    query.camera_model_equals = "Alpha";
    auto listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().capture.camera_model, "Alpha");

    query = {};
    query.focal_length_mm_equals = 85.0;
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);

    query = {};
    query.captured_local_date = "2024:05:01";
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 3U);

    query.camera_make_equals = "RavoCam";
    auto invalid = service->list_assets(query);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_library_camera_facet");

    query = {};
    query.lens_make_equals = "RavoOptics";
    query.lens_model_equals = "Prime 35";
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().capture.lens_model, "Prime 35");

    query.lens_model_equals.reset();
    invalid = service->list_assets(query);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_library_lens_name_facet");
}

TEST_F(CatalogServiceTest, MigratesV13CatalogAddsLensNameColumns)
{
    {
        const auto connection = QStringLiteral("ravo_v13_lens_name");
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
            "CREATE TABLE catalog_folder (id TEXT PRIMARY KEY, uri TEXT NOT NULL UNIQUE, "
            "created_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset (id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "display_name TEXT NOT NULL, folder_uri TEXT NOT NULL, folder_id TEXT NOT NULL "
            "REFERENCES catalog_folder(id), media_type TEXT NOT NULL, size_bytes INTEGER NOT NULL, "
            "mtime_unix_ms INTEGER NOT NULL, content_fingerprint TEXT, width INTEGER, height INTEGER, "
            "import_state TEXT NOT NULL, error_code TEXT, error_message TEXT, "
            "created_unix_ms INTEGER NOT NULL, rating INTEGER NOT NULL DEFAULT 0, "
            "color_label TEXT NOT NULL DEFAULT 'none', rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "title TEXT, description TEXT, creator TEXT, copyright TEXT, camera_make TEXT, "
            "camera_model TEXT, iso REAL, aperture REAL, focal_length_mm REAL, shutter_s REAL, "
            "captured_unix_s INTEGER, captured_local_exif TEXT, captured_subsecond_digits TEXT, "
            "captured_utc_offset_minutes INTEGER, gps_latitude_e6 INTEGER, gps_longitude_e6 INTEGER, "
            "gps_altitude_magnitude_mm INTEGER, gps_altitude_ref INTEGER, country TEXT, "
            "province_state TEXT, city TEXT, sublocation TEXT)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recovery_state (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "generation INTEGER NOT NULL, synchronized_generation INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE library_stack (id TEXT PRIMARY KEY, pick_asset_id TEXT NOT NULL, "
            "created_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE library_stack_member (stack_id TEXT NOT NULL REFERENCES library_stack(id) ON DELETE CASCADE, "
            "asset_id TEXT NOT NULL UNIQUE REFERENCES asset(id) ON DELETE CASCADE, position INTEGER NOT NULL, "
            "PRIMARY KEY (stack_id, asset_id))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 13, 'cat_v13', 1, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO catalog_folder(id, uri, created_unix_ms) VALUES ('fld_one', 'file:///tmp', 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, display_name, folder_uri, folder_id, media_type, "
            "size_bytes, mtime_unix_ms, import_state, created_unix_ms) VALUES "
            "('ast_old', 'file:///tmp/old.jpg', 'old.jpg', 'file:///tmp', 'fld_one', 'image/jpeg', "
            "12, 1, 'imported', 1)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("INSERT INTO asset_metadata(asset_id, camera_make, camera_model) "
                           "VALUES ('ast_old', 'OldCam', 'Body')")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset_recovery_state(asset_id, generation, synchronized_generation) "
            "VALUES ('ast_old', 1, 1)")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto opened = open_service(false);
    ASSERT_TRUE(opened) << opened.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    ASSERT_TRUE(service->close());
    service.reset();

    const auto connection = QStringLiteral("ravo_v13_lens_name_verify");
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QString::fromStdString(database_path));
    ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA table_info(asset_metadata)")));
    bool has_lens_make = false;
    bool has_lens_model = false;
    while (query.next())
    {
        const auto name = query.value(1).toString();
        if (name == QStringLiteral("lens_make"))
            has_lens_make = true;
        if (name == QStringLiteral("lens_model"))
            has_lens_model = true;
    }
    EXPECT_TRUE(has_lens_make);
    EXPECT_TRUE(has_lens_model);
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

TEST_F(CatalogServiceTest, LocationFacetsEnumerateAndFilterThroughService)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    for (int index = 0; index < 5; ++index)
    {
        AssetRecord asset;
        asset.id = "ast_loc_" + std::to_string(index);
        asset.normalized_uri = "file:///library/loc/photo-" + std::to_string(index) + ".jpg";
        asset.media_type = std::string(kMediaTypeJpeg);
        asset.size_bytes = 1000U + static_cast<std::uint64_t>(index);
        asset.mtime_unix_ms = 10'000 + index;
        asset.width = 100;
        asset.height = 80;
        asset.created_unix_ms = 20'000 + index;
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
        if (index < 4)
        {
            WritableMetadata metadata;
            metadata.country = index < 2 ? "China" : "Japan";
            metadata.province_state = index % 2 == 0 ? "Shanghai" : "Tokyo";
            metadata.city = index < 2 ? "Shanghai" : "Tokyo";
            if (index == 0)
                metadata.sublocation = "Bund";
            ASSERT_TRUE(repository.value()->upsert_writable_metadata(asset.id, metadata));
        }
    }
    repository.value().reset();

    auto service_opened = open_service(false);
    ASSERT_TRUE(service_opened) << service_opened.error().message;

    auto facets = service->list_location_facets();
    ASSERT_TRUE(facets) << facets.error().message;
    EXPECT_FALSE(facets.value().truncated);
    ASSERT_EQ(facets.value().countries.size(), 2U);
    EXPECT_EQ(facets.value().countries.front().key, "China");
    ASSERT_EQ(facets.value().cities.size(), 2U);
    ASSERT_EQ(facets.value().sublocations.size(), 1U);
    EXPECT_EQ(facets.value().sublocations.front().label, "Bund");

    LibraryQuery query;
    query.country_equals = "China";
    auto listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);

    query.city_equals = "Shanghai";
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);

    query.sublocation_equals = "Bund";
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);

    query = {};
    query.country_equals = "Japan";
    query.city_equals = "Shanghai";
    listed = service->list_assets(query);
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
}

TEST_F(CatalogServiceTest, PagedLibraryQueryMatchesDomainAndBoundsMaterialization)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    for (int index = 0; index < 60; ++index)
    {
        AssetRecord asset;
        asset.id = "ast_page_" + std::to_string(1000 + index);
        const auto folder = index % 3;
        asset.normalized_uri = "file:///library/folder-" + std::to_string(folder) + "/photo-" +
                               std::to_string(100 - index) + (index % 2 == 0 ? ".jpg" : ".png");
        asset.media_type =
            index % 2 == 0 ? std::string(kMediaTypeJpeg) : std::string(kMediaTypePng);
        asset.size_bytes = static_cast<std::uint64_t>(1000 + index * 17);
        asset.mtime_unix_ms = 10'000 + index;
        asset.width = static_cast<std::uint32_t>(100 + index);
        asset.height = static_cast<std::uint32_t>(50 + index % 5);
        asset.created_unix_ms = 20'000 + index;
        asset.review.rating = index % 6;
        asset.review.color_label = index % 4 == 0 ? ColorLabel::kBlue : ColorLabel::kNone;
        asset.review.rejected = index % 11 == 0;
        if (index % 5 != 0)
            asset.capture.captured_unix_s = 1'000 + index;
        asset.capture.camera_make = index % 2 == 0 ? "RavoCam" : "OtherCam";
        asset.capture.camera_model = "Model " + std::to_string(index % 4);
        asset.capture.iso = static_cast<double>(100 + index * 10);
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
        if (index % 3 == 0)
            ASSERT_TRUE(repository.value()->replace_asset_tags(asset.id, {"featured", "trip"}));
        if (index % 7 == 0)
        {
            WritableMetadata metadata;
            metadata.title = "Golden " + std::to_string(index);
            ASSERT_TRUE(repository.value()->upsert_writable_metadata(asset.id, metadata));
        }
        if (index % 4 == 0)
            ASSERT_TRUE(repository.value()->save_recipe_json(asset.id, 1, "{}"));
    }
    auto all = repository.value()->list_assets();
    ASSERT_TRUE(all) << all.error().message;
    ASSERT_EQ(all.value().size(), 60U);

    std::vector<LibraryQuery> queries;
    queries.push_back({});
    LibraryQuery display;
    display.sort_field = AssetSortField::kDisplayName;
    display.sort_direction = SortDirection::kAscending;
    queries.push_back(display);
    LibraryQuery capture;
    capture.sort_field = AssetSortField::kCaptureTime;
    capture.sort_direction = SortDirection::kDescending;
    queries.push_back(capture);
    LibraryQuery filtered;
    filtered.rating_mode = RatingFilterMode::kMinimum;
    filtered.rating_value = 2;
    filtered.folder_uri = "file:///library/folder-0";
    filtered.tag = "featured";
    filtered.media_types = {std::string(kMediaTypeJpeg)};
    filtered.edit_filter = EditFilter::kEdited;
    filtered.camera = "ravocam";
    filtered.iso = {100.0, 700.0};
    filtered.aspect_ratio = {1.5, 3.5};
    filtered.sort_field = AssetSortField::kFileSize;
    filtered.sort_direction = SortDirection::kAscending;
    queries.push_back(filtered);
    LibraryQuery text;
    text.text = "golden";
    text.sort_field = AssetSortField::kRating;
    queries.push_back(text);

    for (const auto &query : queries)
    {
        const auto expected = filter_and_sort_assets(all.value(), query);
        std::vector<AssetRecord> paged;
        std::size_t offset = 0U;
        std::optional<std::string> cursor;
        std::optional<std::size_t> known_total;
        for (;;)
        {
            LibraryPageRequest request;
            request.query = query;
            request.offset = offset;
            request.limit = 7U;
            request.after_asset_id = cursor;
            request.known_total = known_total;
            auto page = repository.value()->list_assets_page(request);
            ASSERT_TRUE(page) << page.error().message;
            EXPECT_EQ(page.value().offset, offset);
            EXPECT_EQ(page.value().total, expected.size());
            EXPECT_LE(page.value().materialized_rows, 7U);
            EXPECT_EQ(page.value().materialized_rows, page.value().assets.size());
            EXPECT_GE(page.value().query_elapsed_us, 0);
            paged.insert(paged.end(), page.value().assets.begin(), page.value().assets.end());
            if (!page.value().has_more)
                break;
            ASSERT_FALSE(page.value().assets.empty());
            ASSERT_TRUE(page.value().next_cursor);
            offset += page.value().assets.size();
            cursor = page.value().next_cursor;
            known_total = page.value().total;
        }
        ASSERT_EQ(paged.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(paged[index].id, expected[index].id);
            EXPECT_EQ(paged[index].tags, expected[index].tags);
            EXPECT_EQ(paged[index].metadata.title, expected[index].metadata.title);
            EXPECT_EQ(paged[index].capture.camera_model, expected[index].capture.camera_model);
        }
    }

    LibraryPageRequest invalid;
    invalid.limit = kLibraryPageMaximumSize + 1U;
    auto rejected = repository.value()->list_assets_page(invalid);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_library_page");
    ASSERT_TRUE(repository.value()->close());
}

TEST_F(CatalogServiceTest, TenThousandRowLibraryTraversalStaysPageBounded)
{
    auto created = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(created.value()->close());
    created.value().reset();

    const auto connection = QStringLiteral("ravo_scale_seed");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        ASSERT_TRUE(database.transaction()) << database.lastError().text().toStdString();
        QSqlQuery insert_folder(database);
        insert_folder.prepare(QStringLiteral(
            "INSERT INTO catalog_folder(id, uri, created_unix_ms) VALUES (?, ?, 1)"));
        for (int index = 0; index < 100; ++index)
        {
            insert_folder.bindValue(0, QStringLiteral("fld_scale_%1").arg(index));
            insert_folder.bindValue(1, QStringLiteral("file:///synthetic/folder-%1").arg(index));
            ASSERT_TRUE(insert_folder.exec()) << insert_folder.lastError().text().toStdString();
        }
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, display_name, folder_uri, folder_id, media_type, "
            "size_bytes, mtime_unix_ms, content_fingerprint, width, height, import_state, "
            "error_code, error_message, created_unix_ms, rating, color_label, rejected) "
            "VALUES (?, ?, ?, ?, ?, 'image/jpeg', ?, ?, NULL, 64, 48, 'imported', NULL, NULL, ?, "
            "?, 'none', 0)"));
        for (int index = 0; index < 10'000; ++index)
        {
            const auto id = QStringLiteral("ast_scale_%1").arg(index, 5, 10, QLatin1Char('0'));
            const auto folder = QStringLiteral("file:///synthetic/folder-%1").arg(index % 100);
            const auto name = QStringLiteral("photo-%1.jpg").arg(index, 5, 10, QLatin1Char('0'));
            insert.bindValue(0, id);
            insert.bindValue(1, folder + QLatin1Char('/') + name);
            insert.bindValue(2, name);
            insert.bindValue(3, folder);
            insert.bindValue(4, QStringLiteral("fld_scale_%1").arg(index % 100));
            insert.bindValue(5, static_cast<qlonglong>(1000 + index));
            insert.bindValue(6, static_cast<qlonglong>(20'000 + index));
            insert.bindValue(7, static_cast<qlonglong>(30'000 + index));
            insert.bindValue(8, index % 6);
            ASSERT_TRUE(insert.exec()) << insert.lastError().text().toStdString();
        }
        ASSERT_TRUE(database.commit()) << database.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(connection);

    auto repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    std::vector<std::int64_t> elapsed;
    std::size_t offset = 0U;
    std::optional<std::string> cursor;
    std::optional<std::size_t> known_total;
    std::string previous_id;
    for (;;)
    {
        LibraryPageRequest request;
        request.offset = offset;
        request.limit = kLibraryPageDefaultSize;
        request.after_asset_id = cursor;
        request.known_total = known_total;
        auto page = repository.value()->list_assets_page(request);
        ASSERT_TRUE(page) << page.error().message;
        EXPECT_EQ(page.value().total, 10'000U);
        EXPECT_LE(page.value().materialized_rows, kLibraryPageDefaultSize);
        EXPECT_EQ(page.value().materialized_rows, page.value().assets.size());
        elapsed.push_back(page.value().query_elapsed_us);
        for (const auto &asset : page.value().assets)
        {
            if (!previous_id.empty())
                EXPECT_GT(previous_id, asset.id);
            previous_id = asset.id;
        }
        offset += page.value().assets.size();
        if (!page.value().has_more)
            break;
        ASSERT_TRUE(page.value().next_cursor);
        cursor = page.value().next_cursor;
        known_total = page.value().total;
    }
    EXPECT_EQ(offset, 10'000U);
    ASSERT_EQ(elapsed.size(), 50U);
    std::sort(elapsed.begin(), elapsed.end());
    const auto p90 = elapsed[(elapsed.size() * 9U) / 10U];
    std::cout << "library_page rows=10000 pages=" << elapsed.size() << " p90_us=" << p90
              << " max_us=" << elapsed.back() << '\n';
    if (const char *budget = std::getenv("RAVO_LIBRARY_PAGE_P90_BUDGET_US"))
        EXPECT_LE(p90, std::stoll(budget));
    auto folders = repository.value()->list_folders();
    ASSERT_TRUE(folders) << folders.error().message;
    EXPECT_EQ(folders.value().front().asset_count, 10'000);

    const auto explain_connection = QStringLiteral("ravo_scale_explain");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), explain_connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "EXPLAIN QUERY PLAN SELECT id FROM asset ORDER BY created_unix_ms DESC, id DESC "
            "LIMIT 200")));
        QStringList plan;
        while (query.next())
            plan.push_back(query.value(3).toString());
        EXPECT_TRUE(plan.join(QLatin1Char('\n')).contains(QStringLiteral("asset_created_id_idx")))
            << plan.join(QLatin1Char('\n')).toStdString();
        ASSERT_TRUE(
            query.exec(QStringLiteral("EXPLAIN QUERY PLAN SELECT id FROM asset WHERE id IN "
                                      "(SELECT asset_id FROM asset_tag WHERE name = 'featured')")));
        plan.clear();
        while (query.next())
            plan.push_back(query.value(3).toString());
        EXPECT_TRUE(plan.join(QLatin1Char('\n')).contains(QStringLiteral("asset_tag_name_idx")))
            << plan.join(QLatin1Char('\n')).toStdString();
        ASSERT_TRUE(query.exec(QStringLiteral(
            "EXPLAIN QUERY PLAN SELECT id FROM asset WHERE folder_id = 'fld_scale_1' "
            "ORDER BY id")));
        plan.clear();
        while (query.next())
            plan.push_back(query.value(3).toString());
        EXPECT_TRUE(plan.join(QLatin1Char('\n')).contains(QStringLiteral("asset_folder_id_idx")))
            << plan.join(QLatin1Char('\n')).toStdString();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(explain_connection);
    ASSERT_TRUE(repository.value()->close());
}

TEST_F(CatalogServiceTest, PrivatePhotoManagementReleaseProbePreservesCorpus)
{
    const char *corpus = std::getenv("RAVO_PHOTO_CORPUS");
    if (corpus == nullptr || std::string_view(corpus).empty())
        GTEST_SKIP() << "RAVO_PHOTO_CORPUS is not set";
    ASSERT_TRUE(open_service(true));

    struct SourceSnapshot
    {
        std::string path;
        std::uintmax_t size = 0U;
        std::filesystem::file_time_type modified;
        QByteArray sha256;
    };
    const auto percentile_summary = [](std::vector<std::int64_t> values)
    {
        std::sort(values.begin(), values.end());
        const auto value_at = [&](const std::size_t numerator, const std::size_t denominator)
        {
            if (values.empty())
                return std::int64_t{0};
            const auto index =
                std::min(values.size() - 1U, (values.size() * numerator) / denominator);
            return values[index];
        };
        return std::array<std::int64_t, 3>{value_at(1U, 2U), value_at(9U, 10U),
                                           values.empty() ? 0 : values.back()};
    };

    const auto enumeration_started = std::chrono::steady_clock::now();
    auto enumerated = service->enumerate_import_inputs({corpus}, CancellationToken{});
    ASSERT_TRUE(enumerated) << enumerated.error().message;
    ASSERT_FALSE(enumerated.value().empty());
    ASSERT_LE(enumerated.value().size(), kImportBatchMaximumAssets);
    const auto enumeration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - enumeration_started)
                                    .count();
    std::vector<SourceSnapshot> sources;
    sources.reserve(enumerated.value().size());
    for (const auto &path : enumerated.value())
    {
        std::error_code error;
        SourceSnapshot source;
        source.path = path;
        source.size = std::filesystem::file_size(path, error);
        ASSERT_FALSE(error) << path << ": " << error.message();
        source.modified = std::filesystem::last_write_time(path, error);
        ASSERT_FALSE(error) << path << ": " << error.message();
        source.sha256 = file_sha256(path);
        ASSERT_EQ(source.sha256.size(), 32) << path;
        sources.push_back(std::move(source));
    }

    std::vector<std::int64_t> raw_import_us;
    std::vector<std::int64_t> raster_import_us;
    std::vector<std::string> raw_assets;
    std::vector<std::string> raster_assets;
    for (const auto &path : enumerated.value())
    {
        const auto started = std::chrono::steady_clock::now();
        auto imported = service->import_one(path, CancellationToken{});
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        ASSERT_TRUE(imported) << imported.error().message;
        if (!imported.value().asset)
            continue;
        if (is_raw_media_type(imported.value().asset->media_type))
        {
            raw_import_us.push_back(elapsed);
            raw_assets.push_back(imported.value().asset->id);
        }
        else
        {
            raster_import_us.push_back(elapsed);
            raster_assets.push_back(imported.value().asset->id);
        }
    }

    std::vector<std::int64_t> cold_preview_us;
    std::vector<std::int64_t> warm_preview_us;
    std::vector<std::string> preview_assets;
    const std::array<const std::vector<std::string> *, 2> asset_groups{&raw_assets, &raster_assets};
    for (const auto *ids : asset_groups)
        preview_assets.insert(
            preview_assets.end(), ids->begin(),
            ids->begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(ids->size(), 8U)));
    for (const auto &asset_id : preview_assets)
    {
        PreviewRequest request;
        request.asset_id = asset_id;
        request.max_edge = kDefaultPreviewMaxEdge;
        request.prefer_embedded_preview = false;
        auto started = std::chrono::steady_clock::now();
        auto cold = service->request_preview(request);
        cold_preview_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count());
        ASSERT_TRUE(cold) << cold.error().message;
        started = std::chrono::steady_clock::now();
        auto warm = service->request_preview(request);
        warm_preview_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - started)
                                      .count());
        ASSERT_TRUE(warm) << warm.error().message;
        EXPECT_EQ(warm.value().width, cold.value().width);
        EXPECT_EQ(warm.value().height, cold.value().height);
    }

    std::vector<std::int64_t> page_us;
    std::size_t offset = 0U;
    std::optional<std::string> cursor;
    std::optional<std::size_t> known_total;
    for (;;)
    {
        LibraryPageRequest request;
        request.offset = offset;
        request.limit = kLibraryPageDefaultSize;
        request.after_asset_id = cursor;
        request.known_total = known_total;
        auto page = service->list_assets_page(request);
        ASSERT_TRUE(page) << page.error().message;
        EXPECT_LE(page.value().materialized_rows, kLibraryPageDefaultSize);
        page_us.push_back(page.value().query_elapsed_us);
        offset += page.value().assets.size();
        if (!page.value().has_more)
            break;
        ASSERT_TRUE(page.value().next_cursor);
        cursor = page.value().next_cursor;
        known_total = page.value().total;
    }

    for (const auto &source : sources)
    {
        std::error_code error;
        EXPECT_EQ(std::filesystem::file_size(source.path, error), source.size) << source.path;
        EXPECT_FALSE(error) << source.path << ": " << error.message();
        EXPECT_EQ(std::filesystem::last_write_time(source.path, error), source.modified)
            << source.path;
        EXPECT_FALSE(error) << source.path << ": " << error.message();
        EXPECT_EQ(file_sha256(source.path), source.sha256) << source.path;
    }

    const auto raw = percentile_summary(raw_import_us);
    const auto raster = percentile_summary(raster_import_us);
    const auto cold = percentile_summary(cold_preview_us);
    const auto warm = percentile_summary(warm_preview_us);
    const auto pages = percentile_summary(page_us);
    std::cout << "photo_management_probe files=" << sources.size()
              << " enumeration_ms=" << enumeration_ms << " raw_import_us_p50_p90_max=" << raw[0]
              << ',' << raw[1] << ',' << raw[2] << " raster_import_us_p50_p90_max=" << raster[0]
              << ',' << raster[1] << ',' << raster[2] << " cold_preview_us_p50_p90_max=" << cold[0]
              << ',' << cold[1] << ',' << cold[2] << " warm_preview_us_p50_p90_max=" << warm[0]
              << ',' << warm[1] << ',' << warm[2] << " page_us_p50_p90_max=" << pages[0] << ','
              << pages[1] << ',' << pages[2] << '\n';
    if (const char *budget = std::getenv("RAVO_PRIVATE_PAGE_P90_BUDGET_US"))
        EXPECT_LE(pages[1], std::stoll(budget));
    if (const char *budget = std::getenv("RAVO_PRIVATE_COLD_PREVIEW_P90_BUDGET_MS"))
        EXPECT_LE(cold[1], std::stoll(budget) * 1000);
    if (const char *budget = std::getenv("RAVO_PRIVATE_WARM_PREVIEW_P90_BUDGET_MS"))
        EXPECT_LE(warm[1], std::stoll(budget) * 1000);
}

TEST_F(CatalogServiceTest, RemoveFromCatalogLeavesTheOriginalFile)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(400, 400, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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

TEST_F(CatalogServiceTest, RemoveFolderFromCatalogKeepsOriginalsAndRejectsAllPhotographs)
{
    ASSERT_TRUE(open_service(true));
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    const auto folder = root / "folder-remove-root";
    ASSERT_TRUE(std::filesystem::create_directory(folder));
    const auto first = folder / "one.png";
    const auto second = folder / "two.png";
    ASSERT_TRUE(image.save(QString::fromStdString(first.string()), "PNG"));
    image.fill(QColor(120, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(second.string()), "PNG"));
    auto imported = service->import_inputs({folder.string()}, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().size(), 2U);

    auto folders = service->list_folders();
    ASSERT_TRUE(folders) << folders.error().message;
    const auto found =
        std::find_if(folders.value().begin(), folders.value().end(), [](const FolderRecord &item)
                     { return item.display_name == "folder-remove-root"; });
    ASSERT_NE(found, folders.value().end());
    ASSERT_FALSE(found->uri.empty());

    auto rejected = service->remove_folder_from_catalog({});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kInvalidArgument);

    auto removed = service->remove_folder_from_catalog(found->uri);
    ASSERT_TRUE(removed) << removed.error().message;
    EXPECT_EQ(removed.value().asset_count, 2U);
    EXPECT_EQ(removed.value().folder_uri, found->uri);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    EXPECT_TRUE(std::filesystem::exists(first));
    EXPECT_TRUE(std::filesystem::exists(second));
    auto after_folders = service->list_folders();
    ASSERT_TRUE(after_folders) << after_folders.error().message;
    EXPECT_TRUE(std::none_of(after_folders.value().begin(), after_folders.value().end(),
                             [](const FolderRecord &item)
                             { return item.display_name == "folder-remove-root"; }));
}

TEST_F(CatalogServiceTest, RemoveOriginalAndCatalogDeletesTheFile)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(32, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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

TEST_F(CatalogServiceTest, RemoveTransactionFailurePreservesAssetAndRevision)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 50, 60));
    const auto photo = root / "remove-rollback.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before) << before.error().message;

    {
        const auto connection = QStringLiteral("ravo_remove_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_remove_revision BEFORE UPDATE OF revision ON schema_info "
            "BEGIN SELECT RAISE(ABORT, 'forced remove revision failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto removed = service->remove_from_catalog(asset_id);
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, ErrorCode::kIo);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, asset_id);
    auto after = service->snapshot();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().revision, before.value().revision);
    EXPECT_TRUE(std::filesystem::exists(photo));
}

TEST_F(CatalogServiceTest, DiskDeleteDatabaseFailureRestoresOriginalAndCatalog)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 80, 90));
    const auto photo = root / "disk-delete-rollback.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto original_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before) << before.error().message;

    {
        const auto connection = QStringLiteral("ravo_disk_delete_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_disk_delete_revision BEFORE UPDATE OF revision ON schema_info "
            "BEGIN SELECT RAISE(ABORT, 'forced disk delete revision failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto removed = service->remove_original_and_catalog(asset_id);
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, ErrorCode::kIo);
    EXPECT_TRUE(std::filesystem::exists(photo));
    EXPECT_EQ(file_sha256(photo.string()), original_hash);
    EXPECT_FALSE(std::filesystem::exists(photo.string() + ".ravo-delete-0"));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, asset_id);
    auto after = service->snapshot();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().revision, before.value().revision);
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(180, 40, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto rated = service->set_rating(asset_id, 3);
    ASSERT_TRUE(rated) << rated.error().message;

    DevelopParams params;
    params.demosaic_mode = std::string(kDemosaicModePpg);
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
    EXPECT_EQ(roundtrip.value().demosaic_mode, kDemosaicModePpg);
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

TEST_F(CatalogServiceTest, CanonicalMaskGraphSurvivesDevelopPreviewSaveAndCloseReopen)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "masked-develop.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 130, 180));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    const auto source_hash = file_sha256(jpeg_path);
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto develop = develop_from_recipe(baseline.value());
    ASSERT_TRUE(develop) << develop.error().message;
    ASSERT_TRUE(apply_develop_mask_field_strict(develop.value(), "graduatedMaskKind", 2.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(develop.value(), "graduatedMaskAnchorY", 0.3));
    ASSERT_TRUE(apply_develop_mask_field_strict(develop.value(), "graduatedMaskTransition", 0.2));
    ASSERT_TRUE(apply_develop_field_strict(develop.value(), "graduatedDensity", 0.75));
    ASSERT_TRUE(apply_develop_mask_field_strict(develop.value(), "colorHarmonizerMaskKind", 5.0));
    ASSERT_TRUE(apply_develop_mask_field_strict(develop.value(), "colorHarmonizerMaskSource", 1.0));
    ASSERT_TRUE(
        apply_develop_mask_field_strict(develop.value(), "colorHarmonizerMaskThreshold1", 0.25));
    ASSERT_TRUE(
        apply_develop_mask_field_strict(develop.value(), "colorHarmonizerMaskThreshold2", 0.75));
    const auto authored_masks = develop.value().masks;
    ASSERT_EQ(authored_masks.size(), 2U);
    EXPECT_EQ(authored_masks[0].kind, MaskKind::kLinearGradient);
    EXPECT_EQ(authored_masks[1].kind, MaskKind::kParametric);
    EXPECT_TRUE(develop.value().graduated_enabled);
    EXPECT_TRUE(develop.value().color_harmonizer_enabled);
    auto saved = service->save_develop(asset_id, develop.value());
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_TRUE(saved.value().has_edits);

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    const auto first_live = service->request_preview(preview, develop.value());
    ASSERT_TRUE(first_live) << first_live.error().message;

    auto ordinary_edit = develop.value();
    ASSERT_TRUE(apply_develop_field_strict(ordinary_edit, "exposure", 0.25));
    EXPECT_EQ(ordinary_edit.masks, authored_masks);
    EXPECT_EQ(ordinary_edit.graduated_mask_id, develop.value().graduated_mask_id);
    EXPECT_EQ(ordinary_edit.color_harmonizer_mask_id, develop.value().color_harmonizer_mask_id);
    const auto second_live = service->request_preview(preview, ordinary_edit);
    ASSERT_TRUE(second_live) << second_live.error().message;
    EXPECT_NE(first_live.value().cache_key, second_live.value().cache_key);
    ASSERT_TRUE(service->save_develop(asset_id, ordinary_edit));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto loaded = service->load_recipe(asset_id);
    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded.value().masks, authored_masks);
    const auto graduated =
        std::find_if(loaded.value().operations.begin(), loaded.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == "ravo.effect.graduatednd"; });
    ASSERT_NE(graduated, loaded.value().operations.end());
    EXPECT_EQ(graduated->mask_id, ordinary_edit.graduated_mask_id);
    const auto harmonizer =
        std::find_if(loaded.value().operations.begin(), loaded.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == kColorHarmonizerOperationId; });
    ASSERT_NE(harmonizer, loaded.value().operations.end());
    EXPECT_TRUE(harmonizer->enabled);
    EXPECT_EQ(harmonizer->mask_id, ordinary_edit.color_harmonizer_mask_id);
    auto reopened_preview = service->request_preview(preview);
    ASSERT_TRUE(reopened_preview) << reopened_preview.error().message;
    EXPECT_EQ(reopened_preview.value().cache_key, second_live.value().cache_key);
    EXPECT_EQ(file_sha256(jpeg_path), source_hash);

    auto reset = service->reset_recipe(asset_id);
    ASSERT_TRUE(reset) << reset.error().message;
    EXPECT_FALSE(reset.value().has_edits);
}

TEST_F(CatalogServiceTest, TagsMetadataAndHistoryPersistThroughReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "meta.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(12, 34, 56));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    auto tagged = service->set_tags(asset_id, {"landscape", "  landscape  ", "archive"});
    ASSERT_TRUE(tagged) << tagged.error().message;
    ASSERT_EQ(tagged.value().tags.size(), 2U);
    EXPECT_NE(std::find(tagged.value().tags.begin(), tagged.value().tags.end(), "landscape"),
              tagged.value().tags.end());
    EXPECT_NE(std::find(tagged.value().tags.begin(), tagged.value().tags.end(), "archive"),
              tagged.value().tags.end());

    WritableMetadata metadata;
    metadata.title = "Title";
    metadata.creator = "Ravo";
    auto written = service->set_writable_metadata(asset_id, metadata);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written.value().metadata.title, "Title");

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
    query.tag = "landscape";
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
    EXPECT_EQ(listed.value().front().metadata.title, "Title");
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

TEST_F(CatalogServiceTest, HierarchicalKeywordsSurviveReopenBackupAndRename)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto photo_a = root / "keyword-a.jpg";
    const auto photo_b = root / "keyword-b.jpg";
    for (const auto &photo : {photo_a, photo_b})
    {
        QImage image(24, 18, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40, 90, 130));
        ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    }
    auto imported_a = service->import_one(photo_a.string(), CancellationToken{});
    auto imported_b = service->import_one(photo_b.string(), CancellationToken{});
    ASSERT_TRUE(imported_a) << imported_a.error().message;
    ASSERT_TRUE(imported_b) << imported_b.error().message;
    ASSERT_TRUE(imported_a.value().asset);
    ASSERT_TRUE(imported_b.value().asset);
    const auto id_a = imported_a.value().asset->id;
    const auto id_b = imported_b.value().asset->id;

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);

    auto tagged = service->set_tags_selection({id_a, id_b}, {"Nature|Birds", "Archive"},
                                              snapshot.value().revision);
    ASSERT_TRUE(tagged) << tagged.error().message;
    ASSERT_EQ(tagged.value().assets.size(), 2U);
    for (const auto &asset : tagged.value().assets)
    {
        ASSERT_EQ(asset.tags.size(), 2U);
        EXPECT_NE(std::find(asset.tags.begin(), asset.tags.end(), "Nature|Birds"),
                  asset.tags.end());
        EXPECT_NE(std::find(asset.tags.begin(), asset.tags.end(), "Archive"), asset.tags.end());
    }

    auto stale = service->set_tags_selection({id_a}, {"Stale"}, snapshot.value().revision);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);

    auto keywords = service->list_keywords();
    ASSERT_TRUE(keywords) << keywords.error().message;
    ASSERT_GE(keywords.value().size(), 3U);
    const KeywordRecord *birds = nullptr;
    for (const auto &keyword : keywords.value())
    {
        if (keyword.path == "Nature|Birds")
            birds = &keyword;
    }
    ASSERT_NE(birds, nullptr);

    auto renamed = service->rename_keyword(birds->id, "Avian");
    ASSERT_TRUE(renamed) << renamed.error().message;
    EXPECT_EQ(renamed.value().keyword.path, "Nature|Avian");

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    for (const auto &asset : listed.value())
    {
        if (asset.id != id_a && asset.id != id_b)
            continue;
        ASSERT_EQ(asset.tags.size(), 2U);
        const bool has_archive =
            std::find(asset.tags.begin(), asset.tags.end(), "Archive") != asset.tags.end();
        const bool has_avian =
            std::find(asset.tags.begin(), asset.tags.end(), "Nature|Avian") != asset.tags.end();
        EXPECT_TRUE(has_archive);
        EXPECT_TRUE(has_avian);
    }

    LibraryQuery query;
    query.tag = "Nature|Avian";
    auto filtered = service->list_assets(query);
    ASSERT_TRUE(filtered) << filtered.error().message;
    EXPECT_EQ(filtered.value().size(), 2U);

    const auto backup_dir = root / "keyword-backup";
    auto backup = service->create_backup(backup_dir.string());
    ASSERT_TRUE(backup) << backup.error().message;

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto reopened_snapshot = service->snapshot();
    ASSERT_TRUE(reopened_snapshot) << reopened_snapshot.error().message;
    EXPECT_EQ(reopened_snapshot.value().schema_version, kCatalogSchemaVersion);
    auto reopened_keywords = service->list_keywords();
    ASSERT_TRUE(reopened_keywords) << reopened_keywords.error().message;
    bool found_avian = false;
    for (const auto &keyword : reopened_keywords.value())
        found_avian = found_avian || keyword.path == "Nature|Avian";
    EXPECT_TRUE(found_avian);
    auto reopened_assets = service->list_assets();
    ASSERT_TRUE(reopened_assets);
    std::size_t matched = 0;
    for (const auto &asset : reopened_assets.value())
    {
        if (asset.id == id_a || asset.id == id_b)
        {
            ++matched;
            EXPECT_NE(std::find(asset.tags.begin(), asset.tags.end(), "Nature|Avian"),
                      asset.tags.end());
        }
    }
    EXPECT_EQ(matched, 2U);
}

TEST_F(CatalogServiceTest, IptcCoreWritablePatchIsTransactionalAndSurvivesReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto photo_a = root / "iptc-a.jpg";
    const auto photo_b = root / "iptc-b.jpg";
    for (const auto &photo : {photo_a, photo_b})
    {
        QImage image(24, 18, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(50, 70, 90));
        ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    }
    auto imported_a = service->import_one(photo_a.string(), CancellationToken{});
    auto imported_b = service->import_one(photo_b.string(), CancellationToken{});
    ASSERT_TRUE(imported_a) << imported_a.error().message;
    ASSERT_TRUE(imported_b) << imported_b.error().message;
    ASSERT_TRUE(imported_a.value().asset);
    ASSERT_TRUE(imported_b.value().asset);
    const auto id_a = imported_a.value().asset->id;
    const auto id_b = imported_b.value().asset->id;

    WritableMetadata only_a;
    only_a.title = "Alpha";
    only_a.creator = "Alice";
    ASSERT_TRUE(service->set_writable_metadata(id_a, only_a));
    WritableMetadata only_b;
    only_b.title = "Beta";
    only_b.creator = "Bob";
    ASSERT_TRUE(service->set_writable_metadata(id_b, only_b));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto copyright_patch = writable_metadata_patch_for_field("copyright", std::string("© Ravo"));
    ASSERT_TRUE(copyright_patch) << copyright_patch.error().message;
    auto patched = service->set_writable_metadata_selection({id_a, id_b}, copyright_patch.value(),
                                                            snapshot.value().revision);
    ASSERT_TRUE(patched) << patched.error().message;
    ASSERT_EQ(patched.value().assets.size(), 2U);
    for (const auto &asset : patched.value().assets)
    {
        ASSERT_TRUE(asset.metadata.copyright);
        EXPECT_EQ(*asset.metadata.copyright, "© Ravo");
        if (asset.id == id_a)
        {
            ASSERT_TRUE(asset.metadata.title);
            EXPECT_EQ(*asset.metadata.title, "Alpha");
            ASSERT_TRUE(asset.metadata.creator);
            EXPECT_EQ(*asset.metadata.creator, "Alice");
        }
        else if (asset.id == id_b)
        {
            ASSERT_TRUE(asset.metadata.title);
            EXPECT_EQ(*asset.metadata.title, "Beta");
            ASSERT_TRUE(asset.metadata.creator);
            EXPECT_EQ(*asset.metadata.creator, "Bob");
        }
    }

    auto stale = service->set_writable_metadata_selection(
        {id_a}, writable_metadata_patch_for_field("title", std::string("Stale")).value(),
        snapshot.value().revision);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    for (const auto &asset : listed.value())
    {
        if (asset.id == id_a)
        {
            ASSERT_TRUE(asset.metadata.title);
            EXPECT_EQ(*asset.metadata.title, "Alpha");
        }
    }

    const auto backup_dir = root / "iptc-backup";
    auto backup = service->create_backup(backup_dir.string());
    ASSERT_TRUE(backup) << backup.error().message;

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto reopened_assets = service->list_assets();
    ASSERT_TRUE(reopened_assets);
    std::size_t matched = 0;
    for (const auto &asset : reopened_assets.value())
    {
        if (asset.id != id_a && asset.id != id_b)
            continue;
        ++matched;
        ASSERT_TRUE(asset.metadata.copyright);
        EXPECT_EQ(*asset.metadata.copyright, "© Ravo");
    }
    EXPECT_EQ(matched, 2U);
}

TEST_F(CatalogServiceTest, CatalogLocationWritablePatchIsTransactionalAndSurvivesReopen)
{
    ASSERT_TRUE(open_service(true));
    const auto photo_a = root / "loc-a.jpg";
    const auto photo_b = root / "loc-b.jpg";
    for (const auto &photo : {photo_a, photo_b})
    {
        QImage image(24, 18, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40, 60, 80));
        ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    }
    auto imported_a = service->import_one(photo_a.string(), CancellationToken{});
    auto imported_b = service->import_one(photo_b.string(), CancellationToken{});
    ASSERT_TRUE(imported_a) << imported_a.error().message;
    ASSERT_TRUE(imported_b) << imported_b.error().message;
    ASSERT_TRUE(imported_a.value().asset);
    ASSERT_TRUE(imported_b.value().asset);
    const auto id_a = imported_a.value().asset->id;
    const auto id_b = imported_b.value().asset->id;

    WritableMetadata only_a;
    only_a.city = "Shanghai";
    only_a.country = "China";
    ASSERT_TRUE(service->set_writable_metadata(id_a, only_a));
    WritableMetadata only_b;
    only_b.city = "Tokyo";
    only_b.country = "Japan";
    ASSERT_TRUE(service->set_writable_metadata(id_b, only_b));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto province_patch = writable_metadata_patch_for_field("province_state", std::string("Metro"));
    ASSERT_TRUE(province_patch) << province_patch.error().message;
    auto patched = service->set_writable_metadata_selection({id_a, id_b}, province_patch.value(),
                                                            snapshot.value().revision);
    ASSERT_TRUE(patched) << patched.error().message;
    ASSERT_EQ(patched.value().assets.size(), 2U);
    for (const auto &asset : patched.value().assets)
    {
        ASSERT_TRUE(asset.metadata.province_state);
        EXPECT_EQ(*asset.metadata.province_state, "Metro");
        if (asset.id == id_a)
        {
            ASSERT_TRUE(asset.metadata.city);
            EXPECT_EQ(*asset.metadata.city, "Shanghai");
            ASSERT_TRUE(asset.metadata.country);
            EXPECT_EQ(*asset.metadata.country, "China");
        }
        else if (asset.id == id_b)
        {
            ASSERT_TRUE(asset.metadata.city);
            EXPECT_EQ(*asset.metadata.city, "Tokyo");
            ASSERT_TRUE(asset.metadata.country);
            EXPECT_EQ(*asset.metadata.country, "Japan");
        }
    }

    auto stale = service->set_writable_metadata_selection(
        {id_a}, writable_metadata_patch_for_field("sublocation", std::string("Bund")).value(),
        snapshot.value().revision);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto reopened_assets = service->list_assets();
    ASSERT_TRUE(reopened_assets);
    std::size_t matched = 0;
    for (const auto &asset : reopened_assets.value())
    {
        if (asset.id != id_a && asset.id != id_b)
            continue;
        ++matched;
        ASSERT_TRUE(asset.metadata.province_state);
        EXPECT_EQ(*asset.metadata.province_state, "Metro");
        if (asset.id == id_a)
        {
            ASSERT_TRUE(asset.metadata.city);
            EXPECT_EQ(*asset.metadata.city, "Shanghai");
        }
    }
    EXPECT_EQ(matched, 2U);
    auto reopened_snapshot = service->snapshot();
    ASSERT_TRUE(reopened_snapshot);
    EXPECT_EQ(reopened_snapshot.value().schema_version, kCatalogSchemaVersion);
}

TEST_F(CatalogServiceTest, IptcExtensionWritablePatchSurvivesReopenAndRefresh)
{
    ASSERT_TRUE(open_service(true));
    const auto path_a = root / "iptc-ext.jpg";
    ASSERT_TRUE(
        QImage(32, 24, QImage::Format_RGB32).save(QString::fromStdString(path_a.string()), "JPG"));
    auto imported = service->import_one(path_a.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    WritableMetadata metadata;
    metadata.headline = "Press headline";
    metadata.credit = "Ravo Desk";
    metadata.source = "Wire";
    metadata.instructions = "Crop carefully";
    metadata.usage_terms = "Editorial only";
    metadata.job_id = "JOB-0140";
    metadata.title = "Keep title";
    auto written = service->set_writable_metadata(asset_id, metadata);
    ASSERT_TRUE(written) << written.error().message;
    EXPECT_EQ(written.value().metadata.headline, std::optional<std::string>{"Press headline"});
    EXPECT_EQ(written.value().metadata.job_id, std::optional<std::string>{"JOB-0140"});

    auto headline_patch = writable_metadata_patch_for_field("headline", std::string{"Updated HL"});
    ASSERT_TRUE(headline_patch) << headline_patch.error().message;
    auto patched =
        service->set_writable_metadata_selection({asset_id}, headline_patch.value(), std::nullopt);
    ASSERT_TRUE(patched) << patched.error().message;
    ASSERT_FALSE(patched.value().assets.empty());
    EXPECT_EQ(patched.value().assets.front().metadata.headline,
              std::optional<std::string>{"Updated HL"});
    EXPECT_EQ(patched.value().assets.front().metadata.credit,
              std::optional<std::string>{"Ravo Desk"});

    auto refreshed = service->refresh_capture_metadata(asset_id, CancellationToken{});
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    EXPECT_EQ(refreshed.value().metadata.headline, std::optional<std::string>{"Updated HL"});
    EXPECT_EQ(refreshed.value().metadata.usage_terms, std::optional<std::string>{"Editorial only"});
    EXPECT_EQ(refreshed.value().metadata.title, std::optional<std::string>{"Keep title"});

    ASSERT_TRUE(service->close());
    ASSERT_TRUE(open_service(false));
    auto reopened_assets = service->list_assets();
    ASSERT_TRUE(reopened_assets);
    ASSERT_EQ(reopened_assets.value().size(), 1U);
    EXPECT_EQ(reopened_assets.value().front().metadata.headline,
              std::optional<std::string>{"Updated HL"});
    EXPECT_EQ(reopened_assets.value().front().metadata.credit,
              std::optional<std::string>{"Ravo Desk"});
    EXPECT_EQ(reopened_assets.value().front().metadata.source, std::optional<std::string>{"Wire"});
    EXPECT_EQ(reopened_assets.value().front().metadata.instructions,
              std::optional<std::string>{"Crop carefully"});
    EXPECT_EQ(reopened_assets.value().front().metadata.usage_terms,
              std::optional<std::string>{"Editorial only"});
    EXPECT_EQ(reopened_assets.value().front().metadata.job_id,
              std::optional<std::string>{"JOB-0140"});
    auto reopened_snapshot = service->snapshot();
    ASSERT_TRUE(reopened_snapshot);
    EXPECT_EQ(reopened_snapshot.value().schema_version, kCatalogSchemaVersion);
}

} // namespace
} // namespace ravo
