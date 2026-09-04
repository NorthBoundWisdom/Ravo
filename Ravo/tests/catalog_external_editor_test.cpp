#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/adapters/text_file.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/external_editor.h"

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

TEST_F(CatalogServiceTest, ExternalEditorRegisterCreatesDerivedAssetWithoutTouchingOriginal)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "source.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(20, 40, 80)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto source_id = imported.value().asset->id;
    const auto original = original_path_for(*service, source_id);
    ASSERT_FALSE(original.empty());
    const auto before_sha = sha256_file_hex(original);
    ASSERT_TRUE(before_sha) << before_sha.error().message;
    auto before_identity = read_file_identity(original);
    ASSERT_TRUE(before_identity) << before_identity.error().message;

    const auto editor_out = root / "editor-output.jpg";
    ASSERT_TRUE(write_jpeg(editor_out, QColor(200, 30, 30)));
    auto snapshot = service->snapshot();
    ASSERT_TRUE(snapshot) << snapshot.error().message;

    ExternalEditorRegisterRequest request;
    request.source_asset_id = source_id;
    request.editor_output_path = editor_out.string();
    request.editor_id = "photoshop";
    request.editor_version = "2026";
    request.expected_catalog_revision = snapshot.value().revision;
    auto registered = service->register_external_editor_output(request);
    ASSERT_TRUE(registered) << registered.error().message;
    EXPECT_TRUE(registered.value().source_original_unchanged);
    EXPECT_NE(registered.value().derived_asset.id, source_id);
    EXPECT_EQ(registered.value().provenance.schema, kExternalEditorDerivedContractVersion);
    EXPECT_EQ(registered.value().provenance.source_asset_id, source_id);
    EXPECT_EQ(registered.value().provenance.editor_id, "photoshop");
    EXPECT_EQ(registered.value().provenance.editor_version, std::optional<std::string>{"2026"});
    EXPECT_NE(registered.value().derived_asset.normalized_uri,
              imported.value().asset->normalized_uri);

    const auto after_sha = sha256_file_hex(original);
    ASSERT_TRUE(after_sha) << after_sha.error().message;
    EXPECT_EQ(after_sha.value(), before_sha.value());
    auto after_identity = read_file_identity(original);
    ASSERT_TRUE(after_identity) << after_identity.error().message;
    EXPECT_EQ(after_identity.value().size_bytes, before_identity.value().size_bytes);
    EXPECT_EQ(after_identity.value().mtime_unix_ms, before_identity.value().mtime_unix_ms);

    auto shown = service->external_editor_provenance(registered.value().derived_asset.id);
    ASSERT_TRUE(shown) << shown.error().message;
    EXPECT_EQ(shown.value().derived_asset_id, registered.value().derived_asset.id);
    EXPECT_EQ(shown.value().source_original.sha256, before_sha.value());

    // Same destination is fail-closed (no-replace).
    request.expected_catalog_revision = std::nullopt;
    auto again = service->register_external_editor_output(request);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::kConflict);
    EXPECT_EQ(again.error().context.at("reason"), "derived_destination_exists");

    // Registering the original itself is forbidden.
    ExternalEditorRegisterRequest same_file = request;
    same_file.editor_output_path = original;
    auto blocked = service->register_external_editor_output(same_file);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, ErrorCode::kConflict);
    EXPECT_EQ(blocked.error().context.at("reason"), "editor_output_is_source_original");

    // Stale revision fails closed.
    ExternalEditorRegisterRequest stale = request;
    const auto other_out = root / "editor-output-2.jpg";
    ASSERT_TRUE(write_jpeg(other_out, QColor(10, 180, 40)));
    stale.editor_output_path = other_out.string();
    stale.expected_catalog_revision = snapshot.value().revision - 1;
    auto stale_blocked = service->register_external_editor_output(stale);
    ASSERT_FALSE(stale_blocked);
    EXPECT_EQ(stale_blocked.error().code, ErrorCode::kConflict);
    EXPECT_EQ(stale_blocked.error().context.at("reason"), "stale_catalog_revision");

    // Cancel before work leaves no derived asset.
    CancellationSource cancel_source;
    ASSERT_TRUE(cancel_source.cancel("test-timeout"));
    ExternalEditorRegisterRequest cancelled_req = request;
    cancelled_req.editor_output_path = other_out.string();
    cancelled_req.expected_catalog_revision = std::nullopt;
    cancelled_req.cancellation = cancel_source.token();
    auto cancelled = service->register_external_editor_output(cancelled_req);
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

TEST_F(CatalogServiceTest, ExternalEditorRejectsEmptyEditorId)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "source2.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(1, 2, 3)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto editor_out = root / "out2.jpg";
    ASSERT_TRUE(write_jpeg(editor_out, QColor(4, 5, 6)));
    ExternalEditorRegisterRequest request;
    request.source_asset_id = imported.value().asset->id;
    request.editor_output_path = editor_out.string();
    request.editor_id = "";
    auto failed = service->register_external_editor_output(request);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(failed.error().context.at("reason"), "invalid_editor_id");
}

