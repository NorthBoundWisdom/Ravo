#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include <zlib.h>

#include <QBuffer>
#include <QByteArray>
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
#include "catalog_service_test_support.h"
#include "color_balance_fixture.h"
#include "catalog_repository_test_control.h"
#include "temperature_fixture.h"

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

[[nodiscard]] std::string xtrans_fixture_path()
{
    return repository_path(std::filesystem::path("legacy") / "tests" / "images" /
                           "mire1-xtrans.raf");
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
        sqlite_repository = nullptr;
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    Result<void> open_service(const bool create, const bool resume_recovery = true)
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
        auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
        if (!recovery)
        {
            return recovery.error();
        }
        auto owned_repository = std::move(repository).value();
        sqlite_repository = owned_repository.get();
        service = std::make_unique<CatalogService>(
            engine, std::move(owned_repository), std::make_unique<QtRasterDecoder>(),
            std::move(cache).value(), std::move(recovery).value());
        if (resume_recovery)
        {
            auto resumed = service->sync_recovery(std::nullopt);
            if (!resumed)
            {
                return resumed.error();
            }
        }
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
    SqliteCatalogRepository *sqlite_repository = nullptr;
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
    EXPECT_EQ(tagged.value().tags.front(), "landscape");
    EXPECT_EQ(tagged.value().tags.back(), "archive");

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

TEST_F(CatalogServiceTest, RenameRecipeSnapshotUpdatesLabelAndRejectsHistoryRows)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "snapshot-rename.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(8, 16, 32));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams params;
    params.exposure_ev = 0.4;
    ASSERT_TRUE(service->save_develop(asset_id, params));
    auto snapshot = service->create_recipe_snapshot(asset_id, "keep");
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    std::int64_t snapshot_id = 0;
    std::int64_t history_id = 0;
    for (const auto &entry : history.value())
    {
        if (entry.kind == kRecipeHistoryKindSnapshot)
            snapshot_id = entry.id;
        else if (history_id == 0)
            history_id = entry.id;
    }
    ASSERT_NE(snapshot_id, 0);
    ASSERT_NE(history_id, 0);

    auto renamed = service->rename_recipe_snapshot(asset_id, snapshot_id, "  look-a  ");
    ASSERT_TRUE(renamed) << renamed.error().message;
    auto after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(after) << after.error().message;
    bool found = false;
    for (const auto &entry : after.value())
    {
        if (entry.id != snapshot_id)
            continue;
        found = true;
        ASSERT_TRUE(entry.label.has_value());
        EXPECT_EQ(*entry.label, "look-a");
    }
    EXPECT_TRUE(found);

    auto empty = service->rename_recipe_snapshot(asset_id, snapshot_id, "   ");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kValidation);

    auto history_row = service->rename_recipe_snapshot(asset_id, history_id, "nope");
    ASSERT_FALSE(history_row);
    EXPECT_EQ(history_row.error().code, ErrorCode::kValidation);
}

TEST_F(CatalogServiceTest, ReopenUpgradesStoredRecipeV1ToExplicitColorBoundaries)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-v1.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 60, 90));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    Recipe recipe_v1;
    recipe_v1.schema_version = 1;
    recipe_v1.asset = {asset_id, imported.value().asset->normalized_uri,
                       imported.value().asset->content_fingerprint};
    recipe_v1.operations.push_back({"ravo.core.exposure",
                                    1,
                                    "exposure-1",
                                    true,
                                    {{"exposure_ev", ParameterValue{0.5}}},
                                    std::nullopt});
    auto serialized_v1 = serialize_recipe(recipe_v1);
    ASSERT_TRUE(serialized_v1) << serialized_v1.error().message;
    ASSERT_TRUE(repository.value()->save_recipe_json(asset_id, 1, serialized_v1.value()));
    ASSERT_TRUE(repository.value()->close());
    ASSERT_TRUE(service->close());
    service.reset();

    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().schema_version, 3);
    ASSERT_EQ(restored.value().operations.size(), 3U);
    EXPECT_EQ(restored.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(restored.value().operations[1].id, "ravo.core.exposure");
    EXPECT_EQ(restored.value().operations.back().id, "ravo.color.output");
}

TEST_F(CatalogServiceTest, RecipeTransactionFailurePreservesCurrentRecipeAndRevision)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-transaction.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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

TEST_F(CatalogServiceTest, RecipeHistoryCoalescesOnlyTheExpectedLatestOrdinaryRow)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-history-coalesce.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_black = 0.01;
    auto first_saved = service->save_develop_with_history(asset_id, first);
    ASSERT_TRUE(first_saved) << first_saved.error().message;
    ASSERT_TRUE(first_saved.value().history_id);
    const auto coalesce_id = *first_saved.value().history_id;

    DevelopParams second = first;
    second.exposure_black = 0.02;
    auto second_saved = service->save_develop_with_history(
        asset_id, second,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kAppendIfNew,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = coalesce_id});
    ASSERT_TRUE(second_saved) << second_saved.error().message;
    ASSERT_TRUE(second_saved.value().history_id);
    EXPECT_EQ(*second_saved.value().history_id, coalesce_id);
    auto coalesced = service->list_recipe_history(asset_id);
    ASSERT_TRUE(coalesced) << coalesced.error().message;
    ASSERT_EQ(coalesced.value().size(), 1U);
    EXPECT_EQ(coalesced.value().front().id, coalesce_id);
    auto coalesced_recipe = parse_recipe_json(coalesced.value().front().recipe_json);
    ASSERT_TRUE(coalesced_recipe) << coalesced_recipe.error().message;
    auto coalesced_params = develop_from_recipe(coalesced_recipe.value());
    ASSERT_TRUE(coalesced_params) << coalesced_params.error().message;
    EXPECT_NEAR(coalesced_params.value().exposure_black, second.exposure_black, 1e-9);

    auto before_invalid = service->snapshot();
    ASSERT_TRUE(before_invalid) << before_invalid.error().message;
    auto invalid = service->save_develop_with_history(
        asset_id, second,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kUnchanged,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = coalesce_id});
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kValidation);
    auto after_invalid = service->snapshot();
    ASSERT_TRUE(after_invalid) << after_invalid.error().message;
    EXPECT_EQ(after_invalid.value().revision, before_invalid.value().revision);

    auto snapshot = service->create_recipe_snapshot(asset_id, "boundary");
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    DevelopParams third = second;
    third.exposure_black = 0.03;
    auto after_snapshot = service->save_develop_with_history(
        asset_id, third,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kAppendIfNew,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = coalesce_id});
    ASSERT_TRUE(after_snapshot) << after_snapshot.error().message;
    ASSERT_TRUE(after_snapshot.value().history_id);
    EXPECT_NE(*after_snapshot.value().history_id, coalesce_id);
    auto separated = service->list_recipe_history(asset_id);
    ASSERT_TRUE(separated) << separated.error().message;
    ASSERT_EQ(separated.value().size(), 3U);
    EXPECT_EQ(separated.value()[1].kind, kRecipeHistoryKindSnapshot);
    EXPECT_EQ(separated.value()[2].id, coalesce_id);
}

TEST_F(CatalogServiceTest, RecipeHistoryCoalesceFailureRollsBackRecipeHistoryAndRevision)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "recipe-history-coalesce-failure.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(120, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_black = 0.01;
    auto first_saved = service->save_develop_with_history(asset_id, first);
    ASSERT_TRUE(first_saved) << first_saved.error().message;
    ASSERT_TRUE(first_saved.value().history_id);
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    auto history_before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;

    {
        const auto connection = QStringLiteral("ravo_recipe_coalesce_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_recipe_history_coalesce "
            "BEFORE UPDATE OF recipe_json ON asset_recipe_history "
            "BEGIN SELECT RAISE(ABORT, 'forced recipe history coalesce failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    DevelopParams rejected = first;
    rejected.exposure_black = 0.03;
    auto failed = service->save_develop_with_history(
        asset_id, rejected,
        RecipeSaveOptions{.history_write = RecipeHistoryWrite::kAppendIfNew,
                          .discard_history_after_seq = {},
                          .coalesce_history_id = *first_saved.value().history_id});
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);

    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_black, first.exposure_black, 1e-9);
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
    auto history_after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    ASSERT_EQ(history_after.value().size(), history_before.value().size());
    EXPECT_EQ(history_after.value().front().id, history_before.value().front().id);
    EXPECT_EQ(history_after.value().front().recipe_json,
              history_before.value().front().recipe_json);
}

TEST_F(CatalogServiceTest, HistoryPreviewLeavesStackAndEditDiscardsNewerSteps)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "history-cursor.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 40, 80));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_ev = 0.4;
    ASSERT_TRUE(service->save_develop(asset_id, first));
    DevelopParams second;
    second.exposure_ev = -0.5;
    ASSERT_TRUE(service->save_develop(asset_id, second));
    DevelopParams third;
    third.exposure_ev = -0.5;
    third.highlights = 0.3;
    ASSERT_TRUE(service->save_develop(asset_id, third));

    auto history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history) << history.error().message;
    ASSERT_EQ(history.value().size(), 3U);
    const auto newest = history.value()[0];
    const auto middle = history.value()[1];
    const auto oldest = history.value()[2];
    EXPECT_GT(newest.seq, middle.seq);
    EXPECT_GT(middle.seq, oldest.seq);

    auto previewed =
        service->save_develop(asset_id, second,
                              RecipeSaveOptions{.history_write = RecipeHistoryWrite::kUnchanged,
                                                .discard_history_after_seq = {},
                                                .coalesce_history_id = {}});
    ASSERT_TRUE(previewed) << previewed.error().message;
    auto preview_history = service->list_recipe_history(asset_id);
    ASSERT_TRUE(preview_history) << preview_history.error().message;
    ASSERT_EQ(preview_history.value().size(), 3U);
    EXPECT_EQ(preview_history.value()[0].id, newest.id);
    EXPECT_EQ(preview_history.value()[1].id, middle.id);
    auto preview_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(preview_recipe) << preview_recipe.error().message;
    auto preview_params = develop_from_recipe(preview_recipe.value());
    ASSERT_TRUE(preview_params) << preview_params.error().message;
    EXPECT_NEAR(preview_params.value().exposure_ev, second.exposure_ev, 1e-9);
    EXPECT_NEAR(preview_params.value().highlights, 0.0, 1e-9);

    DevelopParams branched = second;
    branched.contrast = 0.2;
    auto edited = service->save_develop(asset_id, branched,
                                        RecipeSaveOptions{
                                            .history_write = RecipeHistoryWrite::kAppendIfNew,
                                            .discard_history_after_seq = middle.seq,
                                            .coalesce_history_id = {},
                                        });
    ASSERT_TRUE(edited) << edited.error().message;
    auto truncated = service->list_recipe_history(asset_id);
    ASSERT_TRUE(truncated) << truncated.error().message;
    ASSERT_EQ(truncated.value().size(), 3U);
    EXPECT_EQ(truncated.value()[1].id, middle.id);
    EXPECT_EQ(truncated.value()[2].id, oldest.id);
    EXPECT_NE(truncated.value()[0].id, newest.id);
    EXPECT_GT(truncated.value()[0].seq, middle.seq);
    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_ev, second.exposure_ev, 1e-9);
    EXPECT_NEAR(current_params.value().contrast, branched.contrast, 1e-9);
    EXPECT_NEAR(current_params.value().highlights, 0.0, 1e-9);
}

TEST_F(CatalogServiceTest, RestoreRecipeHistoryStillAppendsCurrentStep)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "history-restore-append.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(8, 16, 32));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, first));
    DevelopParams second;
    second.exposure_ev = -0.25;
    ASSERT_TRUE(service->save_develop(asset_id, second));
    auto before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_EQ(before.value().size(), 2U);
    const auto oldest_id = before.value().back().id;

    auto restored = service->restore_recipe_history(asset_id, oldest_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().size(), 3U);
    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_NEAR(params.value().exposure_ev, first.exposure_ev, 1e-9);
}

TEST_F(CatalogServiceTest, HistoryDiscardAndAppendShareRecipeTransaction)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "history-discard-transaction.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(64, 32, 16));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams first;
    first.exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, first));
    DevelopParams second;
    second.exposure_ev = -0.75;
    ASSERT_TRUE(service->save_develop(asset_id, second));
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    auto history_before = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_before) << history_before.error().message;
    ASSERT_EQ(history_before.value().size(), 2U);
    const auto cursor_seq = history_before.value().back().seq;

    {
        const auto connection = QStringLiteral("ravo_history_discard_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_recipe_history_discard BEFORE INSERT ON asset_recipe_history "
            "BEGIN SELECT RAISE(ABORT, 'forced recipe history discard failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    DevelopParams branched = first;
    branched.contrast = 0.4;
    auto failed = service->save_develop(asset_id, branched,
                                        RecipeSaveOptions{
                                            .history_write = RecipeHistoryWrite::kAppendIfNew,
                                            .discard_history_after_seq = cursor_seq,
                                            .coalesce_history_id = {},
                                        });
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);

    auto current = service->load_recipe(asset_id);
    ASSERT_TRUE(current) << current.error().message;
    auto current_params = develop_from_recipe(current.value());
    ASSERT_TRUE(current_params) << current_params.error().message;
    EXPECT_NEAR(current_params.value().exposure_ev, second.exposure_ev, 1e-9);
    EXPECT_NEAR(current_params.value().contrast, 0.0, 1e-9);
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
    auto history_after = service->list_recipe_history(asset_id);
    ASSERT_TRUE(history_after) << history_after.error().message;
    ASSERT_EQ(history_after.value().size(), history_before.value().size());
    EXPECT_EQ(history_after.value()[0].id, history_before.value()[0].id);
    EXPECT_EQ(history_after.value()[1].id, history_before.value()[1].id);
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
    ASSERT_EQ(baseline.value().operations.size(), 3U);
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.color.input"; }),
              baseline.value().operations.end());
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.display.sigmoid"; }),
              baseline.value().operations.end());
    EXPECT_NE(std::find_if(baseline.value().operations.begin(), baseline.value().operations.end(),
                           [](const OperationInstance &operation)
                           { return operation.id == "ravo.color.output"; }),
              baseline.value().operations.end());
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(200, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams live;
    live.exposure_ev = 1.25;
    live.temperature.mode = std::string(kTemperatureModeManual);
    live.temperature.coefficients =
        std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kInteractivePreviewMaxEdge;
    request.persist_preview_record = false;
    auto first = service->request_preview(request, live);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first.value().cache_path.empty());
    EXPECT_FALSE(first.value().rgb.empty());
    EXPECT_EQ(first.value().rgb.size(),
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

    const auto first_pixels = first.value().rgb;
    live.exposure_ev = -0.75;
    auto second = service->request_preview(request, live);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_TRUE(second.value().cache_path.empty());
    EXPECT_NE(second.value().rgb, first_pixels);
}

TEST_F(CatalogServiceTest, RgbPrimariesPersistAndReproducePixelsAfterReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto png_path = (root / "primaries.png").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            image.setPixelColor(
                x, y,
                QColor((x * 17 + y * 3) % 256, (x * 5 + y * 13) % 256, (x * 11 + y * 7) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(png_path), "PNG"));
    const auto original_hash = file_sha256(png_path);
    auto imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 48;
    preview.persist_preview_record = true;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().cache_path.empty());
    ASSERT_TRUE(std::filesystem::exists(baseline.value().cache_path));
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());
    const auto same_pixels = [](const QImage &left, const QImage &right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (int y = 0; y < left.height(); ++y)
        {
            for (int x = 0; x < left.width(); ++x)
            {
                if (left.pixel(x, y) != right.pixel(x, y))
                {
                    return false;
                }
            }
        }
        return true;
    };

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto edited = develop_from_recipe(recipe.value());
    ASSERT_TRUE(edited) << edited.error().message;
    edited.value().primaries.achromatic_tint_hue = 0.3;
    edited.value().primaries.achromatic_tint_purity = 0.1;
    edited.value().primaries.red_hue = -0.05;
    edited.value().primaries.red_purity = 0.96;
    edited.value().primaries.green_hue = 0.04;
    edited.value().primaries.green_purity = 1.04;
    edited.value().primaries.blue_hue = -0.03;
    edited.value().primaries.blue_purity = 1.02;
    clamp_develop(edited.value());
    auto saved = service->save_develop(asset_id, edited.value());
    ASSERT_TRUE(saved) << saved.error().message;

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto primaries_operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &operation) { return operation.id == kPrimariesOperationId; });
    ASSERT_NE(primaries_operation, stored_recipe.value().operations.end());
    EXPECT_EQ(primaries_operation->schema_version, 1);
    EXPECT_EQ(primaries_operation->parameters.size(), 8U);

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    ASSERT_TRUE(std::filesystem::exists(before_reopen.value().cache_path));
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_FALSE(same_pixels(before_reopen_image, baseline_image));
    EXPECT_NE(before_reopen.value().cache_key, baseline.value().cache_key);

    PreviewRequest interactive = preview;
    interactive.persist_preview_record = false;
    auto interactive_before_reopen = service->request_preview(interactive);
    ASSERT_TRUE(interactive_before_reopen) << interactive_before_reopen.error().message;
    EXPECT_TRUE(interactive_before_reopen.value().cache_path.empty());
    ASSERT_FALSE(interactive_before_reopen.value().rgb.empty());

    const auto export_path = (root / "primaries-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 48U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_TRUE(same_pixels(export_image, before_reopen_image));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().primaries, edited.value().primaries);

    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    ASSERT_FALSE(after_reopen.value().cache_path.empty());
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_TRUE(same_pixels(after_reopen_image, before_reopen_image));
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);

    auto interactive_after_reopen = service->request_preview(interactive);
    ASSERT_TRUE(interactive_after_reopen) << interactive_after_reopen.error().message;
    EXPECT_EQ(interactive_after_reopen.value().rgb, interactive_before_reopen.value().rgb);
    EXPECT_EQ(file_sha256(png_path), original_hash);
}

