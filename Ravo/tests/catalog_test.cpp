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
#include "ravo/recipe/mask.h"
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

TEST_F(CatalogServiceTest, NamedLibrarySetsPersistMembershipSmartQueryAndRejectStaleRevision)
{
    ASSERT_TRUE(open_service(true));
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 80, 140));
    const auto first_path = root / "one.jpg";
    const auto second_path = root / "two.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(first_path.string()), "JPEG", 90));
    image.fill(QColor(140, 80, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(second_path.string()), "JPEG", 90));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;
    ASSERT_TRUE(service->set_rating(first_id, 5));
    ASSERT_TRUE(service->set_rating(second_id, 2));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot);
    auto created = service->create_library_set(LibrarySetKind::kManual, " Job One ", std::nullopt,
                                               {first_id}, snapshot.value().revision);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value().set.name, "Job One");
    EXPECT_EQ(created.value().set.asset_count, 1U);

    auto stale = service->create_library_set(LibrarySetKind::kManual, "Stale", std::nullopt, {},
                                             snapshot.value().revision);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    EXPECT_EQ(stale.error().context.at("reason"), "stale_catalog_revision");

    LibraryQuery smart_query;
    smart_query.rating_mode = RatingFilterMode::kExact;
    smart_query.rating_value = 5;
    auto smart = service->create_library_set(LibrarySetKind::kSmart, "Five stars", smart_query, {});
    ASSERT_TRUE(smart) << smart.error().message;
    EXPECT_EQ(smart.value().set.asset_count, 1U);

    LibraryQuery listed_query;
    listed_query.collection_id = created.value().set.id;
    auto members = service->list_assets(listed_query);
    ASSERT_TRUE(members) << members.error().message;
    ASSERT_EQ(members.value().size(), 1U);
    EXPECT_EQ(members.value().front().id, first_id);

    ASSERT_TRUE(service->add_library_set_members(created.value().set.id, {second_id}));
    listed_query = {};
    listed_query.collection_id = created.value().set.id;
    members = service->list_assets(listed_query);
    ASSERT_TRUE(members);
    EXPECT_EQ(members.value().size(), 2U);

    listed_query = {};
    listed_query.collection_id = smart.value().set.id;
    auto smart_members = service->list_assets(listed_query);
    ASSERT_TRUE(smart_members) << smart_members.error().message;
    ASSERT_EQ(smart_members.value().size(), 1U);
    EXPECT_EQ(smart_members.value().front().id, first_id);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto reopened = service->list_library_sets();
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened.value().size(), 2U);
    EXPECT_EQ(reopened.value().front().name, "Five stars");
    EXPECT_EQ(reopened.value().back().name, "Job One");
    EXPECT_EQ(reopened.value().back().asset_count, 2U);

    const auto backup_path = root / "sets-backup";
    auto backup = service->create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery);
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "sets-restored.sqlite").string();
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = restored_path;
    auto restored = restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_repo = SqliteCatalogRepository::open(restored_path);
    ASSERT_TRUE(restored_repo) << restored_repo.error().message;
    auto restored_sets = restored_repo.value()->list_library_sets();
    ASSERT_TRUE(restored_sets) << restored_sets.error().message;
    ASSERT_EQ(restored_sets.value().size(), 2U);
}

