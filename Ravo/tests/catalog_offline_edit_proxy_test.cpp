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
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/offline_edit_proxy.h"

#include "catalog_test_support.h"
#include "catalog_service_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color,
                              const int width = 64, const int height = 48)
{
    QImage image(width, height, QImage::Format_RGB888);
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
    ASSERT_EQ(listed.value().manifests.size(), 1U);
    EXPECT_EQ(listed.value().manifests.front().asset_id, asset_id);
    EXPECT_TRUE(listed.value().corrupt.empty());

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

TEST_F(CatalogServiceTest, OfflineEditProxyLoupeDevelopConsumeWhileOriginalMissing)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "offline-preview-source.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(30, 60, 90)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 64;
    auto created = service->create_offline_edit_proxy(create);
    ASSERT_TRUE(created) << created.error().message;

    const auto stashed = root / "stashed-preview-original.jpg";
    std::error_code ec;
    std::filesystem::rename(original, stashed, ec);
    ASSERT_FALSE(ec) << ec.message();

    auto status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status.value().media_state, OfflineEditMediaState::kProxy);
    EXPECT_TRUE(status.value().usable_for_develop);
    EXPECT_FALSE(status.value().usable_for_export);

    PreviewRequest loupe;
    loupe.asset_id = asset_id;
    loupe.max_edge = 64;
    loupe.purpose = PreviewPurpose::kDevelop;
    loupe.prefer_embedded_preview = false;
    auto previewed = service->request_preview(loupe);
    ASSERT_TRUE(previewed) << previewed.error().message;
    EXPECT_TRUE(previewed.value().original_missing);
    EXPECT_EQ(previewed.value().media_state, "proxy");
    EXPECT_GT(previewed.value().width, 0U);
    EXPECT_GT(previewed.value().height, 0U);
    EXPECT_FALSE(previewed.value().cache_path.empty());

    DevelopParams live;
    live.exposure_ev = 0.5;
    auto live_preview = service->request_preview(loupe, live);
    ASSERT_TRUE(live_preview) << live_preview.error().message;
    EXPECT_EQ(live_preview.value().media_state, "proxy");
    EXPECT_TRUE(live_preview.value().original_missing);

    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = (root / "offline-preview-export-should-fail.png").string();
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kNotFound);
    ASSERT_TRUE(exported.error().context.contains("reason"));
    EXPECT_EQ(exported.error().context.at("reason"), "proxy_export_forbidden");
}

TEST_F(CatalogServiceTest, Cor01OfflineProxyCorruptManifestAndPathEscapeFailClosed)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "cor01-proxy.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(11, 22, 33)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 64;
    auto created = service->create_offline_edit_proxy(create);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value().manifest.pixel_provenance,
              kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8);

    auto snap = service->snapshot();
    ASSERT_TRUE(snap);
    const auto proxy_root = std::filesystem::path(snap.value().database_path).string() +
                            ".ravo/offline-edit-proxies/" + asset_id;
    const auto manifest_path = proxy_root + "/manifest.json";
    {
        std::ofstream out(manifest_path, std::ios::binary | std::ios::trunc);
        out << "{\"schema\":\"ravo.offline-edit-proxy/v1\",\"schema_version\":1,"
               "\"asset_id\":\""
            << asset_id
            << "\",\"source_sha256\":\"not-a-hash\",\"source_size_bytes\":1,"
               "\"source_mtime_unix_ms\":1,\"recipe_cache_key\":\"x\",\"max_edge\":64,"
               "\"profile\":\"srgb\",\"proxy_path\":\"/tmp/escape.tif\","
               "\"proxy_sha256\":"
               "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
               "\"width\":1,\"height\":1,\"created_unix_ms\":1}";
    }
    auto listed = service->list_offline_edit_proxies();
    ASSERT_TRUE(listed) << listed.error().message;
    EXPECT_TRUE(listed.value().manifests.empty());
    ASSERT_FALSE(listed.value().corrupt.empty());
    EXPECT_FALSE(listed.value().corrupt.front().reason.empty());

    auto status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_FALSE(status);
}

TEST_F(CatalogServiceTest, Cor01OfflineProxyInterruptedPublishKeepsPrevious)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "cor01-proxy-keep.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(44, 55, 66)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 64;
    auto first = service->create_offline_edit_proxy(create);
    ASSERT_TRUE(first) << first.error().message;
    const auto first_sha = first.value().manifest.proxy_sha256;

    CancellationSource cancel;
    ASSERT_TRUE(cancel.cancel("before-second-publish"));
    OfflineEditProxyCreateRequest second = create;
    second.cancellation = cancel.token();
    // Cancelled token fails early before replacing the good proxy.
    auto failed = service->create_offline_edit_proxy(second);
    ASSERT_FALSE(failed);

    auto status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    ASSERT_TRUE(status.value().manifest);
    EXPECT_EQ(status.value().manifest->proxy_sha256, first_sha);
    EXPECT_TRUE(status.value().proxy_verified);
}

