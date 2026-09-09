#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QByteArray>
#include <gtest/gtest.h>

#include "catalog_test_support.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"

namespace ravo
{
namespace
{

// REL-01: mixed-corpus reopen/recovery qualification.
// Public path uses the checked-in PNG fixture + temp catalogs (no fake C3).
// Private photographer corpora stay UNTESTED unless RAVO_PHOTO_CORPUS is set.
// Availability probe != REL-01 evidence; RAVO_PHOTO_CORPUS is the only private
// photo corpus entry point.

constexpr std::size_t kPhotoCorpusOfflineVerticalMaxAssets = 8U;

struct CorpusSourceIdentity
{
    std::string private_path;
    std::string staged_name;
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type modified{};
    QByteArray sha256;
    FileIdentity identity{};
};

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

TEST_F(MixedCorpusReopenRecoveryTest, PhotoCorpusHarnessReportsUnavailableHonestly)
{
    const char *corpus = std::getenv("RAVO_PHOTO_CORPUS");
    if (corpus == nullptr || *corpus == '\0')
    {
        GTEST_SKIP() << "UNTESTED: RAVO_PHOTO_CORPUS unset "
                        "(availability probe only; not REL-01 evidence)";
    }
    const std::filesystem::path root_path{corpus};
    if (!std::filesystem::is_directory(root_path))
    {
        GTEST_SKIP() << "UNTESTED: RAVO_PHOTO_CORPUS is not a directory";
    }
    // Prove enumeration can begin. One directory entry is not mixed-corpus recovery
    // qualification and must not be described as REL-01 PASS evidence.
    std::error_code ec;
    auto it = std::filesystem::directory_iterator(root_path, ec);
    ASSERT_FALSE(ec) << ec.message();
    std::size_t entries = 0;
    for (; it != std::filesystem::directory_iterator{}; ++it)
    {
        static_cast<void>(*it);
        ++entries;
        break;
    }
    EXPECT_GT(entries, 0U);
}

TEST_F(MixedCorpusReopenRecoveryTest, PhotoCorpusOfflineRestoreReopenVertical)
{
    const char *corpus_env = std::getenv("RAVO_PHOTO_CORPUS");
    if (corpus_env == nullptr || *corpus_env == '\0')
    {
        GTEST_SKIP() << "UNTESTED: RAVO_PHOTO_CORPUS unset "
                        "(bounded offline restore vertical; not C3)";
    }
    const std::filesystem::path private_corpus{corpus_env};
    if (!std::filesystem::is_directory(private_corpus))
    {
        GTEST_SKIP() << "UNTESTED: RAVO_PHOTO_CORPUS is not a directory";
    }

    ASSERT_TRUE(open_service(true));

    auto enumerated =
        service->enumerate_import_inputs({private_corpus.string()}, CancellationToken{});
    ASSERT_TRUE(enumerated) << enumerated.error().message;
    if (enumerated.value().empty())
    {
        GTEST_SKIP() << "UNTESTED: RAVO_PHOTO_CORPUS enumeration produced no importable paths";
    }

    // Deterministic selection: sorted paths, prefer diverse extensions, cap at 8.
    std::vector<std::string> candidates = enumerated.value();
    std::sort(candidates.begin(), candidates.end());
    std::vector<std::string> selected;
    std::map<std::string, std::size_t> extension_counts;
    std::set<std::string> seen_extensions;
    auto extension_of = [](const std::string &path)
    {
        const auto ext = std::filesystem::path(path).extension().string();
        std::string lower;
        lower.reserve(ext.size());
        for (char ch : ext)
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        return lower;
    };
    for (const auto &path : candidates)
    {
        if (selected.size() >= kPhotoCorpusOfflineVerticalMaxAssets)
            break;
        const auto ext = extension_of(path);
        if (!ext.empty() && seen_extensions.insert(ext).second)
            selected.push_back(path);
    }
    for (const auto &path : candidates)
    {
        if (selected.size() >= kPhotoCorpusOfflineVerticalMaxAssets)
            break;
        if (std::find(selected.begin(), selected.end(), path) == selected.end())
            selected.push_back(path);
    }
    ASSERT_FALSE(selected.empty());
    for (const auto &path : selected)
        ++extension_counts[extension_of(path)];

    // Record omitted format classes honestly — do not invent PNG/TIFF/X-Trans coverage.
    const std::set<std::string> interesting{".png", ".tif", ".tiff", ".raf"};
    std::vector<std::string> omitted;
    for (const auto &ext : interesting)
    {
        if (!extension_counts.count(ext))
            omitted.push_back(ext);
    }
    RecordProperty("selected_file_count", static_cast<int>(selected.size()));
    RecordProperty("distinct_extensions", static_cast<int>(extension_counts.size()));
    RecordProperty("omitted_interesting_extensions", static_cast<int>(omitted.size()));
    static_cast<void>(extension_counts);

    std::vector<CorpusSourceIdentity> private_identities;
    private_identities.reserve(selected.size());
    for (std::size_t index = 0; index < selected.size(); ++index)
    {
        CorpusSourceIdentity snap;
        snap.private_path = selected[index];
        snap.staged_name = "src-" + std::to_string(index) +
                           std::filesystem::path(selected[index]).extension().string();
        std::error_code error;
        snap.size = std::filesystem::file_size(snap.private_path, error);
        ASSERT_FALSE(error) << snap.private_path << ": " << error.message();
        snap.modified = std::filesystem::last_write_time(snap.private_path, error);
        ASSERT_FALSE(error) << snap.private_path << ": " << error.message();
        snap.sha256 = file_sha256(snap.private_path);
        ASSERT_EQ(snap.sha256.size(), 32) << snap.private_path;
        auto identity = read_file_identity(snap.private_path);
        ASSERT_TRUE(identity) << identity.error().message;
        snap.identity = identity.value();
        private_identities.push_back(std::move(snap));
    }

    const auto mounted = root / "mounted-source";
    const auto offline = root / "offline-source";
    std::filesystem::create_directories(mounted);
    for (const auto &snap : private_identities)
    {
        const auto dest = mounted / snap.staged_name;
        std::error_code error;
        std::filesystem::copy_file(snap.private_path, dest, std::filesystem::copy_options::none,
                                   error);
        ASSERT_FALSE(error) << snap.private_path << " -> " << dest.string() << ": "
                            << error.message();
        std::filesystem::last_write_time(dest, snap.modified, error);
        ASSERT_FALSE(error) << dest.string() << ": " << error.message();
        EXPECT_EQ(file_sha256(dest.string()), snap.sha256);
        EXPECT_EQ(std::filesystem::file_size(dest), snap.size);
        auto staged_identity = read_file_identity(dest.string());
        ASSERT_TRUE(staged_identity) << staged_identity.error().message;
        EXPECT_EQ(staged_identity.value().size_bytes, snap.identity.size_bytes);
        EXPECT_EQ(staged_identity.value().mtime_unix_ms, snap.identity.mtime_unix_ms);
    }

    std::vector<std::string> asset_ids;
    asset_ids.reserve(private_identities.size());
    for (const auto &snap : private_identities)
    {
        const auto staged_path = (mounted / snap.staged_name).string();
        auto imported = service->import().import_one(staged_path, CancellationToken{});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_TRUE(imported.value().asset)
            << (imported.value().error ? imported.value().error->message : "no asset");
        asset_ids.push_back(imported.value().asset->id);
    }
    ASSERT_FALSE(asset_ids.empty());
    const auto rated_id = asset_ids.front();
    ASSERT_TRUE(service->library().set_rating(rated_id, 3));

    const auto backup_path = root / "corpus-offline-backup";
    auto backup = service->recovery().create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    auto verified = service->recovery().verify_backup(backup_path.string());
    ASSERT_TRUE(verified) << verified.error().message;
    ASSERT_TRUE(service->close());

    std::error_code rename_error;
    std::filesystem::rename(mounted, offline, rename_error);
    ASSERT_FALSE(rename_error) << rename_error.message();
    ASSERT_FALSE(std::filesystem::exists(mounted));
    ASSERT_TRUE(std::filesystem::exists(offline));

    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "corpus-offline-restored.sqlite").string();
    CatalogRestoreRequest request;
    request.backup_directory = backup_path.string();
    request.destination_catalog = restored_path;
    auto restored =
        restore_catalog_backup(verifier, verifier, *backup_recovery.value(), request, {});
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(restored.value().published);

