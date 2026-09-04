#include <filesystem>
#include <fstream>
#include <string>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/services/catalog_service.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/services/dng_smart_preview.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color)
{
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(QString::fromStdString(path.string()), "JPEG", 90);
}

[[nodiscard]] std::string original_path_for(CatalogService &service, const std::string &asset_id)
{
    auto assets = service.list_assets();
    EXPECT_TRUE(assets) << assets.error().message;
    for (const auto &asset : assets.value())
    {
        if (asset.id != asset_id)
            continue;
        auto location = normalize_local_input(asset.normalized_uri);
        EXPECT_TRUE(location) << location.error().message;
        return location.value().path;
    }
    ADD_FAILURE() << "asset not listed: " << asset_id;
    return {};
}

} // namespace

TEST_F(CatalogServiceTest, DngConvertFailsClosedWithoutPackagedConverterAndKeepsOriginal)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "source.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(12, 34, 56)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());
    const auto before_sha = sha256_file_hex(original);
    ASSERT_TRUE(before_sha) << before_sha.error().message;
    auto before_identity = read_file_identity(original);
    ASSERT_TRUE(before_identity) << before_identity.error().message;

    EXPECT_FALSE(dng_converter_is_packaged());

    DngConversionRequest request;
    request.asset_id = asset_id;
    auto converted = service->convert_asset_to_dng(request);
    ASSERT_TRUE(converted) << converted.error().message;
    EXPECT_EQ(converted.value().asset_id, asset_id);
    EXPECT_FALSE(converted.value().converter_available);
    EXPECT_EQ(converted.value().reason, "dng_converter_unavailable");
    EXPECT_TRUE(converted.value().originals_unchanged);
    EXPECT_EQ(converted.value().source_path, original);

    const auto after_sha = sha256_file_hex(original);
    ASSERT_TRUE(after_sha) << after_sha.error().message;
    EXPECT_EQ(after_sha.value(), before_sha.value());
    auto after_identity = read_file_identity(original);
    ASSERT_TRUE(after_identity) << after_identity.error().message;
    EXPECT_EQ(after_identity.value().size_bytes, before_identity.value().size_bytes);
    EXPECT_EQ(after_identity.value().mtime_unix_ms, before_identity.value().mtime_unix_ms);

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_EQ(assets.value().size(), 1U);
}

TEST_F(CatalogServiceTest, SmartPreviewNeverAllowsDevelopFallback)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "browse.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(90, 10, 10)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;

    EXPECT_FALSE(smart_preview_encoder_is_packaged());

    auto status = service->smart_preview_status(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status.value().asset_id, asset_id);
    EXPECT_FALSE(status.value().encoder_available);
    EXPECT_FALSE(status.value().present);
    EXPECT_FALSE(status.value().develop_fallback);
    EXPECT_EQ(status.value().reason, "smart_preview_encoder_unavailable");

    SmartPreviewEnsureRequest ensure;
    ensure.asset_id = asset_id;
    auto ensured = service->ensure_smart_preview(ensure);
    ASSERT_TRUE(ensured) << ensured.error().message;
    EXPECT_FALSE(ensured.value().encoder_available);
    EXPECT_FALSE(ensured.value().present);
    EXPECT_FALSE(ensured.value().develop_fallback);
    EXPECT_EQ(ensured.value().reason, "smart_preview_encoder_unavailable");
}

TEST_F(CatalogServiceTest, DngConvertRejectsMissingAsset)
{
    ASSERT_TRUE(open_service(true));
    DngConversionRequest request;
    request.asset_id = "missing-asset";
    auto converted = service->convert_asset_to_dng(request);
    ASSERT_FALSE(converted);
    EXPECT_EQ(converted.error().code, ErrorCode::kNotFound);
}

TEST_F(CatalogServiceTest, BackupRestorePackagesDngConversionAndSmartPreviewTrees)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "dng-pack-source.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(1, 2, 3)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    const auto support = std::filesystem::path(database_path + ".ravo");
    const auto dng_dir = support / "dng-conversion";
    const auto smart_dir = support / "smart-previews" / asset_id;
    std::error_code ec;
    ASSERT_TRUE(std::filesystem::create_directories(dng_dir, ec)) << ec.message();
    ASSERT_TRUE(std::filesystem::create_directories(smart_dir, ec)) << ec.message();
    const auto dng_file = dng_dir / (asset_id + ".json");
    const auto smart_file = smart_dir / "browse.bin";
    {
        std::ofstream out(dng_file, std::ios::binary);
        out << "{\"schema\":\"ravo.dng.conversion/v1\",\"asset_id\":\"" << asset_id << "\"}";
        ASSERT_TRUE(out.good());
    }
    {
        std::ofstream out(smart_file, std::ios::binary);
        out << "smart-preview-bytes";
        ASSERT_TRUE(out.good());
    }
    const auto dng_sha = sha256_file_hex(dng_file.string());
    const auto smart_sha = sha256_file_hex(smart_file.string());
    ASSERT_TRUE(dng_sha) << dng_sha.error().message;
    ASSERT_TRUE(smart_sha) << smart_sha.error().message;

    const auto backup_path = root / "ingest-trees-backup";
    auto backup = service->create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    EXPECT_EQ(backup.value().format_version, kCatalogBackupFormatVersion);
    EXPECT_EQ(kCatalogBackupFormatVersion, 3);
    EXPECT_GE(backup.value().dng_conversion_count, 1U);
    EXPECT_GE(backup.value().smart_previews_count, 1U);
    EXPECT_TRUE(std::filesystem::is_directory(backup_path / "dng-conversion"));
    EXPECT_TRUE(std::filesystem::is_directory(backup_path / "smart-previews"));
    EXPECT_FALSE(std::filesystem::exists(backup_path / "originals"));
    EXPECT_FALSE(std::filesystem::exists(backup_path / "previews"));

    auto verified = service->verify_backup(backup_path.string());
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified.value().artifact.dng_conversion_count, backup.value().dng_conversion_count);
    EXPECT_EQ(verified.value().artifact.smart_previews_count, backup.value().smart_previews_count);

    ASSERT_TRUE(service->close());
    service.reset();
    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "restored-ingest-trees.sqlite").string();
    CatalogRestoreRequest restore_request;
    restore_request.backup_directory = backup_path.string();
    restore_request.destination_catalog = restored_path;
    auto restored =
        restore_catalog_backup(verifier, verifier, *backup_recovery.value(), restore_request);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(std::filesystem::is_directory(restored_path + ".ravo/dng-conversion"));
    EXPECT_TRUE(std::filesystem::is_directory(restored_path + ".ravo/smart-previews"));

    const auto restored_dng =
        std::filesystem::path(restored_path + ".ravo/dng-conversion") / (asset_id + ".json");
    const auto restored_smart =
        std::filesystem::path(restored_path + ".ravo/smart-previews") / asset_id / "browse.bin";
    ASSERT_TRUE(std::filesystem::is_regular_file(restored_dng));
    ASSERT_TRUE(std::filesystem::is_regular_file(restored_smart));
    const auto restored_dng_sha = sha256_file_hex(restored_dng.string());
    const auto restored_smart_sha = sha256_file_hex(restored_smart.string());
    ASSERT_TRUE(restored_dng_sha) << restored_dng_sha.error().message;
    ASSERT_TRUE(restored_smart_sha) << restored_smart_sha.error().message;
    EXPECT_EQ(restored_dng_sha.value(), dng_sha.value());
    EXPECT_EQ(restored_smart_sha.value(), smart_sha.value());
}

} // namespace ravo
