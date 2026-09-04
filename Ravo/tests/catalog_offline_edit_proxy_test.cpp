#include <filesystem>
#include <string>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/offline_edit_proxy.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color)
{
    QImage image(64, 48, QImage::Format_RGB888);
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

TEST_F(CatalogServiceTest, OfflineEditProxyCreateVerifyAndDistinctFromSmartPreview)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "offline-source.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(20, 40, 60)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());
    const auto before_sha = sha256_file_hex(original);
    ASSERT_TRUE(before_sha) << before_sha.error().message;

    OfflineEditProxyCreateRequest blocked;
    blocked.asset_id = asset_id;
    blocked.user_initiated = false;
    auto missing_flag = service->create_offline_edit_proxy(blocked);
    ASSERT_FALSE(missing_flag);
    EXPECT_EQ(missing_flag.error().code, ErrorCode::kInvalidArgument);

    OfflineEditProxyCreateRequest request;
    request.asset_id = asset_id;
    request.user_initiated = true;
    request.max_edge = 32;
    auto created = service->create_offline_edit_proxy(request);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_TRUE(created.value().originals_unchanged);
    EXPECT_EQ(created.value().manifest.asset_id, asset_id);
    EXPECT_EQ(created.value().manifest.profile, "srgb");
    EXPECT_EQ(created.value().manifest.max_edge, 32U);
    EXPECT_FALSE(created.value().manifest.proxy_path.empty());
    EXPECT_TRUE(std::filesystem::is_regular_file(created.value().manifest.proxy_path));
    EXPECT_TRUE(created.value().manifest.proxy_path.find("offline-edit-proxies") !=
                std::string::npos);
    EXPECT_TRUE(created.value().manifest.proxy_path.find("smart-previews") == std::string::npos);

    const auto after_sha = sha256_file_hex(original);
    ASSERT_TRUE(after_sha) << after_sha.error().message;
    EXPECT_EQ(after_sha.value(), before_sha.value());

    auto status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status.value().media_state, OfflineEditMediaState::kOriginal);
    EXPECT_TRUE(status.value().proxy_present);
    EXPECT_TRUE(status.value().proxy_verified);
    EXPECT_TRUE(status.value().usable_for_develop);
    EXPECT_TRUE(status.value().usable_for_export);

    auto listed = service->list_offline_edit_proxies();
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().asset_id, asset_id);

    auto smart = service->smart_preview_status(asset_id);
    ASSERT_TRUE(smart) << smart.error().message;
    EXPECT_FALSE(smart.value().develop_fallback);
}

TEST_F(CatalogServiceTest, OfflineEditProxyDevelopApplyExportRejectAndReconnect)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "offline-roundtrip.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(90, 10, 10)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 48;
    auto created = service->create_offline_edit_proxy(create);
    ASSERT_TRUE(created) << created.error().message;

    const auto stashed = root / "stashed-original.jpg";
    std::error_code ec;
    std::filesystem::rename(original, stashed, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto offline_status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(offline_status) << offline_status.error().message;
    EXPECT_EQ(offline_status.value().media_state, OfflineEditMediaState::kProxy);
    EXPECT_TRUE(offline_status.value().usable_for_develop);
    EXPECT_FALSE(offline_status.value().usable_for_export);

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params) << params.error().message;
    params.value().exposure_ev = 0.35;
    auto saved = service->save_develop(asset_id, params.value());
    ASSERT_TRUE(saved) << saved.error().message;
    auto reloaded = service->load_recipe(asset_id);
    ASSERT_TRUE(reloaded) << reloaded.error().message;
    auto reloaded_params = develop_from_recipe(reloaded.value());
    ASSERT_TRUE(reloaded_params) << reloaded_params.error().message;
    EXPECT_NEAR(reloaded_params.value().exposure_ev, 0.35, 1e-6);

    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = (root / "should-fail.png").string();
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kNotFound);
    ASSERT_TRUE(exported.error().context.contains("reason"));
    EXPECT_EQ(exported.error().context.at("reason"), "proxy_export_forbidden");

    std::filesystem::rename(stashed, original, ec);
    ASSERT_FALSE(ec) << ec.message();

    OfflineEditProxyReconnectRequest reconnect;
    reconnect.asset_id = asset_id;
    reconnect.user_initiated = true;
    auto reconnected = service->reconnect_offline_edit_proxy(reconnect);
    ASSERT_TRUE(reconnected) << reconnected.error().message;
    EXPECT_TRUE(reconnected.value().source_hash_matched);
    EXPECT_TRUE(reconnected.value().offline_states_cleared);
    EXPECT_EQ(reconnected.value().status.media_state, OfflineEditMediaState::kOriginal);
    EXPECT_EQ(reconnected.value().status.reason, "reconnect_verified");
    EXPECT_TRUE(reconnected.value().status.usable_for_export);

    auto listed_after = service->list_assets();
    ASSERT_TRUE(listed_after) << listed_after.error().message;
    const AssetRecord *after_asset = nullptr;
    for (const auto &asset : listed_after.value())
    {
        if (asset.id == asset_id)
        {
            after_asset = &asset;
            break;
        }
    }
    ASSERT_NE(after_asset, nullptr);
    EXPECT_EQ(after_asset->import_state, kImportStateImported);

    export_request.output_path = (root / "after-reconnect.png").string();
    auto exported_ok = service->export_asset(export_request);
    ASSERT_TRUE(exported_ok) << exported_ok.error().message;
}

