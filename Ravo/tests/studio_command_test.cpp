#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QVariantMap>

#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QKeySequence>
#include <QMetaType>
#include <QProcess>
#include <QSize>
#include <QThread>
#include <QTranslator>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/engine/engine.h"
#include "ravo/control/live_control.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

#include "ravo/desktop/preview_request_owner.h"
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_language_manager.h"

#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(PreviewRequestOwnerTest, SupersededWorkIsCancelledAndLateResultsAreRejected)
{
    PreviewRequestOwner owner;
    const auto first_revision = owner.supersede("first_request");
    const auto first_token = owner.begin();
    EXPECT_FALSE(first_token.is_cancellation_requested());
    EXPECT_TRUE(owner.accepts(first_revision, "asset-a", "asset-a"));

    const auto second_revision = owner.supersede("selection_changed");
    EXPECT_TRUE(first_token.is_cancellation_requested());
    EXPECT_EQ(first_token.reason(), "selection_changed");
    EXPECT_FALSE(owner.accepts(first_revision, "asset-a", "asset-a"));

    const auto second_token = owner.begin();
    EXPECT_FALSE(second_token.is_cancellation_requested());
    EXPECT_FALSE(owner.accepts(second_revision, "asset-a", "asset-b"));
    EXPECT_TRUE(owner.accepts(second_revision, "asset-b", "asset-b"));
}

TEST(StudioSettingsTest, LanguageSettingNormalizesPersistsAndRepairsCorruption)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto previous_format = QSettings::defaultFormat();
    const QString previous_organization = QCoreApplication::organizationName();
    const QString previous_application = QCoreApplication::applicationName();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
    QCoreApplication::setOrganizationName(QStringLiteral("RavoSettingsTest"));
    QCoreApplication::setApplicationName(QStringLiteral("LanguageContract"));
    {
        QSettings settings;
        settings.setValue(QStringLiteral("desktop/language"), QStringLiteral("broken"));
        settings.sync();
        ASSERT_EQ(settings.status(), QSettings::NoError);
    }
    StudioLanguageManager manager;
    ASSERT_TRUE(manager.initialize()) << manager.lastError().toStdString();
    EXPECT_EQ(manager.language(), QStringLiteral("en_US"));
    EXPECT_EQ(
        manager.supportedLanguages(),
        QStringList({QStringLiteral("en_US"), QStringLiteral("de_DE"), QStringLiteral("es_ES"),
                     QStringLiteral("fr_FR"), QStringLiteral("pt_BR"), QStringLiteral("zh_CN"),
                     QStringLiteral("zh_TW"), QStringLiteral("ja_JP"), QStringLiteral("ko_KR")}));
    EXPECT_EQ(manager.languageOptions().size(), manager.supportedLanguages().size());
    {
        QSettings settings;
        EXPECT_FALSE(settings.contains(QStringLiteral("desktop/language")));
    }
    EXPECT_TRUE(manager.setLanguage(QStringLiteral("en-US")));
    {
        QSettings settings;
        EXPECT_EQ(settings.value(QStringLiteral("desktop/language")).toString(),
                  QStringLiteral("en_US"));
    }
    EXPECT_FALSE(manager.setLanguage(QStringLiteral("ar_SA")));
    EXPECT_EQ(manager.language(), QStringLiteral("en_US"));
    EXPECT_FALSE(manager.lastError().isEmpty());
    {
        QSettings settings;
        EXPECT_EQ(settings.value(QStringLiteral("desktop/language")).toString(),
                  QStringLiteral("en_US"));
        settings.clear();
    }
    QCoreApplication::setOrganizationName(previous_organization);
    QCoreApplication::setApplicationName(previous_application);
    QSettings::setDefaultFormat(previous_format);
}

TEST(StudioSettingsTest, LocaleAliasesNormalizeAndMissingCatalogFailsExplicitly)
{
    ensure_qt_core();
    StudioLanguageManager manager(QStringList{QStringLiteral(RAVO_STUDIO_TRANSLATION_DIR)});
    ASSERT_TRUE(manager.initialize(QStringLiteral("fr-CA"))) << manager.lastError().toStdString();
    EXPECT_EQ(manager.language(), QStringLiteral("fr_FR"));
    ASSERT_TRUE(manager.initialize(QStringLiteral("zh-Hant-HK")))
        << manager.lastError().toStdString();
    EXPECT_EQ(manager.language(), QStringLiteral("zh_TW"));

    QTemporaryDir empty_directory;
    ASSERT_TRUE(empty_directory.isValid());
    StudioLanguageManager missing(QStringList{empty_directory.path()});
    ASSERT_TRUE(missing.initialize(QStringLiteral("en_US"))) << missing.lastError().toStdString();
    EXPECT_FALSE(missing.setLanguage(QStringLiteral("de-DE")));
    EXPECT_EQ(missing.language(), QStringLiteral("en_US"));
    EXPECT_FALSE(missing.lastError().isEmpty());
}

