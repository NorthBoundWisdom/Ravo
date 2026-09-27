#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include "capture_metadata_test_support.h"
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <gtest/gtest.h>
#include "catalog_test_support.h"
#include "catalog_service_test_support.h"
#include "ravo/recipe/develop.h"
#include "interactive_perf_report.h"
#include "catalog_repository_test_control.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/services/import_thumbnail.h"
#include "ravo/adapters/qt_raster_decoder.h"

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

TEST_F(CatalogServiceTest, ImportDefaultsAbsentExifAltitudeReferenceAndPreservesSource)
{
    ASSERT_TRUE(open_service(true));
    auto bytes = test_support::make_capture_exif_tiff();
    ASSERT_TRUE(test_support::rewrite_linked_ifd_entry(bytes, 34853U, 5U, 0xC005U));
    const auto path = root / "default-altitude-ref.tif";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    const auto source_hash = file_sha256(path.string());
    const auto imported = service->import_one(path.string(), {});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().status, ImportItemStatus::kImported);
    ASSERT_TRUE(imported.value().asset);
    const auto &capture = imported.value().asset->capture;
    ASSERT_TRUE(capture.location);
    ASSERT_TRUE(capture.location->altitude);
    EXPECT_EQ(capture.location->altitude->magnitude_mm, 123456U);
    EXPECT_EQ(capture.location->altitude->reference, CaptureAltitudeReference::kAboveSeaLevel);
    ASSERT_TRUE(service->close());
    service.reset();
    ASSERT_TRUE(open_service(false));
    const auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value()[0].capture, capture);
    EXPECT_EQ(file_sha256(path.string()), source_hash);
}

