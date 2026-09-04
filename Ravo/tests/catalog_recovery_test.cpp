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
#include "ravo/recipe/mask.h"
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

TEST_F(CatalogServiceTest, CatalogBackupRestorePublishesOpenableCatalogAndRebuildsPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "restore-source.jpg";
    QImage image(28, 20, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(45, 105, 165));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->set_rating(asset_id, 4));
    ASSERT_TRUE(service->set_tags(asset_id, {"restore", "verified"}));
    DevelopParams develop;
    develop.exposure_ev = 0.45;
    ASSERT_TRUE(service->save_develop(asset_id, develop));

    const auto backup_path = root / "restore-backup";
    auto backup = service->create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    const auto backup_catalog_hash = file_sha256((backup_path / "catalog.sqlite").string());
    const auto backup_manifest_hash = file_sha256((backup_path / "manifest.json").string());
    const auto backup_sidecar = *std::filesystem::directory_iterator(backup_path / "sidecars");
    const auto backup_sidecar_hash = file_sha256(backup_sidecar.path().string());

    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "restored.sqlite").string();
    std::vector<CatalogRestoreStage> stages;
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = restored_path;
    auto restored = restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request,
                                           [&stages](const CatalogRestoreProgress &progress)
                                           { stages.push_back(progress.stage); });
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().published);
    EXPECT_TRUE(restored.value().previews_rebuild_required);
    EXPECT_EQ(restored.value().catalog.catalog_id, backup.value().catalog.catalog_id);
    EXPECT_EQ(restored.value().catalog.revision, backup.value().catalog.revision);
    EXPECT_TRUE(std::filesystem::is_regular_file(restored_path));
    EXPECT_TRUE(std::filesystem::is_directory(restored_path + ".ravo/sidecars"));
    EXPECT_FALSE(std::filesystem::exists(restored_path + ".preview"));
    ASSERT_FALSE(stages.empty());
    EXPECT_EQ(stages.front(), CatalogRestoreStage::kVerifySource);
    EXPECT_EQ(stages.back(), CatalogRestoreStage::kComplete);

    auto restored_repository = SqliteCatalogRepository::open(restored_path);
    ASSERT_TRUE(restored_repository) << restored_repository.error().message;
    auto restored_cache = FilesystemPreviewCache::create(restored_path + ".preview");
    ASSERT_TRUE(restored_cache) << restored_cache.error().message;
    auto restored_recovery = FilesystemRecoveryStore::create_for_catalog(restored_path);
    ASSERT_TRUE(restored_recovery) << restored_recovery.error().message;
    CatalogService restored_service(
        engine, std::move(restored_repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(restored_cache).value(), std::move(restored_recovery).value());
    auto synchronized = restored_service.sync_recovery(std::nullopt);
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    EXPECT_EQ(synchronized.value().pending_before, 0U);
    auto restored_assets = restored_service.list_assets();
    ASSERT_TRUE(restored_assets) << restored_assets.error().message;
    ASSERT_EQ(restored_assets.value().size(), 1U);
    EXPECT_EQ(restored_assets.value().front().id, asset_id);
    EXPECT_EQ(restored_assets.value().front().review.rating, 4);
    EXPECT_EQ(restored_assets.value().front().tags,
              (std::vector<std::string>{"restore", "verified"}));
    auto restored_recipe = restored_service.load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored_develop = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_DOUBLE_EQ(restored_develop.value().exposure_ev, 0.45);
    auto previews_before = restored_service.list_previews();
    ASSERT_TRUE(previews_before) << previews_before.error().message;
    EXPECT_TRUE(previews_before.value().empty());
    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    preview_request.max_edge = 320;
    preview_request.request_revision = 1;
    auto preview = restored_service.request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_FALSE(preview.value().cache_path.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(preview.value().cache_path));
    auto previews_after = restored_service.list_previews();
    ASSERT_TRUE(previews_after) << previews_after.error().message;
    ASSERT_EQ(previews_after.value().size(), 1U);
    ASSERT_TRUE(restored_service.close());

    EXPECT_EQ(file_sha256((backup_path / "catalog.sqlite").string()), backup_catalog_hash);
    EXPECT_EQ(file_sha256((backup_path / "manifest.json").string()), backup_manifest_hash);
    EXPECT_EQ(file_sha256(backup_sidecar.path().string()), backup_sidecar_hash);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, CatalogBackupRestoreCancellationAndPublicationRacesAreAtomic)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "restore-race-source.jpg";
    QImage image(18, 14, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(55, 115, 175));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto backup_path = root / "restore-race-backup";
    ASSERT_TRUE(service->create_backup(backup_path.string()));
    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const std::array cancellable_stages{
        CatalogRestoreStage::kVerifySource,   CatalogRestoreStage::kStageDatabase,
        CatalogRestoreStage::kStageSidecars,  CatalogRestoreStage::kVerifyStaging,
        CatalogRestoreStage::kPublishSupport, CatalogRestoreStage::kPublishCatalog,
    };
    const auto assert_no_stage = [&](const std::filesystem::path &destination)
    {
        const auto prefix = destination.filename().string() + ".ravo-catalog-restore-";
        for (const auto &entry : std::filesystem::directory_iterator(root))
            EXPECT_FALSE(entry.path().filename().string().starts_with(prefix));
    };

    std::size_t index = 0U;
    for (const auto target : cancellable_stages)
    {
        const auto destination = root / ("cancelled-restore-" + std::to_string(index++));
        CancellationSource cancellation;
        CatalogRestoreRequest request;
        request.backup_directory = backup_path.string();
        request.destination_catalog = destination.string();
        request.cancellation = cancellation.token();
        auto restored = restore_catalog_backup(
            verifier, verifier, *backup_recovery.value(), request,
            [target, &cancellation](const CatalogRestoreProgress &progress)
            {
                if (progress.stage == target)
                    static_cast<void>(cancellation.cancel("restore-stage-test"));
            });
        ASSERT_FALSE(restored) << catalog_restore_stage_name(target);
        EXPECT_EQ(restored.error().code, ErrorCode::kCancelled)
            << catalog_restore_stage_name(target);
        EXPECT_FALSE(std::filesystem::exists(destination));
        EXPECT_FALSE(std::filesystem::exists(destination.string() + ".ravo"));
        assert_no_stage(destination);
    }

    const auto support_race_destination = root / "support-race.sqlite";
    const auto support_race_root = support_race_destination.string() + ".ravo";
    CatalogRestoreRequest support_race;
    support_race.backup_directory = backup_path.string();
    support_race.destination_catalog = support_race_destination.string();
    auto support_conflict = restore_catalog_backup(
        verifier, verifier, *backup_recovery.value(), support_race,
        [&support_race_root](const CatalogRestoreProgress &progress)
        {
            if (progress.stage == CatalogRestoreStage::kPublishSupport)
            {
                ASSERT_TRUE(std::filesystem::create_directory(support_race_root));
                std::ofstream sentinel(std::filesystem::path(support_race_root) / "sentinel",
                                       std::ios::binary);
                ASSERT_TRUE(sentinel);
                sentinel << "winner";
            }
        });
    ASSERT_FALSE(support_conflict);
    EXPECT_EQ(support_conflict.error().code, ErrorCode::kConflict);
    EXPECT_FALSE(std::filesystem::exists(support_race_destination));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(std::filesystem::path(support_race_root) / "sentinel"));
    assert_no_stage(support_race_destination);

    const auto catalog_race_destination = root / "catalog-race.sqlite";
    CatalogRestoreRequest catalog_race;
    catalog_race.backup_directory = backup_path.string();
    catalog_race.destination_catalog = catalog_race_destination.string();
    auto catalog_conflict = restore_catalog_backup(
        verifier, verifier, *backup_recovery.value(), catalog_race,
        [&catalog_race_destination](const CatalogRestoreProgress &progress)
        {
            if (progress.stage == CatalogRestoreStage::kPublishCatalog)
            {
                std::ofstream sentinel(catalog_race_destination, std::ios::binary);
                ASSERT_TRUE(sentinel);
                sentinel << "winner";
            }
        });
    ASSERT_FALSE(catalog_conflict);
    EXPECT_EQ(catalog_conflict.error().code, ErrorCode::kConflict);
    EXPECT_FALSE(std::filesystem::exists(catalog_race_destination.string() + ".ravo"));
    QFile winner(QString::fromStdString(catalog_race_destination.string()));
    ASSERT_TRUE(winner.open(QIODevice::ReadOnly));
    EXPECT_EQ(winner.readAll(), QByteArray("winner"));
    assert_no_stage(catalog_race_destination);

    const auto committed_destination = root / "postcommit-cancel.sqlite";
    CancellationSource late_cancellation;
    CatalogRestoreRequest committed;
    committed.backup_directory = backup_path.string();
    committed.destination_catalog = committed_destination.string();
    committed.cancellation = late_cancellation.token();
    auto committed_result = restore_catalog_backup(
        verifier, verifier, *backup_recovery.value(), committed,
        [&late_cancellation](const CatalogRestoreProgress &progress)
        {
            if (progress.stage == CatalogRestoreStage::kOpenCatalog)
                static_cast<void>(late_cancellation.cancel("too-late-to-rollback"));
        });
    ASSERT_TRUE(committed_result) << committed_result.error().message;
    EXPECT_TRUE(committed_result.value().published);
    EXPECT_TRUE(std::filesystem::is_regular_file(committed_destination));
    EXPECT_TRUE(std::filesystem::is_directory(committed_destination.string() + ".ravo"));
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