TEST_F(CatalogServiceTest, AssetVersionsStacksAndCollapsedListing)
{
    ASSERT_TRUE(open_service(true));
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 80, 140));
    const auto first_path = root / "one.jpg";
    const auto second_path = root / "two.jpg";
    ASSERT_TRUE(image.save(QString::fromStdString(first_path.string()), "JPEG", 90));
    image.fill(QColor(140, 80, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(second_path.string()), "JPEG", 90));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;
    ASSERT_TRUE(service->set_rating(first_id, 4));
    DevelopParams edited;
    edited.exposure_ev = 0.75;
    ASSERT_TRUE(service->save_develop(first_id, edited));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot);
    auto stale = service->create_asset_version(first_id, snapshot.value().revision - 1);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);

    auto versioned = service->create_asset_version(first_id, snapshot.value().revision);
    ASSERT_TRUE(versioned) << versioned.error().message;
    EXPECT_EQ(versioned.value().version.version_ordinal, 1);
    ASSERT_TRUE(versioned.value().version.source_asset_id);
    EXPECT_EQ(*versioned.value().version.source_asset_id, first_id);
    EXPECT_EQ(versioned.value().version.normalized_uri, first.value().asset->normalized_uri);
    EXPECT_EQ(versioned.value().version.review.rating, 4);
    EXPECT_TRUE(versioned.value().version.has_edits);
    const auto version_id = versioned.value().version.id;

    auto by_uri = sqlite_repository->find_asset_by_uri(first.value().asset->normalized_uri);
    ASSERT_TRUE(by_uri) << by_uri.error().message;
    ASSERT_TRUE(by_uri.value());
    EXPECT_EQ(by_uri.value()->id, first_id);

    DevelopParams version_edit;
    version_edit.exposure_ev = -0.5;
    ASSERT_TRUE(service->save_develop(version_id, version_edit));
    auto primary_recipe = service->load_recipe(first_id);
    auto version_recipe = service->load_recipe(version_id);
    ASSERT_TRUE(primary_recipe) << primary_recipe.error().message;
    ASSERT_TRUE(version_recipe) << version_recipe.error().message;
    auto primary_develop = develop_from_recipe(primary_recipe.value());
    auto version_develop = develop_from_recipe(version_recipe.value());
    ASSERT_TRUE(primary_develop) << primary_develop.error().message;
    ASSERT_TRUE(version_develop) << version_develop.error().message;
    EXPECT_NEAR(primary_develop.value().exposure_ev, 0.75, 1e-6);
    EXPECT_NEAR(version_develop.value().exposure_ev, -0.5, 1e-6);

    const auto primary_export = (root / "primary.jpg").string();
    const auto version_export = (root / "version.jpg").string();
    ExportRequest primary_request;
    primary_request.asset_id = first_id;
    primary_request.output_path = primary_export;
    primary_request.format = ExportFormat::kJpeg;
    ExportRequest version_request;
    version_request.asset_id = version_id;
    version_request.output_path = version_export;
    version_request.format = ExportFormat::kJpeg;
    auto exported_primary = service->export_asset(primary_request);
    auto exported_version = service->export_asset(version_request);
    ASSERT_TRUE(exported_primary) << exported_primary.error().message;
    ASSERT_TRUE(exported_version) << exported_version.error().message;
    EXPECT_NE(file_sha256(primary_export), file_sha256(version_export));
    EXPECT_TRUE(std::filesystem::exists(first_path));

    auto all_versions = service->list_assets(LibraryQuery{}, false);
    ASSERT_TRUE(all_versions);
    EXPECT_EQ(all_versions.value().size(), 3U);

    snapshot = service->snapshot();
    ASSERT_TRUE(snapshot);
    auto stacked =
        service->stack_assets({first_id, second_id}, first_id, snapshot.value().revision);
    ASSERT_TRUE(stacked) << stacked.error().message;
    EXPECT_EQ(stacked.value().stack.member_ids.size(), 2U);
    EXPECT_EQ(stacked.value().stack.pick_asset_id, first_id);

    auto collapsed = service->list_assets();
    ASSERT_TRUE(collapsed) << collapsed.error().message;
    ASSERT_EQ(collapsed.value().size(), 2U);
    bool saw_pick = false;
    bool saw_version = false;
    for (const auto &asset : collapsed.value())
    {
        EXPECT_NE(asset.id, second_id);
        if (asset.id == first_id)
        {
            saw_pick = true;
            EXPECT_TRUE(asset.stack_pick);
            EXPECT_EQ(asset.stack_count, 2);
        }
        if (asset.id == version_id)
            saw_version = true;
    }
    EXPECT_TRUE(saw_pick);
    EXPECT_TRUE(saw_version);

    auto expanded = service->list_assets(LibraryQuery{}, false);
    ASSERT_TRUE(expanded);
    EXPECT_EQ(expanded.value().size(), 3U);

    auto forbidden = service->remove_original_and_catalog(version_id);
    ASSERT_FALSE(forbidden);
    EXPECT_EQ(forbidden.error().code, ErrorCode::kValidation);
    EXPECT_EQ(forbidden.error().context.at("reason"), "version_disk_delete_forbidden");
    EXPECT_TRUE(std::filesystem::exists(first_path));

    ASSERT_TRUE(service->remove_from_catalog(version_id));
    EXPECT_TRUE(std::filesystem::exists(first_path));
    collapsed = service->list_assets(LibraryQuery{}, false);
    ASSERT_TRUE(collapsed);
    EXPECT_EQ(collapsed.value().size(), 2U);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto reopened = service->list_assets(LibraryQuery{}, false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_EQ(reopened.value().size(), 2U);
    auto stacks = service->find_library_stack(stacked.value().stack.id);
    ASSERT_TRUE(stacks) << stacks.error().message;
    ASSERT_TRUE(stacks.value());
    EXPECT_EQ(stacks.value()->pick_asset_id, first_id);

    const auto backup_path = root / "version-backup";
    auto backup = service->create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery);
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "version-restored.sqlite").string();
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = restored_path;
    auto restored = restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_repo = SqliteCatalogRepository::open(restored_path);
    ASSERT_TRUE(restored_repo) << restored_repo.error().message;
    auto restored_stack = restored_repo.value()->find_library_stack(stacked.value().stack.id);
    ASSERT_TRUE(restored_stack);
    ASSERT_TRUE(restored_stack.value());
    EXPECT_EQ(restored_stack.value()->member_ids.size(), 2U);

    auto primary_again = service->create_asset_version(first_id);
    ASSERT_TRUE(primary_again);
    ASSERT_TRUE(service->remove_from_catalog(first_id));
    EXPECT_TRUE(std::filesystem::exists(first_path));
    auto remaining = service->list_assets(LibraryQuery{}, false);
    ASSERT_TRUE(remaining);
    ASSERT_EQ(remaining.value().size(), 1U);
    EXPECT_EQ(remaining.value().front().id, second_id);

    auto stacked_again = service->stack_assets({second_id}, second_id);
    ASSERT_FALSE(stacked_again);
}