TEST(AssetListModelTest, SparsePagesKeepTotalRowsAndBoundResidentRecords)
{
    AssetListModel model;
    const auto make_page = [](const int first)
    {
        std::vector<AssetRecord> assets;
        assets.reserve(kLibraryPageDefaultSize);
        for (int index = 0; index < static_cast<int>(kLibraryPageDefaultSize); ++index)
        {
            AssetRecord asset;
            asset.id = "ast_sparse_" + std::to_string(first + index);
            asset.normalized_uri =
                "file:///library/photo-" + std::to_string(first + index) + ".jpg";
            asset.media_type = std::string(kMediaTypeJpeg);
            assets.push_back(std::move(asset));
        }
        return assets;
    };
    model.setAssets(make_page(0), {}, {}, 10'000U);
    EXPECT_EQ(model.rowCount(), 10'000);
    EXPECT_EQ(model.loadedCount(), static_cast<int>(kLibraryPageDefaultSize));
    EXPECT_TRUE(model.rowLoaded(0));
    EXPECT_FALSE(model.rowLoaded(5000));
    EXPECT_EQ(model.data(model.index(5000, 0), AssetListModel::ThumbnailStateRole).toString(),
              QStringLiteral("unloaded"));

    model.setSelectedIds({"ast_sparse_0"});
    model.setPage(200U, make_page(200), {}, {}, 10'000U);
    model.setPage(400U, make_page(400), {}, {}, 10'000U);
    model.setPage(600U, make_page(600), {}, {}, 10'000U);
    EXPECT_LE(model.loadedCount(), static_cast<int>(kLibraryPageDefaultSize * 3U));
    EXPECT_TRUE(model.rowLoaded(0));
    EXPECT_FALSE(model.rowLoaded(200));
    EXPECT_TRUE(model.rowLoaded(600));
    EXPECT_EQ(model.assetIdAt(600), QStringLiteral("ast_sparse_600"));
    EXPECT_TRUE(model.isSelected("ast_sparse_0"));
}

TEST(AssetListModelTest, GridCaptureSummaryIsCompactAndPerAsset)
{
    AssetRecord asset;
    asset.id = "ast_capture_summary";
    asset.normalized_uri = "file:///library/photo.arw";
    asset.media_type = std::string(kMediaTypeRaw);
    asset.capture.camera_make = "SONY";
    asset.capture.camera_model = "ILCE-7CR";
    asset.capture.iso = 100.0;
    asset.capture.aperture = 5.6;
    asset.capture.focal_length_mm = 46.7;

    AssetListModel model;
    model.setAssets({asset});
    ASSERT_EQ(model.rowCount(), 1);
    EXPECT_EQ(model.roleNames().value(AssetListModel::CaptureSummaryRole),
              QByteArray("captureSummary"));
    EXPECT_EQ(model.data(model.index(0, 0), AssetListModel::CaptureSummaryRole).toString(),
              QStringLiteral("ISO 100 · f/5.6 · 47 mm"));
}

TEST(StudioLiveControlTest, DiscoveryIsMultiSessionAwareAndDescriptorsDisappearWithOwners)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", directory.path().toUtf8());

    const auto make_descriptor = [](const QString &id)
    {
        LiveSessionDescriptor descriptor;
        descriptor.session_id = id.toStdString();
        descriptor.server_name = QStringLiteral("ravo-live-test-%1").arg(id).toStdString();
        descriptor.process_id = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
        descriptor.executable_path = RAVO_CLI_EXECUTABLE;
        descriptor.workspace_root = RAVO_REPOSITORY_ROOT;
        return descriptor;
    };
    auto traversal = LocalControlServer::start(make_descriptor(QStringLiteral("../escape")),
                                               [](const LiveControlRequest &) -> Result<JsonValue>
                                               { return JsonValue{JsonValue::Object{}}; });
    ASSERT_FALSE(traversal);
    EXPECT_EQ(traversal.error().code, ErrorCode::kValidation);
    auto first =
        LocalControlServer::start(make_descriptor(QStringLiteral("first")),
                                  [](const LiveControlRequest &) -> Result<JsonValue>
                                  { return JsonValue{JsonValue::Object{{"state", "unused"}}}; });
    auto second =
        LocalControlServer::start(make_descriptor(QStringLiteral("second")),
                                  [](const LiveControlRequest &) -> Result<JsonValue>
                                  { return JsonValue{JsonValue::Object{{"state", "unused"}}}; });
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    auto found_first = LocalControlClient::find_descriptor("first");
    ASSERT_TRUE(found_first) << found_first.error().message;
    EXPECT_EQ(found_first.value(), first.value()->descriptor());
    auto missing = LocalControlClient::find_descriptor("missing");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);
    EXPECT_TRUE(QFileInfo::exists(
        QString::fromStdString(filesystem_path_to_utf8(first.value()->descriptor_path()))));
    EXPECT_TRUE(QFileInfo::exists(
        QString::fromStdString(filesystem_path_to_utf8(second.value()->descriptor_path()))));
#if !defined(Q_OS_WIN)
    const auto descriptor_permissions =
        QFileInfo(QString::fromStdString(filesystem_path_to_utf8(first.value()->descriptor_path())))
            .permissions();
    EXPECT_EQ(descriptor_permissions &
                  (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                   QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther),
              QFileDevice::Permissions{});
#endif
    JsonValue::Object oversized;
    oversized.emplace("payload", std::string(kLiveControlMaxMessageBytes, 'x'));
    auto rejected = LocalControlClient::request(first.value()->descriptor(), "state",
                                                JsonValue{std::move(oversized)}, 1000);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kValidation);

    QFile stale(directory.filePath(QStringLiteral("stale.json")));
    ASSERT_TRUE(stale.open(QIODevice::WriteOnly | QIODevice::NewOnly));
    ASSERT_GT(stale.write("{broken"), 0);
    stale.close();

    const auto listed = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("sessions"), QStringLiteral("--workspace-root"),
         QStringLiteral(RAVO_REPOSITORY_ROOT), QStringLiteral("--timeout-ms"),
         QStringLiteral("5000"), QStringLiteral("--json")});
    ASSERT_EQ(listed.exit_code, 0) << listed.standard_error.constData();
    auto data = cli_data(listed.standard_output);
    ASSERT_TRUE(data) << data.error().message;
    const auto *sessions = data.value().find("sessions");
    ASSERT_NE(sessions, nullptr);
    ASSERT_NE(sessions->array_if(), nullptr);
    EXPECT_EQ(sessions->array_if()->size(), 2U);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        const auto ambiguous = run_cli_process(
            {QStringLiteral("studio"), QStringLiteral("state"), QStringLiteral("--workspace-root"),
             QStringLiteral(RAVO_REPOSITORY_ROOT), QStringLiteral("--timeout-ms"),
             QStringLiteral("5000"), QStringLiteral("--json")});
        EXPECT_EQ(ambiguous.exit_code, cli_exit_code(ErrorCode::kConflict))
            << ambiguous.standard_output.constData() << ambiguous.standard_error.constData();
        EXPECT_NE(ambiguous.standard_output.indexOf("session_ids"), -1)
            << ambiguous.standard_output.constData() << ambiguous.standard_error.constData();
    }

    const auto first_path = first.value()->descriptor_path();
    const auto second_path = second.value()->descriptor_path();
    first.value().reset();
    second.value().reset();
    EXPECT_FALSE(QFileInfo::exists(QString::fromStdString(filesystem_path_to_utf8(first_path))));
    EXPECT_FALSE(QFileInfo::exists(QString::fromStdString(filesystem_path_to_utf8(second_path))));
    auto removed = LocalControlClient::find_descriptor("first");
    ASSERT_FALSE(removed);
    EXPECT_EQ(removed.error().code, ErrorCode::kNotFound);
}

TEST(StudioLiveControlTest, CliTimeoutCancelsAnUnresponsiveSessionRequest)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", directory.path().toUtf8());
    LiveSessionDescriptor descriptor;
    descriptor.session_id = "slow";
    descriptor.server_name = "ravo-live-test-slow";
    descriptor.process_id = static_cast<std::uint64_t>(QCoreApplication::applicationPid());
    descriptor.executable_path = RAVO_CLI_EXECUTABLE;
    descriptor.workspace_root = RAVO_REPOSITORY_ROOT;
    auto server =
        LocalControlServer::start(descriptor,
                                  [](const LiveControlRequest &) -> Result<JsonValue>
                                  {
                                      QThread::msleep(250);
                                      return JsonValue{JsonValue::Object{{"state", "too-late"}}};
                                  });
    ASSERT_TRUE(server) << server.error().message;

    const auto result = run_cli_process({QStringLiteral("studio"), QStringLiteral("state"),
                                         QStringLiteral("--session-id"), QStringLiteral("slow"),
                                         QStringLiteral("--timeout-ms"), QStringLiteral("100"),
                                         QStringLiteral("--json")});
    EXPECT_EQ(result.exit_code, cli_exit_code(ErrorCode::kCancelled))
        << result.standard_output.constData() << result.standard_error.constData();
    EXPECT_NE(result.standard_output.indexOf("timeout_ms"), -1)
        << result.standard_output.constData() << result.standard_error.constData();
}