TEST_F(CatalogServiceTest, ProfileGammaModesPersistAndReproducePreviewAndExportAfterReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto png_path = (root / "profile-gamma.png").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int y = 0; y < image.height(); ++y)
    {
        for (int x = 0; x < image.width(); ++x)
        {
            image.setPixelColor(
                x, y,
                QColor((x * 11 + y * 7) % 256, (x * 3 + y * 17) % 256, (x * 19 + y * 5) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(png_path), "PNG"));
    const auto original_hash = file_sha256(png_path);
    auto imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto image_rgb = [](const QImage &source)
    {
        const QImage rgb = source.convertToFormat(QImage::Format_RGB888);
        const auto row_bytes = static_cast<std::size_t>(rgb.width()) * 3U;
        std::vector<std::uint8_t> result;
        result.reserve(row_bytes * static_cast<std::size_t>(rgb.height()));
        for (int y = 0; y < rgb.height(); ++y)
        {
            const auto *line = rgb.constScanLine(y);
            result.insert(result.end(), line, line + row_bytes);
        }
        return result;
    };

    PreviewRequest persisted;
    persisted.asset_id = asset_id;
    persisted.max_edge = 48;
    persisted.persist_preview_record = true;
    auto baseline = service->request_preview(persisted);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());
    const auto same_pixels = [](const QImage &left, const QImage &right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (int y = 0; y < left.height(); ++y)
        {
            for (int x = 0; x < left.width(); ++x)
            {
                if (left.pixel(x, y) != right.pixel(x, y))
                {
                    return false;
                }
            }
        }
        return true;
    };

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto logarithmic = develop_from_recipe(recipe.value());
    ASSERT_TRUE(logarithmic) << logarithmic.error().message;
    logarithmic.value().profile_gamma_enabled = true;
    logarithmic.value().profile_gamma.mode = std::string(kProfileGammaModeLogarithmic);
    logarithmic.value().profile_gamma.linear = 0.2;
    logarithmic.value().profile_gamma.gamma = 0.65;
    logarithmic.value().profile_gamma.dynamic_range = 8.5;
    logarithmic.value().profile_gamma.grey_point = 20.0;
    logarithmic.value().profile_gamma.shadows_range = -6.5;
    logarithmic.value().profile_gamma.security_factor = 12.0;
    clamp_develop(logarithmic.value());
    ASSERT_TRUE(service->save_develop(asset_id, logarithmic.value()));

    auto log_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(log_recipe) << log_recipe.error().message;
    const auto profile_gamma_operation =
        std::find_if(log_recipe.value().operations.begin(), log_recipe.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == kProfileGammaOperationId; });
    ASSERT_NE(profile_gamma_operation, log_recipe.value().operations.end());
    EXPECT_EQ(profile_gamma_operation->schema_version, kProfileGammaOperationSchemaVersion);
    EXPECT_TRUE(profile_gamma_operation->enabled);
    EXPECT_EQ(profile_gamma_operation->parameters.size(), 7U);

    auto log_persisted = service->request_preview(persisted);
    ASSERT_TRUE(log_persisted) << log_persisted.error().message;
    const QImage log_image(QString::fromStdString(log_persisted.value().cache_path));
    ASSERT_FALSE(log_image.isNull());
    EXPECT_NE(log_persisted.value().cache_key, baseline.value().cache_key);
    EXPECT_FALSE(same_pixels(log_image, baseline_image));

    PreviewRequest interactive = persisted;
    interactive.persist_preview_record = false;
    auto log_interactive = service->request_preview(interactive);
    ASSERT_TRUE(log_interactive) << log_interactive.error().message;
    ASSERT_FALSE(log_interactive.value().rgb.empty());
    EXPECT_EQ(log_interactive.value().rgb, image_rgb(log_image));

    ExportRequest log_export;
    log_export.asset_id = asset_id;
    log_export.output_path = (root / "profile-gamma-log.png").string();
    log_export.format = ExportFormat::kPng;
    log_export.max_edge = 48U;
    auto exported_log = service->export_asset(log_export);
    ASSERT_TRUE(exported_log) << exported_log.error().message;
    const QImage log_export_image(QString::fromStdString(log_export.output_path));
    ASSERT_FALSE(log_export_image.isNull());
    EXPECT_TRUE(same_pixels(log_export_image, log_image));

    auto gamma = logarithmic.value();
    gamma.profile_gamma.mode = std::string(kProfileGammaModeGamma);
    gamma.profile_gamma.linear = 0.12;
    gamma.profile_gamma.gamma = 0.72;
    clamp_develop(gamma);
    ASSERT_TRUE(service->save_develop(asset_id, gamma));

    auto gamma_persisted = service->request_preview(persisted);
    ASSERT_TRUE(gamma_persisted) << gamma_persisted.error().message;
    const QImage gamma_image(QString::fromStdString(gamma_persisted.value().cache_path));
    ASSERT_FALSE(gamma_image.isNull());
    EXPECT_NE(gamma_persisted.value().cache_key, log_persisted.value().cache_key);
    EXPECT_FALSE(same_pixels(gamma_image, log_image));

    auto gamma_interactive = service->request_preview(interactive);
    ASSERT_TRUE(gamma_interactive) << gamma_interactive.error().message;
    ASSERT_FALSE(gamma_interactive.value().rgb.empty());
    EXPECT_EQ(gamma_interactive.value().rgb, image_rgb(gamma_image));

    ExportRequest gamma_export;
    gamma_export.asset_id = asset_id;
    gamma_export.output_path = (root / "profile-gamma-gamma.png").string();
    gamma_export.format = ExportFormat::kPng;
    gamma_export.max_edge = 48U;
    auto exported_gamma = service->export_asset(gamma_export);
    ASSERT_TRUE(exported_gamma) << exported_gamma.error().message;
    const QImage gamma_export_image(QString::fromStdString(gamma_export.output_path));
    ASSERT_FALSE(gamma_export_image.isNull());
    EXPECT_TRUE(same_pixels(gamma_export_image, gamma_image));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().profile_gamma_enabled);
    EXPECT_EQ(restored.value().profile_gamma, gamma.profile_gamma);

    auto reopened_persisted = service->request_preview(persisted);
    ASSERT_TRUE(reopened_persisted) << reopened_persisted.error().message;
    const QImage reopened_image(QString::fromStdString(reopened_persisted.value().cache_path));
    ASSERT_FALSE(reopened_image.isNull());
    EXPECT_EQ(reopened_persisted.value().cache_key, gamma_persisted.value().cache_key);
    EXPECT_TRUE(same_pixels(reopened_image, gamma_image));

    auto reopened_interactive = service->request_preview(interactive);
    ASSERT_TRUE(reopened_interactive) << reopened_interactive.error().message;
    EXPECT_EQ(reopened_interactive.value().rgb, gamma_interactive.value().rgb);
    EXPECT_EQ(file_sha256(png_path), original_hash);
}

TEST_F(CatalogServiceTest, FileIccContentInvalidatesPreviewAndSurvivesRecipeReopen)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "profiled.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(180, 70, 30));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto profile_path = root / "input.icc";
    const auto write_profile = [&profile_path](const QColorSpace::NamedColorSpace named)
    {
        const QByteArray bytes = QColorSpace(named).iccProfile();
        QFile file(QString::fromStdString(profile_path.string()));
        return !bytes.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(bytes) == bytes.size();
    };
    ASSERT_TRUE(write_profile(QColorSpace::SRgb));

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto develop = develop_from_recipe(baseline.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().input_color.input_profile = std::string(kInputProfileFileIcc);
    develop.value().input_color.input_profile_filename = profile_path.string();
    auto saved = service->save_develop(asset_id, develop.value());
    ASSERT_TRUE(saved) << saved.error().message;

    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = 64;
    auto first = service->request_preview(request);
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(std::filesystem::exists(first.value().cache_path));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_EQ(restored_develop.value().input_color, develop.value().input_color);

    ASSERT_TRUE(write_profile(QColorSpace::DisplayP3));
    auto second = service->request_preview(request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second.value().cache_key, first.value().cache_key);
    EXPECT_NE(second.value().cache_path, first.value().cache_path);
    EXPECT_TRUE(std::filesystem::exists(first.value().cache_path));
    EXPECT_TRUE(std::filesystem::exists(second.value().cache_path));
}

TEST_F(CatalogServiceTest, OutputIccContentInvalidatesPreviewBeforeCachePublication)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "output-profiled.jpg").string();
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(170, 90, 35));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    const auto profile_path = root / "output.icc";
    const auto write_profile = [&profile_path](const QColorSpace::NamedColorSpace named)
    {
        const QByteArray bytes = QColorSpace(named).iccProfile();
        QFile file(QString::fromStdString(profile_path.string()));
        return !bytes.isEmpty() && file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(bytes) == bytes.size();
    };
    ASSERT_TRUE(write_profile(QColorSpace::SRgb));

    auto baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto develop = develop_from_recipe(baseline.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().output_color.output_profile = std::string(kInputProfileFileIcc);
    develop.value().output_color.output_profile_filename = profile_path.string();
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = 64;
    auto first = service->request_preview(request);
    ASSERT_TRUE(first) << first.error().message;
    const QImage first_image(QString::fromStdString(first.value().cache_path));
    ASSERT_FALSE(first_image.isNull());
    ASSERT_TRUE(first_image.colorSpace().isValid());
    EXPECT_EQ(first_image.colorSpace().iccProfile(), QColorSpace(QColorSpace::SRgb).iccProfile());

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_EQ(restored_develop.value().output_color, develop.value().output_color);

    ASSERT_TRUE(write_profile(QColorSpace::DisplayP3));
    auto second = service->request_preview(request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second.value().cache_key, first.value().cache_key);
    EXPECT_NE(second.value().cache_path, first.value().cache_path);
    const QImage second_image(QString::fromStdString(second.value().cache_path));
    ASSERT_FALSE(second_image.isNull());
    ASSERT_TRUE(second_image.colorSpace().isValid());
    EXPECT_EQ(second_image.colorSpace().iccProfile(),
              QColorSpace(QColorSpace::DisplayP3).iccProfile());

    QFile corrupt(QString::fromStdString(profile_path.string()));
    ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(corrupt.write("bad", 3), 3);
    corrupt.close();
    auto rejected = service->request_preview(request);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);
    EXPECT_TRUE(std::filesystem::exists(first.value().cache_path));
    EXPECT_TRUE(std::filesystem::exists(second.value().cache_path));
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
    EXPECT_FALSE(first.value().rgb.empty());
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
    EXPECT_EQ(first.value().rgb, direct.value().rgb);

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
    EXPECT_NE(second.value().rgb, first.value().rgb);

    live.exposure_ev = 0.75;
    auto third = service->request_preview(request, live);
    ASSERT_TRUE(third) << third.error().message;
    EXPECT_EQ(third.value().rgb, first.value().rgb);

    live.temperature.mode = std::string(kTemperatureModeManual);
    live.temperature.coefficients =
        std::array<double, kTemperatureChannelCount>{1.0, 1.0, 1.0, 1.0};
    auto balanced = service->request_preview(request, live);
    ASSERT_TRUE(balanced) << balanced.error().message;
    EXPECT_NE(balanced.value().rgb, first.value().rgb);
    ASSERT_TRUE(live.temperature.coefficients);
    (*live.temperature.coefficients)[0] += 0.25;
    auto rebalanced = service->request_preview(request, live);
    ASSERT_TRUE(rebalanced) << rebalanced.error().message;
    EXPECT_NE(rebalanced.value().rgb, balanced.value().rgb);

    live.raw_highlights = 1.0;
    auto highlighted = service->request_preview(request, live);
    ASSERT_TRUE(highlighted) << highlighted.error().message;
    EXPECT_EQ(highlighted.value().width, first.value().width);
    EXPECT_EQ(highlighted.value().height, first.value().height);
}

TEST_F(CatalogServiceTest, ExposureDeflickerPreviewPersistsReopensAndExportsIdenticalPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().cache_path.empty());
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().exposure_mode = std::string(kExposureModeDeflicker);
    develop.value().exposure_black = -0.01;
    develop.value().exposure_ev = 3.0;
    develop.value().exposure_deflicker_percentile = 65.0;
    develop.value().exposure_deflicker_target_ev = -3.5;
    develop.value().exposure_compensate_exposure_bias = true;
    develop.value().exposure_compensate_highlight_preservation = true;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    EXPECT_NE(before_reopen.value().cache_key, baseline.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, baseline_image);

    const auto export_path = (root / "exposure-deflicker-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    ASSERT_EQ(export_image.size(), before_reopen_image.size());
    for (int y = 0; y < export_image.height(); ++y)
    {
        for (int x = 0; x < export_image.width(); ++x)
        {
            EXPECT_EQ(export_image.pixel(x, y), before_reopen_image.pixel(x, y)) << x << ',' << y;
        }
    }

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().exposure_mode, develop.value().exposure_mode);
    EXPECT_DOUBLE_EQ(restored.value().exposure_black, develop.value().exposure_black);
    EXPECT_DOUBLE_EQ(restored.value().exposure_ev, develop.value().exposure_ev);
    EXPECT_DOUBLE_EQ(restored.value().exposure_deflicker_percentile,
                     develop.value().exposure_deflicker_percentile);
    EXPECT_DOUBLE_EQ(restored.value().exposure_deflicker_target_ev,
                     develop.value().exposure_deflicker_target_ev);
    EXPECT_EQ(restored.value().exposure_compensate_exposure_bias,
              develop.value().exposure_compensate_exposure_bias);
    EXPECT_EQ(restored.value().exposure_compensate_highlight_preservation,
              develop.value().exposure_compensate_highlight_preservation);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultLegacyColorBalancePersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_balance_enabled);
    develop.value().color_balance_enabled = true;
    develop.value().color_balance = ColorBalanceParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorBalanceOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorBalanceOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 19U);
    auto decoded = color_balance_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorBalanceParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "legacy-colorbalance-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    ASSERT_EQ(export_image.size(), before_reopen_image.size());
    for (int y = 0; y < export_image.height(); ++y)
    {
        for (int x = 0; x < export_image.width(); ++x)
        {
            EXPECT_EQ(export_image.pixel(x, y), before_reopen_image.pixel(x, y)) << x << ',' << y;
        }
    }

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_balance_enabled);
    EXPECT_EQ(restored.value().color_balance, ColorBalanceParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorCheckerPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_checker_enabled);
    develop.value().color_checker_enabled = true;
    develop.value().color_checker = ColorCheckerParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorCheckerOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorCheckerOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 3U);
    auto decoded = color_checker_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorCheckerParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorchecker-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_checker_enabled);
    EXPECT_EQ(restored.value().color_checker, ColorCheckerParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorCorrectionPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    ASSERT_FALSE(absent.value().cache_path.empty());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_correction_enabled);
    develop.value().color_correction_enabled = true;
    develop.value().color_correction = ColorCorrectionParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorCorrectionOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorCorrectionOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 7U);
    auto decoded = color_correction_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorCorrectionParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorcorrection-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_correction_enabled);
    EXPECT_EQ(restored.value().color_correction, ColorCorrectionParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorContrastPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    ASSERT_FALSE(absent.value().cache_path.empty());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_contrast_enabled);
    develop.value().color_contrast_enabled = true;
    develop.value().color_contrast = ColorContrastParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorContrastOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorContrastOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 7U);
    auto decoded = color_contrast_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorContrastParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorcontrast-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_contrast_enabled);
    EXPECT_EQ(restored.value().color_contrast, ColorContrastParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ExplicitDefaultColorHarmonizerPersistsReopensAndExportsExactPixels)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    ASSERT_FALSE(absent.value().cache_path.empty());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_harmonizer_enabled);
    develop.value().color_harmonizer_enabled = true;
    develop.value().color_harmonizer = ColorHarmonizerParams{};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorHarmonizerOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorHarmonizerOperationSchemaVersion);
    EXPECT_EQ(operation->parameters.size(), 17U);
    auto decoded = color_harmonizer_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), ColorHarmonizerParams{});

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    ASSERT_FALSE(before_reopen.value().cache_path.empty());
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());

    const auto export_path = (root / "colorharmonizer-default.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_harmonizer_enabled);
    EXPECT_EQ(restored.value().color_harmonizer, ColorHarmonizerParams{});
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
}

