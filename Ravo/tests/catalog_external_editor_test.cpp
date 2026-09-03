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
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
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

} // namespace ravo