TEST_F(CatalogServiceTest, Cor01ExportUsableRequiresCatalogIdentity)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "cor01-identity.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(12, 34, 56)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());

    auto ok_status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(ok_status) << ok_status.error().message;
    EXPECT_TRUE(ok_status.value().usable_for_export);

    // Rewrite bytes in place so presence remains but catalog identity drifts.
    ASSERT_TRUE(write_jpeg(std::filesystem::path(original), QColor(200, 10, 10)));
    auto drifted = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(drifted) << drifted.error().message;
    EXPECT_TRUE(std::filesystem::is_regular_file(original));
    EXPECT_FALSE(drifted.value().usable_for_export);
    EXPECT_EQ(drifted.value().reason, "original_identity_unverified");

    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = (root / "identity-export-should-fail.png").string();
    export_request.format = ExportFormat::kPng;
    auto exported = service->export_asset(export_request);
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::kConflict);
    ASSERT_TRUE(exported.error().context.contains("reason"));
    EXPECT_EQ(exported.error().context.at("reason"), "source_identity_mismatch");
}

TEST_F(CatalogServiceTest, Cor01OfflineProxyPublishInjectRetainsPrior)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "cor01-proxy-inject.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(70, 80, 90)));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 64;
    auto first = service->create_offline_edit_proxy(create);
    ASSERT_TRUE(first) << first.error().message;
    const auto first_sha = first.value().manifest.proxy_sha256;

    testing::CatalogServiceTestControl::set_before_offline_proxy_publish(
        *service,
        [](std::string_view, std::string_view)
        {
            return make_error(
                ErrorCode::kIo, "Injected offline proxy publish failure",
                {{"reason", "offline_edit_proxy_publish_failed"}, {"detail", "injected_enospc"}});
        });
    auto failed = service->create_offline_edit_proxy(create);
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().context.at("reason"), "offline_edit_proxy_publish_failed");
    testing::CatalogServiceTestControl::set_before_offline_proxy_publish(*service, {});

    auto status = service->verify_offline_edit_proxy(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    ASSERT_TRUE(status.value().manifest);
    EXPECT_EQ(status.value().manifest->proxy_sha256, first_sha);
    EXPECT_TRUE(status.value().proxy_verified);
}

TEST_F(CatalogServiceTest, OfflineEditProxyBakedIdentityNoDoubleGradeBeforeAfterAndReconnect)
{
    ASSERT_TRUE(open_service(true));
    const auto source_path = root / "offline-baked.jpg";
    ASSERT_TRUE(write_jpeg(source_path, QColor(16, 32, 64), 320, 240));
    auto imported = service->import_one(source_path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;
    const auto original = original_path_for(*service, asset_id);
    ASSERT_FALSE(original.empty());

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params) << params.error().message;
    params.value().exposure_ev = 0.4;
    auto saved = service->save_develop(asset_id, params.value());
    ASSERT_TRUE(saved) << saved.error().message;

    OfflineEditProxyCreateRequest create;
    create.asset_id = asset_id;
    create.user_initiated = true;
    create.max_edge = 32;
    auto created = service->create_offline_edit_proxy(create);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value().manifest.pixel_provenance,
              kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8);
    EXPECT_FALSE(created.value().manifest.pinned);

    const auto stashed = root / "stashed-baked-original.jpg";
    std::error_code ec;
    std::filesystem::rename(original, stashed, ec);
    ASSERT_FALSE(ec) << ec.message();

    PreviewRequest after;
    after.asset_id = asset_id;
    after.max_edge = 64;
    after.purpose = PreviewPurpose::kDevelop;
    after.prefer_embedded_preview = false;
    DevelopParams live;
    live.exposure_ev = 1.75;
    auto live_preview = service->request_preview(after, live);
    ASSERT_TRUE(live_preview) << live_preview.error().message;
    EXPECT_EQ(live_preview.value().media_state, "proxy");
    EXPECT_EQ(live_preview.value().preview_apply_mode, kOfflineEditPreviewApplyIdentityBaked);
    EXPECT_EQ(live_preview.value().pixel_provenance,
              kOfflineEditProxyPixelProvenanceRecipeBakedSrgb8);

    PreviewRequest before = after;
    before.ignore_edits = true;
    auto before_preview = service->request_preview(before);
    ASSERT_TRUE(before_preview) << before_preview.error().message;
    EXPECT_EQ(before_preview.value().media_state, "proxy");
    EXPECT_EQ(before_preview.value().preview_apply_mode, kOfflineEditPreviewApplyIdentityBaked);
    EXPECT_EQ(before_preview.value().cache_key, live_preview.value().cache_key);
    const auto live_sha = sha256_file_hex(live_preview.value().cache_path);
    const auto before_sha = sha256_file_hex(before_preview.value().cache_path);
    ASSERT_TRUE(live_sha) << live_sha.error().message;
    ASSERT_TRUE(before_sha) << before_sha.error().message;
    EXPECT_EQ(live_sha.value(), before_sha.value());

    PreviewRequest interactive = after;
    interactive.persist_preview_record = false;
    auto scope_source = service->request_preview(interactive);
    ASSERT_TRUE(scope_source) << scope_source.error().message;
    EXPECT_EQ(scope_source.value().media_state, "proxy");
    EXPECT_EQ(scope_source.value().preview_apply_mode, kOfflineEditPreviewApplyIdentityBaked);

    params.value().exposure_ev = 1.2;
    auto saved_offline = service->save_develop(asset_id, params.value());
    ASSERT_TRUE(saved_offline) << saved_offline.error().message;

    std::filesystem::rename(stashed, original, ec);
    ASSERT_FALSE(ec) << ec.message();

    OfflineEditProxyReconnectRequest reconnect;
    reconnect.asset_id = asset_id;
    reconnect.user_initiated = true;
    reconnect.clear_proxy = true;
    auto reconnected = service->reconnect_offline_edit_proxy(reconnect);
    ASSERT_TRUE(reconnected) << reconnected.error().message;
    EXPECT_TRUE(reconnected.value().source_hash_matched);
    EXPECT_TRUE(reconnected.value().offline_states_cleared);
    EXPECT_TRUE(reconnected.value().proxy_cleared);
    EXPECT_EQ(reconnected.value().status.media_state, OfflineEditMediaState::kOriginal);
    EXPECT_FALSE(reconnected.value().status.proxy_present);

    PreviewRequest original_preview = after;
    original_preview.max_edge = 256;
    auto restored = service->request_preview(original_preview);
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(restored.value().media_state, "original");
    EXPECT_FALSE(restored.value().original_missing);
    EXPECT_EQ(restored.value().preview_apply_mode, kOfflineEditPreviewApplyCatalogRecipe);
    EXPECT_GT(restored.value().width, live_preview.value().width);
    EXPECT_NE(restored.value().cache_key, live_preview.value().cache_key);
    const auto restored_sha = sha256_file_hex(restored.value().cache_path);
    ASSERT_TRUE(restored_sha) << restored_sha.error().message;
    EXPECT_NE(restored_sha.value(), live_sha.value());
}