    auto open_restored = [&]()
    {
        auto restored_repository = SqliteCatalogRepository::open(restored_path);
        EXPECT_TRUE(restored_repository) << restored_repository.error().message;
        auto restored_cache = FilesystemPreviewCache::create(restored_path + ".preview");
        EXPECT_TRUE(restored_cache) << restored_cache.error().message;
        auto restored_recovery = FilesystemRecoveryStore::create_for_catalog(restored_path);
        EXPECT_TRUE(restored_recovery) << restored_recovery.error().message;
        return CatalogService(
            engine, std::move(restored_repository).value(), std::make_unique<QtRasterDecoder>(),
            std::move(restored_cache).value(), std::move(restored_recovery).value());
    };

    {
        CatalogService restored_service = open_restored();
        auto synchronized = restored_service.recovery().sync_recovery(std::nullopt);
        ASSERT_TRUE(synchronized) << synchronized.error().message;
        auto assets = restored_service.library().list_assets();
        ASSERT_TRUE(assets) << assets.error().message;
        ASSERT_EQ(assets.value().size(), asset_ids.size());
        bool found_rated = false;
        for (const auto &asset : assets.value())
        {
            EXPECT_NE(std::find(asset_ids.begin(), asset_ids.end(), asset.id), asset_ids.end());
            if (asset.id == rated_id)
            {
                found_rated = true;
                EXPECT_EQ(asset.review.rating, 3);
            }
            EXPECT_EQ(asset.import_state, kImportStateMissing)
                << "offline source must be explicit missing, not silently substituted";
        }
        EXPECT_TRUE(found_rated);
        ASSERT_TRUE(restored_service.close());
    }