TEST_F(CatalogServiceTest, PreviewRebuildIsBoundedPreflightedAndReportsPartialFailure)
{
    ASSERT_TRUE(open_service(true));
    QImage image(20, 14, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const auto first_path = root / "rebuild-first.jpg";
    const auto second_path = root / "rebuild-second.jpg";
    image.fill(QColor(35, 95, 155));
    ASSERT_TRUE(image.save(QString::fromStdString(first_path.string()), "JPEG", 90));
    image.fill(QColor(155, 95, 35));
    ASSERT_TRUE(image.save(QString::fromStdString(second_path.string()), "JPEG", 90));
    auto first = service->import_one(first_path.string(), CancellationToken{});
    auto second = service->import_one(second_path.string(), CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(first.value().asset);
    ASSERT_TRUE(second.value().asset);
    const auto first_id = first.value().asset->id;
    const auto second_id = second.value().asset->id;

    auto duplicate = service->rebuild_previews({first_id, first_id}, CancellationToken{});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kInvalidArgument);
    auto unknown = service->rebuild_previews({"missing-asset"}, CancellationToken{});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::kNotFound);

    std::vector<std::size_t> progress_counts;
    auto selected =
        service->rebuild_previews({first_id}, CancellationToken{},
                                  [&progress_counts](const std::size_t completed, const std::size_t,
                                                     const PreviewRebuildItemResult *)
                                  { progress_counts.push_back(completed); });
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_EQ(selected.value().total, 1U);
    EXPECT_EQ(selected.value().succeeded, 1U);
    ASSERT_EQ(selected.value().items.size(), 1U);
    ASSERT_TRUE(selected.value().items.front().browse_cache_path);
    ASSERT_TRUE(selected.value().items.front().develop_cache_path);
    EXPECT_TRUE(
        std::filesystem::is_regular_file(*selected.value().items.front().browse_cache_path));
    EXPECT_TRUE(
        std::filesystem::is_regular_file(*selected.value().items.front().develop_cache_path));
    EXPECT_EQ(progress_counts, (std::vector<std::size_t>{0U, 1U}));

    ASSERT_TRUE(std::filesystem::remove(first_path));
    ASSERT_TRUE(service->close());
    service.reset();
    sqlite_repository = nullptr;
    ASSERT_TRUE(open_service(false));
    auto partial = service->rebuild_previews({first_id, second_id}, CancellationToken{});
    ASSERT_TRUE(partial) << partial.error().message;
    EXPECT_EQ(partial.value().completed, 2U);
    EXPECT_EQ(partial.value().failed, 1U);
    EXPECT_EQ(partial.value().succeeded, 1U);
    ASSERT_TRUE(partial.value().items.front().error);
    EXPECT_FALSE(partial.value().items.back().error);

    CancellationSource cancellation;
    auto cancelled = service->rebuild_previews(
        {second_id, first_id}, cancellation.token(),
        [&cancellation](const std::size_t completed, const std::size_t,
                        const PreviewRebuildItemResult *)
        {
            if (completed == 1U)
                static_cast<void>(cancellation.cancel("preview-rebuild-test"));
        });
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(cancelled.error().context.at("completed_count"), "1");
    EXPECT_EQ(cancelled.error().context.at("total_count"), "2");
}