TEST(StudioLiveControlTest, CliReadsSelectionRejectsStaleMutationAndPublishesLatestPreview)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString registry_path = directory.filePath(QStringLiteral("live-control"));
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR", registry_path.toUtf8());

    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 120, 170));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    auto live = StudioLiveSessionController::create(presenter, commands);
    ASSERT_TRUE(live) << live.error().message;
    const QString session_id = QString::fromStdString(live.value()->descriptor().session_id);
    const QString descriptor_path =
        QString::fromStdString(filesystem_path_to_utf8(live.value()->descriptorPath()));

    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.importWorkActive() &&
                   !presenter.previewLoading();
        }))
        << presenter.errorText().toStdString();
    presenter.openDevelop();
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();

    const auto state_result = run_cli_process({QStringLiteral("studio"), QStringLiteral("state"),
                                               QStringLiteral("--timeout-ms"),
                                               QStringLiteral("5000"), QStringLiteral("--json")});
    ASSERT_EQ(state_result.exit_code, 0) << state_result.standard_error.constData();
    auto state = cli_data(state_result.standard_output);
    ASSERT_TRUE(state) << state.error().message;
    const auto *selection = state.value().find("selection");
    ASSERT_NE(selection, nullptr);
    ASSERT_NE(selection->object_if(), nullptr);
    const auto *selected = selection->find("primary_asset_id");
    ASSERT_NE(selected, nullptr);
    ASSERT_NE(selected->string_if(), nullptr);
    EXPECT_EQ(QString::fromStdString(*selected->string_if()), presenter.selectedAssetId());
    EXPECT_EQ(serialize_json(state.value()).find("api_key"), std::string::npos);

    const QString baseline_output = directory.filePath(QStringLiteral("live-baseline.png"));
    const auto previewed = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("preview"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--output"), baseline_output, QStringLiteral("--max-edge"),
         QStringLiteral("64"), QStringLiteral("--timeout-ms"), QStringLiteral("10000"),
         QStringLiteral("--json")});
    ASSERT_EQ(previewed.exit_code, 0)
        << previewed.standard_output.constData() << previewed.standard_error.constData();
    EXPECT_TRUE(QFileInfo::exists(baseline_output));
    EXPECT_FALSE(presenter.selectedHasEdits());

    presenter.previewDevelopNumber(QStringLiteral("exposure"), 0.25);
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    const auto pending_state_result =
        run_cli_process({QStringLiteral("studio"), QStringLiteral("state"),
                         QStringLiteral("--session-id"), session_id, QStringLiteral("--timeout-ms"),
                         QStringLiteral("5000"), QStringLiteral("--json")});
    ASSERT_EQ(pending_state_result.exit_code, 0);
    auto pending_state = cli_data(pending_state_result.standard_output);
    ASSERT_TRUE(pending_state) << pending_state.error().message;
    const auto *pending_recipe = pending_state.value().find("recipe");
    ASSERT_NE(pending_recipe, nullptr);
    ASSERT_NE(pending_recipe->find("state"), nullptr);
    ASSERT_NE(pending_recipe->find("state")->string_if(), nullptr);
    EXPECT_EQ(*pending_recipe->find("state")->string_if(), "pending");
    EXPECT_NE(
        serialize_json(*pending_recipe->find("modified_operations")).find("ravo.core.exposure"),
        std::string::npos);
    const QString pending_output = directory.filePath(QStringLiteral("live-pending.png"));
    const auto pending_preview = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("preview"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--output"), pending_output, QStringLiteral("--max-edge"),
         QStringLiteral("64"), QStringLiteral("--timeout-ms"), QStringLiteral("10000"),
         QStringLiteral("--json")});
    ASSERT_EQ(pending_preview.exit_code, 0) << pending_preview.standard_output.constData()
                                            << pending_preview.standard_error.constData();
    EXPECT_TRUE(QFileInfo::exists(pending_output));
    EXPECT_FALSE(presenter.selectedHasEdits());

    const double exposure_before_stale = presenter.editExposure();
    const auto stale = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("develop"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--asset-id"), presenter.selectedAssetId(),
         QStringLiteral("--expect-session-revision"), QStringLiteral("0"), QStringLiteral("--set"),
         QStringLiteral("exposure=0.5"), QStringLiteral("--timeout-ms"), QStringLiteral("5000"),
         QStringLiteral("--json")});
    EXPECT_EQ(stale.exit_code, cli_exit_code(ErrorCode::kConflict));
    EXPECT_NE(stale.standard_output.indexOf("stale_session"), -1);
    EXPECT_NEAR(presenter.editExposure(), exposure_before_stale, 1e-9);

    const double exposure_before_wrong_asset = presenter.editExposure();
    const auto wrong_asset = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("develop"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--asset-id"), QStringLiteral("ast_wrong"),
         QStringLiteral("--set"), QStringLiteral("exposure=0.5"), QStringLiteral("--timeout-ms"),
         QStringLiteral("5000"), QStringLiteral("--json")});
    EXPECT_EQ(wrong_asset.exit_code, cli_exit_code(ErrorCode::kConflict));
    EXPECT_NE(wrong_asset.standard_output.indexOf("wrong_asset"), -1);
    EXPECT_NEAR(presenter.editExposure(), exposure_before_wrong_asset, 1e-9);

    const double exposure_before_invalid = presenter.editExposure();
    const auto invalid = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("develop"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--set"), QStringLiteral("notADevelopField=1"),
         QStringLiteral("--timeout-ms"), QStringLiteral("5000"), QStringLiteral("--json")});
    EXPECT_EQ(invalid.exit_code, cli_exit_code(ErrorCode::kInvalidArgument));
    EXPECT_NE(invalid.standard_output.indexOf("invalid_develop_field"), -1);
    EXPECT_NEAR(presenter.editExposure(), exposure_before_invalid, 1e-9);

    const QString output = directory.filePath(QStringLiteral("live-result.png"));
    const auto developed = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("develop"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--set"), QStringLiteral("exposure=0.75"),
         QStringLiteral("--set"), QStringLiteral("saturation=0.1"), QStringLiteral("--output"),
         output, QStringLiteral("--max-edge"), QStringLiteral("64"), QStringLiteral("--timeout-ms"),
         QStringLiteral("30000"), QStringLiteral("--json")},
        35000);
    ASSERT_EQ(developed.exit_code, 0)
        << developed.standard_output.constData() << developed.standard_error.constData();
    ASSERT_TRUE(QFileInfo::exists(output));
    EXPECT_NEAR(presenter.editExposure(), 0.75, 1e-9);
    EXPECT_NEAR(presenter.editSaturation(), 0.1, 1e-9);
    EXPECT_TRUE(presenter.selectedHasEdits());
    QImage result(output);
    ASSERT_FALSE(result.isNull());
    EXPECT_LE(std::max(result.width(), result.height()), 64);

    auto developed_data = cli_data(developed.standard_output);
    ASSERT_TRUE(developed_data) << developed_data.error().message;
    const auto *artifact = developed_data.value().find("artifact");
    ASSERT_NE(artifact, nullptr);
    ASSERT_NE(artifact->object_if(), nullptr);
    const auto *hash = artifact->find("content_hash");
    ASSERT_NE(hash, nullptr);
    ASSERT_NE(hash->string_if(), nullptr);
    QFile output_file(output);
    ASSERT_TRUE(output_file.open(QIODevice::ReadOnly));
    EXPECT_EQ(*hash->string_if(),
              QCryptographicHash::hash(output_file.readAll(), QCryptographicHash::Sha256)
                  .toHex()
                  .toStdString());

    const auto current_result =
        run_cli_process({QStringLiteral("studio"), QStringLiteral("state"),
                         QStringLiteral("--session-id"), session_id, QStringLiteral("--timeout-ms"),
                         QStringLiteral("5000"), QStringLiteral("--json")});
    ASSERT_EQ(current_result.exit_code, 0);
    auto current = cli_data(current_result.standard_output);
    ASSERT_TRUE(current) << current.error().message;
    const auto *recipe = current.value().find("recipe");
    ASSERT_NE(recipe, nullptr);
    const auto *modified = recipe->find("modified_operations");
    ASSERT_NE(modified, nullptr);
    const auto modified_text = serialize_json(*modified);
    EXPECT_NE(modified_text.find("ravo.core.exposure"), std::string::npos);
    EXPECT_NE(modified_text.find("ravo.color.saturation"), std::string::npos);
    EXPECT_EQ(recipe->find("state")->string_if() != nullptr ? *recipe->find("state")->string_if() :
                                                              std::string{},
              "saved");

    const auto conflict = run_cli_process(
        {QStringLiteral("studio"), QStringLiteral("develop"), QStringLiteral("--session-id"),
         session_id, QStringLiteral("--set"), QStringLiteral("exposure=1.5"),
         QStringLiteral("--output"), output, QStringLiteral("--timeout-ms"), QStringLiteral("5000"),
         QStringLiteral("--json")});
    EXPECT_EQ(conflict.exit_code, cli_exit_code(ErrorCode::kConflict))
        << conflict.standard_output.constData() << conflict.standard_error.constData();
    EXPECT_NEAR(presenter.editExposure(), 0.75, 1e-9);

    live.value().reset();
    EXPECT_FALSE(QFileInfo::exists(descriptor_path));
}

