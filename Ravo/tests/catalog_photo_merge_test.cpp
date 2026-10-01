#include <filesystem>
#include <QColorSpace>
#include <QImage>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "catalog_service_test_support.h"
#include "catalog_repository_test_control.h"
#include "ravo/adapters/text_file.h"
#include "ravo/services/photo_merge.h"
#include "ravo/services/artifact_publication.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{
bool write_merge_input(const std::filesystem::path &path, int level)
{
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(level, level, level));
    return image.save(QString::fromStdString(path.string()), "PNG");
}
Result<PhotoMergeRequest> prepare_merge(CatalogService &service, const std::filesystem::path &root)
{
    PhotoMergeRequest request;
    request.options.auto_align = false;
    request.options.exposure_ev = {0, 1};
    for (int i = 0; i < 2; ++i)
    {
        const auto path = root / ("input-" + std::to_string(i) + ".png");
        if (!write_merge_input(path, 80 + i * 30))
            return make_error(ErrorCode::kIo, "Cannot write test photo");
        auto imported = service.import_one(path.string(), {});
        if (!imported)
            return imported.error();
        if (!imported.value().asset)
            return make_error(ErrorCode::kIo, "Test photo was not imported");
        request.asset_ids.push_back(imported.value().asset->id);
    }
    auto snapshot = service.snapshot();
    if (!snapshot)
        return snapshot.error();
    request.expected_catalog_revision = snapshot.value().revision;
    return request;
}
} // namespace

TEST_F(CatalogServiceTest, PhotoMergePublishesVerifiedTiffProvenanceAndReopens)
{
    ASSERT_TRUE(open_service(true));
    auto request = prepare_merge(*service, root);
    ASSERT_TRUE(request);
    const auto first = file_sha256((root / "input-0.png").string());
    const auto second = file_sha256((root / "input-1.png").string());
    auto merged = service->merge_selected_photos(request.value());
    ASSERT_TRUE(merged) << merged.error().message;
    EXPECT_EQ(merged.value().artifact.mime_type, "image/tiff");
    EXPECT_EQ(merged.value().artifact.width, 96);
    EXPECT_EQ(merged.value().artifact.height, 64);
    EXPECT_EQ(sha256_file_hex(merged.value().output_path).value(),
              merged.value().artifact.content_sha256);
    auto text = read_utf8_text_file(merged.value().provenance_path, 1024 * 1024);
    ASSERT_TRUE(text);
    auto manifest = parse_json(text.value());
    ASSERT_TRUE(manifest);
    EXPECT_EQ(*manifest.value().find("sample_type")->string_if(), "uint16");
    EXPECT_EQ(manifest.value().find("sources")->array_if()->size(), 2);
    EXPECT_EQ(file_sha256((root / "input-0.png").string()), first);
    EXPECT_EQ(file_sha256((root / "input-1.png").string()), second);
    const auto id = merged.value().asset.id;
    const auto hash = merged.value().artifact.content_sha256;
    auto backup = service->create_backup((root / "backup").string());
    ASSERT_TRUE(backup) << backup.error().message;
    EXPECT_GE(backup.value().derived_count, 2);
    EXPECT_TRUE(service->verify_backup((root / "backup").string()));
    service.reset();
    ASSERT_TRUE(open_service(false));
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 3);
    PreviewRequest preview;
    preview.asset_id = id;
    preview.max_edge = 96;
    preview.prefer_embedded_preview = false;
    auto viewed = service->request_preview(preview);
    ASSERT_TRUE(viewed) << viewed.error().message;
    EXPECT_EQ(viewed.value().width, 96);
    ExportRequest output;
    output.asset_id = id;
    output.output_path = (root / "reexport.png").string();
    auto exported = service->export_asset(output);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(sha256_file_hex(merged.value().output_path).value(), hash);
}