TEST_F(CatalogServiceTest, SnapshotRevisionObservesWritesFromAnotherConnection)
{
    ASSERT_TRUE(open_service(true));
    auto before = service->snapshot();
    ASSERT_TRUE(before) << before.error().message;
    auto other = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(other) << other.error().message;
    auto bumped = other.value()->bump_revision();
    ASSERT_TRUE(bumped) << bumped.error().message;
    EXPECT_EQ(bumped.value(), before.value().revision + 1);
    auto after = service->snapshot();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().revision, bumped.value());
    ASSERT_TRUE(other.value()->close());
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

TEST_F(CatalogServiceTest, ManagedCopyPublishesExactMediaAndXmpWithoutChangingSources)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "copy-source";
    const auto destination = root / "copy-destination";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    const auto source = source_dir / "photo.png";
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 90, 150));
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    const auto sidecar = source_dir / "photo.xmp";
    {
        std::ofstream output(sidecar, std::ios::binary);
        output << "<x:xmpmeta>keep me</x:xmpmeta>";
    }
    const auto source_hash = file_sha256(source.string());
    const auto sidecar_hash = file_sha256(sidecar.string());

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kSingleFolder;
    request.preview = ImportPreviewPolicy::kStandard;
    request.destination_directory = destination.string();
    request.defer_previews = true;
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().imported, 1U);
    ASSERT_EQ(imported.value().items.size(), 1U);
    EXPECT_TRUE(imported.value().items[0].preview_pending);
    const auto copied = destination / "photo.png";
    const auto copied_sidecar = destination / "photo.xmp";
    EXPECT_EQ(file_sha256(copied.string()), source_hash);
    EXPECT_EQ(file_sha256(copied_sidecar.string()), sidecar_hash);
    EXPECT_EQ(file_sha256(source.string()), source_hash);
    EXPECT_EQ(file_sha256(sidecar.string()), sidecar_hash);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value()[0].normalized_uri, normalize_local_input(copied.string()).value().uri);
}