TEST_F(CatalogServiceTest, ImportScanAndExecutionExcludeCatalogOwnedTrees)
{
    ASSERT_TRUE(open_service(true));
    const auto cache = std::filesystem::path(database_path + ".preview");
    const auto support = std::filesystem::path(database_path + ".ravo") / "offline-edit-proxies";
    const auto similar = std::filesystem::path(database_path + ".preview-photos");
    std::filesystem::create_directories(cache / "nested");
    std::filesystem::create_directories(support);
    std::filesystem::create_directories(similar);
    const auto original = root / "original.png";
    const auto cached = cache / "nested" / "v11_ast_example.png";
    const auto proxy = support / "proxy.png";
    ASSERT_TRUE(write_photo(original, Qt::red));
    ASSERT_TRUE(write_photo(cached, Qt::green));
    ASSERT_TRUE(write_photo(proxy, Qt::blue));
    ASSERT_TRUE(write_photo(similar / "photo.png", Qt::yellow));
    // A cache-looking filename outside the owned trees is still a user photo.
    ASSERT_TRUE(write_photo(root / "v11_ast_user_photo.png", Qt::cyan));
    const auto original_hash = file_sha256(original.string());
    const auto cache_hash = file_sha256(cached.string());
    const auto revision = service->snapshot().value().revision;
    auto scan = service->scan_import_candidates({root.string()}, root.string(), true, {});
    ASSERT_TRUE(scan) << scan.error().message;
    ASSERT_EQ(scan.value().candidates.size(), 3U);
    EXPECT_EQ(service->snapshot().value().revision, revision);
    EXPECT_TRUE(service->list_assets().value().empty());
    auto flat = service->enumerate_import_inputs({root.string()}, {}, false);
    ASSERT_TRUE(flat) << flat.error().message;
    EXPECT_EQ(flat.value().size(), 2U);
    auto owned = service->enumerate_import_inputs(
        {cache.string(), support.string(), cached.string(), proxy.string()}, {});
    ASSERT_TRUE(owned) << owned.error().message;
    EXPECT_TRUE(owned.value().empty());

    ImportRequest request;
    request.inputs = {root.string(), cached.string(), proxy.string()};
    request.mode = ImportTransferMode::kAdd;
    request.defer_previews = true;
    auto imported = service->execute_import(request);
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported.value().imported, 3U);
    EXPECT_EQ(service->list_assets().value().size(), 3U);
    EXPECT_EQ(file_sha256(original.string()), original_hash);
    EXPECT_EQ(file_sha256(cached.string()), cache_hash);
    service.reset();
    ASSERT_TRUE(open_service(false));
    EXPECT_EQ(service->list_assets().value().size(), 3U);
    auto direct_batch = service->import_inputs({cached.string(), proxy.string()}, {});
    ASSERT_TRUE(direct_batch) << direct_batch.error().message;
    EXPECT_TRUE(direct_batch.value().empty());
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("test"));
    auto cancelled = service->enumerate_import_inputs({root.string()}, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

#ifndef _WIN32
TEST_F(CatalogServiceTest, ImportScanExcludesAliasesIntoCatalogOwnedTrees)
{
    ASSERT_TRUE(open_service(true));
    const auto cache = std::filesystem::path(database_path + ".preview");
    const auto cached = cache / "cached.png";
    ASSERT_TRUE(write_photo(cached, Qt::green));
    const auto alias = root / "alias";
    const auto linked_file = root / "linked.png";
    std::filesystem::create_directory_symlink(cache, alias);
    std::filesystem::create_symlink(cached, linked_file);
    for (const auto &input : {root, alias, alias / "cached.png", linked_file})
    {
        auto paths = service->enumerate_import_inputs({input.string()}, {});
        ASSERT_TRUE(paths) << paths.error().message;
        EXPECT_TRUE(paths.value().empty());
    }
}
#endif

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

TEST_F(CatalogServiceTest, ImportScanVerifiesKnownContentAcrossTimestampOnlyChanges)
{
    ASSERT_TRUE(open_service(true));
    const auto original = root / "original.png";
    const auto copy = root / "copy.png";
    ASSERT_TRUE(write_photo(original, Qt::red));
    ASSERT_TRUE(std::filesystem::copy_file(original, copy));
    ASSERT_TRUE(service->import_one(original.string(), {}));
    const auto hash = file_sha256(original.string());
    const auto revision = service->snapshot().value().revision;
    const auto stamp = std::filesystem::last_write_time(original) + std::chrono::seconds(2);
    std::filesystem::last_write_time(original, stamp);
    auto scan = service->scan_import_candidates({copy.string()}, root.string(), false, {});
    ASSERT_TRUE(scan) << scan.error().message;
    EXPECT_EQ(scan.value().unavailable, 0U);
    EXPECT_EQ(scan.value().duplicates, 1U);
    EXPECT_EQ(file_sha256(original.string()), hash);
    EXPECT_EQ(service->snapshot().value().revision, revision);

    QFile changed(QString::fromStdString(original.string()));
    ASSERT_TRUE(changed.open(QIODevice::ReadWrite));
    ASSERT_TRUE(changed.seek(changed.size() - 1));
    const auto tail = changed.read(1);
    ASSERT_EQ(tail.size(), 1);
    ASSERT_TRUE(changed.seek(changed.size() - 1));
    const char replacement = static_cast<char>(tail[0] ^ 1);
    ASSERT_EQ(changed.write(&replacement, 1), 1);
    changed.close();
    std::filesystem::last_write_time(original, stamp);
    scan = service->scan_import_candidates({copy.string()}, root.string(), false, {});
    ASSERT_TRUE(scan);
    EXPECT_EQ(scan.value().unavailable, 1U);
    ASSERT_TRUE(scan.value().candidates.front().error);
    EXPECT_EQ(scan.value().candidates.front().error->code, ErrorCode::kConflict);
    EXPECT_EQ(service->snapshot().value().revision, revision);
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
TEST_F(CatalogServiceTest, ImportScanEnumeratesBeforeClassificationAndHonorsEnumerationCancellation)
{
    ASSERT_TRUE(open_service(true));
    ASSERT_TRUE(write_photo(root / "b.png", Qt::blue));
    ASSERT_TRUE(write_photo(root / "a.png", Qt::red));
    const auto hash = file_sha256((root / "a.png").string());
    const auto revision = service->snapshot().value().revision;
    bool enumerated = false;
    std::size_t classified = 0;
    auto scan = service->scan_import_candidates(
        {root.string()}, root.string(), false, {},
        [&](std::size_t completed, std::size_t total, const ImportCandidate &)
        {
            EXPECT_TRUE(enumerated);
            EXPECT_EQ(total, 2U);
            classified = completed;
        },
        [&](const std::vector<std::string> &paths)
        {
            enumerated = true;
            EXPECT_EQ(classified, 0U);
            ASSERT_EQ(paths.size(), 2U);
            EXPECT_EQ(std::filesystem::path(paths.front()).filename(), "a.png");
            EXPECT_TRUE(service->list_assets().value().empty());
        });
    ASSERT_TRUE(scan);
    EXPECT_EQ(classified, 2U);
    CancellationSource cancel;
    classified = 0;
    auto stopped = service->scan_import_candidates(
        {root.string()}, root.string(), false, cancel.token(), [&](auto, auto, const auto &)
        { ++classified; }, [&](const auto &) { ASSERT_TRUE(cancel.cancel("enumerated")); });
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(classified, 0U);
    EXPECT_EQ(service->snapshot().value().revision, revision);
    EXPECT_EQ(file_sha256((root / "a.png").string()), hash);
}

TEST_F(CatalogServiceTest, ImportThumbnailDecoderNeedsNoCatalogAndPreservesPixelsAndFailures)
{
    ASSERT_TRUE(open_service(true));
    const QtRasterDecoder raster;
    for (const auto &path : {png_fixture_path(), raw_fixture_path()})
    {
        const auto hash = file_sha256(path);
        auto direct = decode_import_thumbnail(engine, raster, path, {});
        auto catalog = service->decode_import_candidate_thumbnail(path, {});
        ASSERT_TRUE(direct) << direct.error().message;
        ASSERT_TRUE(catalog) << catalog.error().message;
        EXPECT_EQ(direct.value().srgb, catalog.value().srgb);
        EXPECT_EQ(direct.value().width, catalog.value().width);
        EXPECT_EQ(direct.value().height, catalog.value().height);
        EXPECT_EQ(file_sha256(path), hash);
    }
    service.reset();
    EXPECT_TRUE(decode_import_thumbnail(engine, raster, png_fixture_path(), {}));
    auto embedded = engine.extract_embedded_preview(raw_fixture_path(), kThumbnailMaxEdge, {});
    ASSERT_TRUE(embedded) << embedded.error().message;
    auto jpeg = raster.decode_memory(embedded.value().bytes, kThumbnailMaxEdge, {},
                                     embedded.value().rotate_quarters);
    ASSERT_TRUE(jpeg) << jpeg.error().message;
    auto browse = decode_import_thumbnail(engine, raster, raw_fixture_path(), {});
    ASSERT_TRUE(browse) << browse.error().message;
    EXPECT_EQ(browse.value().srgb, jpeg.value().rgb);
    EXPECT_EQ(browse.value().width, jpeg.value().width);
    EXPECT_EQ(browse.value().height, jpeg.value().height);
    EXPECT_NE(browse.value().color_profile.kind, ColorProfileKind::kMissing);
    auto missing = decode_import_thumbnail(engine, raster, (root / "missing.png").string(), {});
    ASSERT_FALSE(missing);
    ASSERT_TRUE(write_utf8_text_file_atomically((root / "corrupt.png").string(), "corrupt"));
    EXPECT_FALSE(decode_import_thumbnail(engine, raster, (root / "corrupt.png").string(), {}));
    CancellationSource cancel;
    ASSERT_TRUE(cancel.cancel("test"));
    auto stopped = decode_import_thumbnail(engine, raster, png_fixture_path(), cancel.token());
    ASSERT_FALSE(stopped);
    EXPECT_EQ(stopped.error().code, ErrorCode::kCancelled);
}

TEST_F(CatalogServiceTest, WarmRawPreviewAndDevelopSelectionDoNotUnpackRawAfterReopen)
{
    ASSERT_TRUE(open_service(true));
    const auto source_hash = file_sha256(raw_fixture_path());
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    auto cold = service->request_preview(request);
    ASSERT_TRUE(cold) << cold.error().message;
    const auto pixels = file_sha256(cold.value().cache_path);
    auto recipe = service->load_recipe(request.asset_id);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    ASSERT_TRUE(service->close());
    ASSERT_TRUE(open_service(false));
    EXPECT_FALSE(testing::CatalogServiceTestControl::has_decoded_raw(*service));

    auto warm = service->request_preview(request);
    ASSERT_TRUE(warm) << warm.error().message;
    EXPECT_EQ(warm.value().cache_key, cold.value().cache_key);
    EXPECT_EQ(file_sha256(warm.value().cache_path), pixels);
    EXPECT_FALSE(testing::CatalogServiceTestControl::has_decoded_raw(*service));

    request.max_edge = kInteractivePreviewMaxEdge;
    request.persist_preview_record = false;
    request.prefer_cached_settled_preview = true;
    request.request_revision = 42U;
    auto selection = service->request_preview(request, params.value());
    ASSERT_TRUE(selection) << selection.error().message;
    EXPECT_EQ(selection.value().cache_path, cold.value().cache_path);
    EXPECT_EQ(selection.value().request_revision, 42U);
    EXPECT_EQ(selection.value().width, cold.value().width);
    EXPECT_FALSE(testing::CatalogServiceTestControl::has_decoded_raw(*service));

    CancellationSource cancellation;
    static_cast<void>(cancellation.cancel("switch_superseded"));
    request.cancellation = cancellation.token();
    auto cancelled = service->request_preview(request, params.value());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(testing::CatalogServiceTestControl::has_decoded_raw(*service));
    EXPECT_EQ(file_sha256(raw_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, CachedSettledSelectionCannotServeChangedLiveRecipe)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->request_preview(request));
    auto recipe = service->load_recipe(request.asset_id);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    ASSERT_TRUE(service->close());
    ASSERT_TRUE(open_service(false));
    request.max_edge = 160U;
    request.persist_preview_record = false;
    request.prefer_cached_settled_preview = true;
    params.value().exposure_ev = 1.0;
    auto changed = service->request_preview(request, params.value());
    ASSERT_TRUE(changed) << changed.error().message;
    EXPECT_TRUE(changed.value().cache_path.empty());
    EXPECT_FALSE(changed.value().rgb.empty());
    EXPECT_TRUE(testing::CatalogServiceTestControl::has_decoded_raw(*service));
}

TEST_F(CatalogServiceTest, WarmProcessedRawThumbnailDoesNotUnpackRawAndCorruptionRebuilds)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(raw_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    PreviewRequest request;
    request.asset_id = imported.value().asset->id;
    request.max_edge = kThumbnailMaxEdge;
    request.purpose = PreviewPurpose::kBrowse;
    // Exercise the processed thumbnail lane, including cameras without JPEGs.
    request.prefer_embedded_preview = false;
    auto cold = service->request_preview(request);
    ASSERT_TRUE(cold) << cold.error().message;
    const auto expected_hash = file_sha256(cold.value().cache_path);
    ASSERT_TRUE(service->close());
    ASSERT_TRUE(open_service(false));
    auto warm = service->request_preview(request);
    ASSERT_TRUE(warm) << warm.error().message;
    EXPECT_FALSE(testing::CatalogServiceTestControl::has_decoded_raw(*service));
    EXPECT_EQ(file_sha256(warm.value().cache_path), expected_hash);
    {
        std::ofstream corrupt(warm.value().cache_path, std::ios::binary | std::ios::trunc);
        corrupt << "invalid PNG";
    }
    auto rebuilt = service->request_preview(request);
    ASSERT_TRUE(rebuilt) << rebuilt.error().message;
    EXPECT_TRUE(testing::CatalogServiceTestControl::has_decoded_raw(*service));
    EXPECT_EQ(file_sha256(rebuilt.value().cache_path), expected_hash);
}

TEST_F(CatalogServiceTest, PhotoEditAllowsUnrelatedImportAndRejectsChangedRecipe)
{
    ASSERT_TRUE(open_service(true));
    ASSERT_TRUE(write_photo(root / "first.png", Qt::green));
    ASSERT_TRUE(write_photo(root / "second.png", Qt::blue));
    auto imported = service->import_one((root / "first.png").string(), {});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto id = imported.value().asset->id;
    auto recipe = service->load_recipe(id);
    ASSERT_TRUE(recipe);
    auto base = develop_from_recipe(recipe.value());
    ASSERT_TRUE(base);
    RecipeSaveOptions observed;
    observed.expected_base = base.value();
    observed.expected_source = recipe.value().asset;
    auto unrelated = service->import_one((root / "second.png").string(), {});
    ASSERT_TRUE(unrelated);
    ASSERT_EQ(unrelated.value().status, ImportItemStatus::kImported);
    auto next = base.value();
    next.exposure_ev = 0.5;
    auto saved = service->save_develop_with_history(id, next, observed);
    ASSERT_TRUE(saved) << saved.error().message;
    auto history = service->list_recipe_history(id);
    ASSERT_TRUE(history);
    next.exposure_ev = 1.0;
    auto stale = service->save_develop_with_history(id, next, observed);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    EXPECT_EQ(stale.error().context.at("reason"), "stale_recipe_state");
    auto actual = service->load_recipe(id);
    ASSERT_TRUE(actual);
    auto params = develop_from_recipe(actual.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0.5);
    auto after = service->list_recipe_history(id);
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().size(), history.value().size());
}

TEST_F(CatalogServiceTest, PreviewPublicationRejectsChangedRecipeAndPreservesCurrentRecord)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto id = imported.value().asset->id;
    auto recipe = service->load_recipe(id);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    params.value().exposure_ev = 0.5;
    auto before = sqlite_repository->list_previews();
    ASSERT_TRUE(before);
    bool changed = false;
    testing::CatalogServiceTestControl::set_before_preview_cache_publication(
        *service,
        [&]
        {
            if (changed)
                return;
            changed = true;
            ASSERT_TRUE(service->save_develop(id, params.value()));
        });
    PreviewRequest request;
    request.asset_id = id;
    request.max_edge = 17U;
    auto rendered = service->request_preview(request);
    ASSERT_TRUE(changed);
    ASSERT_FALSE(rendered);
    EXPECT_EQ(rendered.error().code, ErrorCode::kConflict);
    EXPECT_EQ(rendered.error().context.at("reason"), "stale_preview_state");
    auto after = sqlite_repository->list_previews();
    ASSERT_TRUE(after);
    ASSERT_EQ(after.value().size(), before.value().size());
    ASSERT_FALSE(after.value().empty());
    EXPECT_EQ(after.value().front().cache_key, before.value().front().cache_key);
}