TEST_F(CatalogServiceTest, ColorReconstructionPersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_FALSE(develop.value().color_reconstruction_enabled);
    develop.value().color_reconstruction_enabled = true;
    develop.value().color_reconstruction = ColorReconstructionParams{
        60.0, 300.0, 10.0, 0.6600000262260437, ColorReconstructionPrecedence::kChroma};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kColorReconstructionOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_TRUE(operation->enabled);
    EXPECT_EQ(operation->schema_version, kColorReconstructionOperationSchemaVersion);
    auto decoded = color_reconstruction_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), develop.value().color_reconstruction);
    ASSERT_NE(std::next(operation), stored_recipe.value().operations.end());
    EXPECT_EQ(std::next(operation)->id, "ravo.color.output");

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "colorreconstruct.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_reconstruction_enabled);
    EXPECT_EQ(restored.value().color_reconstruction, develop.value().color_reconstruction);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, SourceExactSharpenPersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 128U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().sharpen = 1.0;
    develop.value().sharpen_radius = 99.0;
    develop.value().sharpen_threshold = 0.0;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kSharpenOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kSharpenOperationSchemaVersion);
    auto decoded = sharpen_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), (SharpenParams{99.0, 1.0, 0.0}));

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "sharpen.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 128U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().sharpen, 1.0);
    EXPECT_DOUBLE_EQ(restored.value().sharpen_radius, 99.0);
    EXPECT_DOUBLE_EQ(restored.value().sharpen_threshold, 0.0);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, DarkChannelDehazePersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 128U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().dehaze = 0.9;
    develop.value().dehaze_distance = 0.8;
    develop.value().dehaze_adaptive = false;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kDehazeOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kDehazeOperationSchemaVersion);
    auto decoded = dehaze_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), (DehazeParams{0.9, 0.8, false}));

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "dehaze.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 128U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_DOUBLE_EQ(restored.value().dehaze, 0.9);
    EXPECT_DOUBLE_EQ(restored.value().dehaze_distance, 0.8);
    EXPECT_FALSE(restored.value().dehaze_adaptive);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, OrderedRetouchPersistsReopensAndExportsExactPixels)
{
    const auto source_hash = file_sha256(raw_fixture_path());
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto absent = service->request_preview(preview);
    ASSERT_TRUE(absent) << absent.error().message;
    const QImage absent_image(QString::fromStdString(absent.value().cache_path));
    ASSERT_FALSE(absent_image.isNull());

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    Mask spot{"catalog-retouch-spot", kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    spot.payload = CircleMask{0.5, 0.5, 0.16, 0.04};
    develop.value().masks.push_back(spot);
    RetouchRegion region;
    region.mask_id = spot.id;
    region.mode = RetouchMode::kFill;
    region.opacity = 0.85;
    region.fill_mode = RetouchFillMode::kColor;
    region.fill_color = {0.9, 0.15, 0.05};
    region.fill_brightness = -0.03;
    develop.value().retouch.regions.push_back(region);
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    const auto operation = std::find_if(
        stored_recipe.value().operations.begin(), stored_recipe.value().operations.end(),
        [](const OperationInstance &item) { return item.id == kRetouchOperationId; });
    ASSERT_NE(operation, stored_recipe.value().operations.end());
    auto decoded = retouch_from_parameters(operation->parameters);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), develop.value().retouch);

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    EXPECT_NE(before_reopen.value().cache_key, absent.value().cache_key);
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, absent_image);

    const auto export_path = (root / "retouch.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().retouch, develop.value().retouch);
    EXPECT_EQ(restored.value().masks, develop.value().masks);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, PositiveColorHarmonizerSmoothingPersistsReopensAndExports)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    auto baseline_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    const auto baseline_serialized = serialize_recipe(baseline_recipe.value());
    ASSERT_TRUE(baseline_serialized) << baseline_serialized.error().message;
    auto develop = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    const DevelopParams before_reject = develop.value();
    EXPECT_FALSE(apply_develop_field_strict(develop.value(), "colorHarmonizerPullStrength", 1.5));
    EXPECT_EQ(develop.value(), before_reject);
    auto still_baseline = service->load_recipe(asset_id);
    ASSERT_TRUE(still_baseline) << still_baseline.error().message;
    auto still_serialized = serialize_recipe(still_baseline.value());
    ASSERT_TRUE(still_serialized) << still_serialized.error().message;
    EXPECT_EQ(still_serialized.value(), baseline_serialized.value());

    ColorHarmonizerParams edited;
    edited.rule = ColorHarmonizerRule::kSplitComplementary;
    edited.anchor_hue = 0.55000001192092896;
    edited.pull_strength = 0.81999999284744263;
    edited.pull_width = 1.8400000333786011;
    edited.node_saturation = {1.2599999904632568, 0.18000000715255737, 1.5199999809265137, 1.0};
    edited.smoothing = 0.5;
    develop.value().color_harmonizer_enabled = true;
    develop.value().color_harmonizer = edited;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64U;
    preview.persist_preview_record = true;
    preview.prefer_embedded_preview = false;
    auto live = service->request_preview(preview);
    ASSERT_TRUE(live) << live.error().message;
    const auto cache_key = live.value().cache_key;

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().color_harmonizer_enabled);
    EXPECT_EQ(restored.value().color_harmonizer, edited);
    auto after = service->request_preview(preview);
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().cache_key, cache_key);
    const auto export_path = (root / "colorharmonizer-0176.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 64U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    const QImage preview_image(QString::fromStdString(after.value().cache_path));
    ASSERT_FALSE(export_image.isNull());
    ASSERT_FALSE(preview_image.isNull());
    EXPECT_EQ(export_image, preview_image);
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
    edited.value().temperature = test::temperature_0000_params();
    edited.value().input_color.input_profile = std::string(kInputProfileEnhancedMatrix);
    edited.value().input_color.rendering_intent = std::string(kColorIntentRelative);
    edited.value().input_color.gamut_normalize = std::string(kColorNormalizeSrgb);
    edited.value().input_color.blue_mapping = true;
    edited.value().input_color.working_profile = std::string(kInputProfileLinearRec2020);
    edited.value().output_color.output_profile = std::string(kInputProfileDisplayP3);
    edited.value().output_color.rendering_intent = std::string(kColorIntentRelative);
    edited.value().color_balance_rgb = test::color_balance_0093_params();
    edited.value().straighten_degrees = 2.0;
    edited.value().perspective_vertical = 0.08;
    edited.value().perspective_horizontal = -0.05;
    edited.value().perspective_shear = 0.015;
    edited.value().perspective_constrain_crop = true;
    edited.value().perspective_interpolation_index = 2;
    clamp_develop(edited.value());

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 96;
    preview.persist_preview_record = false;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_FALSE(baseline.value().rgb.empty());

    auto saved = service->save_develop(asset_id, edited.value());
    ASSERT_TRUE(saved) << saved.error().message;
    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    ASSERT_FALSE(before_reopen.value().rgb.empty());
    EXPECT_EQ(before_reopen.value().color_profile.identifier, kInputProfileDisplayP3);
    EXPECT_FALSE(before_reopen.value().color_profile.icc_bytes.empty());
    EXPECT_NE(before_reopen.value().rgb, baseline.value().rgb);

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
    EXPECT_EQ(after_reopen.value().rgb, before_reopen.value().rgb);
    EXPECT_EQ(after_reopen.value().color_profile, before_reopen.value().color_profile);

    PreviewRequest settled = preview;
    settled.persist_preview_record = true;
    auto settled_preview = service->request_preview(settled);
    ASSERT_TRUE(settled_preview) << settled_preview.error().message;
    const auto export_path = (root / "migrated-develop-perspective.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = preview.max_edge;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage preview_image(QString::fromStdString(settled_preview.value().cache_path));
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(preview_image.isNull());
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(export_image, preview_image);
}

TEST_F(CatalogServiceTest, IgnoreStraightenRemovesCanonicalPerspectiveForAnalysis)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "tilt.jpg").string();
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(200, 80, 40));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    DevelopParams tilted;
    tilted.straighten_degrees = 20.0;
    tilted.perspective_vertical = 0.15;
    tilted.perspective_horizontal = -0.08;
    tilted.perspective_shear = 0.02;
    tilted.perspective_constrain_crop = false;
    PreviewRequest baked;
    baked.asset_id = asset_id;
    baked.max_edge = kInteractivePreviewMaxEdge;
    baked.persist_preview_record = false;
    auto straightened = service->request_preview(baked, tilted);
    ASSERT_TRUE(straightened) << straightened.error().message;
    ASSERT_FALSE(straightened.value().rgb.empty());
    PreviewRequest guide = baked;
    guide.ignore_straighten = true;
    auto unstraightened = service->request_preview(guide, tilted);
    ASSERT_TRUE(unstraightened) << unstraightened.error().message;
    ASSERT_FALSE(unstraightened.value().rgb.empty());
    EXPECT_EQ(unstraightened.value().width, 48U);
    EXPECT_EQ(unstraightened.value().height, 32U);
    EXPECT_TRUE(straightened.value().width != unstraightened.value().width ||
                straightened.value().height != unstraightened.value().height);
    EXPECT_LT(straightened.value().rgb[0] + straightened.value().rgb[1] +
                  straightened.value().rgb[2],
              unstraightened.value().rgb[0] + unstraightened.value().rgb[1] +
                  unstraightened.value().rgb[2]);
}