TEST_F(CatalogServiceTest, SchemaV10MigratesToAssetVersionsAndStacks)
{
    {
        const auto connection = QStringLiteral("ravo_v10_versions");
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
            "CREATE TABLE preview (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "contract_version INTEGER NOT NULL, cache_key TEXT NOT NULL, width INTEGER, "
            "height INTEGER, state TEXT NOT NULL, cache_relpath TEXT, last_success_unix_ms INTEGER)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_recipe ("
                           "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
                           "  recipe_schema_version INTEGER NOT NULL,"
                           "  recipe_json TEXT NOT NULL,"
                           "  updated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_tag (asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, "
            "name TEXT NOT NULL, PRIMARY KEY (asset_id, name))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "title TEXT, description TEXT, creator TEXT, copyright TEXT, camera_make TEXT, "
            "camera_model TEXT, iso REAL, aperture REAL, focal_length_mm REAL, shutter_s REAL, "
            "captured_unix_s INTEGER, captured_local_exif TEXT, captured_subsecond_digits TEXT, "
            "captured_utc_offset_minutes INTEGER, gps_latitude_e6 INTEGER, gps_longitude_e6 INTEGER, "
            "gps_altitude_magnitude_mm INTEGER, gps_altitude_ref INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recipe_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, seq INTEGER NOT NULL, "
            "kind TEXT NOT NULL, label TEXT, recipe_json TEXT NOT NULL, created_unix_ms INTEGER NOT NULL, "
            "UNIQUE(asset_id, seq))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recovery_state (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "generation INTEGER NOT NULL, synchronized_generation INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE library_set (id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT NOT NULL UNIQUE, "
            "query_json TEXT, created_unix_ms INTEGER NOT NULL, updated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE library_set_member (set_id TEXT NOT NULL REFERENCES library_set(id) ON DELETE CASCADE, "
            "asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, added_unix_ms INTEGER NOT NULL, "
            "PRIMARY KEY (set_id, asset_id))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 10, 'cat_v10', 4, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO catalog_folder(id, uri, created_unix_ms) VALUES ('fld_one', 'file:///tmp', 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, display_name, folder_uri, folder_id, media_type, "
            "size_bytes, mtime_unix_ms, import_state, created_unix_ms) VALUES "
            "('ast_old', 'file:///tmp/old.jpg', 'old.jpg', 'file:///tmp', 'fld_one', 'image/jpeg', "
            "12, 1, 'imported', 1)")));
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
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, "ast_old");
    EXPECT_EQ(listed.value().front().version_ordinal, 0);
    EXPECT_FALSE(listed.value().front().source_asset_id);
    auto versioned = service->create_asset_version("ast_old");
    ASSERT_TRUE(versioned) << versioned.error().message;
    ASSERT_TRUE(versioned.value().version.source_asset_id);
    EXPECT_EQ(*versioned.value().version.source_asset_id, "ast_old");
}