    std::filesystem::rename(offline, mounted, rename_error);
    ASSERT_FALSE(rename_error) << rename_error.message();

    {
        CatalogService restored_service = open_restored();
        auto synchronized = restored_service.recovery().sync_recovery(std::nullopt);
        ASSERT_TRUE(synchronized) << synchronized.error().message;
        auto assets = restored_service.library().list_assets();
        ASSERT_TRUE(assets) << assets.error().message;
        ASSERT_EQ(assets.value().size(), asset_ids.size());
        for (const auto &asset : assets.value())
        {
            EXPECT_NE(asset.import_state, kImportStateMissing)
                << "restored mounted source should become usable again";
            if (asset.id == rated_id)
                EXPECT_EQ(asset.review.rating, 3);
        }
        ASSERT_TRUE(restored_service.close());
    }

    // Private corpus must remain byte-identical; never write/move private sources.
    for (const auto &snap : private_identities)
    {
        std::error_code error;
        EXPECT_EQ(std::filesystem::file_size(snap.private_path, error), snap.size);
        ASSERT_FALSE(error) << snap.private_path << ": " << error.message();
        EXPECT_EQ(std::filesystem::last_write_time(snap.private_path, error), snap.modified);
        ASSERT_FALSE(error) << snap.private_path << ": " << error.message();
        EXPECT_EQ(file_sha256(snap.private_path), snap.sha256);
        auto identity = read_file_identity(snap.private_path);
        ASSERT_TRUE(identity) << identity.error().message;
        EXPECT_EQ(identity.value().size_bytes, snap.identity.size_bytes);
        EXPECT_EQ(identity.value().mtime_unix_ms, snap.identity.mtime_unix_ms);
    }
}

} // namespace
} // namespace ravo