TEST_F(CatalogServiceTest, InvalidStoredRecipeFailsStructuredWithoutTouchingReview)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = (root / "bad-recipe.jpg").string();
    QImage image(16, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
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
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 120, 200));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 90));
    const auto original_hash = file_sha256(jpeg_path);
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    const auto export_profile_path = root / "export-display-p3.icc";
    const QByteArray export_profile = QColorSpace(QColorSpace::DisplayP3).iccProfile();
    ASSERT_FALSE(export_profile.isEmpty());
    QFile profile_file(QString::fromStdString(export_profile_path.string()));
    ASSERT_TRUE(profile_file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(profile_file.write(export_profile), export_profile.size());
    profile_file.close();
    DevelopParams params;
    params.exposure_ev = 0.5;
    params.output_color.output_profile = std::string(kInputProfileFileIcc);
    params.output_color.output_profile_filename = export_profile_path.string();
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
    EXPECT_TRUE(read_png.colorSpace().isValid());
    EXPECT_EQ(read_png.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
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
    jpeg.jpeg_options.quality = 85;
    auto exported_jpeg = service->export_asset(jpeg);
    ASSERT_TRUE(exported_jpeg) << exported_jpeg.error().message;
    EXPECT_TRUE(std::filesystem::exists(jpeg_out));
    const QImage read_jpeg(QString::fromStdString(jpeg_out));
    EXPECT_FALSE(read_jpeg.isNull());
    EXPECT_TRUE(read_jpeg.colorSpace().isValid());
    EXPECT_EQ(read_jpeg.colorSpace(), QColorSpace(QColorSpace::DisplayP3));

    const auto tiff_out = (root / "out.tif").string();
    ExportRequest tiff;
    tiff.asset_id = asset_id;
    tiff.output_path = tiff_out;
    tiff.format = ExportFormat::kTiff;
    auto exported_tiff = service->export_asset(tiff);
    if (exported_tiff)
    {
        EXPECT_TRUE(std::filesystem::exists(tiff_out));
        const QImage read_tiff(QString::fromStdString(tiff_out));
        EXPECT_FALSE(read_tiff.isNull());
        EXPECT_TRUE(read_tiff.colorSpace().isValid());
        EXPECT_EQ(read_tiff.colorSpace(), QColorSpace(QColorSpace::DisplayP3));
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
    bad_quality.jpeg_options.quality = 0;
    auto invalid_quality = service->export_asset(bad_quality);
    ASSERT_FALSE(invalid_quality);
    EXPECT_EQ(invalid_quality.error().code, ErrorCode::kValidation);
    EXPECT_FALSE(std::filesystem::exists(bad_quality.output_path));

    ExportRequest missing_directory = png;
    missing_directory.output_path = (root / "missing" / "out.png").string();
    auto missing_result = service->export_asset(missing_directory);
    ASSERT_FALSE(missing_result);
    EXPECT_EQ(missing_result.error().code, ErrorCode::kIo);
    EXPECT_FALSE(std::filesystem::exists(missing_directory.output_path));
}

TEST_F(CatalogServiceTest, BatchExportExpandsInOrderAndPreflightsEveryConflict)
{
    ASSERT_TRUE(open_service(true));
    const auto import_image = [this](const std::string &name, const QColor color)
    {
        const auto path = (root / name).string();
        QImage image(20, 12, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(color);
        EXPECT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        EXPECT_TRUE(imported) << imported.error().message;
        EXPECT_TRUE(imported.value().asset.has_value());
        return imported.value().asset->id;
    };
    const auto first_id = import_image("batch one.jpg", QColor(200, 20, 30));
    const auto second_id = import_image("batch two.jpg", QColor(20, 200, 30));
    const auto output_directory = root / "batch-output";
    std::filesystem::create_directory(output_directory);

    ExportBatchRequest request;
    request.asset_ids = {first_id, second_id};
    request.output_directory = output_directory.string();
    request.filename_template = "{sequence}-{stem}{ext}";
    request.options.format = ExportFormat::kPng;
    std::vector<std::size_t> progress_indices;
    auto exported = service->export_assets(request,
                                           [&progress_indices](const std::size_t current,
                                                               const std::size_t total,
                                                               const ExportResult *result)
                                           {
                                               EXPECT_EQ(total, 2U);
                                               ASSERT_NE(result, nullptr);
                                               progress_indices.push_back(current);
                                           });
    ASSERT_TRUE(exported) << exported.error().message;
    ASSERT_EQ(exported.value().size(), 2U);
    EXPECT_EQ(progress_indices, (std::vector<std::size_t>{1U, 2U}));
    EXPECT_EQ(std::filesystem::path(exported.value()[0].output_path).filename(),
              "0001-batch one.png");
    EXPECT_EQ(std::filesystem::path(exported.value()[1].output_path).filename(),
              "0002-batch two.png");
    EXPECT_FALSE(QImage(QString::fromStdString(exported.value()[0].output_path)).isNull());
    EXPECT_FALSE(QImage(QString::fromStdString(exported.value()[1].output_path)).isNull());

    const auto conflict_directory = root / "batch-conflict";
    std::filesystem::create_directory(conflict_directory);
    const auto conflict_path = conflict_directory / "0002-batch two.png";
    {
        std::ofstream sentinel(conflict_path, std::ios::binary);
        sentinel << "keep-existing";
    }
    const auto conflict_hash = file_sha256(conflict_path.string());
    request.output_directory = conflict_directory.string();
    auto conflict = service->export_assets(request);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, ErrorCode::kConflict);
    EXPECT_EQ(conflict.error().context.at("completed_count"), "0");
    EXPECT_EQ(conflict.error().context.at("partial_batch"), "false");
    EXPECT_FALSE(std::filesystem::exists(conflict_directory / "0001-batch one.png"));
    EXPECT_EQ(file_sha256(conflict_path.string()), conflict_hash);

    request.output_directory = (root / "batch-duplicate-name").string();
    std::filesystem::create_directory(request.output_directory);
    request.filename_template = "same{ext}";
    auto duplicate = service->export_assets(request);
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_export_output");
    EXPECT_TRUE(std::filesystem::is_empty(request.output_directory));
}

TEST_F(CatalogServiceTest, BatchExportCancellationReportsStablePartialDelivery)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> asset_ids;
    for (int index = 0; index < 2; ++index)
    {
        const auto path = (root / ("cancel-batch-" + std::to_string(index + 1) + ".jpg")).string();
        QImage image(20, 12, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(index == 0 ? QColor(200, 20, 30) : QColor(20, 200, 30));
        ASSERT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_TRUE(imported.value().asset);
        asset_ids.push_back(imported.value().asset->id);
    }
    const auto output_directory = root / "cancel-batch-output";
    std::filesystem::create_directory(output_directory);
    CancellationSource cancellation;
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = output_directory.string();
    request.options.format = ExportFormat::kJpeg;
    request.cancellation = cancellation.token();
    auto exported = service->export_assets(
        request,
        [&cancellation](const std::size_t current, const std::size_t, const ExportResult *)
        {
            if (current == 1U)
                EXPECT_TRUE(cancellation.cancel("after-first-batch-output"));
        });
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(exported.error().context.at("completed_count"), "1");
    EXPECT_EQ(exported.error().context.at("partial_batch"), "true");
    EXPECT_EQ(exported.error().context.at("reason"), "after-first-batch-output");
    EXPECT_TRUE(std::filesystem::exists(output_directory / "cancel-batch-1-0001.jpg"));
    EXPECT_FALSE(std::filesystem::exists(output_directory / "cancel-batch-2-0002.jpg"));
}

TEST_F(CatalogServiceTest, BatchExportRuntimeFailureNamesAlreadyDeliveredOutputs)
{
    ASSERT_TRUE(open_service(true));
    std::vector<std::string> asset_ids;
    std::vector<std::string> source_paths;
    for (int index = 0; index < 2; ++index)
    {
        const auto path = (root / ("failure-batch-" + std::to_string(index + 1) + ".jpg")).string();
        QImage image(20, 12, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(index == 0 ? QColor(200, 20, 30) : QColor(20, 200, 30));
        ASSERT_TRUE(image.save(QString::fromStdString(path), "JPEG", 90));
        auto imported = service->import_one(path, CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_TRUE(imported.value().asset);
        asset_ids.push_back(imported.value().asset->id);
        source_paths.push_back(path);
    }
    const auto output_directory = root / "failure-batch-output";
    std::filesystem::create_directory(output_directory);
    ExportBatchRequest request;
    request.asset_ids = asset_ids;
    request.output_directory = output_directory.string();
    request.options.format = ExportFormat::kPng;
    auto exported = service->export_assets(
        request,
        [&source_paths](const std::size_t current, const std::size_t, const ExportResult *)
        {
            if (current == 1U)
                EXPECT_TRUE(std::filesystem::remove(source_paths[1]));
        });
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kNotFound);
    EXPECT_EQ(exported.error().context.at("completed_count"), "1");
    EXPECT_EQ(exported.error().context.at("partial_batch"), "true");
    EXPECT_EQ(exported.error().context.at("batch_index"), "2");
    EXPECT_TRUE(std::filesystem::exists(output_directory / "failure-batch-1-0001.png"));
    EXPECT_FALSE(std::filesystem::exists(output_directory / "failure-batch-2-0002.png"));
    EXPECT_TRUE(std::filesystem::exists(source_paths[0]));
}

TEST_F(CatalogServiceTest, OutputDitherPersistsRebuildsAndExportsTheDisplayedPixels)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "output-dither-source.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int row = 0; row < image.height(); ++row)
    {
        for (int column = 0; column < image.width(); ++column)
        {
            image.setPixelColor(column, row,
                                QColor((column * 23 + row * 7) % 256,
                                       (column * 11 + row * 29) % 256,
                                       (column * 31 + row * 3) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    DevelopParams develop;
    develop.output_dither_present = true;
    develop.output_dither_enabled = true;
    develop.output_dither = {OutputDitherMethod::kPosterize4, -100.0};
    ASSERT_TRUE(service->save_develop(asset_id, develop));

    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    auto preview = service->request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    QImage preview_image(QString::fromStdString(preview.value().cache_path));
    ASSERT_FALSE(preview_image.isNull());
    const auto export_path = (root / "output-dither-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(preview_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(std::filesystem::remove(preview.value().cache_path));
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_TRUE(restored_develop.value().output_dither_enabled);
    EXPECT_EQ(restored_develop.value().output_dither.method, OutputDitherMethod::kPosterize4);
    auto rebuilt = service->request_preview(preview_request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    QImage rebuilt_image(QString::fromStdString(rebuilt.value().cache_path));
    ASSERT_FALSE(rebuilt_image.isNull());
    EXPECT_EQ(rebuilt_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

TEST_F(CatalogServiceTest, VelviaPersistsRebuildsAndExportsTheDisplayedPixels)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "velvia-source.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int row = 0; row < image.height(); ++row)
    {
        for (int column = 0; column < image.width(); ++column)
        {
            image.setPixelColor(column, row,
                                QColor((column * 23 + row * 7) % 256,
                                       (column * 11 + row * 29) % 256,
                                       (column * 31 + row * 3) % 256));
        }
    }
    ASSERT_TRUE(image.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    auto baseline = service->request_preview(preview_request);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());

    DevelopParams develop;
    develop.velvia_present = true;
    develop.velvia_enabled = true;
    develop.velvia = {100.0, 0.15};
    ASSERT_TRUE(service->save_develop(asset_id, develop));
    auto preview = service->request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    const QImage preview_image(QString::fromStdString(preview.value().cache_path));
    ASSERT_FALSE(preview_image.isNull());
    EXPECT_NE(preview.value().cache_key, baseline.value().cache_key);
    EXPECT_NE(preview_image, baseline_image);

    const auto export_path = (root / "velvia-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage export_image(QString::fromStdString(export_path));
    ASSERT_FALSE(export_image.isNull());
    EXPECT_EQ(preview_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(std::filesystem::remove(preview.value().cache_path));
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto restored_develop = develop_from_recipe(restored.value());
    ASSERT_TRUE(restored_develop) << restored_develop.error().message;
    EXPECT_TRUE(restored_develop.value().velvia_present);
    EXPECT_TRUE(restored_develop.value().velvia_enabled);
    EXPECT_EQ(restored_develop.value().velvia, (VelviaParams{100.0, 0.15}));
    auto rebuilt = service->request_preview(preview_request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    const QImage rebuilt_image(QString::fromStdString(rebuilt.value().cache_path));
    ASSERT_FALSE(rebuilt_image.isNull());
    EXPECT_EQ(rebuilt_image, export_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

TEST_F(CatalogServiceTest, CanvasColorZonesMonochromeSplitFrameWatermarkPersistExactPixels)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "canvas-frame-source.png").string();
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 120, 160));
    ASSERT_TRUE(image.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    DevelopParams develop;
    develop.canvas_present = true;
    develop.canvas_enabled = true;
    develop.canvas = {25.0, 25.0, 25.0, 25.0, CanvasColor::kBlue};
    develop.frame_present = true;
    develop.frame_enabled = true;
    develop.frame.size = 0.1;
    develop.frame.border_color = {1.0, 1.0, 1.0};
    develop.watermark_present = true;
    develop.watermark_enabled = true;
    develop.watermark.text = "A";
    develop.watermark.color = {0.0, 0.0, 0.0};
    develop.watermark.opacity = 1.0;
    develop.watermark.scale_percent = 50.0;
    develop.watermark.alignment = WatermarkAlignment::kTopLeft;
    develop.color_zones_present = true;
    develop.color_zones_enabled = true;
    for (auto &point : develop.color_zones.curves[1].points)
        point.y = 0.75;
    develop.monochrome_present = true;
    develop.monochrome_enabled = true;
    develop.monochrome = {20.0, 10.0, 2.0, 0.25, 1.0};
    develop.split_toning_present = true;
    develop.split_toning_enabled = true;
    develop.split_toning = {0.34, 0.9, 0.93, 0.9, 0.35, 15.0, 1.0};
    ASSERT_TRUE(service->save_develop(asset_id, develop));
    PreviewRequest preview_request;
    preview_request.asset_id = asset_id;
    auto preview = service->request_preview(preview_request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().width, 20U);
    EXPECT_EQ(preview.value().height, 14U);
    QImage preview_image(QString::fromStdString(preview.value().cache_path));
    ASSERT_FALSE(preview_image.isNull());

    ExportRequest request;
    request.asset_id = asset_id;
    request.output_path = (root / "canvas-frame-export.png").string();
    request.format = ExportFormat::kPng;
    auto exported = service->export_asset(request);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported.value().width, 20U);
    EXPECT_EQ(exported.value().height, 14U);
    QImage exported_image(QString::fromStdString(request.output_path));
    EXPECT_EQ(exported_image, preview_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
    for (const auto &[format, filename] :
         {std::pair{ExportFormat::kJpeg, std::string("canvas-frame-export.jpg")},
          std::pair{ExportFormat::kTiff, std::string("canvas-frame-export.tif")}})
    {
        ExportRequest additional = request;
        additional.format = format;
        additional.output_path = (root / filename).string();
        auto result = service->export_asset(additional);
        ASSERT_TRUE(result) << filename << ": " << result.error().message;
        EXPECT_EQ(result.value().width, 20U);
        EXPECT_EQ(result.value().height, 14U);
        const QImage decoded(QString::fromStdString(additional.output_path));
        ASSERT_FALSE(decoded.isNull()) << filename;
        EXPECT_EQ(decoded.width(), 20);
        EXPECT_EQ(decoded.height(), 14);
    }

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(std::filesystem::remove(preview.value().cache_path));
    ASSERT_TRUE(open_service(false));
    auto restored = service->load_recipe(asset_id);
    ASSERT_TRUE(restored) << restored.error().message;
    auto params = develop_from_recipe(restored.value());
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_TRUE(params.value().canvas_enabled);
    EXPECT_TRUE(params.value().frame_enabled);
    EXPECT_TRUE(params.value().watermark_enabled);
    EXPECT_EQ(params.value().watermark.text, "A");
    EXPECT_TRUE(params.value().color_zones_enabled);
    EXPECT_DOUBLE_EQ(params.value().color_zones.curves[1].points[0].y, 0.75);
    EXPECT_TRUE(params.value().monochrome_enabled);
    EXPECT_DOUBLE_EQ(params.value().monochrome.highlights, 0.25);
    EXPECT_TRUE(params.value().split_toning_enabled);
    EXPECT_DOUBLE_EQ(params.value().split_toning.compress, 15.0);
    EXPECT_EQ(params.value().canvas.color, CanvasColor::kBlue);
    auto rebuilt = service->request_preview(preview_request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_EQ(QImage(QString::fromStdString(rebuilt.value().cache_path)), exported_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
}

[[nodiscard]] std::vector<std::uint8_t> make_synthetic_capture_exif_tiff()
{
    return {
        0x49U, 0x49U, 0x2AU, 0x00U, 0x08U, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x69U, 0x87U, 0x04U,
        0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x26U, 0x00U, 0x00U, 0x00U, 0x25U, 0x88U, 0x04U, 0x00U,
        0x01U, 0x00U, 0x00U, 0x00U, 0x6CU, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U,
        0x00U, 0x03U, 0x90U, 0x02U, 0x00U, 0x14U, 0x00U, 0x00U, 0x00U, 0x50U, 0x00U, 0x00U, 0x00U,
        0x11U, 0x90U, 0x02U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x64U, 0x00U, 0x00U, 0x00U, 0x91U,
        0x92U, 0x02U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x31U, 0x38U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x32U, 0x30U, 0x30U, 0x37U, 0x3AU, 0x30U, 0x39U, 0x3AU, 0x31U, 0x31U, 0x20U,
        0x31U, 0x33U, 0x3AU, 0x35U, 0x33U, 0x3AU, 0x33U, 0x33U, 0x00U, 0x2BU, 0x30U, 0x32U, 0x3AU,
        0x30U, 0x30U, 0x00U, 0x00U, 0x07U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x04U, 0x00U, 0x00U,
        0x00U, 0x02U, 0x03U, 0x00U, 0x00U, 0x01U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U,
        0x4EU, 0x00U, 0x00U, 0x00U, 0x02U, 0x00U, 0x05U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0xC6U,
        0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x45U, 0x00U,
        0x00U, 0x00U, 0x04U, 0x00U, 0x05U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0xDEU, 0x00U, 0x00U,
        0x00U, 0x05U, 0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x06U, 0x00U, 0x05U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0xF6U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x31U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x0FU, 0x00U,
        0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0xDFU, 0x71U, 0x00U, 0x00U, 0xC4U, 0x09U, 0x00U,
        0x00U, 0x03U, 0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x03U, 0x00U, 0x00U, 0x00U,
        0x01U, 0x00U, 0x00U, 0x00U, 0xEEU, 0x1AU, 0x00U, 0x00U, 0xC4U, 0x09U, 0x00U, 0x00U, 0x48U,
        0x3CU, 0x00U, 0x00U, 0x7DU, 0x00U, 0x00U, 0x00U};
}

[[nodiscard]] std::string write_synthetic_jpeg_with_capture(const std::filesystem::path &path)
{
    QImage image(8, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 40, 60));
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    EXPECT_TRUE(image.save(&buffer, "JPEG", 90));
    EXPECT_GE(jpeg.size(), 2);
    EXPECT_EQ(static_cast<unsigned char>(jpeg[0]), 0xFF);
    EXPECT_EQ(static_cast<unsigned char>(jpeg[1]), 0xD8);
    const auto tiff = make_synthetic_capture_exif_tiff();
    QByteArray app1;
    app1.append(static_cast<char>(0xFF));
    app1.append(static_cast<char>(0xE1));
    const auto payload = 2 + 6 + static_cast<int>(tiff.size());
    app1.append(static_cast<char>((payload >> 8) & 0xFF));
    app1.append(static_cast<char>(payload & 0xFF));
    app1.append("Exif", 4);
    app1.append('\0');
    app1.append('\0');
    app1.append(reinterpret_cast<const char *>(tiff.data()), static_cast<int>(tiff.size()));
    jpeg.insert(2, app1);
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(jpeg), jpeg.size());
    file.close();
    return path.string();
}

TEST_F(CatalogServiceTest, ImportsMire1AsLocalCaptureWithoutOffsetOrGps)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto source_hash = file_sha256(raw_fixture_path());
    const auto source_mtime = std::filesystem::last_write_time(raw_fixture_path());
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime);
    EXPECT_EQ(imported.value().asset->capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime->subsecond_digits);
    EXPECT_EQ(*imported.value().asset->capture.captured_datetime->subsecond_digits, "18");
    EXPECT_FALSE(imported.value().asset->capture.captured_datetime->utc_offset_minutes);
    EXPECT_FALSE(imported.value().asset->capture.location);
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
    EXPECT_EQ(std::filesystem::last_write_time(raw_fixture_path()), source_mtime);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    ASSERT_TRUE(listed.value().front().capture.captured_datetime);
    EXPECT_EQ(listed.value().front().capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    EXPECT_EQ(*listed.value().front().capture.captured_datetime->subsecond_digits, "18");
    EXPECT_FALSE(listed.value().front().capture.captured_datetime->utc_offset_minutes);
    EXPECT_FALSE(listed.value().front().capture.location);

    auto before_duplicate = service->snapshot();
    ASSERT_TRUE(before_duplicate);
    auto duplicate = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(duplicate) << duplicate.error().message;
    EXPECT_EQ(duplicate.value().status, ImportItemStatus::kDuplicate);
    EXPECT_EQ(duplicate.value().asset->id, imported.value().asset->id);
    EXPECT_EQ(duplicate.value().asset->capture.captured_datetime,
              imported.value().asset->capture.captured_datetime);
    EXPECT_EQ(duplicate.value().asset->capture.location, imported.value().asset->capture.location);
    auto after_duplicate = service->snapshot();
    ASSERT_TRUE(after_duplicate);
    EXPECT_EQ(after_duplicate.value().revision, before_duplicate.value().revision);
}

TEST_F(CatalogServiceTest, ImportsSyntheticJpegCaptureTimeAndGps)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "zoned.jpg");
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime);
    EXPECT_EQ(imported.value().asset->capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    EXPECT_EQ(*imported.value().asset->capture.captured_datetime->subsecond_digits, "18");
    ASSERT_TRUE(imported.value().asset->capture.captured_datetime->utc_offset_minutes);
    EXPECT_EQ(*imported.value().asset->capture.captured_datetime->utc_offset_minutes, 120);
    ASSERT_TRUE(imported.value().asset->capture.location);
    EXPECT_EQ(imported.value().asset->capture.location->latitude_e6, 49253239);
    EXPECT_EQ(imported.value().asset->capture.location->longitude_e6, 3050766);
    ASSERT_TRUE(imported.value().asset->capture.location->altitude);
    EXPECT_EQ(imported.value().asset->capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(imported.value().asset->capture.location->altitude->reference,
              CaptureAltitudeReference::kAboveSeaLevel);
}

void replace_synthetic_capture_year(const std::string &path, const QByteArray &year)
{
    QFile file(QString::fromStdString(path));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    file.close();
    const auto position = bytes.indexOf("2007:09:11 13:53:33");
    ASSERT_GE(position, 0);
    ASSERT_EQ(year.size(), 4);
    bytes.replace(position, 4, year);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
}

TEST_F(CatalogServiceTest, RefreshCaptureMetadataPublishesSourceChangesAtomically)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "refresh-capture.jpg");
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    replace_synthetic_capture_year(jpeg_path, QByteArray("2008"));

    auto refreshed = service->refresh_capture_metadata(asset_id, CancellationToken{});
    ASSERT_TRUE(refreshed) << refreshed.error().message;
    ASSERT_TRUE(refreshed.value().capture.captured_datetime);
    EXPECT_EQ(refreshed.value().capture.captured_datetime->local_exif, "2008:09:11 13:53:33");
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision + 1);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    ASSERT_TRUE(listed.value().front().capture.captured_datetime);
    EXPECT_EQ(listed.value().front().capture.captured_datetime->local_exif, "2008:09:11 13:53:33");
}

TEST_F(CatalogServiceTest, RefreshFailurePreservesCaptureAndRevision)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "refresh-rollback.jpg");
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    replace_synthetic_capture_year(jpeg_path, QByteArray("2009"));

    {
        const auto connection = QStringLiteral("ravo_refresh_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TRIGGER fail_refresh_revision BEFORE UPDATE OF revision ON schema_info "
            "BEGIN SELECT RAISE(ABORT, 'forced refresh revision failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }
    auto refreshed = service->refresh_capture_metadata(asset_id, CancellationToken{});
    ASSERT_FALSE(refreshed);
    EXPECT_EQ(refreshed.error().code, ErrorCode::kIo);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    ASSERT_TRUE(listed.value().front().capture.captured_datetime);
    EXPECT_EQ(listed.value().front().capture.captured_datetime->local_exif, "2007:09:11 13:53:33");
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("refresh-cancelled"));
    auto cancelled_refresh = service->refresh_capture_metadata(asset_id, cancelled.token());
    ASSERT_FALSE(cancelled_refresh);
    EXPECT_EQ(cancelled_refresh.error().code, ErrorCode::kCancelled);
}

TEST_F(CatalogServiceTest, MigratesV4CatalogLeavingNewCaptureColumnsNull)
{
    {
        const auto connection = QStringLiteral("ravo_v4_seed");
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
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL, "
            "rating INTEGER NOT NULL DEFAULT 0, color_label TEXT NOT NULL DEFAULT 'none', "
            "rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_recipe ("
                           "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
                           "  recipe_schema_version INTEGER NOT NULL,"
                           "  recipe_json TEXT NOT NULL,"
                           "  updated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE preview (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "contract_version INTEGER NOT NULL, cache_key TEXT NOT NULL, width INTEGER, "
            "height INTEGER, state TEXT NOT NULL, cache_relpath TEXT, last_success_unix_ms INTEGER)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_tag ("
                           "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
                           "  name TEXT NOT NULL, PRIMARY KEY (asset_id, name))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "title TEXT, description TEXT, creator TEXT, copyright TEXT, camera_make TEXT, "
            "camera_model TEXT, iso REAL, aperture REAL, focal_length_mm REAL, shutter_s REAL, "
            "captured_unix_s INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recipe_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, seq INTEGER NOT NULL, "
            "kind TEXT NOT NULL, label TEXT, recipe_json TEXT NOT NULL, created_unix_ms INTEGER NOT NULL, "
            "UNIQUE(asset_id, seq))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 4, 'cat_v4', 3, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, media_type, size_bytes, mtime_unix_ms, "
            "import_state, created_unix_ms) VALUES ('ast_old', 'file:///tmp/old.png', "
            "'image/png', 12, 1, 'imported', 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset_metadata(asset_id, captured_unix_s) VALUES ('ast_old', 1189514013)")));
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
    EXPECT_EQ(listed.value().front().capture.captured_unix_s, 1189514013);
    EXPECT_FALSE(listed.value().front().capture.captured_datetime);
    EXPECT_FALSE(listed.value().front().capture.location);
}

TEST_F(CatalogServiceTest, RepairsPreAdrV5CatalogsThatUsedSignedAltitudeMm)
{
    {
        const auto connection = QStringLiteral("ravo_v5_signed_altitude");
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
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL, "
            "rating INTEGER NOT NULL DEFAULT 0, color_label TEXT NOT NULL DEFAULT 'none', "
            "rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_recipe ("
                           "  asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE,"
                           "  recipe_schema_version INTEGER NOT NULL,"
                           "  recipe_json TEXT NOT NULL,"
                           "  updated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE preview (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "contract_version INTEGER NOT NULL, cache_key TEXT NOT NULL, width INTEGER, "
            "height INTEGER, state TEXT NOT NULL, cache_relpath TEXT, last_success_unix_ms INTEGER)")));
        ASSERT_TRUE(query.exec(
            QStringLiteral("CREATE TABLE asset_tag ("
                           "  asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE,"
                           "  name TEXT NOT NULL, PRIMARY KEY (asset_id, name))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY REFERENCES asset(id) ON DELETE CASCADE, "
            "title TEXT, description TEXT, creator TEXT, copyright TEXT, camera_make TEXT, "
            "camera_model TEXT, iso REAL, aperture REAL, focal_length_mm REAL, shutter_s REAL, "
            "captured_unix_s INTEGER, captured_local_exif TEXT, captured_subsecond_digits TEXT, "
            "captured_utc_offset_minutes INTEGER, gps_latitude_e6 INTEGER, gps_longitude_e6 INTEGER, "
            "gps_altitude_mm INTEGER)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_recipe_history (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "asset_id TEXT NOT NULL REFERENCES asset(id) ON DELETE CASCADE, seq INTEGER NOT NULL, "
            "kind TEXT NOT NULL, label TEXT, recipe_json TEXT NOT NULL, created_unix_ms INTEGER NOT NULL, "
            "UNIQUE(asset_id, seq))")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 5, 'cat_v5_signed', 8, 1, 1)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset(id, normalized_uri, media_type, size_bytes, mtime_unix_ms, "
            "import_state, created_unix_ms) VALUES "
            "('ast_above', 'file:///tmp/above.png', 'image/png', 12, 1, 'imported', 1), "
            "('ast_below', 'file:///tmp/below.png', 'image/png', 12, 1, 'imported', 2)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO asset_metadata(asset_id, gps_latitude_e6, gps_longitude_e6, gps_altitude_mm) "
            "VALUES ('ast_above', 49253239, 3050766, 123456), "
            "('ast_below', 1000000, 2000000, -2500)")));
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
    ASSERT_TRUE(listed)
        << listed.error().message << " action="
        << (listed.error().context.contains("action") ? listed.error().context.at("action") : "")
        << " qt="
        << (listed.error().context.contains("qt_error") ? listed.error().context.at("qt_error") :
                                                          "");
    ASSERT_EQ(listed.value().size(), 2U);
    const AssetRecord *above = nullptr;
    const AssetRecord *below = nullptr;
    for (const auto &asset : listed.value())
    {
        if (asset.id == "ast_above")
            above = &asset;
        if (asset.id == "ast_below")
            below = &asset;
    }
    ASSERT_NE(above, nullptr);
    ASSERT_NE(below, nullptr);
    ASSERT_TRUE(above->capture.location);
    ASSERT_TRUE(above->capture.location->altitude);
    EXPECT_EQ(above->capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(above->capture.location->altitude->reference,
              CaptureAltitudeReference::kAboveSeaLevel);
    ASSERT_TRUE(below->capture.location);
    ASSERT_TRUE(below->capture.location->altitude);
    EXPECT_EQ(below->capture.location->altitude->magnitude_mm, 2500U);
    EXPECT_EQ(below->capture.location->altitude->reference,
              CaptureAltitudeReference::kBelowSeaLevel);

    ASSERT_TRUE(service->close());
    service.reset();
    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 2U);
}