TEST(StudioPresenterTest, MigratedColorPropertiesExposeCanonicalIdentity)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerRR(), 1.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerRG(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerRB(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerGR(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerGG(), 1.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerGB(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerBR(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerBG(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editChannelMixerBB(), 1.0);
    EXPECT_DOUBLE_EQ(presenter.editHotPixelsStrength(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editHotPixelsThreshold(), 0.05);
    EXPECT_FALSE(presenter.editHotPixelsPermissive());
    EXPECT_EQ(presenter.editRawCaIterations(), 0);
    EXPECT_FALSE(presenter.editRawCaAvoidShift());
    EXPECT_DOUBLE_EQ(presenter.editSharpen(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editSharpenRadius(), 2.0);
    EXPECT_DOUBLE_EQ(presenter.editSharpenThreshold(), 0.5);
    const auto retouch = presenter.editRetouch();
    EXPECT_EQ(retouch.value(QStringLiteral("regionCount")).toInt(), 0);
    EXPECT_TRUE(retouch.value(QStringLiteral("regions")).toList().isEmpty());
    EXPECT_EQ(retouch.value(QStringLiteral("numScales")).toInt(), 0);
    EXPECT_DOUBLE_EQ(presenter.editDehaze(), 0.0);
    EXPECT_DOUBLE_EQ(presenter.editDehazeDistance(), 0.2);
    EXPECT_TRUE(presenter.editDehazeAdaptive());
    EXPECT_TRUE(presenter.filterText().isEmpty());
    EXPECT_EQ(presenter.mediaFilter(), QStringLiteral("any"));
    EXPECT_EQ(presenter.editFilter(), QStringLiteral("any"));
    const auto legacy_balance = presenter.editLegacyColorBalance();
    EXPECT_EQ(legacy_balance.size(), 18);
    EXPECT_FALSE(legacy_balance.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(legacy_balance.value(QStringLiteral("modeIndex")).toInt(), 1);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("liftFactor")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("gammaBlue")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("gainGreen")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("inputSaturation")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("contrast")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("greyFulcrum")).toDouble(), 18.0);
    EXPECT_DOUBLE_EQ(legacy_balance.value(QStringLiteral("outputSaturation")).toDouble(), 1.0);
    const auto color_checker = presenter.editColorChecker();
    EXPECT_EQ(color_checker.size(), 10);
    EXPECT_FALSE(color_checker.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(color_checker.value(QStringLiteral("presetIndex")).toInt(), -1);
    EXPECT_EQ(color_checker.value(QStringLiteral("patchIndex")).toInt(), 0);
    EXPECT_EQ(color_checker.value(QStringLiteral("patchCount")).toInt(), 24);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("sourceL")).toDouble(), 37.990001678466797);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("sourceA")).toDouble(), 13.5600004196167);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("sourceB")).toDouble(), 14.0600004196167);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("targetL")).toDouble(), 37.990001678466797);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("targetA")).toDouble(), 13.5600004196167);
    EXPECT_DOUBLE_EQ(color_checker.value(QStringLiteral("targetB")).toDouble(), 14.0600004196167);
    const auto balance = presenter.editColorBalanceRgb();
    EXPECT_EQ(balance.size(), 33);
    const auto balance_mask = presenter.editColorBalanceRgbMask();
    EXPECT_FALSE(balance_mask.value(QStringLiteral("attached")).toBool());
    EXPECT_EQ(balance_mask.value(QStringLiteral("kindField")).toString(),
              QStringLiteral("colorBalanceRgbMaskKind"));
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("globalY")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("shadowsFalloff")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("highlightsFalloff")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("maskGreyFulcrum")).toDouble(), 0.1845);
    EXPECT_DOUBLE_EQ(balance.value(QStringLiteral("greyFulcrum")).toDouble(), 0.1845);
    EXPECT_EQ(balance.value(QStringLiteral("formulaIndex")).toInt(), 0);
    const auto color_correction = presenter.editColorCorrection();
    EXPECT_EQ(color_correction.size(), 6);
    EXPECT_FALSE(color_correction.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("highlightA")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("highlightB")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("shadowA")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("shadowB")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_correction.value(QStringLiteral("saturation")).toDouble(), 1.0);
    const auto color_contrast = presenter.editColorContrast();
    EXPECT_EQ(color_contrast.size(), 6);
    EXPECT_FALSE(color_contrast.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("aSteepness")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("aOffset")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("bSteepness")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(color_contrast.value(QStringLiteral("bOffset")).toDouble(), 0.0);
    EXPECT_TRUE(color_contrast.value(QStringLiteral("unbound")).toBool());
    const auto color_reconstruction = presenter.editColorReconstruction();
    EXPECT_EQ(color_reconstruction.size(), 7);
    EXPECT_FALSE(color_reconstruction.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("threshold")).toDouble(), 100.0);
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("spatial")).toDouble(), 400.0);
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("range")).toDouble(), 10.0);
    EXPECT_DOUBLE_EQ(color_reconstruction.value(QStringLiteral("hueDegrees")).toDouble(), 237.6);
    EXPECT_EQ(color_reconstruction.value(QStringLiteral("precedenceIndex")).toInt(), 0);
    EXPECT_EQ(color_reconstruction.value(QStringLiteral("precedenceChoices")).toStringList().size(),
              3);
    const auto color_harmonizer = presenter.editColorHarmonizer();
    EXPECT_EQ(color_harmonizer.size(), 24);
    EXPECT_FALSE(color_harmonizer.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("ruleIndex")).toInt(), 3);
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("ruleChoices")).toStringList().size(), 10);
    EXPECT_FALSE(color_harmonizer.value(QStringLiteral("customRule")).toBool());
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("activeNodeCount")).toInt(), 2);
    EXPECT_TRUE(color_harmonizer.value(QStringLiteral("anchorVisible")).toBool());
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("anchorHueDegrees")).toDouble(), 36.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("pullStrength")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("neutralProtection")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("pullWidth")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("smoothing")).toDouble(), 0.0);
    EXPECT_EQ(color_harmonizer.value(QStringLiteral("customNodeCount")).toInt(), 4);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("customHue0Degrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(color_harmonizer.value(QStringLiteral("nodeSaturation0")).toDouble(), 1.0);
    const auto shared_controls = color_harmonizer.value(QStringLiteral("sharedControls")).toList();
    ASSERT_EQ(shared_controls.size(), 5);
    const auto anchor_control = shared_controls.front().toMap();
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("minimum")).toDouble(),
                     kColorHarmonizerHueDegreesMin);
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("maximum")).toDouble(),
                     kColorHarmonizerHueDegreesMax);
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("step")).toDouble(), 0.1);
    EXPECT_DOUBLE_EQ(anchor_control.value(QStringLiteral("reset")).toDouble(), 36.0);
    EXPECT_TRUE(anchor_control.value(QStringLiteral("visible")).toBool());
    const auto smoothing_control =
        std::find_if(shared_controls.cbegin(), shared_controls.cend(),
                     [](const QVariant &candidate)
                     {
                         return candidate.toMap().value(QStringLiteral("field")).toString() ==
                                QStringLiteral("colorHarmonizerSmoothing");
                     });
    ASSERT_NE(smoothing_control, shared_controls.cend());
    EXPECT_DOUBLE_EQ(smoothing_control->toMap().value(QStringLiteral("minimum")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(smoothing_control->toMap().value(QStringLiteral("maximum")).toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(smoothing_control->toMap().value(QStringLiteral("step")).toDouble(), 0.01);
    EXPECT_FALSE(color_harmonizer.value(QStringLiteral("customNodeControl"))
                     .toMap()
                     .value(QStringLiteral("visible"))
                     .toBool());
    const auto custom_hues = color_harmonizer.value(QStringLiteral("customHueControls")).toList();
    const auto node_sats =
        color_harmonizer.value(QStringLiteral("nodeSaturationControls")).toList();
    ASSERT_EQ(custom_hues.size(), 4);
    ASSERT_EQ(node_sats.size(), 4);
    EXPECT_FALSE(custom_hues[0].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_TRUE(node_sats[0].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_TRUE(node_sats[1].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_FALSE(node_sats[2].toMap().value(QStringLiteral("visible")).toBool());
    EXPECT_FALSE(node_sats[3].toMap().value(QStringLiteral("visible")).toBool());
    const auto harmonizer_mask = presenter.editColorHarmonizerMask();
    EXPECT_FALSE(harmonizer_mask.value(QStringLiteral("attached")).toBool());
    EXPECT_TRUE(harmonizer_mask.value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindIndex")).toInt(), 0);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindName")).toString(), QStringLiteral("none"));
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("statusCode")).toString(),
              QStringLiteral("no_mask"));
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("kindChoices")).toStringList().size(), 9);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("sourceChoices")).toStringList().size(), 2);
    EXPECT_EQ(harmonizer_mask.value(QStringLiteral("channelChoices")).toStringList().size(), 4);
    const auto mask_controls = harmonizer_mask.value(QStringLiteral("numericControls")).toList();
    ASSERT_EQ(mask_controls.size(), 22);
    const auto radius = std::find_if(
        mask_controls.cbegin(), mask_controls.cend(), [](const QVariant &candidate)
        { return candidate.toMap().value(QStringLiteral("key")) == QStringLiteral("radius"); });
    ASSERT_NE(radius, mask_controls.cend());
    EXPECT_DOUBLE_EQ(radius->toMap().value(QStringLiteral("min")).toDouble(), 0.01);
    EXPECT_DOUBLE_EQ(radius->toMap().value(QStringLiteral("max")).toDouble(),
                     kCanonicalMaskUnitMax);
    const auto graduated_mask = presenter.editGraduatedMask();
    EXPECT_EQ(graduated_mask.value(QStringLiteral("target")).toString(),
              QStringLiteral("graduatednd"));
    EXPECT_EQ(graduated_mask.value(QStringLiteral("detachField")).toString(),
              QStringLiteral("graduatedMask"));
    const auto primaries = presenter.editPrimaries();
    EXPECT_EQ(primaries.size(), 8);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("achromaticTintHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("achromaticTintPurity")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("redHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("redPurity")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("greenHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("greenPurity")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("blueHueDegrees")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(primaries.value(QStringLiteral("bluePurity")).toDouble(), 1.0);
    const auto white_balance = presenter.editWhiteBalance();
    EXPECT_EQ(white_balance.size(), 7);
    EXPECT_EQ(white_balance.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_FALSE(white_balance.value(QStringLiteral("hasCoefficients")).toBool());
    EXPECT_FALSE(white_balance.value(QStringLiteral("canPick")).toBool());
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("red")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("green")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("blue")).toDouble(), 1.0);
    EXPECT_DOUBLE_EQ(white_balance.value(QStringLiteral("fourth")).toDouble(), 1.0);
    EXPECT_EQ(presenter.editDemosaicModeIndex(), 0);
    EXPECT_EQ(presenter.editColorEqBands().size(), 8);
    const auto input_color = presenter.editInputColor();
    EXPECT_EQ(input_color.size(), 7);
    EXPECT_EQ(input_color.value(QStringLiteral("inputProfileIndex")).toInt(), 0);
    EXPECT_EQ(input_color.value(QStringLiteral("workingProfileIndex")).toInt(), 0);
    EXPECT_EQ(input_color.value(QStringLiteral("intentIndex")).toInt(), 0);
    EXPECT_EQ(input_color.value(QStringLiteral("normalizeIndex")).toInt(), 0);
    EXPECT_FALSE(input_color.value(QStringLiteral("blueMapping")).toBool());
    EXPECT_EQ(input_color.value(QStringLiteral("inputProfile")).toString(),
              QStringLiteral("source"));
    EXPECT_EQ(input_color.value(QStringLiteral("workingProfile")).toString(),
              QStringLiteral("linear_rec709"));
    const auto profile_gamma = presenter.editProfileGamma();
    EXPECT_EQ(profile_gamma.size(), 8);
    EXPECT_FALSE(profile_gamma.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(profile_gamma.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("linear")).toDouble(), 0.1);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("gamma")).toDouble(), 0.45);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("dynamicRange")).toDouble(), 10.0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("greyPoint")).toDouble(), 18.0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("shadowsRange")).toDouble(), -5.0);
    EXPECT_DOUBLE_EQ(profile_gamma.value(QStringLiteral("securityFactor")).toDouble(), 0.0);
    const auto output_color = presenter.editOutputColor();
    EXPECT_EQ(output_color.size(), 9);
    EXPECT_EQ(output_color.value(QStringLiteral("outputProfileIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("intentIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("proofModeIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("proofProfileIndex")).toInt(), 0);
    EXPECT_EQ(output_color.value(QStringLiteral("proofIntentIndex")).toInt(), 1);
    EXPECT_TRUE(output_color.value(QStringLiteral("blackPointCompensation")).toBool());
    EXPECT_EQ(output_color.value(QStringLiteral("outputProfile")).toString(),
              QStringLiteral("srgb"));
    EXPECT_EQ(output_color.value(QStringLiteral("proofMode")).toString(), QStringLiteral("off"));
    EXPECT_EQ(output_color.value(QStringLiteral("proofProfile")).toString(),
              QStringLiteral("srgb"));
    const auto exposure = presenter.editExposureParams();
    EXPECT_EQ(exposure.size(), 7);
    EXPECT_EQ(exposure.value(QStringLiteral("modeIndex")).toInt(), 0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("black")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("exposureEv")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("deflickerPercentile")).toDouble(), 50.0);
    EXPECT_DOUBLE_EQ(exposure.value(QStringLiteral("deflickerTargetEv")).toDouble(), -4.0);
    EXPECT_FALSE(exposure.value(QStringLiteral("compensateExposureBias")).toBool());
    EXPECT_FALSE(exposure.value(QStringLiteral("compensateHighlightPreservation")).toBool());
}

TEST(StudioPresenterTest, ZoomModesAndFactorBoundsHaveOneDeterministicOwner)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fit"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 1.0);
    presenter.setZoomMode(QStringLiteral("fill"));
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fill"));
    presenter.setZoomMode(QStringLiteral("100"));
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("actual"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 1.0);
    presenter.setZoomFactor(0.0);
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("custom"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 0.1);
    presenter.adjustZoom(120);
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 0.11);
    presenter.setZoomFactor(100.0);
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 8.0);
    presenter.setZoomMode(QStringLiteral("future"));
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fit"));
    presenter.setZoomMode(QStringLiteral("fill"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("actual"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fill"));
    presenter.setZoomFactor(2.0);
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("actual"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("custom"));
    EXPECT_DOUBLE_EQ(presenter.zoomFactor(), 2.0);
    presenter.setZoomMode(QStringLiteral("fit"));
    presenter.setZoomMode(QStringLiteral("actual"));
    presenter.toggleActualSize();
    EXPECT_EQ(presenter.zoomMode(), QStringLiteral("fit"));
}

TEST(StudioPresenterTest, CopiedParameterClipboardStartsEmptyAndIgnoresEmptySelection)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_FALSE(presenter.hasCopiedParameters());
    presenter.copyParametersSelected(QVariantList{QStringLiteral("exposure")});
    EXPECT_FALSE(presenter.hasCopiedParameters());
    presenter.pasteParameters();
    EXPECT_FALSE(presenter.hasCopiedParameters());
}

TEST(StudioDebugInfo, FormattersEmitStableSingleLineFields)
{
    PhotoDebugIdentity photo;
    photo.catalog = QStringLiteral("/tmp/Ravo Library.sqlite");
    photo.asset_id = QStringLiteral("ast_642f1d7e545bf6872ee9e3cd8357c877");
    photo.uri = QStringLiteral("file:///tmp/folder/_DSC5950.jpg");
    photo.path = QStringLiteral("/tmp/folder/_DSC5950.jpg");
    photo.fingerprint = QStringLiteral("123-456");
    photo.media_type = QStringLiteral("image/jpeg");
    photo.display_name = QStringLiteral("_DSC5950.jpg");
    photo.width = QStringLiteral("6000");
    photo.height = QStringLiteral("4000");
    photo.size_bytes = QStringLiteral("2048");
    photo.has_edits = true;
    photo.import_state = QStringLiteral("imported");
    const auto photo_text = format_photo_debug_info(photo);
    EXPECT_TRUE(photo_text.startsWith(QStringLiteral("ravo.debug.photo 1\n")));
    EXPECT_TRUE(
        photo_text.contains(QStringLiteral("asset_id=ast_642f1d7e545bf6872ee9e3cd8357c877\n")));
    EXPECT_TRUE(photo_text.contains(QStringLiteral("has_edits=true")));
    EXPECT_FALSE(photo_text.contains(QStringLiteral("ravo.debug.preset")));

    PhotoParametersDebugInfo parameters;
    parameters.catalog = photo.catalog;
    parameters.asset_id = photo.asset_id;
    parameters.display_name = photo.display_name;
    parameters.recipe_state = QStringLiteral("pending");
    parameters.recipe_json = QStringLiteral(
        "{\"operations\":[{\"id\":\"ravo.core.exposure\",\"parameters\":{\"black\":0.0553}}]}");
    const auto parameters_text = format_photo_parameters_debug_info(parameters);
    EXPECT_EQ(
        parameters_text,
        QStringLiteral(
            "ravo.debug.parameters 1\n"
            "catalog=/tmp/Ravo Library.sqlite\n"
            "asset_id=ast_642f1d7e545bf6872ee9e3cd8357c877\n"
            "display_name=_DSC5950.jpg\n"
            "recipe_state=pending\n"
            "recipe_json={\"operations\":[{\"id\":\"ravo.core.exposure\",\"parameters\":{\"black\":0.0553}}]}"));

    PhotoDebugIdentity messy;
    messy.display_name = QStringLiteral("line\nbreak");
    const auto sanitized = format_photo_debug_info(messy);
    EXPECT_TRUE(sanitized.contains(QStringLiteral("display_name=line break\n")));
    EXPECT_FALSE(sanitized.contains(QStringLiteral("display_name=line\nbreak")));

    PresetDebugIdentity preset;
    preset.name = QStringLiteral("黑石礁大坝");
    preset.path = QStringLiteral("/tmp/Ravo Presets/黑石礁大坝.xmp");
    preset.kind = QStringLiteral("crs");
    preset.sha256 = QStringLiteral("abc");
    preset.size_bytes = QStringLiteral("12");
    preset.mtime_unix_ms = QStringLiteral("1");
    const auto preset_text = format_preset_debug_info(preset);
    EXPECT_TRUE(preset_text.startsWith(QStringLiteral("ravo.debug.preset 1\n")));
    EXPECT_TRUE(preset_text.contains(QStringLiteral("name=黑石礁大坝\n")));
    EXPECT_TRUE(preset_text.contains(QStringLiteral("kind=crs\n")));
    EXPECT_TRUE(preset_text.contains(QStringLiteral("sha256=abc\n")));
}

TEST(StudioPresenterTest, PhotoDebugInfoIsEmptyWithoutSelection)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_TRUE(presenter.selectedPhotoDebugInfo().isEmpty());
    EXPECT_TRUE(presenter.selectedPhotoParametersDebugInfo().isEmpty());
    presenter.copySelectedPhotoDebugInfo();
}