TEST_F(CatalogServiceTest, ManagedMoveDeletesVerifiedSourcesAndPreflightConflictPublishesNothing)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "move-source";
    const auto destination = root / "move-destination";
    const auto second_copy = root / "move-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    const auto source = source_dir / "move.png";
    QImage image(40, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(120, 50, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    const auto source_hash = file_sha256(source.string());

    ImportRequest move;
    move.inputs = {source.string()};
    move.source_root = source_dir.string();
    move.mode = ImportTransferMode::kMove;
    move.destination_directory = destination.string();
    move.second_copy_directory = second_copy.string();
    move.preview = ImportPreviewPolicy::kMinimal;
    auto moved = service->execute_import(move);
    ASSERT_TRUE(moved) << moved.error().message;
    ASSERT_EQ(moved.value().imported, 1U);
    EXPECT_EQ(moved.value().verified_second_copies, 1U);
    ASSERT_TRUE(moved.value().items[0].copies_verified);
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(file_sha256((destination / "move.png").string()), source_hash);
    EXPECT_EQ(file_sha256((second_copy / "move.png").string()), source_hash);

    const auto conflict_source = source_dir / "conflict.png";
    ASSERT_TRUE(image.save(QString::fromStdString(conflict_source.string()), "PNG"));
    ASSERT_TRUE(image.save(QString::fromStdString((destination / "conflict.png").string()), "PNG"));
    ImportRequest conflict;
    conflict.inputs = {conflict_source.string()};
    conflict.source_root = source_dir.string();
    conflict.mode = ImportTransferMode::kCopy;
    conflict.destination_directory = destination.string();
    auto rejected = service->execute_import(conflict);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "import_destination_conflict");
    EXPECT_TRUE(std::filesystem::exists(conflict_source));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed.value().size(), 1U);
}

TEST_F(CatalogServiceTest, ManagedCopyRenamesAndVerifiesPrimarySecondCopyAndXmp)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "shoot-source";
    const auto destination = root / "shoot-primary";
    const auto second_copy = root / "shoot-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    QImage image(64, 40, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(24, 80, 160));
    const auto first_source = source_dir / "a.png";
    const auto second_source = source_dir / "b.png";
    ASSERT_TRUE(image.save(QString::fromStdString(first_source.string()), "PNG"));
    image.fill(QColor(160, 80, 24));
    ASSERT_TRUE(image.save(QString::fromStdString(second_source.string()), "PNG"));
    const auto sidecar = source_dir / "a.xmp";
    {
        std::ofstream output(sidecar, std::ios::binary);
        output << "<x:xmpmeta>verified companion</x:xmpmeta>";
    }
    const auto first_hash = file_sha256(first_source.string());
    const auto second_hash = file_sha256(second_source.string());
    const auto sidecar_hash = file_sha256(sidecar.string());
    const auto first_mtime = std::filesystem::last_write_time(first_source);
    const auto second_mtime = std::filesystem::last_write_time(second_source);

    ImportRequest request;
    request.inputs = {source_dir.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kSingleFolder;
    request.destination_directory = destination.string();
    request.filename_template = "shoot-{sequence}-{stem}{ext}";
    request.second_copy_directory = second_copy.string();
    request.preview = ImportPreviewPolicy::kMinimal;
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().imported, 2U);
    EXPECT_EQ(imported.value().verified_second_copies, 2U);
    ASSERT_EQ(imported.value().items.size(), 2U);
    EXPECT_TRUE(imported.value().items[0].copies_verified);
    EXPECT_TRUE(imported.value().items[1].copies_verified);

    const auto primary_a = destination / "shoot-0001-a.png";
    const auto primary_b = destination / "shoot-0002-b.png";
    const auto primary_xmp = destination / "shoot-0001-a.xmp";
    const auto second_a = second_copy / "shoot-0001-a.png";
    const auto second_b = second_copy / "shoot-0002-b.png";
    const auto second_xmp = second_copy / "shoot-0001-a.xmp";
    EXPECT_EQ(file_sha256(primary_a.string()), first_hash);
    EXPECT_EQ(file_sha256(primary_b.string()), second_hash);
    EXPECT_EQ(file_sha256(primary_xmp.string()), sidecar_hash);
    EXPECT_EQ(file_sha256(second_a.string()), first_hash);
    EXPECT_EQ(file_sha256(second_b.string()), second_hash);
    EXPECT_EQ(file_sha256(second_xmp.string()), sidecar_hash);
    EXPECT_EQ(file_sha256(first_source.string()), first_hash);
    EXPECT_EQ(file_sha256(second_source.string()), second_hash);
    EXPECT_EQ(file_sha256(sidecar.string()), sidecar_hash);
    EXPECT_EQ(std::filesystem::last_write_time(first_source), first_mtime);
    EXPECT_EQ(std::filesystem::last_write_time(second_source), second_mtime);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);
    std::set<std::string> catalog_uris;
    for (const auto &asset : listed.value())
        catalog_uris.insert(asset.normalized_uri);
    EXPECT_TRUE(catalog_uris.contains(normalize_local_input(primary_a.string()).value().uri));
    EXPECT_TRUE(catalog_uris.contains(normalize_local_input(primary_b.string()).value().uri));
    EXPECT_FALSE(catalog_uris.contains(normalize_local_input(second_a.string()).value().uri));
    EXPECT_FALSE(catalog_uris.contains(normalize_local_input(second_b.string()).value().uri));
}

TEST_F(CatalogServiceTest, SecondCopyConflictRejectsTheCompletePlanBeforePrimaryPublication)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "conflict-source";
    const auto destination = root / "conflict-primary";
    const auto second_copy = root / "conflict-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 40, 130));
    const auto source = source_dir / "photo.png";
    const auto conflict = second_copy / "job-0001.png";
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    ASSERT_TRUE(image.save(QString::fromStdString(conflict.string()), "PNG"));
    const auto conflict_hash = file_sha256(conflict.string());

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.filename_template = "job-{sequence}{ext}";
    request.second_copy_directory = second_copy.string();
    auto imported = service->execute_import(request);
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().context.at("reason"), "import_destination_conflict");
    EXPECT_FALSE(std::filesystem::exists(destination / "job-0001.png"));
    EXPECT_EQ(file_sha256(conflict.string()), conflict_hash);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_TRUE(listed.value().empty());
}