TEST_F(CatalogServiceTest, PreviewDimensionsCannotOverwriteConcurrentPhotoReview)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    auto stale = *imported.value().asset;
    ASSERT_TRUE(service->set_rating(stale.id, 4));
    auto before = sqlite_repository->recovery_state(stale.id);
    ASSERT_TRUE(before);
    stale.width = 123U;
    auto updated = sqlite_repository->update_preview_state(stale, before.value().generation);
    ASSERT_TRUE(updated);
    auto after = sqlite_repository->find_asset_by_id(stale.id);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after.value());
    EXPECT_EQ(after.value()->review.rating, 4);
    EXPECT_EQ(after.value()->width, 123U);
    auto generation = sqlite_repository->recovery_state(stale.id);
    ASSERT_TRUE(generation);
    EXPECT_EQ(generation.value().generation, updated.value());
    EXPECT_GT(updated.value(), before.value().generation);
    stale.width = 456U;
    auto obsolete = sqlite_repository->update_preview_state(stale, before.value().generation);
    ASSERT_FALSE(obsolete);
    EXPECT_EQ(obsolete.error().code, ErrorCode::kConflict);
    stale.normalized_uri += ".replaced";
    auto wrong_source = sqlite_repository->update_preview_state(stale, updated.value());
    ASSERT_FALSE(wrong_source);
    EXPECT_EQ(wrong_source.error().context.at("reason"), "stale_preview_source");
    after = sqlite_repository->find_asset_by_id(stale.id);
    ASSERT_TRUE(after);
    ASSERT_TRUE(after.value());
    EXPECT_EQ(after.value()->width, 123U);
    EXPECT_EQ(after.value()->review.rating, 4);
}