TEST(CatalogSchemaMigrationTest, FolderIdentityMigrationRollsBackAndThenAssignsStableIds)
{
    const auto root = make_catalog_test_temp_root();
    const auto path = root / "folder-v8.sqlite";
    const auto connection = QStringLiteral("ravo_folder_v8_fixture");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(path.string()));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE schema_info(id INTEGER PRIMARY KEY, schema_version INTEGER NOT NULL, "
            "catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, created_unix_ms INTEGER NOT "
            "NULL, migrated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset(id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "display_name TEXT NOT NULL, folder_uri TEXT NOT NULL, media_type TEXT NOT NULL, "
            "size_bytes INTEGER NOT NULL, mtime_unix_ms INTEGER NOT NULL, content_fingerprint "
            "TEXT, width INTEGER, height INTEGER, import_state TEXT NOT NULL, error_code TEXT, "
            "error_message TEXT, created_unix_ms INTEGER NOT NULL, rating INTEGER NOT NULL, "
            "color_label TEXT NOT NULL, rejected INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata(asset_id TEXT PRIMARY KEY, captured_local_exif TEXT, "
            "captured_subsecond_digits TEXT, captured_utc_offset_minutes INTEGER, "
            "gps_latitude_e6 INTEGER, gps_longitude_e6 INTEGER, "
            "gps_altitude_magnitude_mm INTEGER, gps_altitude_ref INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE catalog_backup_policy(id INTEGER PRIMARY KEY, enabled INTEGER NOT NULL, "
            "destination_directory TEXT NOT NULL, interval_minutes INTEGER NOT NULL, "
            "retention_count INTEGER NOT NULL, last_success_unix_ms INTEGER, "
            "next_run_unix_ms INTEGER, last_backup_bytes INTEGER NOT NULL, last_error TEXT)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("INSERT INTO schema_info VALUES (1, 8, 'cat_folder_v8', 4, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO catalog_backup_policy VALUES (1, 0, '', 1440, 7, NULL, NULL, 0, NULL)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("INSERT INTO asset VALUES ('ast_folder_v8', "
                           "'file:///missing-root/photo.jpg', 'photo.jpg', 'file:///missing-root', "
                           "'image/jpeg', 123, 456, '123-456', 8, 8, 'imported', NULL, NULL, 2, 0, "
                           "'none', 0)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER reject_folder_migration BEFORE UPDATE OF schema_version ON "
            "schema_info BEGIN SELECT RAISE(ABORT, 'injected folder migration failure'); END")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto failed = SqliteCatalogRepository::open(path.string());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    {
        const auto inspect_connection = QStringLiteral("ravo_folder_v8_inspect");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), inspect_connection);
        database.setDatabaseName(QString::fromStdString(path.string()));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA table_info(asset)")));
        bool has_folder_id = false;
        while (query.next())
            has_folder_id =
                has_folder_id || query.value(1).toString() == QStringLiteral("folder_id");
        EXPECT_FALSE(has_folder_id);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='catalog_folder'")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toInt(), 0);
        ASSERT_TRUE(
            query.exec(QStringLiteral("SELECT schema_version FROM schema_info WHERE id=1")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toInt(), 8);
        ASSERT_TRUE(query.exec(QStringLiteral("DROP TRIGGER reject_folder_migration")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(inspect_connection);
    }

    auto migrated = SqliteCatalogRepository::open(path.string());
    ASSERT_TRUE(migrated) << migrated.error().message << " action="
                          << (migrated.error().context.contains("action") ?
                                  migrated.error().context.at("action") :
                                  "")
                          << " qt="
                          << (migrated.error().context.contains("qt_error") ?
                                  migrated.error().context.at("qt_error") :
                                  "");
    auto snapshot = migrated.value()->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    auto folders = migrated.value()->list_folders();
    ASSERT_TRUE(folders) << folders.error().message;
    const auto folder =
        std::find_if(folders.value().begin(), folders.value().end(),
                     [](const FolderRecord &item) { return item.uri == "file:///missing-root"; });
    ASSERT_NE(folder, folders.value().end());
    EXPECT_TRUE(folder->id.starts_with("fld_"));
    EXPECT_EQ(folder->asset_count, 1);
    const auto stable_id = folder->id;
    ASSERT_TRUE(migrated.value()->close());
    migrated = SqliteCatalogRepository::open(path.string());
    ASSERT_TRUE(migrated) << migrated.error().message;
    auto reopened = migrated.value()->find_folder_by_id(stable_id);
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_TRUE(reopened.value());
    EXPECT_EQ(reopened.value()->uri, "file:///missing-root");
    ASSERT_TRUE(migrated.value()->close());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CatalogServiceTest, RecoverySidecarTracksDurableStateAndRejectsTampering)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "recovery.jpg";
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 80, 140));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());

    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    EXPECT_FALSE(imported.value().error);
    const auto asset_id = imported.value().asset->id;
    auto initial = service->recovery_state(asset_id);
    ASSERT_TRUE(initial) << initial.error().message;
    EXPECT_FALSE(initial.value().pending());
    EXPECT_GT(initial.value().generation, 0);
    const auto recovery_root =
        std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(database_path));
    const auto initial_path =
        recovery_root /
        (asset_id + "." + std::to_string(initial.value().generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::is_regular_file(initial_path));

    auto rated = service->set_rating(asset_id, 4);
    ASSERT_TRUE(rated) << rated.error().message;
    auto current = service->recovery_state(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_GT(current.value().generation, initial.value().generation);
    EXPECT_EQ(current.value().synchronized_generation, current.value().generation);
    EXPECT_FALSE(std::filesystem::exists(initial_path));
    auto current_path = recovery_root / (asset_id + "." +
                                         std::to_string(current.value().generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::is_regular_file(current_path));

    DevelopParams develop;
    develop.exposure_ev = 0.25;
    RecipeSaveOptions deferred_options;
    deferred_options.defer_recovery_publication = true;
    auto deferred = service->save_develop(asset_id, develop, deferred_options);
    ASSERT_TRUE(deferred) << deferred.error().message;
    auto pending = service->recovery_state(asset_id);
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_TRUE(pending.value().pending());
    EXPECT_TRUE(std::filesystem::is_regular_file(current_path));

    // Publication and acknowledgement are separate crash-safe steps. An
    // unrelated asset may advance the global catalog revision between them;
    // the pending asset generation must remain byte-stable and retryable.
    ASSERT_NE(sqlite_repository, nullptr);
    auto pending_snapshot = sqlite_repository->load_recovery_snapshot(asset_id);
    ASSERT_TRUE(pending_snapshot) << pending_snapshot.error().message;
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    ASSERT_TRUE(recovery) << recovery.error().message;
    auto published = recovery.value()->publish(pending_snapshot.value(), CancellationToken{});
    ASSERT_TRUE(published) << published.error().message;
    const auto other_photo = root / "unrelated-revision.jpg";
    image.fill(QColor(140, 80, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(other_photo.string()), "JPEG", 90));
    auto unrelated = service->import_one(other_photo.string(), CancellationToken{});
    ASSERT_TRUE(unrelated) << unrelated.error().message;
    auto retry_snapshot = sqlite_repository->load_recovery_snapshot(asset_id);
    ASSERT_TRUE(retry_snapshot) << retry_snapshot.error().message;
    EXPECT_EQ(retry_snapshot.value().state.generation, pending_snapshot.value().state.generation);
    EXPECT_GT(retry_snapshot.value().catalog_revision, pending_snapshot.value().catalog_revision);
    auto drained = service->sync_recovery(std::string_view{asset_id});
    ASSERT_TRUE(drained) << drained.error().message;
    ASSERT_EQ(drained.value().artifacts.size(), 1U);
    current = service->recovery_state(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_FALSE(current.value().pending());
    current_path = recovery_root /
                   (asset_id + "." + std::to_string(current.value().generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::is_regular_file(current_path));
    EXPECT_EQ(drained.value().artifacts.front().sha256,
              file_sha256(current_path.string()).toHex().toStdString());

    QFile sidecar(QString::fromStdString(current_path.string()));
    ASSERT_TRUE(sidecar.open(QIODevice::Append));
    ASSERT_EQ(sidecar.write("tamper", 6), 6);
    sidecar.close();
    auto verified = service->sync_recovery(std::string_view{asset_id});
    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().code, ErrorCode::kValidation);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, PendingRecoveryRetriesAfterRestartWithoutLosingCommittedEdit)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "pending-recovery.jpg";
    QImage image(20, 20, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 30, 10));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto recovery_root =
        std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(database_path));
    const auto parked = root / "parked-sidecars";
    std::filesystem::rename(recovery_root, parked);
    {
        std::ofstream blocker(recovery_root, std::ios::binary);
        ASSERT_TRUE(blocker);
        blocker << "blocked";
    }
    auto rated = service->set_rating(asset_id, 5);
    ASSERT_FALSE(rated);
    EXPECT_EQ(rated.error().context.at("catalog_committed"), "true");
    EXPECT_EQ(rated.error().context.at("recovery_pending"), "true");
    auto pending = service->recovery_state(asset_id);
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_TRUE(pending.value().pending());

    service.reset();
    sqlite_repository = nullptr;
    std::filesystem::remove(recovery_root);
    std::filesystem::rename(parked, recovery_root);
    ASSERT_TRUE(open_service(false));
    auto resumed = service->recovery_state(asset_id);
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_FALSE(resumed.value().pending());
    auto reopened = service->list_assets();
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened.value().size(), 1U);
    EXPECT_EQ(reopened.value().front().review.rating, 5);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, RecoverySidecarStrictlyRejectsChecksumValidMalformedNestedState)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "nested-recovery.jpg";
    QImage image(24, 18, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 90, 150));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->set_tags(asset_id, {"alpha", "omega"}));
    DevelopParams develop;
    develop.exposure_ev = 0.5;
    ASSERT_TRUE(service->save_develop(asset_id, develop));
    auto state = service->recovery_state(asset_id);
    ASSERT_TRUE(state) << state.error().message;
    const auto recovery_root =
        std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(database_path));
    const auto source_sidecar =
        recovery_root / (asset_id + "." + std::to_string(state.value().generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::is_regular_file(source_sidecar));
    const auto sidecar_hash = file_sha256(source_sidecar.string());
    auto recovery = FilesystemRecoveryStore::open_existing(recovery_root.string());
    ASSERT_TRUE(recovery) << recovery.error().message;

    using Mutation = std::pair<std::string, std::function<void(JsonValue::Object &)>>;
    const std::vector<Mutation> mutations{
        {"unknown-asset-field",
         [](JsonValue::Object &payload)
         {
             auto asset = *payload.at("asset").object_if();
             asset.emplace("future", true);
             payload.insert_or_assign("asset", JsonValue{std::move(asset)});
         }},
        {"invalid-rating",
         [](JsonValue::Object &payload)
         {
             auto asset = *payload.at("asset").object_if();
             asset.insert_or_assign("rating", JsonValue::number("9"));
             payload.insert_or_assign("asset", JsonValue{std::move(asset)});
         }},
        {"noncanonical-tags",
         [](JsonValue::Object &payload)
         {
             auto asset = *payload.at("asset").object_if();
             asset.insert_or_assign("tags",
                                    JsonValue::Array{JsonValue{"omega"}, JsonValue{"alpha"}});
             payload.insert_or_assign("asset", JsonValue{std::move(asset)});
         }},
        {"invalid-capture",
         [](JsonValue::Object &payload)
         {
             auto asset = *payload.at("asset").object_if();
             auto capture = *asset.at("capture").object_if();
             capture.insert_or_assign(
                 "location", JsonValue::Object{{"altitude", nullptr},
                                               {"latitude_e6", JsonValue::number("90000001")},
                                               {"longitude_e6", JsonValue::number("0")}});
             asset.insert_or_assign("capture", JsonValue{std::move(capture)});
             payload.insert_or_assign("asset", JsonValue{std::move(asset)});
         }},
        {"invalid-capture-number",
         [](JsonValue::Object &payload)
         {
             auto asset = *payload.at("asset").object_if();
             auto capture = *asset.at("capture").object_if();
             capture.insert_or_assign("iso", JsonValue::number("1e309"));
             asset.insert_or_assign("capture", JsonValue{std::move(capture)});
             payload.insert_or_assign("asset", JsonValue{std::move(asset)});
         }},
        {"invalid-history-order",
         [](JsonValue::Object &payload)
         {
             auto history = *payload.at("history").array_if();
             ASSERT_FALSE(history.empty());
             auto entry = *history.front().object_if();
             entry.insert_or_assign("seq", JsonValue::number("0"));
             history.front() = JsonValue{std::move(entry)};
             payload.insert_or_assign("history", JsonValue{std::move(history)});
         }},
        {"recipe-state-mismatch",
         [](JsonValue::Object &payload)
         {
             auto asset = *payload.at("asset").object_if();
             asset.insert_or_assign("has_edits", false);
             payload.insert_or_assign("asset", JsonValue{std::move(asset)});
         }},
    };
    for (const auto &[name, mutate] : mutations)
    {
        const auto output = root / (name + ".ravo.json");
        const auto document = recovery_document_with_mutated_payload(source_sidecar, mutate);
        ASSERT_FALSE(document.empty());
        QFile file(QString::fromStdString(output.string()));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(document.data(), static_cast<qint64>(document.size())),
                  static_cast<qint64>(document.size()));
        file.close();
        auto verified = recovery.value()->verify_artifact(
            output.string(), asset_id, state.value().generation, CancellationToken{});
        EXPECT_FALSE(verified) << name;
        if (!verified)
            EXPECT_EQ(verified.error().code, ErrorCode::kValidation) << name;
    }

    const auto valid_number_output = root / "valid-capture-number.ravo.json";
    const auto valid_number_document = recovery_document_with_mutated_payload(
        source_sidecar,
        [](JsonValue::Object &payload)
        {
            auto asset = *payload.at("asset").object_if();
            auto capture = *asset.at("capture").object_if();
            capture.insert_or_assign("iso", JsonValue::number("1.25e2"));
            asset.insert_or_assign("capture", JsonValue{std::move(capture)});
            payload.insert_or_assign("asset", JsonValue{std::move(asset)});
        });
    ASSERT_FALSE(valid_number_document.empty());
    QFile valid_number_file(QString::fromStdString(valid_number_output.string()));
    ASSERT_TRUE(valid_number_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(valid_number_file.write(valid_number_document.data(),
                                      static_cast<qint64>(valid_number_document.size())),
              static_cast<qint64>(valid_number_document.size()));
    valid_number_file.close();
    auto valid_number_verified = recovery.value()->verify_artifact(
        valid_number_output.string(), asset_id, state.value().generation, CancellationToken{});
    EXPECT_TRUE(valid_number_verified) << valid_number_verified.error().message;
    EXPECT_EQ(file_sha256(source_sidecar.string()), sidecar_hash);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, RecoveryAcknowledgementFailureKeepsExactPublishedGenerationPending)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "acknowledgement-recovery.jpg";
    QImage image(18, 14, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 110, 150));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams develop;
    develop.exposure_ev = 0.4;
    RecipeSaveOptions options;
    options.defer_recovery_publication = true;
    ASSERT_TRUE(service->save_develop(asset_id, develop, options));
    auto before = service->recovery_state(asset_id);
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_TRUE(before.value().pending());
    ASSERT_NE(sqlite_repository, nullptr);
    testing::SqliteCatalogTestControl::inject_recovery(
        *sqlite_repository, testing::SqliteRecoveryFailure::kAcknowledge);

    auto failed = service->sync_recovery(std::string_view{asset_id});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    EXPECT_EQ(failed.error().context.at("sidecar_published"), "true");
    const auto published_path = failed.error().context.at("sidecar_path");
    EXPECT_TRUE(std::filesystem::is_regular_file(published_path));
    auto pending = service->recovery_state(asset_id);
    ASSERT_TRUE(pending) << pending.error().message;
    EXPECT_EQ(pending.value(), before.value());
    EXPECT_TRUE(pending.value().pending());

    auto retried = service->sync_recovery(std::string_view{asset_id});
    ASSERT_TRUE(retried) << retried.error().message;
    ASSERT_EQ(retried.value().artifacts.size(), 1U);
    EXPECT_EQ(retried.value().artifacts.front().path, published_path);
    auto synchronized = service->recovery_state(asset_id);
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    EXPECT_EQ(synchronized.value().generation, before.value().generation);
    EXPECT_FALSE(synchronized.value().pending());
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, RecoveryCleanupFailureKeepsAcknowledgedGenerationAndReportsOrphan)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "cleanup-recovery.jpg";
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 120, 160));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto initial = service->recovery_state(asset_id);
    ASSERT_TRUE(initial) << initial.error().message;
    const auto recovery_root =
        std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(database_path));
    const auto obsolete =
        recovery_root /
        (asset_id + "." + std::to_string(initial.value().generation) + ".ravo.json");
    ASSERT_TRUE(std::filesystem::remove(obsolete));
    ASSERT_TRUE(std::filesystem::create_directory(obsolete));
    {
        std::ofstream blocker(obsolete / "owned-by-test", std::ios::binary);
        ASSERT_TRUE(blocker);
        blocker << "block cleanup";
    }

    DevelopParams develop;
    develop.exposure_ev = 0.3;
    RecipeSaveOptions options;
    options.defer_recovery_publication = true;
    ASSERT_TRUE(service->save_develop(asset_id, develop, options));
    auto pending = service->recovery_state(asset_id);
    ASSERT_TRUE(pending) << pending.error().message;
    ASSERT_TRUE(pending.value().pending());
    auto failed = service->sync_recovery(std::string_view{asset_id});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    EXPECT_EQ(failed.error().context.at("reason"), "recovery_cleanup_failed");
    EXPECT_EQ(failed.error().context.at("recovery_acknowledged"), "true");
    EXPECT_EQ(failed.error().context.at("sidecar_published"), "true");
    EXPECT_TRUE(std::filesystem::is_directory(obsolete));

    auto synchronized = service->recovery_state(asset_id);
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    EXPECT_EQ(synchronized.value().generation, pending.value().generation);
    EXPECT_FALSE(synchronized.value().pending());
    EXPECT_TRUE(std::filesystem::is_regular_file(failed.error().context.at("sidecar_path")));
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST(RecoveryPublicationInternalTest, EveryFailureAndCancellationCheckpointCleansOwnedTemporary)
{
    const auto root = make_catalog_test_temp_root();
    const std::string document(130U * 1024U, 'r');
    const std::array checkpoints{
        recovery_publication_internal::Checkpoint::kBeforeTemporaryOpen,
        recovery_publication_internal::Checkpoint::kTemporaryCreated,
        recovery_publication_internal::Checkpoint::kBeforeTemporaryWrite,
        recovery_publication_internal::Checkpoint::kTemporaryChunkWritten,
        recovery_publication_internal::Checkpoint::kBeforeTemporarySync,
        recovery_publication_internal::Checkpoint::kBeforePublish,
    };
    std::size_t index = 0U;
    for (const auto checkpoint : checkpoints)
    {
        const auto output = root / ("failure-" + std::to_string(index++) + ".ravo.json");
        RecoveryPublicationHookState state;
        state.target = checkpoint;
        state.injected_error = std::make_error_code(std::errc::io_error);
        auto result = recovery_publication_internal::publish_no_replace(
            output.string(), document, CancellationToken{}, {recovery_publication_hook, &state});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::kIo);
        EXPECT_FALSE(std::filesystem::exists(output));
        for (const auto &observed : state.observed_paths)
        {
            if (observed != output.string())
                EXPECT_FALSE(std::filesystem::exists(observed));
        }
    }

    index = 0U;
    for (const auto checkpoint : checkpoints)
    {
        const auto output = root / ("cancel-" + std::to_string(index++) + ".ravo.json");
        CancellationSource cancellation;
        RecoveryPublicationHookState state;
        state.target = checkpoint;
        state.cancellation = &cancellation;
        auto result = recovery_publication_internal::publish_no_replace(
            output.string(), document, cancellation.token(), {recovery_publication_hook, &state});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
        EXPECT_FALSE(std::filesystem::exists(output));
        for (const auto &observed : state.observed_paths)
        {
            if (observed != output.string())
                EXPECT_FALSE(std::filesystem::exists(observed));
        }
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(RecoveryPublicationInternalTest, ReleasesTemporaryFileOwnershipBeforePublish)
{
    const auto root = make_catalog_test_temp_root();
    const auto output = root / "ownership.ravo.json";
    RecoveryPublicationHookState state;
    state.target = recovery_publication_internal::Checkpoint::kBeforePublish;
    state.probe_temporary_rename = true;
    auto result = recovery_publication_internal::publish_no_replace(
        output.string(), "candidate", CancellationToken{}, {recovery_publication_hook, &state});
    ASSERT_TRUE(result) << result.error().message;
    QFile published(QString::fromStdString(output.string()));
    ASSERT_TRUE(published.open(QIODevice::ReadOnly));
    EXPECT_EQ(published.readAll(), QByteArray("candidate"));
    published.close();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(RecoveryPublicationInternalTest, LateCompetitorWinsWithoutReplacement)
{
    const auto root = make_catalog_test_temp_root();
    const auto output = root / "winner.ravo.json";
    RecoveryPublicationHookState state;
    state.target = recovery_publication_internal::Checkpoint::kBeforePublish;
    state.competitor_output = output.string();
    auto result = recovery_publication_internal::publish_no_replace(
        output.string(), "candidate", CancellationToken{}, {recovery_publication_hook, &state});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kConflict);
    QFile winner(QString::fromStdString(output.string()));
    ASSERT_TRUE(winner.open(QIODevice::ReadOnly));
    EXPECT_EQ(winner.readAll(), QByteArray("winner"));
    for (const auto &observed : state.observed_paths)
    {
        if (observed != output.string())
            EXPECT_FALSE(std::filesystem::exists(observed));
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CatalogServiceTest, CatalogBackupIsImmutableVerifiedAndExcludesPreviewArtifacts)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "backup-source.jpg";
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(14, 70, 120));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    ASSERT_TRUE(service->set_tags(imported.value().asset->id, {"backup", "verified"}));

    const auto destination = root / "catalog-backup";
    auto backup = service->create_backup(destination.string());
    ASSERT_TRUE(backup) << backup.error().message;
    EXPECT_EQ(backup.value().sidecar_count, 1U);
    EXPECT_GT(backup.value().catalog.bytes, 0U);
    EXPECT_GT(backup.value().sidecar_bytes, 0U);
    EXPECT_TRUE(std::filesystem::is_regular_file(destination / "catalog.sqlite"));
    EXPECT_TRUE(std::filesystem::is_regular_file(destination / "manifest.json"));
    EXPECT_TRUE(std::filesystem::is_directory(destination / "sidecars"));
    EXPECT_FALSE(std::filesystem::exists(destination / "preview"));
    EXPECT_FALSE(std::filesystem::exists(destination / "originals"));

    auto verified = service->verify_backup(destination.string());
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_FALSE(verified.value().originals_included);
    EXPECT_FALSE(verified.value().previews_included);
    EXPECT_EQ(verified.value().artifact.catalog.sha256, backup.value().catalog.sha256);
    const auto manifest_hash = file_sha256((destination / "manifest.json").string());
    auto conflict = service->create_backup(destination.string());
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_EQ(file_sha256((destination / "manifest.json").string()), manifest_hash);

    const auto sidecar = *std::filesystem::directory_iterator(destination / "sidecars");
    QFile tampered(QString::fromStdString(sidecar.path().string()));
    ASSERT_TRUE(tampered.open(QIODevice::Append));
    ASSERT_EQ(tampered.write("x", 1), 1);
    tampered.close();
    auto rejected = service->verify_backup(destination.string());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, CatalogBackupFailureAndCancellationMatrixPublishesNothing)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "backup-failure-source.jpg";
    QImage image(20, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 100, 160));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const std::vector<std::string> checkpoints{
        "before_snapshot",           "stage_created",    "sidecar_directory_created",
        "database_snapshot_created", "sidecar_copied",   "source_rechecked",
        "manifest_published",        "staging_verified", "before_publish",
    };
    const auto assert_no_stage = [&](const std::filesystem::path &destination)
    {
        const auto prefix = destination.filename().string() + ".ravo-catalog-backup-";
        for (const auto &entry : std::filesystem::directory_iterator(root))
            EXPECT_FALSE(entry.path().filename().string().starts_with(prefix));
    };

    std::size_t index = 0U;
    for (const auto &target : checkpoints)
    {
        const auto destination = root / ("failure-backup-" + std::to_string(index++));
        testing::CatalogServiceTestControl::set_backup_checkpoint(
            *service,
            [target](const std::string_view checkpoint, const std::string_view) -> Result<void>
            {
                if (checkpoint == target)
                    return make_error(ErrorCode::kIo, "Injected backup failure",
                                      {{"reason", "injected_backup_failure"}});
                return {};
            });
        auto backup = service->create_backup(destination.string(), CancellationToken{});
        ASSERT_FALSE(backup) << target;
        EXPECT_EQ(backup.error().code, ErrorCode::kIo) << target;
        EXPECT_EQ(backup.error().context.at("checkpoint"), target);
        EXPECT_FALSE(std::filesystem::exists(destination));
        assert_no_stage(destination);
    }

    index = 0U;
    for (const auto &target : checkpoints)
    {
        const auto destination = root / ("cancel-backup-" + std::to_string(index++));
        CancellationSource cancellation;
        testing::CatalogServiceTestControl::set_backup_checkpoint(
            *service,
            [target, &cancellation](const std::string_view checkpoint,
                                    const std::string_view) -> Result<void>
            {
                if (checkpoint == target)
                    static_cast<void>(cancellation.cancel("backup-checkpoint-test"));
                return {};
            });
        auto backup = service->create_backup(destination.string(), cancellation.token());
        ASSERT_FALSE(backup) << target;
        EXPECT_EQ(backup.error().code, ErrorCode::kCancelled) << target;
        EXPECT_EQ(backup.error().context.at("checkpoint"), target);
        EXPECT_FALSE(std::filesystem::exists(destination));
        assert_no_stage(destination);
    }
    testing::CatalogServiceTestControl::set_backup_checkpoint(*service, {});
    auto state = service->recovery_state(imported.value().asset->id);
    ASSERT_TRUE(state) << state.error().message;
    EXPECT_FALSE(state.value().pending());
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, ScheduledBackupsPersistPolicyAndRetainOnlyVerifiedOwnedArtifacts)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "scheduled-backup-source.jpg";
    QImage image(20, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(65, 105, 145));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto destination = root / "scheduled-backups";
    ASSERT_TRUE(std::filesystem::create_directory(destination));
    constexpr std::int64_t start = 1'000'000;
    CatalogBackupPolicy policy;
    policy.enabled = true;
    policy.destination_directory = destination.string();
    policy.interval_minutes = kBackupScheduleIntervalMinutesMin;
    policy.retention_count = 2;
    auto configured = service->set_backup_policy(policy, start);
    ASSERT_TRUE(configured) << configured.error().message;
    ASSERT_TRUE(configured.value().next_run_unix_ms);
    const auto first_due = *configured.value().next_run_unix_ms;
    auto early = service->run_scheduled_backup(first_due - 1, CancellationToken{});
    ASSERT_TRUE(early) << early.error().message;
    EXPECT_FALSE(early.value().ran);

    auto first = service->run_scheduled_backup(first_due, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(first.value().backup);
    const auto first_path = first.value().backup->path;
    EXPECT_TRUE(std::filesystem::is_directory(first_path));
    auto second =
        service->run_scheduled_backup(*first.value().policy.next_run_unix_ms, CancellationToken{});
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(second.value().backup);
    EXPECT_TRUE(std::filesystem::is_directory(second.value().backup->path));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    const auto unverified =
        destination / ("ravo-" + snapshot.value().catalog_id + "-1999999.ravobackup");
    ASSERT_TRUE(std::filesystem::create_directory(unverified));
    {
        std::ofstream sentinel(unverified / "user-content", std::ios::binary);
        ASSERT_TRUE(sentinel);
        sentinel << "never delete";
    }
    auto third =
        service->run_scheduled_backup(*second.value().policy.next_run_unix_ms, CancellationToken{});
    ASSERT_TRUE(third) << third.error().message;
    ASSERT_TRUE(third.value().backup);
    ASSERT_EQ(third.value().removed_backups.size(), 1U);
    EXPECT_EQ(third.value().removed_backups.front(), first_path);
    EXPECT_FALSE(std::filesystem::exists(first_path));
    EXPECT_TRUE(std::filesystem::is_directory(second.value().backup->path));
    EXPECT_TRUE(std::filesystem::is_directory(third.value().backup->path));
    EXPECT_TRUE(std::filesystem::is_regular_file(unverified / "user-content"));
    const auto unverified_utf8 = unverified.generic_u8string();
    const std::string unverified_path(unverified_utf8.begin(), unverified_utf8.end());
    EXPECT_NE(std::find(third.value().retained_unverified_paths.begin(),
                        third.value().retained_unverified_paths.end(), unverified_path),
              third.value().retained_unverified_paths.end());
    EXPECT_GT(third.value().policy.last_backup_bytes, 0U);
    EXPECT_FALSE(third.value().policy.last_error);

    auto persisted = service->backup_policy();
    ASSERT_TRUE(persisted) << persisted.error().message;
    EXPECT_EQ(persisted.value().destination_directory, destination.string());
    EXPECT_EQ(persisted.value().retention_count, 2);
    EXPECT_EQ(persisted.value().last_success_unix_ms, third.value().policy.last_success_unix_ms);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
    service.reset();
    sqlite_repository = nullptr;
    ASSERT_TRUE(open_service(false));
    auto reopened_policy = service->backup_policy();
    ASSERT_TRUE(reopened_policy) << reopened_policy.error().message;
    EXPECT_TRUE(reopened_policy.value().enabled);
    EXPECT_EQ(reopened_policy.value().destination_directory, destination.string());
    EXPECT_EQ(reopened_policy.value().last_success_unix_ms,
              third.value().policy.last_success_unix_ms);
}

} // namespace