TEST_F(CatalogServiceTest, RenamePlanRejectsAsciiCaseCollisionsBeforePublication)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "case-source";
    const auto destination = root / "case-primary";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 100, 130));
    const auto upper = source_dir / "a.PNG";
    const auto lower = source_dir / "b.png";
    ASSERT_TRUE(image.save(QString::fromStdString(upper.string()), "PNG"));
    ASSERT_TRUE(image.save(QString::fromStdString(lower.string()), "PNG"));

    ImportRequest request;
    request.inputs = {source_dir.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.filename_template = "same{ext}";
    auto imported = service->execute_import(request);
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().context.at("reason"), "duplicate_import_output");
    EXPECT_TRUE(std::filesystem::is_empty(destination));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_TRUE(listed.value().empty());
}

TEST_F(CatalogServiceTest, CaptureDateOrganizationAndRenameUseOneDeterministicDate)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "date-source";
    const auto destination = root / "date-primary";
    const auto second_copy = root / "date-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    QImage image(36, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(100, 120, 140));
    const auto source = source_dir / "photo.png";
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    using namespace std::chrono;
    const auto desired = sys_days{year{2024} / month{5} / day{6}} + hours{12};
    const auto file_time =
        std::filesystem::file_time_type::clock::now() + (desired - system_clock::now());
    std::filesystem::last_write_time(source, file_time);

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kCaptureDate;
    request.destination_directory = destination.string();
    request.filename_template = "job-{date}-{sequence}{ext}";
    request.second_copy_directory = second_copy.string();
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().imported, 1U);
    const auto relative = std::filesystem::path("2024") / "05" / "06" / "job-20240506-0001.png";
    EXPECT_TRUE(std::filesystem::exists(destination / relative));
    EXPECT_TRUE(std::filesystem::exists(second_copy / relative));
    EXPECT_TRUE(imported.value().items[0].copies_verified);
}

TEST_F(CatalogServiceTest, CopyImportOrganizesByCaptureMonth)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "month-source";
    const auto destination = root / "month-destination";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    QImage image(36, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 110, 140));
    const auto source = source_dir / "photo.png";
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    using namespace std::chrono;
    const auto desired = sys_days{year{2024} / month{5} / day{6}} + hours{12};
    const auto file_time =
        std::filesystem::file_time_type::clock::now() + (desired - system_clock::now());
    std::filesystem::last_write_time(source, file_time);

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.organization = ImportOrganization::kCaptureMonth;
    request.destination_directory = destination.string();
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().imported, 1U);
    const auto relative = std::filesystem::path("2024") / "05" / "photo.png";
    EXPECT_TRUE(std::filesystem::exists(destination / relative));
    EXPECT_FALSE(std::filesystem::exists(destination / "2024" / "05" / "06" / "photo.png"));
}

TEST_F(CatalogServiceTest, VerificationMismatchRemovesOnlyOwnedCopiesAndPublishesNoAsset)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "verify-source";
    const auto destination = root / "verify-primary";
    const auto second_copy = root / "verify-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(12, 100, 200));
    const auto source = source_dir / "photo.png";
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    const auto source_hash = file_sha256(source.string());

    testing::CatalogServiceTestControl::set_import_checkpoint(
        *service,
        [](const std::string_view checkpoint, const std::string_view path) -> Result<void>
        {
            if (checkpoint != "before_copy_verification")
                return {};
            std::fstream output(std::string(path), std::ios::in | std::ios::out | std::ios::binary);
            if (!output)
                return make_error(ErrorCode::kIo, "Unable to inject copy corruption");
            char byte = 0;
            output.read(&byte, 1);
            byte ^= static_cast<char>(0x5a);
            output.seekp(0);
            output.write(&byte, 1);
            output.flush();
            if (!output)
                return make_error(ErrorCode::kIo, "Unable to inject copy corruption");
            return {};
        });

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kMove;
    request.destination_directory = destination.string();
    request.filename_template = "job{ext}";
    request.second_copy_directory = second_copy.string();
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().failed, 1U);
    EXPECT_EQ(imported.value().imported, 0U);
    EXPECT_EQ(imported.value().verified_second_copies, 0U);
    ASSERT_EQ(imported.value().items.size(), 1U);
    ASSERT_TRUE(imported.value().items[0].error);
    EXPECT_EQ(imported.value().items[0].error->context.at("reason"), "import_copy_verify_mismatch");
    EXPECT_FALSE(imported.value().items[0].copies_verified);
    EXPECT_FALSE(std::filesystem::exists(destination / "job.png"));
    EXPECT_FALSE(std::filesystem::exists(second_copy / "job.png"));
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_EQ(file_sha256(source.string()), source_hash);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_TRUE(listed.value().empty());
    testing::CatalogServiceTestControl::set_import_checkpoint(*service, {});
}

