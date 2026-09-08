#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

// REL-01: mixed-corpus reopen/recovery qualification.
// Public path uses the checked-in PNG fixture + temp catalogs (no fake C3).
// Private photographer corpora stay UNTESTED unless RAVO_REL01_PRIVATE_CORPUS is set.

class MixedCorpusReopenRecoveryTest : public CatalogServiceTest
{
};

TEST_F(MixedCorpusReopenRecoveryTest, PublicFixtureImportBackupRestoreReopen)
{
    ASSERT_TRUE(open_service(true));
    auto imported = service->import().import_one(png_fixture_path(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    ASSERT_TRUE(service->library().set_rating(asset_id, 3));

    const auto backup_path = root / "rel01-backup";
    auto backup = service->recovery().create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;

    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "rel01-restored.sqlite").string();
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = restored_path;
    auto restored =
        restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request, {});
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
    auto synchronized = restored_service.recovery().sync_recovery(std::nullopt);
    ASSERT_TRUE(synchronized) << synchronized.error().message;
    auto restored_assets = restored_service.library().list_assets();
    ASSERT_TRUE(restored_assets) << restored_assets.error().message;
    ASSERT_EQ(restored_assets.value().size(), 1U);
    EXPECT_EQ(restored_assets.value().front().id, asset_id);
    EXPECT_EQ(restored_assets.value().front().review.rating, 3);
    ASSERT_TRUE(restored_service.close());
}

TEST_F(MixedCorpusReopenRecoveryTest, PrivateCorpusAvailabilityIsHonest)
{
    const char *corpus = std::getenv("RAVO_REL01_PRIVATE_CORPUS");
    if (corpus == nullptr || *corpus == '\0')
    {
        GTEST_SKIP() << "UNTESTED: private REL-01 corpus unavailable "
                        "(set RAVO_REL01_PRIVATE_CORPUS to exercise private C3)";
    }
    const std::filesystem::path root_path{corpus};
    if (!std::filesystem::is_directory(root_path))
    {
        GTEST_SKIP() << "UNTESTED: RAVO_REL01_PRIVATE_CORPUS is not a directory";
    }
    std::size_t entries = 0;
    for (const auto &entry : std::filesystem::directory_iterator(root_path))
    {
        static_cast<void>(entry);
        ++entries;
        break;
    }
    EXPECT_GT(entries, 0U);
}

} // namespace
} // namespace ravo