TEST_F(CatalogServiceTest, CropWorkspaceIsFullSourceAndCannotPersist)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import_one(png_fixture_path(), {});
    ASSERT_TRUE(imported);
    ASSERT_TRUE(imported.value().asset);
    const auto id = imported.value().asset->id;
    auto recipe = service->load_recipe(id);
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    params.value().straighten_degrees = 22.0;
    params.value().crop_width = 0.7;
    params.value().crop_height = 0.7;
    auto before = sqlite_repository->list_previews();
    ASSERT_TRUE(before);
    const auto source_hash = file_sha256(png_fixture_path());
    PreviewRequest request;
    request.asset_id = id;
    request.max_edge = 320;
    request.ignore_crop = true;
    request.crop_workspace = true;
    auto invalid = service->request_preview(request, params.value());
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().code, ErrorCode::kInvalidArgument);
    request.persist_preview_record = false;
    auto preview = service->request_preview(request, params.value());
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_TRUE(preview.value().crop_geometry);
    EXPECT_GT(preview.value().width, 320U);
    ASSERT_GE(preview.value().rgb.size(), 3U);
    EXPECT_EQ(preview.value().rgb[0], 118);
    EXPECT_EQ(preview.value().rgb[1], 118);
    EXPECT_EQ(preview.value().rgb[2], 118);
    EXPECT_TRUE(preview.value().cache_path.empty());
    EXPECT_LT(preview.value().crop_geometry->region.width, 1.0);
    auto after = sqlite_repository->list_previews();
    ASSERT_TRUE(after);
    ASSERT_EQ(after.value().size(), before.value().size());
    EXPECT_EQ(after.value().front().cache_key, before.value().front().cache_key);
    EXPECT_EQ(serialize_recipe(service->load_recipe(id).value()).value(),
              serialize_recipe(recipe.value()).value());
    EXPECT_EQ(file_sha256(png_fixture_path()), source_hash);
}