TEST_F(CatalogServiceTest, V5MigrationFailureRollsBackUnchangedV4)
{
    {
        const auto connection = QStringLiteral("ravo_v4_conflict");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
            "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
            "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset (id TEXT PRIMARY KEY, normalized_uri TEXT NOT NULL UNIQUE, "
            "media_type TEXT NOT NULL, size_bytes INTEGER NOT NULL, mtime_unix_ms INTEGER NOT NULL, "
            "content_fingerprint TEXT, width INTEGER, height INTEGER, import_state TEXT NOT NULL, "
            "error_code TEXT, error_message TEXT, created_unix_ms INTEGER NOT NULL, "
            "rating INTEGER NOT NULL DEFAULT 0, color_label TEXT NOT NULL DEFAULT 'none', "
            "rejected INTEGER NOT NULL DEFAULT 0)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY, captured_unix_s INTEGER, "
            "captured_local_exif TEXT)")));
        ASSERT_TRUE(query.exec(QStringLiteral(
            "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
            "migrated_unix_ms) VALUES (1, 4, 'cat_v4_conflict', 2, 1, 1)")));
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto opened = open_service(false);
    ASSERT_FALSE(opened);
    EXPECT_EQ(opened.error().code, ErrorCode::kIo);

    const auto connection = QStringLiteral("ravo_v4_conflict_check");
    auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    database.setDatabaseName(QString::fromStdString(database_path));
    ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
    QSqlQuery query(database);
    ASSERT_TRUE(query.exec(
        QStringLiteral("SELECT schema_version, revision FROM schema_info WHERE id = 1")));
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toLongLong(), 4);
    EXPECT_EQ(query.value(1).toLongLong(), 2);
    database.close();
    database = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection);
}

TEST_F(CatalogServiceTest, EveryV5MigrationPublicationFailureRestoresTheV4Catalog)
{
    enum class FailureKind
    {
        kAlter,
        kSchemaInfo,
        kCommit,
    };
    struct FailureCase
    {
        const char *name;
        FailureKind kind;
        const char *conflicting_column;
        const char *conflicting_type;
    };
    const std::array<FailureCase, 9> cases{{
        {"captured-local", FailureKind::kAlter, "captured_local_exif", "TEXT"},
        {"captured-subsecond", FailureKind::kAlter, "captured_subsecond_digits", "TEXT"},
        {"captured-offset", FailureKind::kAlter, "captured_utc_offset_minutes", "INTEGER"},
        {"gps-latitude", FailureKind::kAlter, "gps_latitude_e6", "INTEGER"},
        {"gps-longitude", FailureKind::kAlter, "gps_longitude_e6", "INTEGER"},
        {"gps-altitude", FailureKind::kAlter, "gps_altitude_magnitude_mm", "INTEGER"},
        {"gps-altitude-ref", FailureKind::kAlter, "gps_altitude_ref", "INTEGER"},
        {"schema-info", FailureKind::kSchemaInfo, nullptr, nullptr},
        {"commit", FailureKind::kCommit, nullptr, nullptr},
    }};
    const std::array<std::string, 7> capture_columns{
        "captured_local_exif", "captured_subsecond_digits", "captured_utc_offset_minutes",
        "gps_latitude_e6",     "gps_longitude_e6",          "gps_altitude_magnitude_mm",
        "gps_altitude_ref",
    };

    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const auto &test_case = cases[index];
        const auto path =
            (root / (std::string("migration-") + test_case.name + ".sqlite")).string();
        const QString seed_connection = QStringLiteral("ravo_v5_failure_seed_%1").arg(index);
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seed_connection);
            database.setDatabaseName(QString::fromStdString(path));
            ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
            QSqlQuery query(database);
            ASSERT_TRUE(query.exec(QStringLiteral(
                "CREATE TABLE schema_info (id INTEGER PRIMARY KEY CHECK (id = 1), "
                "schema_version INTEGER NOT NULL, catalog_id TEXT NOT NULL, revision INTEGER NOT NULL, "
                "created_unix_ms INTEGER NOT NULL, migrated_unix_ms INTEGER NOT NULL)")));
            QString metadata = QStringLiteral(
                "CREATE TABLE asset_metadata (asset_id TEXT PRIMARY KEY, captured_unix_s INTEGER");
            if (test_case.kind == FailureKind::kAlter)
            {
                metadata +=
                    QStringLiteral(", %1 %2").arg(QString::fromLatin1(test_case.conflicting_column),
                                                  QString::fromLatin1(test_case.conflicting_type));
            }
            metadata += QLatin1Char(')');
            ASSERT_TRUE(query.exec(metadata)) << query.lastError().text().toStdString();
            ASSERT_TRUE(query.exec(QStringLiteral(
                "INSERT INTO schema_info(id, schema_version, catalog_id, revision, created_unix_ms, "
                "migrated_unix_ms) VALUES (1, 4, 'cat_v4_failure', 7, 11, 13)")));
            ASSERT_TRUE(query.exec(QStringLiteral(
                "INSERT INTO asset_metadata(asset_id, captured_unix_s) VALUES ('ast_old', 123)")));
            if (test_case.kind == FailureKind::kSchemaInfo)
            {
                ASSERT_TRUE(query.exec(QStringLiteral(
                    "CREATE TRIGGER reject_schema_info BEFORE UPDATE OF schema_version ON schema_info "
                    "BEGIN SELECT RAISE(ABORT, 'forced schema-info failure'); END")));
            }
            if (test_case.kind == FailureKind::kCommit)
            {
                ASSERT_TRUE(query.exec(
                    QStringLiteral("CREATE TABLE migration_parent(id INTEGER PRIMARY KEY)")));
                ASSERT_TRUE(query.exec(QStringLiteral(
                    "CREATE TABLE migration_child(parent_id INTEGER, FOREIGN KEY(parent_id) "
                    "REFERENCES migration_parent(id) DEFERRABLE INITIALLY DEFERRED)")));
                ASSERT_TRUE(query.exec(QStringLiteral(
                    "CREATE TRIGGER reject_migration_commit AFTER UPDATE OF schema_version ON "
                    "schema_info BEGIN INSERT INTO migration_child(parent_id) VALUES (99); END")));
            }
            database.close();
            database = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(seed_connection);

        auto opened = SqliteCatalogRepository::open(path);
        ASSERT_FALSE(opened) << test_case.name;
        EXPECT_EQ(opened.error().code, ErrorCode::kIo) << test_case.name;

        const QString check_connection = QStringLiteral("ravo_v5_failure_check_%1").arg(index);
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), check_connection);
            database.setDatabaseName(QString::fromStdString(path));
            ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
            QSqlQuery query(database);
            ASSERT_TRUE(query.exec(QStringLiteral(
                "SELECT schema_version, revision, created_unix_ms, migrated_unix_ms FROM "
                "schema_info WHERE id = 1")));
            ASSERT_TRUE(query.next());
            EXPECT_EQ(query.value(0).toLongLong(), 4) << test_case.name;
            EXPECT_EQ(query.value(1).toLongLong(), 7) << test_case.name;
            EXPECT_EQ(query.value(2).toLongLong(), 11) << test_case.name;
            EXPECT_EQ(query.value(3).toLongLong(), 13) << test_case.name;
            ASSERT_TRUE(query.exec(QStringLiteral("SELECT captured_unix_s FROM asset_metadata "
                                                  "WHERE asset_id = 'ast_old'")));
            ASSERT_TRUE(query.next());
            EXPECT_EQ(query.value(0).toLongLong(), 123) << test_case.name;
            ASSERT_TRUE(query.exec(QStringLiteral("PRAGMA table_info(asset_metadata)")));
            std::vector<std::string> columns;
            while (query.next())
            {
                columns.push_back(query.value(1).toString().toStdString());
            }
            for (const auto &column : capture_columns)
            {
                const bool existed_before =
                    test_case.kind == FailureKind::kAlter && column == test_case.conflicting_column;
                EXPECT_EQ(std::find(columns.begin(), columns.end(), column) != columns.end(),
                          existed_before)
                    << test_case.name << ": " << column;
            }
            if (test_case.kind == FailureKind::kCommit)
            {
                ASSERT_TRUE(query.exec(QStringLiteral("SELECT COUNT(*) FROM migration_child")));
                ASSERT_TRUE(query.next());
                EXPECT_EQ(query.value(0).toLongLong(), 0) << test_case.name;
            }
            database.close();
            database = QSqlDatabase();
        }
        QSqlDatabase::removeDatabase(check_connection);
    }
}

TEST_F(CatalogServiceTest, V6RecoveryMigrationFailureRollsBackTheV5Catalog)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    AssetRecord asset;
    asset.id = "ast_v5_recovery";
    asset.normalized_uri = "file:///tmp/v5-recovery.png";
    asset.media_type = "image/png";
    asset.size_bytes = 12U;
    asset.mtime_unix_ms = 1;
    asset.created_unix_ms = 2;
    ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();

    const auto seed_connection = QStringLiteral("ravo_v6_failure_seed");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), seed_connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT name FROM sqlite_master WHERE type = 'trigger' AND instr(name, 'recovery') > 0")));
        std::vector<QString> recovery_triggers;
        while (query.next())
        {
            recovery_triggers.push_back(query.value(0).toString());
        }
        ASSERT_FALSE(recovery_triggers.empty());
        for (const auto &trigger : recovery_triggers)
        {
            ASSERT_TRUE(query.exec(QStringLiteral("DROP TRIGGER \"%1\"").arg(trigger)))
                << query.lastError().text().toStdString();
        }
        ASSERT_TRUE(query.exec(QStringLiteral("DROP TABLE asset_recovery_state")))
            << query.lastError().text().toStdString();
        ASSERT_TRUE(query.exec(QStringLiteral(
            "UPDATE schema_info SET schema_version = 5, migrated_unix_ms = 1 WHERE id = 1")))
            << query.lastError().text().toStdString();
        ASSERT_TRUE(
            query.exec(QStringLiteral("CREATE TRIGGER asset_recovery_insert AFTER INSERT ON asset "
                                      "BEGIN SELECT 1; END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(seed_connection);

    auto failed = SqliteCatalogRepository::open(database_path);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kIo);
    EXPECT_EQ(failed.error().context.at("action"), "migrate_v6_recovery_trigger");

    const auto check_connection = QStringLiteral("ravo_v6_failure_check");
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), check_connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(query.exec(QStringLiteral(
            "SELECT schema_version, revision, migrated_unix_ms FROM schema_info WHERE id = 1")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 5);
        EXPECT_EQ(query.value(1).toLongLong(), 1);
        EXPECT_EQ(query.value(2).toLongLong(), 1);
        ASSERT_TRUE(
            query.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' "
                                      "AND name = 'asset_recovery_state'")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 0);
        ASSERT_TRUE(
            query.exec(QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE type = 'trigger' "
                                      "AND name = 'asset_recovery_insert'")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 1);
        ASSERT_TRUE(query.exec(QStringLiteral("SELECT COUNT(*) FROM asset")));
        ASSERT_TRUE(query.next());
        EXPECT_EQ(query.value(0).toLongLong(), 1);
        ASSERT_TRUE(query.exec(QStringLiteral("DROP TRIGGER asset_recovery_insert")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
    }
    QSqlDatabase::removeDatabase(check_connection);

    auto reopened = open_service(false);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;
    EXPECT_EQ(snapshot.value().schema_version, kCatalogSchemaVersion);
    EXPECT_EQ(snapshot.value().revision, 1);
    auto recovery = service->recovery_state(asset.id);
    ASSERT_TRUE(recovery) << recovery.error().message;
    EXPECT_EQ(recovery.value().generation, 1);
    EXPECT_EQ(recovery.value().synchronized_generation, 1);
}

TEST_F(CatalogServiceTest, CaptureRowFailureRollsBackInvisibleAsset)
{
    auto created = open_service(true);
    ASSERT_TRUE(created) << created.error().message;
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before) << snapshot_before.error().message;
    {
        const auto connection = QStringLiteral("ravo_capture_failure_injection");
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        ASSERT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        ASSERT_TRUE(
            query.exec(QStringLiteral("CREATE TRIGGER fail_capture BEFORE INSERT ON asset_metadata "
                                      "BEGIN SELECT RAISE(ABORT, 'forced capture failure'); END")))
            << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    }

    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(imported.value().asset);
    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().empty());
    auto snapshot_after = service->snapshot();
    ASSERT_TRUE(snapshot_after) << snapshot_after.error().message;
    EXPECT_EQ(snapshot_after.value().revision, snapshot_before.value().revision);
}