TEST_F(CatalogServiceTest, CancellationBeforeSecondCopyVerificationCleansBothTrees)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "cancel-source";
    const auto destination = root / "cancel-primary";
    const auto second_copy = root / "cancel-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 160, 90));
    const auto source = source_dir / "photo.png";
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    const auto source_hash = file_sha256(source.string());
    CancellationSource cancellation;
    testing::CatalogServiceTestControl::set_import_checkpoint(
        *service,
        [&cancellation](const std::string_view checkpoint, const std::string_view) -> Result<void>
        {
            if (checkpoint == "before_copy_verification")
                static_cast<void>(cancellation.cancel("verify-cancel-test"));
            return {};
        });

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kCopy;
    request.destination_directory = destination.string();
    request.filename_template = "job{ext}";
    request.second_copy_directory = second_copy.string();
    request.cancellation = cancellation.token();
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().items.size(), 1U);
    ASSERT_TRUE(imported.value().items[0].error);
    EXPECT_EQ(imported.value().items[0].error->code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(destination / "job.png"));
    EXPECT_FALSE(std::filesystem::exists(second_copy / "job.png"));
    EXPECT_EQ(file_sha256(source.string()), source_hash);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_TRUE(listed.value().empty());
    testing::CatalogServiceTestControl::set_import_checkpoint(*service, {});
}

TEST_F(CatalogServiceTest, MoveRetainsSourceSetWhenXmpChangesAfterCopyVerification)
{
    ASSERT_TRUE(open_service(true));
    const auto source_dir = root / "move-change-source";
    const auto destination = root / "move-change-primary";
    const auto second_copy = root / "move-change-second-copy";
    std::filesystem::create_directories(source_dir);
    std::filesystem::create_directories(destination);
    std::filesystem::create_directories(second_copy);
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(140, 80, 30));
    const auto source = source_dir / "photo.png";
    const auto sidecar = source_dir / "photo.xmp";
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "PNG"));
    {
        std::ofstream output(sidecar, std::ios::binary);
        output << "original-xmp";
    }
    const auto source_hash = file_sha256(source.string());
    const auto original_sidecar_hash = file_sha256(sidecar.string());
    testing::CatalogServiceTestControl::set_before_import_publication(
        *service,
        [&sidecar]
        {
            std::ofstream output(sidecar, std::ios::binary | std::ios::app);
            output << "-changed";
        });

    ImportRequest request;
    request.inputs = {source.string()};
    request.source_root = source_dir.string();
    request.mode = ImportTransferMode::kMove;
    request.destination_directory = destination.string();
    request.second_copy_directory = second_copy.string();
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().imported, 1U);
    EXPECT_EQ(imported.value().source_cleanup_failed, 1U);
    ASSERT_TRUE(imported.value().items[0].source_cleanup_error);
    EXPECT_EQ(imported.value().items[0].source_cleanup_error->context.at("reason"),
              "import_source_changed_before_cleanup");
    EXPECT_TRUE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::exists(sidecar));
    EXPECT_EQ(file_sha256(source.string()), source_hash);
    EXPECT_NE(file_sha256(sidecar.string()), original_sidecar_hash);
    EXPECT_EQ(file_sha256((destination / "photo.xmp").string()), original_sidecar_hash);
    EXPECT_EQ(file_sha256((second_copy / "photo.xmp").string()), original_sidecar_hash);
}

TEST(QtRasterDecoderTest, DecodeMemoryAppliesClockwiseQuarterTurnsWithoutExif)
{
    QImage source(32, 16, QImage::Format_RGB888);
    source.fill(QColor(12, 80, 200));
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
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
    EXPECT_EQ(identity.value().source_width, 32U);
    EXPECT_EQ(identity.value().source_height, 16U);
    EXPECT_NE(identity.value().color_profile.kind, ColorProfileKind::kMissing);

    auto rotated = decoder.decode_memory(bytes, 64, CancellationToken{}, 3);
    ASSERT_TRUE(rotated) << rotated.error().message;
    EXPECT_EQ(rotated.value().width, 16U);
    EXPECT_EQ(rotated.value().height, 32U);
    EXPECT_EQ(rotated.value().source_width, 16U);
    EXPECT_EQ(rotated.value().source_height, 32U);
}