TEST(StudioPresenterTest, PhotoDebugInfoIdentifiesImportedAsset)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(120, 130, 140));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();

    const auto text = presenter.selectedPhotoDebugInfo();
    EXPECT_TRUE(text.startsWith(QStringLiteral("ravo.debug.photo 1\n")));
    EXPECT_TRUE(
        text.contains(QStringLiteral("catalog=") + presenter.catalogPath() + QLatin1Char('\n')));
    EXPECT_TRUE(text.contains(QStringLiteral("asset_id=") + presenter.selectedAssetId() +
                              QLatin1Char('\n')));
    EXPECT_TRUE(text.contains(QStringLiteral("display_name=photo.png\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("width=32\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("height=24\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("has_edits=false")));
    EXPECT_TRUE(text.contains(QStringLiteral("path=")));
    EXPECT_FALSE(text.contains(QStringLiteral("ravo.debug.preset")));

    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && !presenter.previewUrl().isEmpty(); }))
        << presenter.errorText().toStdString();
    const auto baseline_parameters = presenter.selectedPhotoParametersDebugInfo();
    EXPECT_TRUE(baseline_parameters.startsWith(QStringLiteral("ravo.debug.parameters 1\n")));
    EXPECT_TRUE(baseline_parameters.contains(QStringLiteral("recipe_state=saved\n")));
    EXPECT_TRUE(baseline_parameters.contains(QStringLiteral("recipe_json={")));
    EXPECT_TRUE(baseline_parameters.contains(QStringLiteral("\"operations\":")));

    presenter.previewDevelopNumber(QStringLiteral("exposureBlack"), 0.0553);
    const auto pending_parameters = presenter.selectedPhotoParametersDebugInfo();
    EXPECT_TRUE(pending_parameters.contains(QStringLiteral("recipe_state=pending\n")));
    EXPECT_TRUE(pending_parameters.contains(QStringLiteral("\"id\":\"ravo.core.exposure\"")));
    EXPECT_TRUE(pending_parameters.contains(QStringLiteral("\"black\":0.0553")));
}