TEST_F(CatalogServiceTest, RejectsPartialAndOutOfRangePersistedCaptureCoordinates)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->close());
    service.reset();

    const auto write_capture_sql = [&](const QString &connection, const QString &statement)
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        EXPECT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery query(database);
        query.prepare(statement);
        query.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    };

    write_capture_sql(
        QStringLiteral("ravo_partial_capture_state"),
        QStringLiteral(
            "UPDATE asset_metadata SET gps_latitude_e6 = 1, gps_longitude_e6 = NULL, "
            "gps_altitude_magnitude_mm = NULL, gps_altitude_ref = NULL WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    auto listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_location");
    auto closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_location");
    service.reset();

    write_capture_sql(
        QStringLiteral("ravo_oversized_capture_state"),
        QStringLiteral("UPDATE asset_metadata SET gps_latitude_e6 = 9223372036854775807, "
                       "gps_longitude_e6 = 0 WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_integer");
    EXPECT_EQ(listed.error().context.at("field"), "gps_latitude_e6");
    closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_integer");
}

TEST_F(CatalogServiceTest, RejectsWrongStorageClassesAndPartialDatetimeAltitude)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->close());
    service.reset();

    const auto write_sql = [&](const QString &connection, const QString &statement)
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setDatabaseName(QString::fromStdString(database_path));
        EXPECT_TRUE(database.open()) << database.lastError().text().toStdString();
        QSqlQuery ensure(database);
        ensure.prepare(QStringLiteral("INSERT OR IGNORE INTO asset_metadata(asset_id) VALUES (?)"));
        ensure.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(ensure.exec()) << ensure.lastError().text().toStdString();
        QSqlQuery reset(database);
        reset.prepare(QStringLiteral(
            "UPDATE asset_metadata SET captured_local_exif = NULL, "
            "captured_subsecond_digits = NULL, captured_utc_offset_minutes = NULL, "
            "gps_latitude_e6 = NULL, gps_longitude_e6 = NULL, "
            "gps_altitude_magnitude_mm = NULL, gps_altitude_ref = NULL WHERE asset_id = ?"));
        reset.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(reset.exec()) << reset.lastError().text().toStdString();
        QSqlQuery query(database);
        query.prepare(statement);
        query.addBindValue(QString::fromStdString(asset_id));
        EXPECT_TRUE(query.exec()) << query.lastError().text().toStdString();
        database.close();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
    };

    struct WrongStorageCase
    {
        const char *connection;
        const char *field;
        const char *statement;
    };
    const std::array<WrongStorageCase, 7U> wrong_storage{{
        {"ravo_blob_in_local", "captured_local_exif",
         "UPDATE asset_metadata SET captured_local_exif = X'31' WHERE asset_id = ?"},
        {"ravo_blob_in_subsecond", "captured_subsecond_digits",
         "UPDATE asset_metadata SET captured_subsecond_digits = X'31' WHERE asset_id = ?"},
        {"ravo_text_in_offset", "captured_utc_offset_minutes",
         "UPDATE asset_metadata SET captured_utc_offset_minutes = 'bad' WHERE asset_id = ?"},
        {"ravo_text_in_latitude", "gps_latitude_e6",
         "UPDATE asset_metadata SET gps_latitude_e6 = 'north' WHERE asset_id = ?"},
        {"ravo_text_in_longitude", "gps_longitude_e6",
         "UPDATE asset_metadata SET gps_longitude_e6 = 'east' WHERE asset_id = ?"},
        {"ravo_text_in_altitude", "gps_altitude_magnitude_mm",
         "UPDATE asset_metadata SET gps_altitude_magnitude_mm = 'high' WHERE asset_id = ?"},
        {"ravo_text_in_altitude_ref", "gps_altitude_ref",
         "UPDATE asset_metadata SET gps_altitude_ref = 'below' WHERE asset_id = ?"},
    }};
    for (const auto &test_case : wrong_storage)
    {
        write_sql(QString::fromUtf8(test_case.connection), QString::fromUtf8(test_case.statement));
        ASSERT_TRUE(open_service(false, false));
        auto listed = service->list_assets();
        ASSERT_FALSE(listed) << test_case.field;
        EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_storage_class")
            << test_case.field;
        EXPECT_EQ(listed.error().context.at("field"), test_case.field);
        auto closed = service->close();
        ASSERT_FALSE(closed);
        EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_storage_class");
        service.reset();
    }

    write_sql(QStringLiteral("ravo_real_in_int"),
              QStringLiteral("UPDATE asset_metadata SET gps_latitude_e6 = 1.5 WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    auto listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_storage_class");
    auto closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_storage_class");
    service.reset();

    write_sql(QStringLiteral("ravo_partial_datetime"),
              QStringLiteral("UPDATE asset_metadata SET gps_latitude_e6 = NULL, "
                             "gps_longitude_e6 = NULL, captured_local_exif = NULL, "
                             "captured_subsecond_digits = '18' WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_datetime");
    closed = service->close();
    ASSERT_FALSE(closed);
    EXPECT_EQ(closed.error().context.at("reason"), "invalid_persisted_capture_datetime");
    service.reset();

    write_sql(QStringLiteral("ravo_partial_altitude"),
              QStringLiteral("UPDATE asset_metadata SET captured_subsecond_digits = NULL, "
                             "gps_latitude_e6 = 1, gps_longitude_e6 = 2, "
                             "gps_altitude_magnitude_mm = 0, gps_altitude_ref = NULL "
                             "WHERE asset_id = ?"));
    ASSERT_TRUE(open_service(false, false));
    listed = service->list_assets();
    ASSERT_FALSE(listed);
    EXPECT_EQ(listed.error().context.at("reason"), "invalid_persisted_capture_altitude");
}

TEST_F(CatalogServiceTest, ReopensZeroOffsetCoordinatesAndBothZeroAltitudeReferencesExactly)
{
    auto repository = SqliteCatalogRepository::create(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    for (const auto reference :
         {CaptureAltitudeReference::kAboveSeaLevel, CaptureAltitudeReference::kBelowSeaLevel})
    {
        AssetRecord asset;
        asset.id = reference == CaptureAltitudeReference::kAboveSeaLevel ? "ast_zero_above" :
                                                                           "ast_zero_below";
        asset.normalized_uri = "file:///tmp/" + asset.id + ".tif";
        asset.media_type = "image/tiff";
        asset.size_bytes = 3U;
        asset.mtime_unix_ms = 1;
        asset.created_unix_ms = 2;
        asset.capture.captured_datetime = CaptureDateTime{"2007:09:11 13:53:33", std::nullopt, 0};
        asset.capture.location = CaptureLocation{0, 0, CaptureAltitude{0U, reference}};
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    }
    auto snapshot = repository.value()->snapshot();
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().revision, 2);
    ASSERT_TRUE(repository.value()->close());
    repository = SqliteCatalogRepository::open(database_path);
    ASSERT_TRUE(repository) << repository.error().message;
    auto assets = repository.value()->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 2U);
    for (const auto &asset : assets.value())
    {
        ASSERT_TRUE(asset.capture.captured_datetime);
        ASSERT_TRUE(asset.capture.captured_datetime->utc_offset_minutes);
        EXPECT_EQ(*asset.capture.captured_datetime->utc_offset_minutes, 0);
        ASSERT_TRUE(asset.capture.location);
        EXPECT_EQ(asset.capture.location->latitude_e6, 0);
        EXPECT_EQ(asset.capture.location->longitude_e6, 0);
        ASSERT_TRUE(asset.capture.location->altitude);
        EXPECT_EQ(asset.capture.location->altitude->magnitude_mm, 0U);
        const auto expected = asset.id == "ast_zero_above" ?
                                  CaptureAltitudeReference::kAboveSeaLevel :
                                  CaptureAltitudeReference::kBelowSeaLevel;
        EXPECT_EQ(asset.capture.location->altitude->reference, expected);
    }
}

TEST_F(CatalogServiceTest, ImportInjectionMatrixLeavesNoVisibleAsset)
{
    ASSERT_TRUE(open_service(true));
    auto snapshot_before = service->snapshot();
    ASSERT_TRUE(snapshot_before);
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "inject.jpg");
    const std::array<testing::SqliteImportFailure, 10> failures{
        testing::SqliteImportFailure::kTransactionBegin,
        testing::SqliteImportFailure::kAssetBind,
        testing::SqliteImportFailure::kAssetWrite,
        testing::SqliteImportFailure::kCaptureBind,
        testing::SqliteImportFailure::kCaptureWrite,
        testing::SqliteImportFailure::kRevisionUpdate,
        testing::SqliteImportFailure::kRevisionRead,
        testing::SqliteImportFailure::kCommit,
        testing::SqliteImportFailure::kRollback,
        testing::SqliteImportFailure::kNone,
    };
    for (const auto failure : failures)
    {
        if (failure == testing::SqliteImportFailure::kNone)
        {
            continue;
        }
        ASSERT_NE(sqlite_repository, nullptr);
        testing::SqliteCatalogTestControl::inject(*sqlite_repository, failure);
        auto imported = service->import_one(jpeg_path, CancellationToken{});
        ASSERT_TRUE(imported) << static_cast<int>(failure);
        EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
        EXPECT_FALSE(imported.value().asset);
        auto listed = service->list_assets();
        ASSERT_TRUE(listed) << listed.error().message;
        EXPECT_TRUE(listed.value().empty());
        auto snapshot = service->snapshot();
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot.value().revision, snapshot_before.value().revision);
        if (failure == testing::SqliteImportFailure::kRollback)
        {
            ASSERT_TRUE(imported.value().error);
            EXPECT_EQ(imported.value().error->context.at("rollback_failed"), "true");
            EXPECT_EQ(imported.value().error->context.at("rollback_error"),
                      "injected_import_rollback");
        }
        ASSERT_TRUE(service->close());
        service.reset();
        sqlite_repository = nullptr;
        ASSERT_TRUE(open_service(false));
        auto reopened = service->list_assets();
        ASSERT_TRUE(reopened) << reopened.error().message;
        EXPECT_TRUE(reopened.value().empty());
    }
    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto reopened = service->list_assets();
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(reopened.value().empty());
}

TEST_F(CatalogServiceTest, CancellationAfterExifReadPreventsPublication)
{
    ASSERT_TRUE(open_service(true));
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "cancel-after-exif.jpg");
    CancellationSource cancellation;
    testing::CatalogServiceTestControl::set_before_import_publication(
        *service, [&cancellation] { EXPECT_TRUE(cancellation.cancel("after-exif")); });
    auto imported = service->import_one(jpeg_path, cancellation.token());
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    ASSERT_TRUE(imported.value().error);
    EXPECT_EQ(imported.value().error->code, ErrorCode::kCancelled);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);
}

[[nodiscard]] std::string write_png_with_exif_payload(const std::filesystem::path &path,
                                                      const std::vector<std::uint8_t> &tiff,
                                                      const bool bad_crc = false,
                                                      const bool duplicate = false,
                                                      const bool empty = false,
                                                      const bool jpeg_prefix = false)
{
    QImage image(8, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(10, 20, 30));
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    EXPECT_TRUE(image.save(&buffer, "PNG"));
    std::vector<std::uint8_t> payload;
    if (jpeg_prefix)
    {
        payload.insert(payload.end(), {'E', 'x', 'i', 'f', 0, 0});
    }
    if (!empty)
    {
        payload.insert(payload.end(), tiff.begin(), tiff.end());
    }
    const auto append_exif = [&](QByteArray &out)
    {
        const auto length = static_cast<std::uint32_t>(payload.size());
        unsigned char header[8] = {static_cast<unsigned char>(length >> 24U),
                                   static_cast<unsigned char>(length >> 16U),
                                   static_cast<unsigned char>(length >> 8U),
                                   static_cast<unsigned char>(length),
                                   'e',
                                   'X',
                                   'I',
                                   'f'};
        out.append(reinterpret_cast<const char *>(header), 8);
        if (!payload.empty())
        {
            out.append(reinterpret_cast<const char *>(payload.data()),
                       static_cast<int>(payload.size()));
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef *>("eXIf"), 4);
        if (!payload.empty())
        {
            crc = crc32(crc, payload.data(), static_cast<uInt>(payload.size()));
        }
        auto stored = static_cast<std::uint32_t>(crc);
        if (bad_crc)
        {
            stored ^= 1U;
        }
        const unsigned char crc_bytes[4] = {
            static_cast<unsigned char>(stored >> 24U), static_cast<unsigned char>(stored >> 16U),
            static_cast<unsigned char>(stored >> 8U), static_cast<unsigned char>(stored)};
        out.append(reinterpret_cast<const char *>(crc_bytes), 4);
    };
    QByteArray rebuilt;
    rebuilt.append(png.left(8));
    qsizetype offset = 8;
    bool inserted = false;
    while (offset + 12 <= png.size())
    {
        const auto length =
            (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset])) << 24U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 1])) << 16U) |
            (static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 2])) << 8U) |
            static_cast<std::uint32_t>(static_cast<unsigned char>(png[offset + 3]));
        const QByteArray type = png.mid(offset + 4, 4);
        if (!inserted && type == "IDAT")
        {
            append_exif(rebuilt);
            if (duplicate)
            {
                append_exif(rebuilt);
            }
            inserted = true;
        }
        rebuilt.append(png.mid(offset, 12 + static_cast<qsizetype>(length)));
        offset += 12 + static_cast<qsizetype>(length);
    }
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(rebuilt), rebuilt.size());
    file.close();
    return path.string();
}

TEST_F(CatalogServiceTest, ImportsPngAndTiffCaptureContainersIndependently)
{
    ASSERT_TRUE(open_service(true));
    const auto tiff = make_synthetic_capture_exif_tiff();
    const auto png_path = write_png_with_exif_payload(root / "located.png", tiff);
    const auto png_hash = file_sha256(png_path);
    auto png_imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(png_imported) << png_imported.error().message;
    ASSERT_TRUE(png_imported.value().asset);
    ASSERT_TRUE(png_imported.value().asset->capture.location);
    EXPECT_EQ(png_imported.value().asset->capture.location->latitude_e6, 49253239);
    ASSERT_TRUE(png_imported.value().asset->capture.location->altitude);
    EXPECT_EQ(png_imported.value().asset->capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(file_sha256(png_path), png_hash);

    const auto standalone_tiff = root / "independent-located.tif";
    const auto tiff_bytes = test_support::make_capture_exif_tiff();
    {
        std::ofstream output(standalone_tiff, std::ios::binary);
        output.write(reinterpret_cast<const char *>(tiff_bytes.data()),
                     static_cast<std::streamsize>(tiff_bytes.size()));
    }
    const auto tiff_hash = file_sha256(standalone_tiff.string());
    auto tiff_imported = service->import_one(standalone_tiff.string(), CancellationToken{});
    ASSERT_TRUE(tiff_imported) << tiff_imported.error().message;
    ASSERT_TRUE(tiff_imported.value().asset)
        << (tiff_imported.value().error ? tiff_imported.value().error->message : "");
    ASSERT_TRUE(tiff_imported.value().asset->capture.location);
    EXPECT_EQ(tiff_imported.value().asset->capture.location->longitude_e6, 3050766);
    EXPECT_EQ(file_sha256(standalone_tiff.string()), tiff_hash);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_EQ(listed.value().size(), 2U);
}

[[nodiscard]] std::string write_truncated_raster(const std::filesystem::path &path,
                                                 const char *format)
{
    QImage image(12, 8, QImage::Format_RGB888);
    image.fill(QColor(40, 80, 120));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    QByteArray encoded;
    QBuffer buffer(&encoded);
    EXPECT_TRUE(buffer.open(QIODevice::WriteOnly));
    EXPECT_TRUE(image.save(&buffer, format));
    EXPECT_GT(encoded.size(), 16);
    encoded.chop(8);
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(encoded), encoded.size());
    return path.string();
}

[[nodiscard]] std::string write_float_rgb_tiff(const std::filesystem::path &path)
{
    QByteArray encoded;
    const auto append_u16 = [&](const std::uint16_t value)
    {
        encoded.append(static_cast<char>(value & 0xFFU));
        encoded.append(static_cast<char>((value >> 8U) & 0xFFU));
    };
    const auto append_u32 = [&](const std::uint32_t value)
    {
        encoded.append(static_cast<char>(value & 0xFFU));
        encoded.append(static_cast<char>((value >> 8U) & 0xFFU));
        encoded.append(static_cast<char>((value >> 16U) & 0xFFU));
        encoded.append(static_cast<char>((value >> 24U) & 0xFFU));
    };
    encoded.append("II*\0", 4);
    append_u32(8U);
    append_u16(11U);
    const auto entry = [&](const std::uint16_t tag, const std::uint16_t type,
                           const std::uint32_t count, const std::uint32_t value)
    {
        append_u16(tag);
        append_u16(type);
        append_u32(count);
        append_u32(value);
    };
    entry(256U, 4U, 1U, 1U);
    entry(257U, 4U, 1U, 1U);
    entry(258U, 3U, 3U, 146U);
    entry(259U, 3U, 1U, 1U);
    entry(262U, 3U, 1U, 2U);
    entry(273U, 4U, 1U, 158U);
    entry(277U, 3U, 1U, 3U);
    entry(278U, 4U, 1U, 1U);
    entry(279U, 4U, 1U, 12U);
    entry(284U, 3U, 1U, 1U);
    entry(339U, 3U, 3U, 152U);
    append_u32(0U);
    append_u16(32U);
    append_u16(32U);
    append_u16(32U);
    append_u16(3U);
    append_u16(3U);
    append_u16(3U);
    encoded.append(12, '\0');
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::WriteOnly));
    EXPECT_EQ(file.write(encoded), encoded.size());
    return path.string();
}

