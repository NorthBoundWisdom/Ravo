#include <filesystem>
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <gtest/gtest.h>

#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/foundation/json.h"
#include "ravo/services/catalog_service.h"
#include "catalog_service_test_support.h"
#include "catalog_test_support.h"

namespace ravo
{
namespace
{
TEST_F(CatalogServiceTest, PrePickPhotoRecoveryBacksUpRestoresAndReopensWithoutRewritingSidecar)
{
    ASSERT_TRUE(open_service(true));
    const auto photo = root / "pre-pick.jpg";
    QImage image(24, 16, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(20, 80, 140));
    ASSERT_TRUE(image.save(QString::fromStdString(photo.string()), "JPEG", 90));
    const auto source_hash = file_sha256(photo.string());
    auto imported = service->import().import_one(photo.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->library().set_rejected(asset_id, true));
    auto state = service->recovery().recovery_state(asset_id);
    ASSERT_TRUE(state) << state.error().message;
    const auto recovery_root =
        std::filesystem::path(FilesystemRecoveryStore::default_root_for_catalog(database_path));
    const auto filename = asset_id + "." + std::to_string(state.value().generation) + ".ravo.json";
    const auto sidecar = recovery_root / filename;
    const auto historical = recovery_document_with_mutated_payload(
        sidecar,
        [](JsonValue::Object &payload)
        {
            auto asset = *payload.at("asset").object_if();
            asset.erase("picked");
            auto capture = *asset.at("capture").object_if();
            capture.erase("lens_make");
            capture.erase("lens_model");
            asset.insert_or_assign("capture", JsonValue{std::move(capture)});
            asset.insert_or_assign("metadata", JsonValue::Object{{"copyright", nullptr},
                                                                 {"creator", nullptr},
                                                                 {"description", nullptr},
                                                                 {"title", nullptr}});
            payload.insert_or_assign("asset", JsonValue{std::move(asset)});
        });
    ASSERT_FALSE(historical.empty());
    QFile output(QString::fromStdString(sidecar.string()));
    ASSERT_TRUE(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(output.write(historical.data(), static_cast<qint64>(historical.size())),
              static_cast<qint64>(historical.size()));
    output.close();
    const auto historical_hash = file_sha256(sidecar.string());
    auto recovery = FilesystemRecoveryStore::open_existing(recovery_root.string());
    ASSERT_TRUE(recovery) << recovery.error().message;
    ASSERT_TRUE(recovery.value()->verify(asset_id, state.value().generation, CancellationToken{}));
    const auto backup_path = root / "pre-pick-backup";
    auto backup = service->recovery().create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    EXPECT_EQ(file_sha256((backup_path / "sidecars" / filename).string()), historical_hash);
    const SqliteCatalogBackupVerifier verifier;
    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = (root / "pre-pick-restored.sqlite").string();
    auto restored = restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request);
    ASSERT_TRUE(restored) << restored.error().message;
    auto reopened = SqliteCatalogRepository::open(request.destination_catalog);
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto assets = reopened.value()->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 1U);
    EXPECT_EQ(assets.value().front().id, asset_id);
    EXPECT_TRUE(assets.value().front().review.rejected);
    EXPECT_FALSE(assets.value().front().review.picked);
    EXPECT_EQ(file_sha256(sidecar.string()), historical_hash);
    EXPECT_EQ(file_sha256((request.destination_catalog + ".ravo/sidecars/" + filename)),
              historical_hash);
    EXPECT_EQ(file_sha256(photo.string()), source_hash);
}

} // namespace
} // namespace ravo