TEST(StudioPresenterTest, ColdCatalogBuildsOnlyDemandedThumbnails)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    auto repository = SqliteCatalogRepository::create(catalog.toStdString());
    ASSERT_TRUE(repository) << repository.error().message;

    constexpr int kAssetCount = 12;
    for (int index = 0; index < kAssetCount; ++index)
    {
        const QString photo =
            directory.filePath(QStringLiteral("photo-%1.png").arg(index, 2, 10, QLatin1Char('0')));
        QImage image(32, 24, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40 + index, 80 + index, 120 + index));
        ASSERT_TRUE(image.save(photo, "PNG"));
        auto location = normalize_local_input(photo.toStdString());
        ASSERT_TRUE(location) << location.error().message;
        auto identity = read_file_identity(location.value().path);
        ASSERT_TRUE(identity) << identity.error().message;

        AssetRecord asset;
        asset.id = "ast_cold_" + std::to_string(index);
        asset.normalized_uri = location.value().uri;
        asset.media_type = std::string(kMediaTypePng);
        asset.size_bytes = identity.value().size_bytes;
        asset.mtime_unix_ms = identity.value().mtime_unix_ms;
        asset.content_fingerprint = make_content_fingerprint(identity.value());
        asset.width = 32U;
        asset.height = 24U;
        asset.created_unix_ms = 1000 + index;
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    }
    ASSERT_TRUE(repository.value()->close());
    repository.value().reset();

    StudioPresenter presenter;
    int maximum_preview_total = 0;
    QObject::connect(
        &presenter, &StudioPresenter::libraryWorkChanged, &presenter, [&]
        { maximum_preview_total = std::max(maximum_preview_total, presenter.previewWorkTotal()); });
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogOpen() && !presenter.busy() &&
                   presenter.visibleCount() == kAssetCount &&
                   !presenter.selectedAssetId().isEmpty();
        }))
        << presenter.errorText().toStdString();
    QCoreApplication::processEvents();
    QThread::msleep(100);
    QCoreApplication::processEvents();
    EXPECT_EQ(maximum_preview_total, 0);
    EXPECT_FALSE(presenter.previewWorkActive());
    EXPECT_TRUE(presenter.selectedThumbnailUrl().isEmpty());
    EXPECT_EQ(presenter.previewViewportWidth(), 32);
    EXPECT_EQ(presenter.previewViewportHeight(), 24);

    presenter.ensureThumbnail(presenter.selectedAssetId());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.assets()->thumbnailState(presenter.selectedAssetId().toStdString()) ==
                       QLatin1String("ready") &&
                   !presenter.previewWorkActive();
        }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(maximum_preview_total, 1);
    EXPECT_EQ(presenter.previewWorkTotal(), 1);
    EXPECT_FALSE(presenter.selectedThumbnailUrl().isEmpty());

    bool saw_loading_thumbnail = false;
    QObject::connect(&presenter, &StudioPresenter::previewChanged, &presenter,
                     [&]
                     {
                         saw_loading_thumbnail =
                             saw_loading_thumbnail ||
                             (presenter.previewLoading() && presenter.previewUrl().isEmpty() &&
                              !presenter.selectedThumbnailUrl().isEmpty());
                     });
    for (int row = 0; row < presenter.assets()->rowCount(); ++row)
        presenter.ensureThumbnail(presenter.assets()->assetIdAt(row));
    presenter.openDevelop();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && !presenter.previewUrl().isEmpty(); }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(saw_loading_thumbnail);
    presenter.returnToGrid();
    ASSERT_TRUE(wait_until(
        [&]
        {
            if (presenter.previewWorkActive())
                return false;
            for (int row = 0; row < presenter.assets()->rowCount(); ++row)
            {
                const auto id = presenter.assets()->assetIdAt(row).toStdString();
                if (presenter.assets()->thumbnailState(id) != QLatin1String("ready"))
                    return false;
            }
            return true;
        }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(maximum_preview_total, kAssetCount - 1);
}

TEST(StudioPresenterTest, ConsecutiveCommitsForOneControlShareHistoryAndUndo)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("coalesced-history.png"));
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 100, 120));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    presenter.openDevelop();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && !presenter.previewUrl().isEmpty() &&
                   presenter.selectedPhotoParametersDebugInfo().contains(
                       QStringLiteral("recipe_state=saved\n"));
        }))
        << presenter.errorText().toStdString();

    const auto black_value = [&]
    { return presenter.editExposureParams().value(QStringLiteral("black")).toDouble(); };
    presenter.setDevelopNumber(QStringLiteral("exposureBlack"), 0.01);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(black_value() - 0.01) < 1e-9 &&
                   presenter.recipeHistory().size() == 1;
        }))
        << presenter.errorText().toStdString();
    const auto history_id = presenter.recipeHistory().front().toMap().value(QStringLiteral("id"));
    const auto first_summary =
        presenter.recipeHistory().front().toMap().value(QStringLiteral("summary")).toString();

    presenter.setDevelopNumber(QStringLiteral("exposureBlack"), 0.02);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(black_value() - 0.02) < 1e-9 &&
                   presenter.recipeHistory().size() == 1 &&
                   presenter.recipeHistory().front().toMap().value(QStringLiteral("id")) ==
                       history_id;
        }))
        << presenter.errorText().toStdString();
    presenter.setDevelopNumber(QStringLiteral("exposureBlack"), 0.03);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(black_value() - 0.03) < 1e-9 &&
                   presenter.recipeHistory().size() == 1 &&
                   presenter.recipeHistory().front().toMap().value(QStringLiteral("id")) ==
                       history_id &&
                   presenter.recipeHistory()
                           .front()
                           .toMap()
                           .value(QStringLiteral("summary"))
                           .toString() != first_summary;
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.canUndo());

    presenter.undoEdit();
    ASSERT_TRUE(
        wait_until([&] { return !presenter.previewLoading() && std::abs(black_value()) < 1e-9; }))
        << presenter.errorText().toStdString() << " black=" << black_value()
        << " canUndo=" << presenter.canUndo() << " canRedo=" << presenter.canRedo();
    EXPECT_FALSE(presenter.canUndo());
    EXPECT_TRUE(presenter.canRedo());

    presenter.redoEdit();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && std::abs(black_value() - 0.03) < 1e-9; }))
        << presenter.errorText().toStdString();
    presenter.setDevelopNumber(QStringLiteral("exposure"), 0.25);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() &&
                   std::abs(presenter.editExposure() - 0.25) < 1e-9 &&
                   presenter.recipeHistory().size() == 2;
        }))
        << presenter.errorText().toStdString();
    presenter.setDevelopNumber(QStringLiteral("exposureBlack"), 0.04);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(black_value() - 0.04) < 1e-9 &&
                   presenter.recipeHistory().size() == 3;
        }))
        << presenter.errorText().toStdString();

    presenter.setDevelopNumber(QStringLiteral("saturation"), 0.1);
    presenter.setDevelopNumber(QStringLiteral("saturation"), 0.2);
    presenter.setDevelopNumber(QStringLiteral("saturation"), 0.3);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() &&
                   std::abs(presenter.editSaturation() - 0.3) < 1e-9 &&
                   presenter.recipeHistory().size() == 4;
        }))
        << presenter.errorText().toStdString();

    auto repository = SqliteCatalogRepository::open(catalog.toUtf8().toStdString());
    ASSERT_TRUE(repository) << repository.error().message;
    const auto asset_id = presenter.selectedAssetId().toStdString();
    ASSERT_TRUE(wait_until(
        [&]
        {
            auto state = repository.value()->recovery_state(asset_id);
            return state && !state.value().pending();
        }))
        << presenter.errorText().toStdString();
    auto state = repository.value()->recovery_state(asset_id);
    ASSERT_TRUE(state) << state.error().message;
    auto recovery = FilesystemRecoveryStore::create_for_catalog(catalog.toUtf8().toStdString());
    ASSERT_TRUE(recovery) << recovery.error().message;
    auto sidecar =
        recovery.value()->verify(asset_id, state.value().generation, CancellationToken{});
    ASSERT_TRUE(sidecar) << sidecar.error().message;
    ASSERT_TRUE(repository.value()->close());
}