TEST_F(CatalogServiceTest, CorruptPngAndTiffNeverPublishAnAssetOrPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto png_path = write_truncated_raster(root / "truncated.png", "PNG");
    const auto png_hash = file_sha256(png_path);
    auto png_imported = service->import_one(png_path, CancellationToken{});
    ASSERT_TRUE(png_imported) << png_imported.error().message;
    EXPECT_EQ(png_imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(png_imported.value().asset);
    EXPECT_FALSE(png_imported.value().preview_cache_path);
    ASSERT_TRUE(png_imported.value().error);
    EXPECT_EQ(png_imported.value().error->code, ErrorCode::kValidation);
    EXPECT_EQ(png_imported.value().error->context.at("format"), "png");
    EXPECT_EQ(file_sha256(png_path), png_hash);

    const auto tiff_path = write_truncated_raster(root / "truncated.tif", "TIFF");
    const auto tiff_hash = file_sha256(tiff_path);
    auto tiff_imported = service->import_one(tiff_path, CancellationToken{});
    ASSERT_TRUE(tiff_imported) << tiff_imported.error().message;
    EXPECT_EQ(tiff_imported.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(tiff_imported.value().asset);
    EXPECT_FALSE(tiff_imported.value().preview_cache_path);
    ASSERT_TRUE(tiff_imported.value().error);
    EXPECT_EQ(tiff_imported.value().error->code, ErrorCode::kValidation);
    EXPECT_EQ(tiff_imported.value().error->context.at("format"), "tiff");
    EXPECT_EQ(file_sha256(tiff_path), tiff_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
    auto previews = service->list_previews();
    ASSERT_TRUE(previews) << previews.error().message;
    EXPECT_TRUE(previews.value().empty());
}

TEST_F(CatalogServiceTest, RecognizedTiffLayoutsDoNotStealRawRouting)
{
    ASSERT_TRUE(open_service(true));
    const auto float_path = write_float_rgb_tiff(root / "float.tif");
    const auto float_hash = file_sha256(float_path);
    auto floating = service->import_one(float_path, CancellationToken{});
    ASSERT_TRUE(floating) << floating.error().message;
    EXPECT_EQ(floating.value().status, ImportItemStatus::kUnsupported);
    EXPECT_FALSE(floating.value().asset);
    ASSERT_TRUE(floating.value().error);
    EXPECT_EQ(floating.value().error->code, ErrorCode::kUnsupported);
    EXPECT_EQ(floating.value().error->context.at("format"), "tiff");
    EXPECT_EQ(floating.value().error->context.at("reason"), "unsupported_tiff_float_samples");
    EXPECT_EQ(file_sha256(float_path), float_hash);

    const auto arw = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" /
                     "hlrecovery.arw";
    const auto disguised = root / "camera.tif";
    std::filesystem::copy_file(arw, disguised);
    const auto disguised_hash = file_sha256(disguised.string());
    auto imported = service->import_one(disguised.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : "");
    EXPECT_EQ(imported.value().status, ImportItemStatus::kImported);
    EXPECT_EQ(imported.value().asset->media_type, kMediaTypeRaw);
    EXPECT_EQ(file_sha256(disguised.string()), disguised_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 1U);
    EXPECT_EQ(assets.value().front().media_type, kMediaTypeRaw);
}

TEST_F(CatalogServiceTest, CorruptAndUnrecognizedRawNeverPublishAnAsset)
{
    ASSERT_TRUE(open_service(true));
    const auto missing = service->import_one((root / "missing.cr2").string(), CancellationToken{});
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_EQ(missing.value().status, ImportItemStatus::kFailed);
    EXPECT_FALSE(missing.value().asset);
    ASSERT_TRUE(missing.value().error);
    EXPECT_EQ(missing.value().error->code, ErrorCode::kNotFound);

    const auto garbage_path = root / "notes.cr2";
    {
        std::ofstream output(garbage_path, std::ios::binary);
        output << "not a camera raw";
    }
    const auto garbage_hash = file_sha256(garbage_path.string());
    auto garbage = service->import_one(garbage_path.string(), CancellationToken{});
    ASSERT_TRUE(garbage) << garbage.error().message;
    EXPECT_TRUE(garbage.value().status == ImportItemStatus::kUnsupported ||
                garbage.value().status == ImportItemStatus::kFailed);
    EXPECT_FALSE(garbage.value().asset);
    EXPECT_EQ(file_sha256(garbage_path.string()), garbage_hash);

    const auto truncated_path = root / "truncated.cr2";
    {
        std::ifstream input(std::filesystem::path(raw_fixture_path()), std::ios::binary);
        std::ofstream output(truncated_path, std::ios::binary);
        std::vector<char> prefix(1024);
        input.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
        output.write(prefix.data(), input.gcount());
    }
    const auto truncated_hash = file_sha256(truncated_path.string());
    auto truncated = service->import_one(truncated_path.string(), CancellationToken{});
    ASSERT_TRUE(truncated) << truncated.error().message;
    EXPECT_TRUE(truncated.value().status == ImportItemStatus::kUnsupported ||
                truncated.value().status == ImportItemStatus::kFailed);
    EXPECT_FALSE(truncated.value().asset);
    EXPECT_EQ(file_sha256(truncated_path.string()), truncated_hash);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
}

TEST_F(CatalogServiceTest, DngSuffixImportsAsRawWithoutRewritingTheSource)
{
    ASSERT_TRUE(open_service(true));
    const auto dng_path = root / "camera.dng";
    std::filesystem::copy_file(raw_fixture_path(), dng_path);
    const auto dng_hash = file_sha256(dng_path.string());
    auto imported = service->import_one(dng_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : "");
    EXPECT_EQ(imported.value().status, ImportItemStatus::kImported);
    EXPECT_EQ(imported.value().asset->media_type, kMediaTypeRaw);
    EXPECT_GT(imported.value().asset->width.value_or(0), 0U);
    EXPECT_GT(imported.value().asset->height.value_or(0), 0U);
    EXPECT_EQ(file_sha256(dng_path.string()), dng_hash);
    ASSERT_TRUE(imported.value().preview_cache_path);
    EXPECT_TRUE(std::filesystem::exists(*imported.value().preview_cache_path));
}

TEST_F(CatalogServiceTest, XTransImportsAndPublishesAnEngineRenderedPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto path = xtrans_fixture_path();
    const auto hash = file_sha256(path);
    auto decoded = engine.decode_raw_frame(path, CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value().cfa_width, 6U);
    EXPECT_EQ(decoded.value().cfa_height, 6U);

    auto imported = service->import_one(path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(file_sha256(path), hash);
    ASSERT_TRUE(imported.value().asset);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kImported);
    EXPECT_EQ(imported.value().asset->media_type, kMediaTypeRaw);
    ASSERT_TRUE(imported.value().preview_cache_path);
    EXPECT_TRUE(std::filesystem::exists(*imported.value().preview_cache_path));

    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    request.max_edge = 320U;
    auto preview = service->request_preview(request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_EQ(preview.value().width, 320U);
    EXPECT_GT(preview.value().height, 0U);
    EXPECT_TRUE(std::filesystem::exists(preview.value().cache_path));
}

TEST_F(CatalogServiceTest, CancellationAfterRawInspectPreventsPublication)
{
    ASSERT_TRUE(open_service(true));
    auto before = service->snapshot();
    ASSERT_TRUE(before);
    CancellationSource cancellation;
    testing::CatalogServiceTestControl::set_before_import_publication(
        *service, [&cancellation] { EXPECT_TRUE(cancellation.cancel("after-raw-inspect")); });
    auto imported = service->import_one(raw_fixture_path(), cancellation.token());
    ASSERT_TRUE(imported);
    EXPECT_EQ(imported.value().status, ImportItemStatus::kFailed);
    ASSERT_TRUE(imported.value().error);
    EXPECT_EQ(imported.value().error->code, ErrorCode::kCancelled);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_TRUE(assets.value().empty());
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().revision, before.value().revision);
}

TEST_F(CatalogServiceTest, CancellationBeforePreviewCacheCommitPublishesNoFileOrRecord)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "cancel-preview-cache.jpg").string();
    QImage image(24, 16, QImage::Format_RGB888);
    image.fill(QColor(10, 20, 30));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 95));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    auto before = service->list_previews();
    ASSERT_TRUE(before) << before.error().message;

    const auto count_cache_files = [this]
    {
        std::size_t count = 0;
        const std::filesystem::path cache_root(database_path + ".preview");
        for (const auto &entry : std::filesystem::directory_iterator(cache_root))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".png")
                ++count;
        }
        return count;
    };
    const auto files_before = count_cache_files();
    CancellationSource cancellation;
    bool hook_called = false;
    testing::CatalogServiceTestControl::set_before_preview_cache_publication(
        *service,
        [&]
        {
            hook_called = true;
            EXPECT_TRUE(cancellation.cancel("before-preview-cache-commit"));
        });
    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    request.max_edge = 11;
    request.cancellation = cancellation.token();
    auto preview = service->request_preview(request);
    ASSERT_FALSE(preview);
    EXPECT_TRUE(hook_called);
    EXPECT_EQ(preview.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(preview.error().context.at("reason"), "before-preview-cache-commit");
    EXPECT_EQ(count_cache_files(), files_before);
    auto after = service->list_previews();
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(after.value().size(), before.value().size());
}