TEST(QtRasterDecoderTest, KeepsEmbeddedIccAndRejectsImplicitOutputProfiles)
{
    QtRasterDecoder decoder;
    const std::vector<std::uint8_t> pixels(8U * 4U * 3U, 128U);
    ColorProfileState srgb;
    srgb.kind = ColorProfileKind::kBuiltin;
    srgb.model = ColorModel::kRgb;
    srgb.identifier = "srgb";
    auto encoded = decoder.encode(8, 4, pixels, srgb, ExportFormat::kPng, {}, CancellationToken{});
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = decoder.decode_memory(encoded.value(), 32, CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_NE(decoded.value().color_profile.kind, ColorProfileKind::kMissing);

    ColorProfileState display_p3 = srgb;
    display_p3.identifier = "display_p3";
    auto wide_encoded =
        decoder.encode(8, 4, pixels, display_p3, ExportFormat::kPng, {}, CancellationToken{});
    ASSERT_TRUE(wide_encoded) << wide_encoded.error().message;
    auto wide_decoded = decoder.decode_memory(wide_encoded.value(), 32, CancellationToken{});
    ASSERT_TRUE(wide_decoded) << wide_decoded.error().message;
    EXPECT_NE(wide_decoded.value().color_profile.kind, ColorProfileKind::kMissing);

    ColorProfileState missing;
    auto rejected =
        decoder.encode(8, 4, pixels, missing, ExportFormat::kPng, {}, CancellationToken{});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
}

TEST_F(CatalogServiceTest, SampleWhiteBalanceReturnsManualCoefficientsForBayerRaw)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    WhiteBalancePickRequest request;
    request.preview_x = 0.5;
    request.preview_y = 0.5;
    auto sampled =
        service->sample_white_balance(imported.value().asset->id, request, CancellationToken{});
    ASSERT_TRUE(sampled) << sampled.error().message;
    EXPECT_GT(sampled.value()[0], 0.0);
    EXPECT_NEAR(sampled.value()[1], 1.0, 1.0e-6);
    EXPECT_GT(sampled.value()[2], 0.0);
    EXPECT_GT(sampled.value()[3], 0.0);
    auto png = service->import_one(png_fixture_path(), CancellationToken{});
    ASSERT_TRUE(png) << png.error().message;
    ASSERT_TRUE(png.value().asset);
    auto raster =
        service->sample_white_balance(png.value().asset->id, request, CancellationToken{});
    ASSERT_FALSE(raster);
    EXPECT_EQ(raster.error().code, ErrorCode::kUnsupported);
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
    browse.purpose = PreviewPurpose::kBrowse;
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

TEST_F(CatalogServiceTest, InteractiveAndSettledRawWorkingBuffersRemainIndependentlyBounded)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);

    PreviewRequest settled;
    settled.asset_id = imported.value().asset->id;
    settled.max_edge = kDefaultPreviewMaxEdge;
    settled.prefer_embedded_preview = false;
    auto full = service->request_preview(settled);
    ASSERT_TRUE(full) << full.error().message;
    auto cache_state = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_FALSE(cache_state[0].has_value());
    EXPECT_EQ(cache_state[1], kDefaultPreviewMaxEdge);

    auto develop = service->load_recipe(settled.asset_id);
    ASSERT_TRUE(develop) << develop.error().message;
    auto live_develop = develop_from_recipe(develop.value());
    ASSERT_TRUE(live_develop) << live_develop.error().message;
    PreviewRequest interactive = settled;
    interactive.max_edge = kInteractivePreviewMaxEdge;
    interactive.persist_preview_record = false;
    auto first = service->request_preview(interactive, live_develop.value());
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_FALSE(first.value().rgb.empty());
    cache_state = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_EQ(cache_state[0], kInteractivePreviewMaxEdge);
    EXPECT_EQ(cache_state[1], kDefaultPreviewMaxEdge);

    ASSERT_TRUE(service->close());
    cache_state = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_FALSE(cache_state[0].has_value());
    EXPECT_FALSE(cache_state[1].has_value());

    ASSERT_TRUE(open_service(false));
    auto interactive_first = service->request_preview(interactive, live_develop.value());
    ASSERT_TRUE(interactive_first) << interactive_first.error().message;
    cache_state = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_EQ(cache_state[0], kInteractivePreviewMaxEdge);
    EXPECT_EQ(cache_state[1], kDefaultPreviewMaxEdge);
    auto settled_after = service->request_preview(settled);
    ASSERT_TRUE(settled_after) << settled_after.error().message;
    cache_state = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_EQ(cache_state[0], kInteractivePreviewMaxEdge);
    EXPECT_EQ(cache_state[1], kDefaultPreviewMaxEdge);
}