TEST_F(CatalogServiceTest, OfflineEditProxyReconnectRejectsHashMismatch)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "offline-mismatch.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(1, 2, 3)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 32;
    ASSERT_TRUE(service->create_offline_edit_proxy(create));

    const auto stashed = root / "mismatch-stashed.jpg";
    std::error_code ec;
    std::filesystem::rename(original, stashed, ec);
    ASSERT_FALSE(ec) << ec.message();
    ASSERT_TRUE(write_jpeg(original, QColor(200, 200, 200)));

    OfflineEditProxyReconnectRequest reconnect;
    reconnect.asset_id = asset_id;
    reconnect.user_initiated = true;
    auto reconnected = service->reconnect_offline_edit_proxy(reconnect);
    ASSERT_FALSE(reconnected);
    EXPECT_EQ(reconnected.error().code, ErrorCode::kConflict);
    ASSERT_TRUE(reconnected.error().context.contains("reason"));
    EXPECT_EQ(reconnected.error().context.at("reason"), "source_hash_mismatch");
}

TEST_F(CatalogServiceTest, OfflineEditProxyReconnectClearsMissingImportState)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "offline-missing-clear.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(30, 60, 90)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 32;
    ASSERT_TRUE(service->create_offline_edit_proxy(create));

    const auto stashed = root / "missing-clear-stashed.jpg";
    std::error_code ec;
    std::filesystem::rename(original, stashed, ec);
    ASSERT_FALSE(ec) << ec.message();

    // Preview marks the catalog asset missing when the original disappears.
    PreviewRequest preview;
    preview.asset_id = asset_id;
    preview.max_edge = 64;
    preview.persist_preview_record = true;
    static_cast<void>(service->request_preview(preview));
    auto listed_missing = service->list_assets();
    ASSERT_TRUE(listed_missing);
    bool saw_missing = false;
    for (const auto &asset : listed_missing.value())
    {
        if (asset.id == asset_id && asset.import_state == kImportStateMissing)
            saw_missing = true;
    }
    EXPECT_TRUE(saw_missing);

    std::filesystem::rename(stashed, original, ec);
    ASSERT_FALSE(ec) << ec.message();

    OfflineEditProxyReconnectRequest reconnect;
    reconnect.asset_id = asset_id;
    reconnect.user_initiated = true;
    auto reconnected = service->reconnect_offline_edit_proxy(reconnect);
    ASSERT_TRUE(reconnected) << reconnected.error().message;
    EXPECT_TRUE(reconnected.value().source_hash_matched);
    EXPECT_TRUE(reconnected.value().offline_states_cleared);
    EXPECT_EQ(reconnected.value().status.media_state, OfflineEditMediaState::kOriginal);
    EXPECT_TRUE(reconnected.value().status.usable_for_export);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed) << listed.error().message;
    bool cleared = false;
    for (const auto &asset : listed.value())
    {
        if (asset.id != asset_id)
            continue;
        EXPECT_EQ(asset.import_state, kImportStateImported);
        EXPECT_FALSE(asset.error_code.has_value());
        EXPECT_FALSE(asset.error_message.has_value());
        cleared = true;
    }
    EXPECT_TRUE(cleared);

    auto status = service->offline_edit_media_status(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status.value().media_state, OfflineEditMediaState::kOriginal);
    EXPECT_TRUE(status.value().usable_for_export);
    EXPECT_NE(status.value().reason, "proxy_ready");
}

} // namespace ravo