TEST_F(CatalogServiceTest, CloseAndCorruptCacheStillAllowReopenPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = (root / "cached.jpg").string();
    QImage image(24, 16, QImage::Format_RGB888);
    image.fill(QColor(10, 20, 30));
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(image.save(QString::fromStdString(jpeg_path), "JPEG", 95));
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(imported.value().preview_cache_path);
    const auto cache_path = *imported.value().preview_cache_path;
    EXPECT_TRUE(std::filesystem::exists(cache_path));

    {
        QFile corrupt(QString::fromStdString(cache_path));
        ASSERT_TRUE(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(corrupt.write("not-a-png", 9), 9);
    }
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kThumbnailMaxEdge;
    auto rebuilt = service->request_preview(request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_TRUE(std::filesystem::exists(rebuilt.value().cache_path));
    QFile rebuilt_file(QString::fromStdString(rebuilt.value().cache_path));
    ASSERT_TRUE(rebuilt_file.open(QIODevice::ReadOnly));
    EXPECT_EQ(rebuilt_file.read(8), QByteArray("\x89PNG\r\n\x1a\n", 8));

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    PreviewRequest after_close;
    after_close.asset_id = asset_id;
    after_close.max_edge = kThumbnailMaxEdge;
    auto reopened = service->request_preview(after_close);
    ASSERT_TRUE(reopened) << reopened.error().message;
    EXPECT_TRUE(std::filesystem::exists(reopened.value().cache_path));
}

TEST_F(CatalogServiceTest, ExportsLocatedCaptureThroughJpegPngTiffDeterministically)
{
    ASSERT_TRUE(open_service(true));
    const auto jpeg_path = write_synthetic_jpeg_with_capture(root / "export-source.jpg");
    const auto source_hash = file_sha256(jpeg_path);
    auto imported = service->import_one(jpeg_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    const auto parse_has = [](const std::string &path, const char *needle)
    {
        QFile file(QString::fromStdString(path));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const auto bytes = file.readAll();
        return bytes.contains(needle);
    };
    const auto verify_embedded_capture = [&](const std::string &path)
    {
        auto capture = engine.read_embedded_capture_metadata(path, CancellationToken{});
        ASSERT_TRUE(capture) << capture.error().message;
        ASSERT_TRUE(capture.value().captured_datetime);
        EXPECT_EQ(capture.value().captured_datetime->local_exif, "2007:09:11 13:53:33");
        EXPECT_EQ(capture.value().captured_datetime->subsecond_digits, "18");
        EXPECT_EQ(capture.value().captured_datetime->utc_offset_minutes, 120);
        ASSERT_TRUE(capture.value().location);
        EXPECT_EQ(capture.value().location->latitude_e6, 49253239);
        EXPECT_EQ(capture.value().location->longitude_e6, 3050766);
        ASSERT_TRUE(capture.value().location->altitude);
        EXPECT_EQ(capture.value().location->altitude->magnitude_mm, 123456U);
        EXPECT_EQ(capture.value().location->altitude->reference,
                  EngineCaptureAltitudeReference::kAboveSeaLevel);
    };

    ExportRequest jpeg;
    jpeg.asset_id = asset_id;
    jpeg.output_path = (root / "located-out.jpg").string();
    jpeg.format = ExportFormat::kJpeg;
    auto first = service->export_asset(jpeg);
    ASSERT_TRUE(first) << first.error().message;
    ExportRequest second_request = jpeg;
    second_request.output_path = (root / "located-out-2.jpg").string();
    auto second = service->export_asset(second_request);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(file_sha256(jpeg.output_path), file_sha256(second.value().output_path));
    verify_embedded_capture(jpeg.output_path);
    EXPECT_TRUE(parse_has(jpeg.output_path, "2007:09:11 13:53:33"));
    EXPECT_TRUE(parse_has(jpeg.output_path, "2007-09-11T13:53:33.18+02:00"));

    ExportRequest png;
    png.asset_id = asset_id;
    png.output_path = (root / "located-out.png").string();
    png.format = ExportFormat::kPng;
    auto png_out = service->export_asset(png);
    ASSERT_TRUE(png_out) << png_out.error().message;
    verify_embedded_capture(png.output_path);
    EXPECT_TRUE(parse_has(png.output_path, "2007:09:11 13:53:33"));

    ExportRequest tiff;
    tiff.asset_id = asset_id;
    tiff.output_path = (root / "located-out.tif").string();
    tiff.format = ExportFormat::kTiff;
    auto tiff_out = service->export_asset(tiff);
    ASSERT_TRUE(tiff_out) << tiff_out.error().message;
    verify_embedded_capture(tiff.output_path);
    EXPECT_TRUE(parse_has(tiff.output_path, "2007:09:11 13:53:33"));
    EXPECT_EQ(file_sha256(jpeg_path), source_hash);
}

TEST_F(CatalogServiceTest, ExportMetadataPrivacyOmitsLocationOrAllPublicPackets)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = write_synthetic_jpeg_with_capture(root / "privacy-source.jpg");
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    ExportRequest no_location;
    no_location.asset_id = asset_id;
    no_location.output_path = (root / "privacy-no-location.jpg").string();
    no_location.format = ExportFormat::kJpeg;
    no_location.metadata_mode = ExportMetadataMode::kNoLocation;
    auto no_location_result = service->export_asset(no_location);
    ASSERT_TRUE(no_location_result) << no_location_result.error().message;
    auto no_location_capture =
        engine.read_embedded_capture_metadata(no_location.output_path, CancellationToken{});
    ASSERT_TRUE(no_location_capture) << no_location_capture.error().message;
    EXPECT_TRUE(no_location_capture.value().captured_datetime.has_value());
    EXPECT_FALSE(no_location_capture.value().location.has_value());

    const std::array cases{
        std::pair{ExportFormat::kJpeg, std::string("privacy-none.jpg")},
        std::pair{ExportFormat::kPng, std::string("privacy-none.png")},
        std::pair{ExportFormat::kTiff, std::string("privacy-none.tif")},
    };
    for (const auto &[format, name] : cases)
    {
        ExportRequest request;
        request.asset_id = asset_id;
        request.output_path = (root / name).string();
        request.format = format;
        request.metadata_mode = ExportMetadataMode::kNone;
        auto exported = service->export_asset(request);
        ASSERT_TRUE(exported) << name << ": " << exported.error().message;
        QFile file(QString::fromStdString(request.output_path));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        EXPECT_FALSE(bytes.contains("2007:09:11 13:53:33")) << name;
        EXPECT_FALSE(bytes.contains("http://ns.adobe.com/xap/1.0/")) << name;
        EXPECT_FALSE(bytes.contains("XML:com.adobe.xmp")) << name;
        auto capture =
            engine.read_embedded_capture_metadata(request.output_path, CancellationToken{});
        ASSERT_TRUE(capture) << name << ": " << capture.error().message;
        EXPECT_EQ(capture.value(), EngineCaptureMetadata{}) << name;
    }

    ExportRequest original;
    original.asset_id = asset_id;
    original.output_path = (root / "privacy-original.jpg").string();
    original.format = ExportFormat::kOriginalCopy;
    original.metadata_mode = ExportMetadataMode::kNone;
    const auto rejected = service->export_asset(original);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "metadata_mode_not_applicable");
    EXPECT_FALSE(std::filesystem::exists(original.output_path));
}

TEST(PresetPerformanceProbe, AppliesCrsToWarmRawPreviewWithinRequestedBudget)
{
    const char *catalog_path = std::getenv("RAVO_PRESET_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_PRESET_PERF_ASSET_ID");
    const char *preset_path = std::getenv("RAVO_PRESET_PERF_XMP");
    if (catalog_path == nullptr || asset_id == nullptr || preset_path == nullptr)
    {
        GTEST_SKIP() << "set the RAVO_PRESET_PERF_* environment variables";
    }

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());

    auto baseline_recipe = measured.load_baseline_recipe(asset_id);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    auto baseline = develop_from_recipe(baseline_recipe.value());
    ASSERT_TRUE(baseline) << baseline.error().message;
    ASSERT_TRUE(measured.save_develop(asset_id, baseline.value()));

    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = kDefaultPreviewMaxEdge;
    request.persist_preview_record = true;
    request.prefer_embedded_preview = false;
    auto warmed = measured.request_preview(request);
    ASSERT_TRUE(warmed) << warmed.error().message;

    const auto started = std::chrono::steady_clock::now();
    auto text = read_utf8_text_file(preset_path, 8U * 1024U * 1024U);
    ASSERT_TRUE(text) << text.error().message;
    auto imported = import_crs_xmp({text.value(), baseline_recipe.value().asset});
    ASSERT_TRUE(imported) << imported.error().message;
    auto applied = baseline.value();
    apply_crs_look(applied, imported.value().look, imported.value().mask);
    const auto parsed_at = std::chrono::steady_clock::now();
    ASSERT_TRUE(measured.save_develop(asset_id, applied));
    const auto saved_at = std::chrono::steady_clock::now();
    PreviewRequest interactive = request;
    interactive.max_edge = kInteractivePreviewMaxEdge;
    interactive.persist_preview_record = false;
    auto first_preview = measured.request_preview(interactive, applied);
    ASSERT_TRUE(first_preview) << first_preview.error().message;
    const auto first_preview_at = std::chrono::steady_clock::now();
    auto preview = measured.request_preview(request);
    ASSERT_TRUE(preview) << preview.error().message;
    const auto finished = std::chrono::steady_clock::now();
    const auto parse_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(parsed_at - started).count();
    const auto save_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(saved_at - parsed_at).count();
    const auto first_preview_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(first_preview_at - saved_at).count();
    const auto settled_preview_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - first_preview_at).count();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(finished - started);
    std::cerr << "preset_apply_elapsed_ms=" << elapsed.count() << " parse_ms=" << parse_ms
              << " save_ms=" << save_ms << " first_preview_ms=" << first_preview_ms
              << " settled_preview_ms=" << settled_preview_ms << '\n';

    if (const char *budget = std::getenv("RAVO_PRESET_PERF_BUDGET_MS"))
    {
        EXPECT_LT(elapsed.count(), std::stoll(budget));
    }
    if (const char *budget = std::getenv("RAVO_PRESET_PERF_FIRST_PREVIEW_BUDGET_MS"))
    {
        EXPECT_LT(first_preview_ms, std::stoll(budget));
    }
    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST(InteractivePreviewPerformanceProbe, MeasuresWarmExposureSweepWithoutCatalogMutation)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    const std::uint32_t max_edge =
        std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE") != nullptr ?
            static_cast<std::uint32_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE"))) :
            kInteractivePreviewMaxEdge;
    const std::size_t runs =
        std::getenv("RAVO_INTERACTIVE_PERF_RUNS") != nullptr ?
            static_cast<std::size_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_RUNS"))) :
            9U;
    ASSERT_GT(max_edge, 0U);
    ASSERT_GT(runs, 0U);

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());

    auto recipe = measured.load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto baseline = develop_from_recipe(recipe.value());
    ASSERT_TRUE(baseline) << baseline.error().message;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = max_edge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = false;

    auto warm = measured.request_preview(request, baseline.value());
    ASSERT_TRUE(warm) << warm.error().message;
    ASSERT_FALSE(warm.value().rgb.empty());
    ASSERT_LE(std::max(warm.value().width, warm.value().height), max_edge);
    if (max_edge <= kInteractivePreviewMaxEdge)
    {
        PreviewRequest settled = request;
        settled.max_edge = kDefaultPreviewMaxEdge;
        auto settled_warm = measured.request_preview(settled);
        ASSERT_TRUE(settled_warm) << settled_warm.error().message;
        ASSERT_FALSE(settled_warm.value().rgb.empty());
    }

    std::vector<std::int64_t> render_elapsed_ms;
    std::vector<std::int64_t> visible_elapsed_ms;
    render_elapsed_ms.reserve(runs);
    visible_elapsed_ms.reserve(runs);
    for (std::size_t run = 0; run < runs; ++run)
    {
        auto adjusted = baseline.value();
        const double offset = static_cast<double>(static_cast<int>(run % 7U) - 3) * 0.01;
        adjusted.exposure_ev = std::clamp(adjusted.exposure_ev + offset, -18.0, 18.0);
        const auto started = std::chrono::steady_clock::now();
        auto preview = measured.request_preview(request, adjusted);
        const auto rendered = std::chrono::steady_clock::now();
        ASSERT_TRUE(preview) << preview.error().message;
        ASSERT_FALSE(preview.value().rgb.empty());
        const QImage view(preview.value().rgb.data(), static_cast<int>(preview.value().width),
                          static_cast<int>(preview.value().height),
                          static_cast<int>(preview.value().width * 3U), QImage::Format_RGB888);
        const QImage owned = view.copy();
        RasterBuffer raster;
        raster.width = preview.value().width;
        raster.height = preview.value().height;
        raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
        for (std::uint32_t row = 0; row < raster.height; ++row)
        {
            std::copy_n(
                owned.constScanLine(static_cast<int>(row)),
                static_cast<std::size_t>(raster.width) * 3U,
                raster.srgb.begin() +
                    static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * raster.width * 3U));
        }
        auto histogram = collect_rgb_histogram(raster);
        ASSERT_TRUE(histogram) << histogram.error().message;
        const auto visible = std::chrono::steady_clock::now();
        render_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(rendered - started).count());
        visible_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(visible - started).count());
    }
    std::sort(render_elapsed_ms.begin(), render_elapsed_ms.end());
    std::sort(visible_elapsed_ms.begin(), visible_elapsed_ms.end());
    const auto p90_index = (visible_elapsed_ms.size() * 9U - 1U) / 10U;
    const auto render_median = render_elapsed_ms[render_elapsed_ms.size() / 2U];
    const auto render_p90 = render_elapsed_ms[p90_index];
    const auto visible_median = visible_elapsed_ms[visible_elapsed_ms.size() / 2U];
    const auto visible_p90 = visible_elapsed_ms[p90_index];
    std::cerr << "interactive_preview_edge=" << max_edge << " runs=" << runs
              << " render_min_ms=" << render_elapsed_ms.front()
              << " render_median_ms=" << render_median << " render_p90_ms=" << render_p90
              << " render_max_ms=" << render_elapsed_ms.back()
              << " visible_min_ms=" << visible_elapsed_ms.front()
              << " visible_median_ms=" << visible_median << " visible_p90_ms=" << visible_p90
              << " visible_max_ms=" << visible_elapsed_ms.back() << '\n';
    if (const char *budget = std::getenv("RAVO_INTERACTIVE_PERF_P90_BUDGET_MS"))
    {
        EXPECT_LE(visible_p90, std::stoll(budget));
    }

    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST(PerspectiveInteractivePerformanceProbe, MeasuresWarmManualTransformWithoutCatalogMutation)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    const std::uint32_t max_edge =
        std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE") != nullptr ?
            static_cast<std::uint32_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_MAX_EDGE"))) :
            kInteractivePreviewMaxEdge;
    const std::size_t runs =
        std::getenv("RAVO_INTERACTIVE_PERF_RUNS") != nullptr ?
            static_cast<std::size_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_RUNS"))) :
            9U;
    ASSERT_GT(max_edge, 0U);
    ASSERT_GT(runs, 0U);

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());

    auto recipe = measured.load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto baseline = develop_from_recipe(recipe.value());
    ASSERT_TRUE(baseline) << baseline.error().message;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = max_edge;
    request.persist_preview_record = false;
    request.prefer_embedded_preview = false;
    auto warm = measured.request_preview(request, baseline.value());
    ASSERT_TRUE(warm) << warm.error().message;
    ASSERT_FALSE(warm.value().rgb.empty());

    std::vector<std::int64_t> render_elapsed_ms;
    std::vector<std::int64_t> visible_elapsed_ms;
    render_elapsed_ms.reserve(runs);
    visible_elapsed_ms.reserve(runs);
    for (std::size_t run = 0U; run < runs; ++run)
    {
        auto adjusted = baseline.value();
        const double offset = static_cast<double>(static_cast<int>(run % 9U) - 4) * 0.015;
        adjusted.perspective_vertical = std::clamp(adjusted.perspective_vertical + offset,
                                                   kPerspectiveShiftMin, kPerspectiveShiftMax);
        adjusted.perspective_constrain_crop = true;
        adjusted.perspective_interpolation_index = 2;
        const auto started = std::chrono::steady_clock::now();
        auto preview = measured.request_preview(request, adjusted);
        const auto rendered = std::chrono::steady_clock::now();
        ASSERT_TRUE(preview) << preview.error().message;
        ASSERT_FALSE(preview.value().rgb.empty());
        const QImage view(preview.value().rgb.data(), static_cast<int>(preview.value().width),
                          static_cast<int>(preview.value().height),
                          static_cast<int>(preview.value().width * 3U), QImage::Format_RGB888);
        const QImage owned = view.copy();
        RasterBuffer raster;
        raster.width = preview.value().width;
        raster.height = preview.value().height;
        raster.srgb.resize(static_cast<std::size_t>(raster.width) * raster.height * 3U);
        for (std::uint32_t row = 0U; row < raster.height; ++row)
        {
            std::copy_n(
                owned.constScanLine(static_cast<int>(row)),
                static_cast<std::size_t>(raster.width) * 3U,
                raster.srgb.begin() +
                    static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * raster.width * 3U));
        }
        auto histogram = collect_rgb_histogram(raster);
        ASSERT_TRUE(histogram) << histogram.error().message;
        const auto visible = std::chrono::steady_clock::now();
        render_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(rendered - started).count());
        visible_elapsed_ms.push_back(
            std::chrono::duration_cast<std::chrono::milliseconds>(visible - started).count());
    }
    std::sort(render_elapsed_ms.begin(), render_elapsed_ms.end());
    std::sort(visible_elapsed_ms.begin(), visible_elapsed_ms.end());
    const auto p90_index = (visible_elapsed_ms.size() * 9U - 1U) / 10U;
    const auto visible_p90 = visible_elapsed_ms[p90_index];
    std::cerr << "perspective_interactive_edge=" << max_edge << " runs=" << runs
              << " render_min_ms=" << render_elapsed_ms.front()
              << " render_median_ms=" << render_elapsed_ms[render_elapsed_ms.size() / 2U]
              << " render_p90_ms=" << render_elapsed_ms[p90_index]
              << " render_max_ms=" << render_elapsed_ms.back()
              << " visible_min_ms=" << visible_elapsed_ms.front()
              << " visible_median_ms=" << visible_elapsed_ms[visible_elapsed_ms.size() / 2U]
              << " visible_p90_ms=" << visible_p90
              << " visible_max_ms=" << visible_elapsed_ms.back() << '\n';
    if (const char *budget = std::getenv("RAVO_PERSPECTIVE_PERF_P90_BUDGET_MS"))
        EXPECT_LE(visible_p90, std::stoll(budget));

    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST(InteractivePreviewQualityProbe, ComparesInteractiveEdgesWithSettledDisplayPixels)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }

    auto created_engine = EngineFacade::create_phase1();
    ASSERT_TRUE(created_engine) << created_engine.error().message;
    auto repository = SqliteCatalogRepository::open(catalog_path);
    ASSERT_TRUE(repository) << repository.error().message;
    const auto cache_root = make_temp_root();
    auto cache = FilesystemPreviewCache::create((cache_root / "preview").string());
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create((cache_root / "recovery").string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService measured(created_engine.value(), std::move(repository).value(),
                            std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                            std::move(recovery).value());
    auto recipe = measured.load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;

    const auto render = [&](const std::uint32_t edge) -> Result<PreviewResult>
    {
        PreviewRequest request;
        request.asset_id = asset_id;
        request.max_edge = edge;
        request.persist_preview_record = false;
        request.prefer_embedded_preview = false;
        return measured.request_preview(request, develop.value());
    };
    auto former = render(640U);
    auto interactive = render(kInteractivePreviewMaxEdge);
    auto settled = render(kDefaultPreviewMaxEdge);
    ASSERT_TRUE(former) << former.error().message;
    ASSERT_TRUE(interactive) << interactive.error().message;
    ASSERT_TRUE(settled) << settled.error().message;

    struct Quality
    {
        double psnr_db = 0.0;
        double mean_absolute_error = 0.0;
        int p99_absolute_error = 0;
    };
    const auto compare = [](const PreviewResult &candidate,
                            const PreviewResult &reference) -> Quality
    {
        const QImage reference_view(reference.rgb.data(), static_cast<int>(reference.width),
                                    static_cast<int>(reference.height),
                                    static_cast<int>(reference.width * 3U), QImage::Format_RGB888);
        const QImage scaled = reference_view.scaled(
            static_cast<int>(candidate.width), static_cast<int>(candidate.height),
            Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        std::vector<int> absolute_errors;
        absolute_errors.reserve(candidate.rgb.size());
        double squared_error = 0.0;
        double absolute_error = 0.0;
        for (std::uint32_t row = 0; row < candidate.height; ++row)
        {
            const auto *reference_row = scaled.constScanLine(static_cast<int>(row));
            const auto candidate_offset = static_cast<std::size_t>(row) * candidate.width * 3U;
            for (std::size_t column = 0; column < static_cast<std::size_t>(candidate.width) * 3U;
                 ++column)
            {
                const int difference = static_cast<int>(candidate.rgb[candidate_offset + column]) -
                                       static_cast<int>(reference_row[column]);
                const int magnitude = std::abs(difference);
                squared_error += static_cast<double>(difference * difference);
                absolute_error += static_cast<double>(magnitude);
                absolute_errors.push_back(magnitude);
            }
        }
        std::sort(absolute_errors.begin(), absolute_errors.end());
        const double count = static_cast<double>(absolute_errors.size());
        const double mse = squared_error / count;
        const auto p99_index = (absolute_errors.size() * 99U - 1U) / 100U;
        return Quality{
            .psnr_db = mse == 0.0 ? std::numeric_limits<double>::infinity() :
                                    20.0 * std::log10(255.0 / std::sqrt(mse)),
            .mean_absolute_error = absolute_error / count,
            .p99_absolute_error = absolute_errors[p99_index],
        };
    };
    const Quality former_quality = compare(former.value(), settled.value());
    const Quality interactive_quality = compare(interactive.value(), settled.value());
    std::cerr << "interactive_quality_edge=" << kInteractivePreviewMaxEdge
              << " settled_edge=" << kDefaultPreviewMaxEdge
              << " psnr_db=" << interactive_quality.psnr_db
              << " mean_abs_error=" << interactive_quality.mean_absolute_error
              << " p99_abs_error=" << interactive_quality.p99_absolute_error
              << " former_640_psnr_db=" << former_quality.psnr_db
              << " former_640_mean_abs_error=" << former_quality.mean_absolute_error
              << " former_640_p99_abs_error=" << former_quality.p99_absolute_error << '\n';
    EXPECT_GE(interactive_quality.psnr_db, former_quality.psnr_db);
    EXPECT_LE(interactive_quality.mean_absolute_error, former_quality.mean_absolute_error);
    EXPECT_LE(interactive_quality.p99_absolute_error, former_quality.p99_absolute_error);
    if (const char *minimum = std::getenv("RAVO_INTERACTIVE_QUALITY_MIN_PSNR_DB"))
    {
        EXPECT_GE(interactive_quality.psnr_db, std::stod(minimum));
    }

    ASSERT_TRUE(measured.close());
    std::error_code ignored;
    std::filesystem::remove_all(cache_root, ignored);
}

TEST_F(CatalogServiceTest, TexturePersistsAndReproducesPreviewAndExportAfterReopen)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = (root / "texture.png").string();
    QImage source(96, 64, QImage::Format_RGB888);
    source.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int row = 0; row < source.height(); ++row)
    {
        for (int column = 0; column < source.width(); ++column)
        {
            const int checker = ((column / 2 + row / 2) & 1) == 0 ? -18 : 18;
            source.setPixelColor(column, row,
                                 QColor(std::clamp(110 + checker + column / 4, 0, 255),
                                        std::clamp(90 + checker + row / 3, 0, 255),
                                        std::clamp(70 + checker, 0, 255)));
        }
    }
    ASSERT_TRUE(source.save(QString::fromStdString(source_path), "PNG"));
    const auto source_hash = file_sha256(source_path);
    auto imported = service->import_one(source_path, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const std::string asset_id = imported.value().asset->id;

    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 96U;
    preview.persist_preview_record = true;
    auto baseline = service->request_preview(preview);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const QImage baseline_image(QString::fromStdString(baseline.value().cache_path));
    ASSERT_FALSE(baseline_image.isNull());

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop) << develop.error().message;
    develop.value().texture = TextureParams{0.75, 4.0, 1};
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));

    auto stored = service->load_recipe(asset_id);
    ASSERT_TRUE(stored) << stored.error().message;
    const auto operation =
        std::find_if(stored.value().operations.begin(), stored.value().operations.end(),
                     [](const OperationInstance &item) { return item.id == kTextureOperationId; });
    ASSERT_NE(operation, stored.value().operations.end());
    auto stored_params = texture_from_parameters(operation->parameters);
    ASSERT_TRUE(stored_params) << stored_params.error().message;
    EXPECT_EQ(stored_params.value(), develop.value().texture);

    auto before_reopen = service->request_preview(preview);
    ASSERT_TRUE(before_reopen) << before_reopen.error().message;
    const QImage before_reopen_image(QString::fromStdString(before_reopen.value().cache_path));
    ASSERT_FALSE(before_reopen_image.isNull());
    EXPECT_NE(before_reopen_image, baseline_image);

    const auto export_path = (root / "texture-export.png").string();
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = export_path;
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 96U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    const QImage exported_image(QString::fromStdString(export_path));
    ASSERT_FALSE(exported_image.isNull());
    EXPECT_EQ(exported_image, before_reopen_image);

    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto restored_recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(restored_recipe) << restored_recipe.error().message;
    auto restored = develop_from_recipe(restored_recipe.value());
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().texture, develop.value().texture);
    auto after_reopen = service->request_preview(preview);
    ASSERT_TRUE(after_reopen) << after_reopen.error().message;
    EXPECT_EQ(after_reopen.value().cache_key, before_reopen.value().cache_key);
    const QImage after_reopen_image(QString::fromStdString(after_reopen.value().cache_path));
    ASSERT_FALSE(after_reopen_image.isNull());
    EXPECT_EQ(after_reopen_image, before_reopen_image);
    EXPECT_EQ(file_sha256(source_path), source_hash);
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