TEST_F(CatalogServiceTest, PhotoMergeBackupRestoreRetainsPixelsAndProvenance)
{
    ASSERT_TRUE(open_service(true));
    auto request = prepare_merge(*service, root);
    ASSERT_TRUE(request);
    auto merged = service->merge_selected_photos(request.value());
    ASSERT_TRUE(merged);
    const auto backup_path = root / "merge-backup";
    auto backed = service->create_backup(backup_path.string());
    ASSERT_TRUE(backed);
    ASSERT_TRUE(service->close());
    service.reset();
    auto recovery = FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(recovery);
    const SqliteCatalogBackupVerifier verifier;
    CatalogRestoreRequest restore;
    restore.backup_directory = backup_path.string();
    restore.destination_catalog = (root / "restored.sqlite").string();
    auto restored = restore_catalog_backup(verifier, verifier, *recovery.value(), restore);
    ASSERT_TRUE(restored) << restored.error().message;
    database_path = restore.destination_catalog;
    ASSERT_TRUE(open_service(false));
    auto assets = service->list_assets();
    ASSERT_TRUE(assets);
    EXPECT_EQ(assets.value().size(), 3);
    const auto found =
        std::find_if(assets.value().begin(), assets.value().end(),
                     [&](const auto &asset) { return asset.id == merged.value().asset.id; });
    ASSERT_NE(found, assets.value().end());
    auto location = normalize_local_input(found->normalized_uri);
    ASSERT_TRUE(location);
    EXPECT_NE(location.value().path, merged.value().output_path);
    EXPECT_EQ(sha256_file_hex(location.value().path).value(),
              merged.value().artifact.content_sha256);
    EXPECT_TRUE(std::filesystem::exists(location.value().path + ".ravo-merge.json"));
    PreviewRequest preview;
    preview.asset_id = found->id;
    preview.max_edge = 96;
    auto viewed = service->request_preview(preview);
    ASSERT_TRUE(viewed) << viewed.error().message;
}

TEST_F(CatalogServiceTest, PhotoMergeFrozenRawDecodePreservesSources)
{
    ASSERT_TRUE(open_service(true));
    const auto original = file_sha256(raw_fixture_path());
    PhotoMergeRequest request;
    request.max_edge = 320;
    request.options.auto_align = false;
    request.options.exposure_ev = {0, 1};
    for (int i = 0; i < 2; ++i)
    {
        const auto path = root / ("raw-" + std::to_string(i) + ".cr2");
        std::filesystem::copy_file(raw_fixture_path(), path);
        auto imported = service->import_one(path.string(), {}, ImportPreviewPolicy::kMinimal, true);
        ASSERT_TRUE(imported);
        ASSERT_TRUE(imported.value().asset);
        request.asset_ids.push_back(imported.value().asset->id);
    }
    request.expected_catalog_revision = service->snapshot().value().revision;
    // This verifies real RAW decode/publication, not exposure-bracket quality:
    // the two immutable copies intentionally share one sensor exposure.
    auto merged = service->merge_selected_photos(request);
    ASSERT_TRUE(merged) << merged.error().message;
    EXPECT_LE(std::max(merged.value().artifact.width, merged.value().artifact.height), 320);
    EXPECT_EQ(file_sha256(raw_fixture_path()), original);
    EXPECT_EQ(file_sha256((root / "raw-0.cr2").string()), original);
    EXPECT_EQ(file_sha256((root / "raw-1.cr2").string()), original);
}

TEST_F(CatalogServiceTest, PhotoMergeConflictsAndMissingExposurePublishNothing)
{
    ASSERT_TRUE(open_service(true));
    auto prepared = prepare_merge(*service, root);
    ASSERT_TRUE(prepared);
    auto request = prepared.value();
    request.output_path = (root / "output.tiff").string();
    request.expected_catalog_revision = *request.expected_catalog_revision - 1;
    auto stale = service->merge_selected_photos(request);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, ErrorCode::kConflict);
    request = prepared.value();
    request.output_path = (root / "output.tiff").string();
    request.options.exposure_ev.clear();
    auto no_exposure = service->merge_selected_photos(request);
    ASSERT_FALSE(no_exposure);
    EXPECT_EQ(no_exposure.error().context.at("reason"), "hdr_exposure_required");
    request = prepared.value();
    request.asset_ids[1] = request.asset_ids[0];
    EXPECT_FALSE(service->merge_selected_photos(request));
    EXPECT_FALSE(std::filesystem::exists(root / "output.tiff"));
    EXPECT_EQ(service->list_assets().value().size(), 2);
    request = prepared.value();
    request.output_path = (root / "existing.tiff").string();
    ASSERT_TRUE(publish_text_artifact_no_replace(request.output_path, "competitor", {}));
    auto collision = service->merge_selected_photos(request);
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code, ErrorCode::kConflict);
    EXPECT_EQ(read_utf8_text_file(request.output_path, 100).value(), "competitor");
}