TEST_F(CatalogServiceTest, ImportThumbnailPerformanceObservation)
{
    const char *path = std::getenv("RAVO_IMPORT_THUMBNAIL_PERF_INPUT");
    if (path == nullptr)
        GTEST_SKIP() << "set RAVO_IMPORT_THUMBNAIL_PERF_INPUT for a real-camera observation";
    const auto hash = file_sha256(path);
    const QtRasterDecoder raster;
    auto reference = decode_import_thumbnail(engine, raster, path, {});
    ASSERT_TRUE(reference) << reference.error().message;
    const auto warmups = interactive_perf_report::warmups_from_env();
    const auto recorded = interactive_perf_report::recorded_samples_from_env();
    ASSERT_GT(recorded, 0U);
    std::vector<std::int64_t> samples;
    for (std::size_t index = 0; index < warmups + recorded; ++index)
    {
        const auto started = std::chrono::steady_clock::now();
        auto decoded = decode_import_thumbnail(engine, raster, path, {});
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        ASSERT_TRUE(decoded) << decoded.error().message;
        EXPECT_EQ(decoded.value().srgb, reference.value().srgb);
        EXPECT_EQ(decoded.value().width, reference.value().width);
        EXPECT_EQ(decoded.value().height, reference.value().height);
        if (index >= warmups)
            samples.push_back(elapsed);
    }
    interactive_perf_report::CaseMeta meta;
    meta.case_id = "import_camera_jpeg_thumbnail";
    meta.path = "import_thumbnail_decode";
    meta.source_kind = "caller_supplied_camera_file";
    meta.cache_state = "warm_os_cache_no_decoded_image_cache";
    meta.file_count = 1U;
    meta.workers = 1;
    meta.max_edge = kThumbnailMaxEdge;
    meta.warmups = warmups;
    meta.recorded_samples = recorded;
    interactive_perf_report::emit_case(meta, samples);
    EXPECT_EQ(file_sha256(path), hash);
}

} // namespace ravo