TEST(StudioPresenterTest, PresetDebugInfoHashesStyleAndRejectsUnknownFiles)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    DevelopParams develop;
    auto recipe = recipe_from_develop({"asset-a", "file:///source-a.jpg", "hash-a"}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto style = recipe_style_from_recipe("Warm debug", "", recipe.value());
    ASSERT_TRUE(style) << style.error().message;
    auto serialized = serialize_recipe_style(style.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    const QString path = directory.filePath(QStringLiteral("Warm debug.rstyle.json"));
    {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            << file.errorString().toStdString();
        file.write(QByteArray::fromStdString(serialized.value()));
    }

    StudioPresenter presenter;
    const auto text = presenter.presetDebugInfo(path);
    EXPECT_TRUE(text.startsWith(QStringLiteral("ravo.debug.preset 1\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("name=Warm debug\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("kind=style\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("path=")));
    QFile hashed(path);
    ASSERT_TRUE(hashed.open(QIODevice::ReadOnly));
    const auto digest =
        QCryptographicHash::hash(hashed.readAll(), QCryptographicHash::Sha256).toHex();
    EXPECT_TRUE(
        text.contains(QStringLiteral("sha256=") + QString::fromLatin1(digest) + QLatin1Char('\n')));

    EXPECT_TRUE(presenter.presetDebugInfo(QStringLiteral("/missing/preset.xmp")).isEmpty());
    const QString junk = directory.filePath(QStringLiteral("notes.txt"));
    {
        QFile file(junk);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("not a preset");
    }
    EXPECT_TRUE(presenter.presetDebugInfo(junk).isEmpty());
}

TEST(StudioPresenterTest, ManagedPresetRenameAndDeleteAreScopedAndPreserveContent)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source_directory = directory.filePath(QStringLiteral("source"));
    ASSERT_TRUE(QDir().mkpath(source_directory));

    DevelopParams develop;
    auto recipe = recipe_from_develop({"asset-a", "file:///source-a.jpg", "hash-a"}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto style = recipe_style_from_recipe("Warm", "", recipe.value());
    ASSERT_TRUE(style) << style.error().message;
    auto serialized = serialize_recipe_style(style.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    const QByteArray original = QByteArray::fromStdString(serialized.value());
    const QString external_path =
        QDir(source_directory).filePath(QStringLiteral("Warm.rstyle.json"));
    {
        QFile file(external_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            << file.errorString().toStdString();
        ASSERT_EQ(file.write(original), original.size());
    }

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.importPresetFromPath(external_path);
    ASSERT_EQ(presenter.editPresets().size(), 1) << presenter.errorText().toStdString();
    auto preset = presenter.editPresets().front().toMap();
    ASSERT_EQ(preset.value(QStringLiteral("name")).toString(), QStringLiteral("Warm"));
    const QString imported_path = preset.value(QStringLiteral("path")).toString();
    ASSERT_TRUE(QFileInfo::exists(imported_path));

    presenter.renamePreset(imported_path, QStringLiteral("Evening Warm"));
    ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    ASSERT_EQ(presenter.editPresets().size(), 1);
    preset = presenter.editPresets().front().toMap();
    EXPECT_EQ(preset.value(QStringLiteral("name")).toString(), QStringLiteral("Evening Warm"));
    const QString renamed_path = preset.value(QStringLiteral("path")).toString();
    EXPECT_TRUE(renamed_path.endsWith(QStringLiteral("Evening Warm.rstyle.json")));
    EXPECT_FALSE(QFileInfo::exists(imported_path));
    // Windows rejects deletion while this process still owns a read handle.
    {
        QFile renamed(renamed_path);
        ASSERT_TRUE(renamed.open(QIODevice::ReadOnly));
        EXPECT_EQ(renamed.readAll(), original);
    }

    const QString conflict_path =
        QFileInfo(renamed_path).dir().filePath(QStringLiteral("Existing.rstyle.json"));
    ASSERT_TRUE(QFile::copy(renamed_path, conflict_path));
    presenter.renamePreset(renamed_path, QStringLiteral("Existing"));
    EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("already exists")));
    EXPECT_TRUE(QFileInfo::exists(renamed_path));
    ASSERT_TRUE(QFile::remove(conflict_path));

    presenter.renamePreset(renamed_path, QStringLiteral("../escape"));
    EXPECT_FALSE(presenter.errorText().isEmpty());
    EXPECT_TRUE(QFileInfo::exists(renamed_path));
    EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral("escape.rstyle.json"))));
    presenter.renamePreset(QStringLiteral("/missing/preset.xmp"), QStringLiteral("Missing"));
    EXPECT_EQ(presenter.errorText(),
              QCoreApplication::translate("StudioPresenter", "Preset file was not found."));
    presenter.renamePreset(external_path, QStringLiteral("External"));
    EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("imported into this library")));
    EXPECT_TRUE(QFileInfo::exists(external_path));

    presenter.deletePreset(external_path);
    EXPECT_TRUE(presenter.errorText().contains(QStringLiteral("imported into this library")));
    EXPECT_TRUE(QFileInfo::exists(external_path));
    presenter.deletePreset(renamed_path);
    EXPECT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    EXPECT_FALSE(QFileInfo::exists(renamed_path));
    EXPECT_TRUE(presenter.editPresets().isEmpty());
    EXPECT_EQ(presenter.statusText(),
              QCoreApplication::translate("StudioPresenter", "Preset deleted."));
}

} // namespace
} // namespace ravo