TEST_F(CatalogServiceTest, OfflineEditProxyPinDeleteAndPinnedSurvivesEvict)
{
    ASSERT_TRUE(open_service(true));
    const auto keep_path = root / "offline-keep.jpg";
    const auto drop_path = root / "offline-drop.jpg";
    ASSERT_TRUE(write_jpeg(keep_path, QColor(10, 20, 30)));
    ASSERT_TRUE(write_jpeg(drop_path, QColor(200, 10, 10)));
    auto keep_imported = service->import_one(keep_path.string(), CancellationToken{});
    auto drop_imported = service->import_one(drop_path.string(), CancellationToken{});
    ASSERT_TRUE(keep_imported) << keep_imported.error().message;
    ASSERT_TRUE(drop_imported) << drop_imported.error().message;
    const auto keep_id = keep_imported.value().asset->id;
    const auto drop_id = drop_imported.value().asset->id;

    OfflineEditProxyCreateRequest create;
    create.user_initiated = true;
    create.max_edge = 32;
    create.asset_id = keep_id;
    ASSERT_TRUE(service->create_offline_edit_proxy(create)) << "keep create";
    create.asset_id = drop_id;
    ASSERT_TRUE(service->create_offline_edit_proxy(create)) << "drop create";

    OfflineEditProxyPinRequest pin;
    pin.asset_id = keep_id;
    pin.user_initiated = true;
    pin.pinned = true;
    auto pinned = service->pin_offline_edit_proxy(pin);
    ASSERT_TRUE(pinned) << pinned.error().message;
    EXPECT_TRUE(pinned.value().manifest.pinned);

    OfflineEditProxyDeleteRequest blocked;
    blocked.asset_id = keep_id;
    blocked.user_initiated = true;
    auto refuse = service->delete_offline_edit_proxy(blocked);
    ASSERT_FALSE(refuse);
    EXPECT_EQ(refuse.error().context.at("reason"), "proxy_pinned");

    OfflineEditProxyEvictRequest evict;
    evict.user_initiated = true;
    evict.max_total_bytes = 1;
    auto evicted = service->evict_offline_edit_proxies(evict);
    ASSERT_TRUE(evicted) << evicted.error().message;
    EXPECT_GE(evicted.value().evicted, 1U);
    EXPECT_EQ(evicted.value().retained_pinned, 1U);
    ASSERT_FALSE(evicted.value().retained_pinned_asset_ids.empty());
    EXPECT_EQ(evicted.value().retained_pinned_asset_ids.front(), keep_id);

    auto keep_status = service->verify_offline_edit_proxy(keep_id);
    ASSERT_TRUE(keep_status) << keep_status.error().message;
    EXPECT_TRUE(keep_status.value().proxy_verified);
    EXPECT_TRUE(keep_status.value().manifest->pinned);

    auto drop_status = service->verify_offline_edit_proxy(drop_id);
    ASSERT_TRUE(drop_status) << drop_status.error().message;
    EXPECT_FALSE(drop_status.value().proxy_present);
}

} // namespace ravo