TEST_F(CatalogServiceTest, BrowseWorkingSetCannotEvictForegroundDevelopBuffers)
{
    ASSERT_TRUE(open_service(true));
    QImage first_image(800, 600, QImage::Format_RGB888);
    first_image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    first_image.fill(QColor(24, 96, 180));
    QImage second_image(800, 600, QImage::Format_RGB888);
    second_image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    second_image.fill(QColor(180, 72, 36));
    const auto first_path = (root / "foreground.jpg").string();
    const auto second_path = (root / "background.jpg").string();
    ASSERT_TRUE(first_image.save(QString::fromStdString(first_path), "JPEG", 95));
    ASSERT_TRUE(second_image.save(QString::fromStdString(second_path), "JPEG", 95));
    auto first = service->import_one(first_path, CancellationToken{});
    auto second = service->import_one(second_path, CancellationToken{});
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    ASSERT_TRUE(first.value().asset);
    ASSERT_TRUE(second.value().asset);

    PreviewRequest settled;
    settled.asset_id = first.value().asset->id;
    settled.max_edge = kDefaultPreviewMaxEdge;
    auto settled_result = service->request_preview(settled);
    ASSERT_TRUE(settled_result) << settled_result.error().message;
    auto recipe = service->load_recipe(settled.asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    PreviewRequest interactive = settled;
    interactive.max_edge = kInteractivePreviewMaxEdge;
    interactive.persist_preview_record = false;
    auto interactive_result = service->request_preview(interactive, develop.value());
    ASSERT_TRUE(interactive_result) << interactive_result.error().message;

    auto foreground = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_EQ(foreground[0], kInteractivePreviewMaxEdge);
    EXPECT_EQ(foreground[1], kDefaultPreviewMaxEdge);

    constexpr std::uint32_t kProbeBrowseEdge = kThumbnailMaxEdge - 7U;
    PreviewRequest browse;
    browse.asset_id = second.value().asset->id;
    browse.max_edge = kProbeBrowseEdge;
    browse.purpose = PreviewPurpose::kBrowse;
    browse.prefer_embedded_preview = true;
    auto browse_result = service->request_preview(browse);
    ASSERT_TRUE(browse_result) << browse_result.error().message;
    foreground = testing::CatalogServiceTestControl::linear_working_max_edges(*service);
    EXPECT_EQ(foreground[0], kInteractivePreviewMaxEdge);
    EXPECT_EQ(foreground[1], kDefaultPreviewMaxEdge);
    EXPECT_EQ(testing::CatalogServiceTestControl::browse_linear_working_max_edge(*service),
              kProbeBrowseEdge);

    ASSERT_TRUE(service->close());
    EXPECT_FALSE(testing::CatalogServiceTestControl::browse_linear_working_max_edge(*service));
}

TEST_F(CatalogServiceTest, ImportJpegAndDirectorySkipsSidecars)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;

    const auto jpeg_path = (root / "probe.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 20));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : std::string("no asset"));
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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

TEST_F(CatalogServiceTest, Local01MultiInstanceBackupRestoreSmoke)
{
    // LOCAL-01 C2 smoke: multi-instance Exposure/CBR + masks survive backup/restore.
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "local01-backup-multi.jpg";
    QImage image(24, 18, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 40, 110));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    auto imported = service->import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    DevelopParams multi;
    DevelopExposureInstance master;
    master.instance_id = "exposure-1";
    master.name = "Master";
    master.exposure_ev = 0.2;
    DevelopExposureInstance local;
    local.instance_id = "exposure-2";
    local.name = "Burn";
    local.exposure_ev = -0.35;
    local.mask_id = "backup-mask-circle";
    multi.exposure_instances = {master, local};
    Mask circle{"backup-mask-circle", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    circle.payload = CircleMask{0.3, 0.7, 0.2, 0.02};
    multi.masks.push_back(circle);

    DevelopColorBalanceRgbInstance c0;
    c0.instance_id = "colorbalancergb-1";
    c0.name = "Master";
    DevelopColorBalanceRgbInstance c1;
    c1.instance_id = "colorbalancergb-2";
    c1.name = "Cool";
    c1.params.global_y = -0.12;
    c1.mask_id = "backup-mask-ellipse";
    multi.color_balance_rgb_instances = {c0, c1};
    Mask ellipse{"backup-mask-ellipse", kCanonicalMaskSchemaVersion, MaskKind::kEllipse};
    ellipse.payload = EllipseMask{0.55, 0.45, 0.22, 0.14, 5.0, 0.01};
    multi.masks.push_back(ellipse);
    ASSERT_TRUE(service->save_develop(asset_id, multi));

    const auto backup_path = root / "local01-multi-backup";
    auto backup = service->create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;

    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "local01-restored.sqlite").string();
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = restored_path;
    auto restored = restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().published);

    auto restored_repository = SqliteCatalogRepository::open(restored_path);
    ASSERT_TRUE(restored_repository) << restored_repository.error().message;
    auto restored_cache = FilesystemPreviewCache::create(restored_path + ".preview");
    ASSERT_TRUE(restored_cache) << restored_cache.error().message;
    auto restored_recovery = FilesystemRecoveryStore::create_for_catalog(restored_path);
    ASSERT_TRUE(restored_recovery) << restored_recovery.error().message;
    CatalogService restored_service(
        engine, std::move(restored_repository).value(), std::make_unique<QtRasterDecoder>(),
        std::move(restored_cache).value(), std::move(restored_recovery).value());
    ASSERT_TRUE(restored_service.sync_recovery(std::nullopt));

    auto restored_recipe = restored_service.load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto params = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(params) << params.error().message;
    ASSERT_EQ(params.value().exposure_instances.size(), 2U);
    EXPECT_EQ(params.value().exposure_instances[1].name, "Burn");
    EXPECT_EQ(params.value().exposure_instances[1].mask_id, "backup-mask-circle");
    EXPECT_NEAR(params.value().exposure_instances[1].exposure_ev, -0.35, 1e-9);
    ASSERT_EQ(params.value().color_balance_rgb_instances.size(), 2U);
    EXPECT_EQ(params.value().color_balance_rgb_instances[1].name, "Cool");
    EXPECT_EQ(params.value().color_balance_rgb_instances[1].mask_id, "backup-mask-ellipse");
    EXPECT_NEAR(params.value().color_balance_rgb_instances[1].params.global_y, -0.12, 1e-9);
    EXPECT_EQ(params.value().masks.size(), 2U);
    ASSERT_TRUE(restored_service.close());
}

} // namespace
} // namespace ravo