TEST_F(CatalogServiceTest, VirtualCopyPreservesMultiInstanceExposureRecipe)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "vc-multi.jpg";
    // Prefer JPEG helper if available via other includes; fall back to import fixture.
    QImage image(16, 12, QImage::Format_RGB888);
    image.fill(QColor(40, 90, 130));
    ASSERT_TRUE(image.save(QString::fromStdString(path.string()), "JPEG", 90));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto source_id = imported.value().asset->id;

    DevelopParams develop;
    DevelopExposureInstance global;
    global.instance_id = "exposure-global";
    global.name = "Global";
    global.exposure_ev = 0.1;
    DevelopExposureInstance local;
    local.instance_id = "exposure-local";
    local.name = "Local";
    local.exposure_ev = 0.35;
    local.mask_id = "vc-mask";
    develop.exposure_instances = {global, local};
    Mask mask{"vc-mask", kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
    mask.payload = EllipseMask{0.4, 0.5, 0.2, 0.15, 10.0, 0.05};
    develop.masks.push_back(mask);
    ASSERT_TRUE(service->save_develop(source_id, develop));

    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot);
    auto versioned = service->create_asset_version(source_id, snapshot.value().revision);
    ASSERT_TRUE(versioned) << versioned.error().message;
    const auto version_id = versioned.value().version.id;

    auto version_recipe = service->load_recipe(version_id);
    ASSERT_TRUE(version_recipe) << version_recipe.error().message;
    auto version_develop = develop_from_recipe(version_recipe.value());
    ASSERT_TRUE(version_develop) << version_develop.error().message;
    ASSERT_EQ(version_develop.value().exposure_instances.size(), 2U);
    EXPECT_EQ(version_develop.value().exposure_instances[0].instance_id, "exposure-global");
    EXPECT_EQ(version_develop.value().exposure_instances[1].mask_id, "vc-mask");
    ASSERT_EQ(version_develop.value().masks.size(), 1U);
    EXPECT_EQ(version_develop.value().masks.front().id, "vc-mask");
}

} // namespace ravo