TEST_F(CatalogServiceTest, BackupRestorePreservesDerivedAndExternalEditorTrees)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "backup-source.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(11, 22, 33)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto source_id = imported.value().asset->id;
    const auto original = original_path_for(*service, source_id);
    ASSERT_FALSE(original.empty());
    const auto original_sha = sha256_file_hex(original);
    ASSERT_TRUE(original_sha) << original_sha.error().message;

    const auto editor_out = root / "backup-editor-out.jpg";
    ASSERT_TRUE(write_jpeg(editor_out, QColor(240, 10, 10)));
    ExternalEditorRegisterRequest request;
    request.source_asset_id = source_id;
    request.editor_output_path = editor_out.string();
    request.editor_id = "photoshop";
    request.editor_version = "2026";
    auto registered = service->register_external_editor_output(request);
    ASSERT_TRUE(registered) << registered.error().message;
    const auto derived_id = registered.value().derived_asset.id;
    const auto derived_location =
        normalize_local_input(registered.value().derived_asset.normalized_uri);
    ASSERT_TRUE(derived_location) << derived_location.error().message;
    const auto derived_sha = sha256_file_hex(derived_location.value().path);
    ASSERT_TRUE(derived_sha) << derived_sha.error().message;

    const auto backup_path = root / "derived-backup";
    auto backup = service->create_backup(backup_path.string());
    ASSERT_TRUE(backup) << backup.error().message;
    EXPECT_EQ(backup.value().format_version, kCatalogBackupFormatVersion);
    EXPECT_GE(backup.value().derived_count, 1U);
    EXPECT_GE(backup.value().external_editor_count, 1U);
    EXPECT_TRUE(std::filesystem::is_directory(backup_path / "derived"));
    EXPECT_TRUE(std::filesystem::is_directory(backup_path / "external-editor"));
    EXPECT_FALSE(std::filesystem::exists(backup_path / "originals"));

    auto verified = service->verify_backup(backup_path.string());
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified.value().artifact.derived_count, backup.value().derived_count);
    EXPECT_EQ(verified.value().artifact.external_editor_count,
              backup.value().external_editor_count);
    EXPECT_FALSE(verified.value().originals_included);

    ASSERT_TRUE(service->close());
    service.reset();
    auto backup_recovery =
        FilesystemRecoveryStore::open_existing((backup_path / "sidecars").string());
    ASSERT_TRUE(backup_recovery) << backup_recovery.error().message;
    const SqliteCatalogBackupVerifier verifier;
    const auto restored_path = (root / "restored-with-derived.sqlite").string();
    CatalogRestoreRequest restore_request;
    restore_request.backup_directory = backup_path.string();
    restore_request.destination_catalog = restored_path;
    auto restored =
        restore_catalog_backup(verifier, verifier, *backup_recovery.value(), restore_request);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_TRUE(std::filesystem::is_directory(restored_path + ".ravo/derived"));
    EXPECT_TRUE(std::filesystem::is_directory(restored_path + ".ravo/external-editor"));

    // Byte-compare restored support trees against the verified backup package.
    std::error_code walk_error;
    std::size_t restored_derived_files = 0U;
    for (std::filesystem::recursive_directory_iterator it(backup_path / "derived", walk_error), end;
         it != end; it.increment(walk_error))
    {
        ASSERT_FALSE(walk_error) << walk_error.message();
        if (!it->is_regular_file())
            continue;
        const auto relative = std::filesystem::relative(it->path(), backup_path / "derived");
        const auto restored_file =
            std::filesystem::path(restored_path + ".ravo/derived") / relative;
        ASSERT_TRUE(std::filesystem::is_regular_file(restored_file)) << restored_file.string();
        const auto backup_digest = sha256_file_hex(it->path().string());
        const auto restored_digest = sha256_file_hex(restored_file.string());
        ASSERT_TRUE(backup_digest) << backup_digest.error().message;
        ASSERT_TRUE(restored_digest) << restored_digest.error().message;
        EXPECT_EQ(restored_digest.value(), backup_digest.value());
        ++restored_derived_files;
    }
    EXPECT_EQ(restored_derived_files, backup.value().derived_count);

    std::size_t restored_external_files = 0U;
    for (std::filesystem::recursive_directory_iterator
             it(backup_path / "external-editor", walk_error),
         end;
         it != end; it.increment(walk_error))
    {
        ASSERT_FALSE(walk_error) << walk_error.message();
        if (!it->is_regular_file())
            continue;
        const auto relative =
            std::filesystem::relative(it->path(), backup_path / "external-editor");
        const auto restored_file =
            std::filesystem::path(restored_path + ".ravo/external-editor") / relative;
        ASSERT_TRUE(std::filesystem::is_regular_file(restored_file)) << restored_file.string();
        const auto backup_digest = sha256_file_hex(it->path().string());
        const auto restored_digest = sha256_file_hex(restored_file.string());
        ASSERT_TRUE(backup_digest) << backup_digest.error().message;
        ASSERT_TRUE(restored_digest) << restored_digest.error().message;
        EXPECT_EQ(restored_digest.value(), backup_digest.value());
        ++restored_external_files;
    }
    EXPECT_EQ(restored_external_files, backup.value().external_editor_count);
    EXPECT_TRUE(std::filesystem::is_regular_file(restored_path + ".ravo/external-editor/" +
                                                 derived_id + ".json"));

    // Removing the source catalog support must not remove restored derived bytes.
    std::error_code remove_error;
    std::filesystem::remove_all(database_path + ".ravo", remove_error);
    ASSERT_FALSE(remove_error) << remove_error.message();
    EXPECT_TRUE(std::filesystem::is_directory(restored_path + ".ravo/derived"));
    EXPECT_EQ(sha256_file_hex(original).value(), original_sha.value());
}

} // namespace ravo