TEST_F(CatalogServiceTest, PhotoMergeCancelSourceChangeAndCommitFailureCleanOwnedArtifacts)
{
    ASSERT_TRUE(open_service(true));
    auto prepared = prepare_merge(*service, root);
    ASSERT_TRUE(prepared);
    auto request = prepared.value();
    request.output_path = (root / "cancelled.tiff").string();
    CancellationSource cancel;
    request.cancellation = cancel.token();
    testing::CatalogServiceTestControl::set_merge_checkpoint(
        *service,
        [&](std::string_view stage) -> Result<void>
        {
            if (stage == "before_catalog_commit")
                static_cast<void>(cancel.cancel("test"));
            return {};
        });
    auto cancelled = service->merge_selected_photos(request);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(request.output_path));
    EXPECT_FALSE(std::filesystem::exists(request.output_path + ".ravo-merge.json"));
    EXPECT_EQ(service->snapshot().value().revision, *prepared.value().expected_catalog_revision);
    testing::CatalogServiceTestControl::set_merge_checkpoint(*service, {});
    request = prepared.value();
    request.output_path = (root / "aborted.tiff").string();
    testing::SqliteCatalogTestControl::inject(*sqlite_repository,
                                              testing::SqliteImportFailure::kCommit);
    auto aborted = service->merge_selected_photos(request);
    ASSERT_FALSE(aborted);
    EXPECT_FALSE(std::filesystem::exists(request.output_path));
    EXPECT_EQ(service->list_assets().value().size(), 2);
    EXPECT_EQ(service->snapshot().value().revision, *request.expected_catalog_revision);
    request.output_path = (root / "source-changed.tiff").string();
    testing::CatalogServiceTestControl::set_merge_checkpoint(
        *service,
        [&](std::string_view stage) -> Result<void>
        {
            if (stage == "before_publication")
                if (!write_merge_input(root / "input-1.png", 190))
                    return make_error(ErrorCode::kIo, "fixture rewrite failed");
            return {};
        });
    auto changed = service->merge_selected_photos(request);
    ASSERT_FALSE(changed);
    EXPECT_EQ(changed.error().context.at("reason"), "merge_source_changed");
    EXPECT_FALSE(std::filesystem::exists(request.output_path));
    EXPECT_EQ(service->list_assets().value().size(), 2);
}

TEST_F(CatalogServiceTest, PhotoMergeRejectsConcurrentRevisionAtPublication)
{
    ASSERT_TRUE(open_service(true));
    auto prepared = prepare_merge(*service, root);
    ASSERT_TRUE(prepared);
    auto request = prepared.value();
    request.output_path = (root / "concurrent.tiff").string();
    testing::CatalogServiceTestControl::set_merge_checkpoint(
        *service,
        [&](std::string_view stage) -> Result<void>
        {
            if (stage == "before_catalog_commit")
            {
                auto changed = service->set_rating(request.asset_ids[0], 4);
                if (!changed)
                    return changed.error();
            }
            return {};
        });
    auto conflicted = service->merge_selected_photos(request);
    ASSERT_FALSE(conflicted);
    EXPECT_EQ(conflicted.error().context.at("reason"), "stale_catalog_revision");
    EXPECT_FALSE(std::filesystem::exists(request.output_path));
    EXPECT_EQ(service->list_assets().value().size(), 2);
}
} // namespace ravo
