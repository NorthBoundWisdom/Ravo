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
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_language_manager.h"

namespace ravo
{
namespace
{

void ensure_qt_core();

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

void ensure_qt_core()
{
    if (QCoreApplication::instance() != nullptr)
        return;
    static int argc = 1;
    static char executable[] = "ravo-desktop-command-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

[[nodiscard]] bool wait_until(const std::function<bool()> &ready, const int timeout_ms = 15000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms)
    {
        if (ready())
        {
            return true;
        }
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    return ready();
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

class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(const char *name, const QByteArray &value)
        : name_(name)
        , old_value_(qgetenv(name))
        , was_set_(qEnvironmentVariableIsSet(name))
    {
        qputenv(name, value);
    }

    ~ScopedEnvironmentVariable()
    {
        if (was_set_)
            qputenv(name_.constData(), old_value_);
        else
            qunsetenv(name_.constData());
    }

private:
    QByteArray name_;
    QByteArray old_value_;
    bool was_set_ = false;
};

struct CliProcessResult
{
    int exit_code = -1;
    QByteArray standard_output;
    QByteArray standard_error;
};

[[nodiscard]] CliProcessResult run_cli_process(const QStringList &arguments,
                                               const int timeout_ms = 30000)
{
    QProcess process;
    process.setProgram(QStringLiteral(RAVO_CLI_EXECUTABLE));
    process.setArguments(arguments);
    process.start();
    const bool finished =
        wait_until([&] { return process.state() == QProcess::NotRunning; }, timeout_ms);
    if (!finished)
    {
        process.kill();
        process.waitForFinished(5000);
    }
    return {finished ? process.exitCode() : -1, process.readAllStandardOutput(),
            process.readAllStandardError()};
}

[[nodiscard]] Result<JsonValue> cli_data(const QByteArray &output)
{
    const QByteArray trimmed = output.trimmed();
    auto envelope =
        parse_json(std::string_view(trimmed.constData(), static_cast<std::size_t>(trimmed.size())));
    if (!envelope)
        return envelope.error();
    const auto *data = envelope.value().find("data");
    if (data == nullptr)
        return make_error(ErrorCode::kValidation, "CLI response has no data object");
    return *data;
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

[[nodiscard]] QString qml_model_entry(const QString &source, const char *field)
{
    const auto needle = QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field));
    const auto field_position = source.indexOf(needle);
    if (field_position < 0)
    {
        return {};
    }
    const auto begin = source.lastIndexOf(QLatin1Char('{'), field_position);
    const auto end = source.indexOf(QLatin1Char('}'), field_position);
    if (begin < 0 || end < field_position)
    {
        return {};
    }
    return source.mid(begin, end - begin + 1);
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

TEST(StudioPresenterTest, SavesSelectedModifiedParametersAndAppliesThemAsOverlay)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("selective-preset.png"));
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 120, 170));
    ASSERT_TRUE(image.save(photo, "PNG"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
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
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.modifiedParameterChoices().isEmpty());

    presenter.setDevelopNumber(QStringLiteral("exposure"), 0.75);
    presenter.setDevelopNumber(QStringLiteral("saturation"), 0.4);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() &&
                   std::abs(presenter.editExposure() - 0.75) < 1e-9 &&
                   std::abs(presenter.editSaturation() - 0.4) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    const auto candidates = presenter.modifiedParameterChoices();
    const auto has_field = [&candidates](const QString &field)
    {
        return std::any_of(
            candidates.cbegin(), candidates.cend(), [&field](const QVariant &entry)
            { return entry.toMap().value(QStringLiteral("field")).toString() == field; });
    };
    EXPECT_TRUE(has_field(QStringLiteral("exposure")));
    EXPECT_TRUE(has_field(QStringLiteral("saturation")));

    presenter.copyParametersSelected(QVariantList{QStringLiteral("exposure")});
    ASSERT_TRUE(presenter.hasCopiedParameters());
    EXPECT_EQ(presenter.statusText(),
              QCoreApplication::translate("StudioPresenter", "Parameters copied."));

    presenter.savePreset(QStringLiteral("Exposure only"), QVariantList{QStringLiteral("exposure")});
    ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    ASSERT_EQ(presenter.editPresets().size(), 1);
    const QString preset_path =
        presenter.editPresets().front().toMap().value(QStringLiteral("path")).toString();
    QFile preset_file(preset_path);
    ASSERT_TRUE(preset_file.open(QIODevice::ReadOnly)) << preset_file.errorString().toStdString();
    auto style = parse_recipe_style_json(preset_file.readAll().toStdString());
    ASSERT_TRUE(style) << style.error().message;
    EXPECT_EQ(style.value().schema_version, kRecipeStyleSelectedSchemaVersion);
    EXPECT_EQ(style.value().selected_fields, (std::vector<std::string>{"exposure"}));

    presenter.setDevelopNumber(QStringLiteral("exposure"), -0.25);
    presenter.setDevelopNumber(QStringLiteral("saturation"), -0.3);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() &&
                   std::abs(presenter.editExposure() + 0.25) < 1e-9 &&
                   std::abs(presenter.editSaturation() + 0.3) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.pasteParameters();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(presenter.editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.editSaturation(), -0.3, 1e-9);
    EXPECT_EQ(presenter.statusText(),
              QCoreApplication::translate("StudioPresenter", "Parameters pasted."));

    presenter.setDevelopNumber(QStringLiteral("exposure"), -0.25);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(presenter.editExposure() + 0.25) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.applyStyleFromPath(preset_path);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(presenter.editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.editSaturation(), -0.3, 1e-9);

    presenter.savePreset(QStringLiteral("Exposure only"), QVariantList{QStringLiteral("exposure")});
    EXPECT_EQ(
        presenter.errorText(),
        QCoreApplication::translate("StudioPresenter", "A preset with that name already exists."));
    presenter.savePreset(QStringLiteral("Stale selection"),
                         QVariantList{QStringLiteral("colorContrast")});
    EXPECT_EQ(presenter.errorText(),
              QCoreApplication::translate("StudioPresenter",
                                          "The selected parameters are no longer modified."));

    presenter.copyParametersSelected(QVariantList{QStringLiteral("colorContrast")});
    EXPECT_TRUE(presenter.hasCopiedParameters());
    EXPECT_EQ(presenter.errorText(),
              QCoreApplication::translate("StudioPresenter",
                                          "The selected parameters are no longer modified."));
    presenter.setDevelopNumber(QStringLiteral("exposure"), -0.1);
    ASSERT_TRUE(wait_until(
        [&]
        { return !presenter.previewLoading() && std::abs(presenter.editExposure() + 0.1) < 1e-9; }))
        << presenter.errorText().toStdString();
    presenter.pasteParameters();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(presenter.editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.editSaturation(), -0.3, 1e-9);
}

TEST(StudioPresenterTest, ApplyingStylePublishesLivePreviewBeforeSettledCache)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(1920, 1280, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 120, 170));
    ASSERT_TRUE(image.save(photo, "PNG"));

    DevelopParams style_develop;
    style_develop.exposure_ev = 1.0;
    auto recipe = recipe_from_develop({"style", "file:///style.png", "style-hash"}, style_develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto style = recipe_style_from_recipe("Progressive", {}, recipe.value());
    ASSERT_TRUE(style) << style.error().message;
    auto serialized = serialize_recipe_style(style.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    const QString style_path = directory.filePath(QStringLiteral("Progressive.rstyle.json"));
    {
        QFile file(style_path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(QByteArray::fromStdString(serialized.value())),
                  static_cast<qint64>(serialized.value().size()));
    }

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
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
    bool saw_initial_live = false;
    QObject::connect(&presenter, &StudioPresenter::previewChanged, &presenter,
                     [&]
                     {
                         const auto url = presenter.previewUrl();
                         saw_initial_live =
                             saw_initial_live || (url.scheme() == QLatin1String("image") &&
                                                  url.path() == QLatin1String("/live"));
                     });
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.previewUrl().isLocalFile(); }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(saw_initial_live);
    const QSize settled_viewport(presenter.previewViewportWidth(),
                                 presenter.previewViewportHeight());
    EXPECT_EQ(settled_viewport, presenter.previewImage().size());
    EXPECT_EQ(std::max(settled_viewport.width(), settled_viewport.height()), 1600);

    bool saw_uncommitted_live = false;
    QSize uncommitted_live_image_size;
    QSize uncommitted_live_viewport;
    QObject::connect(
        &presenter, &StudioPresenter::previewChanged, &presenter,
        [&]
        {
            const auto url = presenter.previewUrl();
            if (url.scheme() == QLatin1String("image") && url.path() == QLatin1String("/live"))
            {
                saw_uncommitted_live = true;
                uncommitted_live_image_size = presenter.previewImage().size();
                uncommitted_live_viewport =
                    QSize(presenter.previewViewportWidth(), presenter.previewViewportHeight());
            }
        });
    presenter.previewDevelopNumber(QStringLiteral("exposure"), 0.25);
    ASSERT_TRUE(wait_until([&] { return saw_uncommitted_live && !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.editExposure(), 0.25, 1e-9);
    EXPECT_EQ(std::max(uncommitted_live_image_size.width(), uncommitted_live_image_size.height()),
              960);
    EXPECT_EQ(uncommitted_live_viewport, settled_viewport);

    bool saw_live = false;
    bool saw_settled = false;
    bool live_viewport_stayed_settled = false;
    QObject::connect(&presenter, &StudioPresenter::previewChanged, &presenter,
                     [&]
                     {
                         const auto url = presenter.previewUrl();
                         if (url.scheme() == QLatin1String("image") &&
                             url.path() == QLatin1String("/live"))
                         {
                             saw_live = true;
                             live_viewport_stayed_settled =
                                 QSize(presenter.previewViewportWidth(),
                                       presenter.previewViewportHeight()) == settled_viewport;
                         }
                         saw_settled = saw_live && url.isLocalFile();
                     });
    presenter.applyStyleFromPath(style_path);
    ASSERT_TRUE(wait_until([&] { return saw_live && saw_settled; }))
        << presenter.errorText().toStdString()
        << " url=" << presenter.previewUrl().toString().toStdString();
    EXPECT_FALSE(presenter.previewLoading());
    EXPECT_NEAR(presenter.editExposure(), 1.0, 1e-9);
    EXPECT_TRUE(live_viewport_stayed_settled);
    EXPECT_EQ(QSize(presenter.previewViewportWidth(), presenter.previewViewportHeight()),
              settled_viewport);
}

TEST(StudioPresenterTest, ToolbarComparisonKeepsBeforeStableWhileAfterUpdates)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("comparison.png"));
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 105, 150));
    ASSERT_TRUE(image.save(photo, "PNG"));

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
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
            return !presenter.previewLoading() && !presenter.previewImage().isNull() &&
                   !presenter.previewUrl().isEmpty();
        }))
        << presenter.errorText().toStdString();

    presenter.setDevelopNumber(QStringLiteral("exposure"), 0.5);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && presenter.previewUrl().isLocalFile() &&
                   std::abs(presenter.editExposure() - 0.5) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    const QImage first_after = presenter.previewImage();
    ASSERT_FALSE(first_after.isNull());

    const auto comparison_action =
        commands.ids().value(QStringLiteral("editComparison")).toString();
    ASSERT_EQ(comparison_action, QStringLiteral("studio.edit.toggle_comparison"));
    bool found_y_shortcut = false;
    for (const auto &entry : commands.shortcutEntries())
    {
        const auto fields = entry.toMap();
        found_y_shortcut =
            found_y_shortcut ||
            (fields.value(QStringLiteral("actionId")).toString() == comparison_action &&
             fields.value(QStringLiteral("sequence")).toString() == QLatin1String("Y"));
    }
    EXPECT_TRUE(found_y_shortcut);
    const auto activated = commands.executeAction(comparison_action, QStringLiteral("control"));
    ASSERT_TRUE(activated.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.comparisonActive() && !presenter.previewLoading() &&
                   !presenter.comparisonBeforeUrl().isEmpty() &&
                   !presenter.comparisonBeforeImage().isNull();
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(commands.action(comparison_action).value(QStringLiteral("checked")).toBool());
    const QImage before = presenter.comparisonBeforeImage();
    const QImage after = presenter.previewImage();
    ASSERT_EQ(before.size(), after.size());
    const QPoint center(before.width() / 2, before.height() / 2);
    EXPECT_NE(before.pixelColor(center), after.pixelColor(center));
    EXPECT_EQ(first_after.pixelColor(center), after.pixelColor(center));

    presenter.setDevelopNumber(QStringLiteral("exposure"), 1.0);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && presenter.previewUrl().isLocalFile() &&
                   std::abs(presenter.editExposure() - 1.0) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.comparisonActive());
    EXPECT_EQ(before.pixelColor(center), presenter.comparisonBeforeImage().pixelColor(center));
    EXPECT_NE(after.pixelColor(center), presenter.previewImage().pixelColor(center));

    const auto deactivated = commands.executeAction(comparison_action, QStringLiteral("control"));
    ASSERT_TRUE(deactivated.value(QStringLiteral("accepted")).toBool());
    EXPECT_FALSE(presenter.comparisonActive());
    EXPECT_TRUE(presenter.comparisonBeforeUrl().isEmpty());
    EXPECT_TRUE(presenter.comparisonBeforeImage().isNull());
    EXPECT_FALSE(commands.action(comparison_action).value(QStringLiteral("checked")).toBool());

    ASSERT_TRUE(commands.executeAction(comparison_action, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(commands.executeAction(comparison_action, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }));
    EXPECT_FALSE(presenter.comparisonActive());
    EXPECT_TRUE(presenter.comparisonBeforeUrl().isEmpty());
}

TEST(StudioPresenterTest, RapidDevelopIntentsPublishProgressAndLatestExactIdentity)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedEnvironmentVariable registry("RAVO_LIVE_CONTROL_DIR",
                                       directory.filePath(QStringLiteral("live-control")).toUtf8());
    const QString photo = directory.filePath(QStringLiteral("rapid-preview.png"));
    QImage image(1440, 960, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 120, 170));
    ASSERT_TRUE(image.save(photo, "PNG"));

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    auto live = StudioLiveSessionController::create(presenter, commands);
    ASSERT_TRUE(live) << live.error().message;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
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
        [&] { return !presenter.previewLoading() && presenter.previewUrl().isLocalFile(); }))
        << presenter.errorText().toStdString();

    std::vector<qulonglong> published_revisions;
    std::vector<qulonglong> timed_revisions;
    QUrl last_url = presenter.previewUrl();
    QObject::connect(&presenter, &StudioPresenter::interactivePreviewPublished, &presenter,
                     [&](const qulonglong revision, const qlonglong intent_to_image_us)
                     {
                         EXPECT_GT(intent_to_image_us, 0);
                         timed_revisions.push_back(revision);
                     });
    QObject::connect(&presenter, &StudioPresenter::previewChanged, &presenter,
                     [&]
                     {
                         const QUrl current = presenter.previewUrl();
                         if (current == last_url || current.scheme() != QLatin1String("image") ||
                             current.path() != QLatin1String("/live"))
                         {
                             return;
                         }
                         last_url = current;
                         published_revisions.push_back(
                             QUrlQuery(current).queryItemValue(QStringLiteral("r")).toULongLong());
                     });

    presenter.previewDevelopNumber(QStringLiteral("exposure"), -0.2);
    constexpr int kIntentCount = 40;
    for (int index = 1; index < kIntentCount; ++index)
    {
        presenter.previewDevelopNumber(QStringLiteral("exposure"),
                                       -0.2 + static_cast<double>(index) * 0.02);
    }
    const double latest_exposure = -0.2 + static_cast<double>(kIntentCount - 1) * 0.02;

    ASSERT_TRUE(wait_until([&] { return published_revisions.size() >= 2U; }, 10000))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(wait_until(
        [&]
        {
            const auto state = live.value()->snapshot();
            const auto *preview = state.find("preview");
            if (preview == nullptr || preview->object_if() == nullptr)
                return false;
            const auto *status = preview->find("state");
            const auto *matches = preview->find("matches_current_recipe");
            const auto *digest = preview->find("pixel_sha256");
            return status != nullptr && status->string_if() != nullptr &&
                   *status->string_if() == "ready" && matches != nullptr &&
                   matches->boolean_if() != nullptr && *matches->boolean_if() &&
                   digest != nullptr && digest->string_if() != nullptr &&
                   !digest->string_if()->empty();
        },
        10000))
        << presenter.errorText().toStdString();

    EXPECT_EQ(published_revisions.size(), 2U);
    EXPECT_EQ(timed_revisions, published_revisions);
    EXPECT_LT(published_revisions.front(), published_revisions.back());
    EXPECT_NEAR(presenter.editExposure(), latest_exposure, 1e-9);

    const auto state = live.value()->snapshot();
    const auto *preview = state.find("preview");
    ASSERT_NE(preview, nullptr);
    const auto *profile = preview->find("color_profile");
    const auto *digest = preview->find("pixel_sha256");
    ASSERT_NE(profile, nullptr);
    ASSERT_NE(profile->string_if(), nullptr);
    ASSERT_NE(digest, nullptr);
    ASSERT_NE(digest->string_if(), nullptr);
    const QImage displayed = presenter.previewImage();
    ASSERT_EQ(displayed.format(), QImage::Format_RGB888);
    QCryptographicHash expected(QCryptographicHash::Sha256);
    for (int row = 0; row < displayed.height(); ++row)
    {
        expected.addData(
            QByteArrayView(reinterpret_cast<const char *>(displayed.constScanLine(row)),
                           static_cast<qsizetype>(displayed.width() * 3)));
    }
    expected.addData(QByteArray::fromStdString(*profile->string_if()));
    EXPECT_EQ(*digest->string_if(), expected.result().toHex().toStdString());
}

TEST(StudioInteractivePreviewPerformanceProbe, MeasuresExposureIntentThroughImagePublication)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    const std::size_t runs =
        std::getenv("RAVO_INTERACTIVE_PERF_RUNS") != nullptr ?
            static_cast<std::size_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_PERF_RUNS"))) :
            9U;
    ASSERT_GT(runs, 0U);

    StudioPresenter presenter;
    presenter.openCatalogFromPath(QString::fromUtf8(catalog_path));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    presenter.selectAsset(QString::fromUtf8(asset_id));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedAssetId() == QString::fromUtf8(asset_id) && !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.previewUrl().isLocalFile(); }, 30000))
        << presenter.errorText().toStdString();

    const double baseline = presenter.editExposure();
    const double sweep_center = std::clamp(baseline, -2.9, 3.9);
    std::vector<std::int64_t> elapsed_us;
    elapsed_us.reserve(runs);
    for (std::size_t run = 0U; run < runs; ++run)
    {
        const double offset = static_cast<double>(static_cast<int>(run % 7U) - 3) * 0.01;
        const QUrl previous = presenter.previewUrl();
        QElapsedTimer timer;
        QEventLoop event_loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        std::optional<std::int64_t> published_us;
        QObject::connect(&timeout, &QTimer::timeout, &event_loop, &QEventLoop::quit);
        const auto connection = QObject::connect(
            &presenter, &StudioPresenter::previewChanged, &presenter,
            [&]
            {
                const QUrl current = presenter.previewUrl();
                if (!published_us.has_value() && current != previous &&
                    current.scheme() == QLatin1String("image") &&
                    current.path() == QLatin1String("/live") && !presenter.previewImage().isNull())
                {
                    published_us = timer.nsecsElapsed() / 1000;
                    event_loop.quit();
                }
            });
        timer.start();
        presenter.previewDevelopNumber(QStringLiteral("exposure"), sweep_center + offset);
        timeout.start(5000);
        if (!published_us.has_value())
        {
            event_loop.exec();
        }
        QObject::disconnect(connection);
        ASSERT_TRUE(published_us.has_value()) << presenter.errorText().toStdString();
        elapsed_us.push_back(*published_us);
    }
    std::sort(elapsed_us.begin(), elapsed_us.end());
    const std::size_t p90_index = (elapsed_us.size() * 9U - 1U) / 10U;
    const auto median_us = elapsed_us[elapsed_us.size() / 2U];
    const auto p90_us = elapsed_us[p90_index];
    std::cerr << "studio_interactive_runs=" << runs
              << " intent_to_publish_min_us=" << elapsed_us.front()
              << " intent_to_publish_median_us=" << median_us
              << " intent_to_publish_p90_us=" << p90_us
              << " intent_to_publish_max_us=" << elapsed_us.back() << '\n';
    if (const char *budget = std::getenv("RAVO_INTERACTIVE_PERF_P90_BUDGET_MS"))
    {
        EXPECT_LE(p90_us, std::stoll(budget) * 1000);
    }
}

TEST(StudioInteractivePreviewPerformanceProbe, MeasuresRapidIntentBurstToLatestPublication)
{
    const char *catalog_path = std::getenv("RAVO_INTERACTIVE_PERF_CATALOG");
    const char *asset_id = std::getenv("RAVO_INTERACTIVE_PERF_ASSET_ID");
    if (catalog_path == nullptr || asset_id == nullptr)
    {
        GTEST_SKIP() << "set RAVO_INTERACTIVE_PERF_CATALOG and RAVO_INTERACTIVE_PERF_ASSET_ID";
    }
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir registry;
    ASSERT_TRUE(registry.isValid());
    ScopedEnvironmentVariable registry_path("RAVO_LIVE_CONTROL_DIR", registry.path().toUtf8());
    const std::size_t intents =
        std::getenv("RAVO_INTERACTIVE_BURST_RUNS") != nullptr ?
            static_cast<std::size_t>(std::stoul(std::getenv("RAVO_INTERACTIVE_BURST_RUNS"))) :
            40U;
    ASSERT_GT(intents, 1U);

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    auto live = StudioLiveSessionController::create(presenter, commands);
    ASSERT_TRUE(live) << live.error().message;
    presenter.openCatalogFromPath(QString::fromUtf8(catalog_path));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    presenter.selectAsset(QString::fromUtf8(asset_id));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedAssetId() == QString::fromUtf8(asset_id) && !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && presenter.previewUrl().isLocalFile(); }, 30000))
        << presenter.errorText().toStdString();

    const double baseline = presenter.editExposure();
    const double burst_start = std::clamp(baseline, -2.8, 3.8) - 0.02;
    QElapsedTimer timer;
    QEventLoop event_loop;
    QTimer intent_timer;
    QTimer timeout;
    intent_timer.setTimerType(Qt::PreciseTimer);
    intent_timer.setInterval(1);
    timeout.setSingleShot(true);
    std::size_t sent = 0U;
    std::optional<std::int64_t> first_intent_us;
    std::optional<std::int64_t> last_intent_us;
    std::optional<std::int64_t> latest_published_us;
    std::vector<std::int64_t> frame_us;
    QUrl last_url = presenter.previewUrl();
    const auto preview_matches_current = [&]
    {
        const auto state = live.value()->snapshot();
        const auto *preview = state.find("preview");
        if (preview == nullptr || preview->object_if() == nullptr)
            return false;
        const auto *matches = preview->find("matches_current_recipe");
        return matches != nullptr && matches->boolean_if() != nullptr && *matches->boolean_if();
    };
    QObject::connect(&intent_timer, &QTimer::timeout, &event_loop,
                     [&]
                     {
                         const auto now_us = timer.nsecsElapsed() / 1000;
                         if (!first_intent_us)
                             first_intent_us = now_us;
                         presenter.previewDevelopNumber(QStringLiteral("exposure"),
                                                        burst_start +
                                                            static_cast<double>(sent) * 0.001);
                         last_intent_us = now_us;
                         ++sent;
                         if (sent == intents)
                             intent_timer.stop();
                     });
    QObject::connect(&presenter, &StudioPresenter::previewChanged, &event_loop,
                     [&]
                     {
                         const QUrl current = presenter.previewUrl();
                         if (current == last_url || current.scheme() != QLatin1String("image") ||
                             current.path() != QLatin1String("/live"))
                         {
                             return;
                         }
                         last_url = current;
                         const auto now_us = timer.nsecsElapsed() / 1000;
                         frame_us.push_back(now_us);
                         if (sent == intents && preview_matches_current())
                         {
                             latest_published_us = now_us;
                             event_loop.quit();
                         }
                     });
    QObject::connect(&timeout, &QTimer::timeout, &event_loop, &QEventLoop::quit);

    timer.start();
    intent_timer.start();
    timeout.start(5000);
    event_loop.exec();
    ASSERT_TRUE(first_intent_us.has_value());
    ASSERT_TRUE(last_intent_us.has_value());
    ASSERT_TRUE(latest_published_us.has_value()) << presenter.errorText().toStdString();
    ASSERT_GE(frame_us.size(), 2U);
    const auto first_latency_us = frame_us.front() - *first_intent_us;
    const auto latest_latency_us = *latest_published_us - *last_intent_us;
    std::int64_t maximum_frame_gap_us = 0;
    for (std::size_t index = 1U; index < frame_us.size(); ++index)
    {
        maximum_frame_gap_us =
            std::max(maximum_frame_gap_us, frame_us[index] - frame_us[index - 1U]);
    }
    std::cerr << "studio_interactive_burst_intents=" << intents
              << " published_frames=" << frame_us.size()
              << " first_intent_to_publish_us=" << first_latency_us
              << " latest_intent_to_publish_us=" << latest_latency_us
              << " maximum_frame_gap_us=" << maximum_frame_gap_us << '\n';
    if (const char *budget = std::getenv("RAVO_INTERACTIVE_BURST_BUDGET_MS"))
    {
        const auto budget_us = std::stoll(budget) * 1000;
        EXPECT_LE(first_latency_us, budget_us);
        EXPECT_LE(latest_latency_us, budget_us);
    }
}

TEST(StudioPresenterTest, SessionUndoStartsEmptyAndHistoryRestoreWithoutSelectionIsIgnored)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_FALSE(presenter.canUndo());
    EXPECT_FALSE(presenter.canRedo());
    presenter.restoreHistory(0);
    presenter.undoEdit();
    presenter.redoEdit();
    EXPECT_FALSE(presenter.canUndo());
    EXPECT_FALSE(presenter.canRedo());
}

TEST(StudioPresenterTest, PollAppliesDevelopWrittenByAnotherCatalogClient)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("photo.png"));
    QImage image(32, 24, QImage::Format_RGB888);
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
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    {
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 500)
        {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
    }
    EXPECT_NEAR(presenter.editExposure(), 0.0, 1e-9);
    const auto asset_id = presenter.selectedAssetId().toStdString();

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    const auto catalog_utf8 = catalog.toUtf8().toStdString();
    auto repository = SqliteCatalogRepository::open(catalog_utf8);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(catalog_utf8 + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    auto recovery = FilesystemRecoveryStore::create_for_catalog(catalog_utf8);
    ASSERT_TRUE(recovery) << recovery.error().message;
    CatalogService writer(engine.value(), std::move(repository).value(),
                          std::make_unique<QtRasterDecoder>(), std::move(cache).value(),
                          std::move(recovery).value());
    DevelopParams params;
    params.exposure_ev = 1.0;
    auto saved = writer.save_develop(asset_id, params);
    ASSERT_TRUE(saved) << saved.error().message;
    ASSERT_TRUE(writer.close());

    ASSERT_TRUE(wait_until(
        [&]
        {
            presenter.pollCatalogRevision();
            return std::abs(presenter.editExposure() - 1.0) < 1e-6;
        }))
        << presenter.errorText().toStdString() << " exposure=" << presenter.editExposure();
    EXPECT_TRUE(presenter.selectedHasEdits());
    EXPECT_FALSE(presenter.canUndo());
}

TEST(StudioPresenterTest, ScopeModeOwnsAllAcceptedDiagnosticsAndRejectsFutureState)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.scopeMode(), QStringLiteral("histogram"));
    for (const auto &mode : {QStringLiteral("waveform"), QStringLiteral("parade"),
                             QStringLiteral("vectorscope"), QStringLiteral("split")})
    {
        presenter.setScopeMode(mode);
        EXPECT_EQ(presenter.scopeMode(), mode);
    }
    presenter.setScopeMode(QStringLiteral("future"));
    EXPECT_EQ(presenter.scopeMode(), QStringLiteral("histogram"));
    EXPECT_TRUE(presenter.scopeParadeUrl().isEmpty());
    EXPECT_TRUE(presenter.scopeWaveformUrl().isEmpty());
    EXPECT_TRUE(presenter.scopeVectorscopeUrl().isEmpty());
    EXPECT_TRUE(presenter.scopeSplitUrl().isEmpty());
}

TEST(StudioQmlContract, LegacyColorBalanceSlidersExposeEverySchemaHardEndpoint)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    constexpr std::array<const char *, 14> zero_to_two_fields{
        "legacyColorBalanceLiftFactor",      "legacyColorBalanceLiftRed",
        "legacyColorBalanceLiftGreen",       "legacyColorBalanceLiftBlue",
        "legacyColorBalanceGammaFactor",     "legacyColorBalanceGammaRed",
        "legacyColorBalanceGammaGreen",      "legacyColorBalanceGammaBlue",
        "legacyColorBalanceGainFactor",      "legacyColorBalanceGainRed",
        "legacyColorBalanceGainGreen",       "legacyColorBalanceGainBlue",
        "legacyColorBalanceInputSaturation", "legacyColorBalanceOutputSaturation",
    };
    for (const auto *field : zero_to_two_fields)
    {
        const auto entry = qml_model_entry(source, field);
        ASSERT_FALSE(entry.isEmpty()) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"minimum\": 0"))) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"maximum\": 2"))) << field;
    }
    const auto contrast = qml_model_entry(source, "legacyColorBalanceContrast");
    ASSERT_FALSE(contrast.isEmpty());
    EXPECT_TRUE(contrast.contains(QStringLiteral("\"minimum\": 0.01")));
    EXPECT_TRUE(contrast.contains(QStringLiteral("\"maximum\": 1.99")));
    const auto fulcrum = qml_model_entry(source, "legacyColorBalanceGreyFulcrum");
    ASSERT_FALSE(fulcrum.isEmpty());
    EXPECT_TRUE(fulcrum.contains(QStringLiteral("\"minimum\": 0.1")));
    EXPECT_TRUE(fulcrum.contains(QStringLiteral("\"maximum\": 100")));
}

TEST(StudioQmlContract, ColorCheckerExposesEveryLabFieldWithoutClampingCanonicalFloats)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    constexpr std::array<const char *, 6> fields{
        "colorCheckerSourceL", "colorCheckerSourceA", "colorCheckerSourceB",
        "colorCheckerTargetL", "colorCheckerTargetA", "colorCheckerTargetB",
    };
    for (const auto *field : fields)
    {
        EXPECT_TRUE(
            source.contains(QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field))))
            << field;
    }
    EXPECT_TRUE(source.contains(QStringLiteral("bottom: -3.402823466e38")));
    EXPECT_TRUE(source.contains(QStringLiteral("top: 3.402823466e38")));
    EXPECT_TRUE(source.contains(QStringLiteral("DoubleValidator.ScientificNotation")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorCheckerPreset")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorCheckerPatch")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editColorChecker.patchCount > 0")));
    EXPECT_FALSE(source.contains(QStringLiteral("root.hasSelection && count > 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"colorChecker\")")));
}

TEST(StudioQmlContract, ColorCorrectionUsesHardBoundsAndGenericDevelopIntents)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    constexpr std::array<const char *, 4> endpoint_fields{
        "colorCorrectionHighlightA", "colorCorrectionHighlightB", "colorCorrectionShadowA",
        "colorCorrectionShadowB"};
    for (const auto *field : endpoint_fields)
    {
        const auto entry = qml_model_entry(source, field);
        ASSERT_FALSE(entry.isEmpty()) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"minimum\": -40"))) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"maximum\": 40"))) << field;
    }
    const auto saturation = qml_model_entry(source, "colorCorrectionSaturation");
    ASSERT_FALSE(saturation.isEmpty());
    EXPECT_TRUE(saturation.contains(QStringLiteral("\"minimum\": -3")));
    EXPECT_TRUE(saturation.contains(QStringLiteral("\"maximum\": 3")));

    const auto section_begin = source.indexOf(QStringLiteral("colorCorrectionEnabled"));
    const auto section_end = source.indexOf(QStringLiteral("colorContrast"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorCorrection")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorCorrectionEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(modelData.field)")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorCorrection\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("affine_lab_v1")));

    const auto rgb_balance = source.indexOf(QStringLiteral("colorBalanceGlobalY"));
    const auto correction = source.indexOf(QStringLiteral("colorCorrectionHighlightA"));
    const auto contrast = source.indexOf(QStringLiteral("colorContrast"), correction);
    ASSERT_GE(rgb_balance, 0);
    ASSERT_GE(correction, 0);
    ASSERT_GE(contrast, 0);
    EXPECT_LT(rgb_balance, correction);
    EXPECT_LT(correction, contrast);
}

TEST(StudioQmlContract, ColorContrastExposesFullV2SurfaceThroughGenericDevelopIntents)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    for (const auto *field : {"colorContrastASteepness", "colorContrastBSteepness"})
    {
        const auto entry = qml_model_entry(source, field);
        ASSERT_FALSE(entry.isEmpty()) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"minimum\": 0"))) << field;
        EXPECT_TRUE(entry.contains(QStringLiteral("\"maximum\": 5"))) << field;
    }
    for (const auto *field : {"colorContrastAOffset", "colorContrastBOffset"})
    {
        EXPECT_TRUE(
            source.contains(QStringLiteral("\"field\": \"%1\"").arg(QString::fromLatin1(field))))
            << field;
    }

    const auto section_begin = source.indexOf(QStringLiteral("colorContrastEnabled"));
    const auto section_end =
        source.indexOf(QStringLiteral("colorHarmonizerEnabled"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorContrast")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorContrastEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Enable Color contrast\")")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(modelData.field, value)")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorContrastUnbound\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Allow extended chroma\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("DoubleValidator.ScientificNotation")));
    EXPECT_TRUE(source.contains(QStringLiteral("bottom: -3.4028234663852886e38")));
    EXPECT_TRUE(source.contains(QStringLiteral("top: 3.4028234663852886e38")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(modelData.field)")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorContrast\")")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Disable and reset Color contrast\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("axis_affine_v2")));

    const auto correction = source.indexOf(QStringLiteral("colorCorrectionEnabled"));
    const auto contrast = source.indexOf(QStringLiteral("colorContrastEnabled"), correction);
    const auto a_steepness = source.indexOf(QStringLiteral("colorContrastASteepness"), contrast);
    const auto b_steepness = source.indexOf(QStringLiteral("colorContrastBSteepness"), contrast);
    const auto a_offset = source.indexOf(QStringLiteral("colorContrastAOffset"), contrast);
    const auto b_offset = source.indexOf(QStringLiteral("colorContrastBOffset"), contrast);
    const auto unbound = source.indexOf(QStringLiteral("colorContrastUnbound"), contrast);
    ASSERT_GE(correction, 0);
    ASSERT_GE(contrast, 0);
    ASSERT_GE(a_steepness, 0);
    ASSERT_GE(b_steepness, 0);
    ASSERT_GE(a_offset, 0);
    ASSERT_GE(b_offset, 0);
    ASSERT_GE(unbound, 0);
    EXPECT_LT(correction, contrast);
    EXPECT_LT(contrast, a_steepness);
    EXPECT_LT(a_steepness, b_steepness);
    EXPECT_LT(b_steepness, a_offset);
    EXPECT_LT(a_offset, b_offset);
    EXPECT_LT(b_offset, unbound);
}

TEST(StudioQmlContract, VelviaExposesTheFullV2SurfaceThroughGenericDevelopIntents)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    const auto section_begin = source.indexOf(QStringLiteral("objectName: \"velviaEnabled\""));
    const auto section_end = source.indexOf(QStringLiteral("Color Balance RGB"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editVelviaParams.enabled")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editVelviaParams.strength")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editVelviaParams.bias")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editVelviaParams.masked")));
    EXPECT_TRUE(
        section.contains(QStringLiteral("setDevelopNumber(\"velviaEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(
        section.contains(QStringLiteral("previewDevelopNumber(\"velviaStrength\", value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(\"velviaStrength\", value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"velviaBias\", value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(\"velviaBias\", value)")));
    EXPECT_TRUE(section.contains(QStringLiteral("objectName: \"velviaStrength\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("from: 0")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 100")));
    EXPECT_TRUE(section.contains(QStringLiteral("objectName: \"velviaBias\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 1")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"velvia\")")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Disable and reset Velvia\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("frozen_velvia_v2")));
    EXPECT_FALSE(section.contains(QStringLiteral("luminance"), Qt::CaseInsensitive));
}

TEST(StudioQmlContract, ThreeDimensionalLutUsesTypedPresenterAndGenericDevelopIntents)
{
    StudioPresenter presenter;
    const auto state = presenter.editLut3d();
    EXPECT_FALSE(state.value(QStringLiteral("present")).toBool());
    EXPECT_FALSE(state.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(state.value(QStringLiteral("hasFile")).toBool());
    EXPECT_EQ(state.value(QStringLiteral("spaceChoices")).toList().size(), 6);
    EXPECT_EQ(state.value(QStringLiteral("interpolationChoices")).toList().size(), 2);

    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto begin = source.indexOf(QStringLiteral("objectName: \"lut3dFile\""));
    const auto end = source.indexOf(QStringLiteral("Color Balance RGB"), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(source.contains(QStringLiteral("QmlFileDialogPage")));
    EXPECT_TRUE(source.contains(QStringLiteral("Cube LUT (*.cube *.CUBE)")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editLut3d.filePath")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopText(\"lut3dFile\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("objectName: \"lut3dEnabled\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("lut3dInputSpaceIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("lut3dOutputSpaceIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("lut3dInterpolationIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"lut3dStrength\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"lut3d\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("LUT_3D_SIZE")));
}

TEST(StudioQmlContract, ColorHarmonizerLoadsNumericControlsWithoutForbiddenPresentation)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    const auto section_begin = source.indexOf(QStringLiteral("colorHarmonizerEnabled"));
    const auto section_end =
        source.indexOf(QStringLiteral("qsTr(\"Color Reconstruction\")"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorHarmonizer")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorHarmonizerEnabled\", checked ? 1 : 0)")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorHarmonizerRuleIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.ruleChoices")));
    EXPECT_TRUE(section.contains(
        QStringLiteral("setDevelopNumber(\"colorHarmonizerRuleIndex\", currentIndex)")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.sharedControls")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customNodeControl")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customHueControls")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.nodeSaturationControls")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.minimum")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.maximum")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.step")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.reset")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.visible")));
    EXPECT_TRUE(section.contains(QStringLiteral("nodeControl.field")));
    EXPECT_TRUE(section.contains(QStringLiteral("modelData.field")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customRule")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizer.customNodeCount")));
    EXPECT_FALSE(section.contains(QStringLiteral("\"minimum\": 0, \"maximum\": 360")));
    EXPECT_FALSE(section.contains(
        QStringLiteral("modelData.index < root.presenter.editColorHarmonizer.customNodeCount")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorHarmonizer\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(section.contains(QStringLiteral("auto-detect")));
    EXPECT_FALSE(section.contains(QStringLiteral("histogram")));
    EXPECT_FALSE(section.contains(QStringLiteral("picker")));
    EXPECT_FALSE(section.contains(QStringLiteral("harmony guide"), Qt::CaseInsensitive));
    EXPECT_TRUE(section.contains(QStringLiteral("MaskEditor")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizerMask")));
    EXPECT_TRUE(source.contains(QStringLiteral("component MaskEditor")));
    EXPECT_TRUE(source.contains(QStringLiteral("editGraduatedMask")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.numericControls")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.kindChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.sourceChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("maskEditor.mask.channelChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(maskEditor.mask.detachField)")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("maskEditor.mask.editable === true && modelData.visible")));
    EXPECT_TRUE(source.contains(QStringLiteral("Show mask overlay")));
    EXPECT_TRUE(source.contains(QStringLiteral("setMaskOverlay")));
    EXPECT_FALSE(source.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(source.contains(QStringLiteral("JSON")));

    const auto contrast = source.indexOf(QStringLiteral("colorContrastEnabled"));
    const auto harmonizer = source.indexOf(QStringLiteral("colorHarmonizerEnabled"), contrast);
    const auto reconstruction =
        source.indexOf(QStringLiteral("qsTr(\"Color Reconstruction\")"), harmonizer);
    ASSERT_GE(contrast, 0);
    ASSERT_GE(harmonizer, 0);
    ASSERT_GE(reconstruction, 0);
    EXPECT_LT(contrast, harmonizer);
    EXPECT_LT(harmonizer, reconstruction);
}

TEST(StudioQmlContract, ColorReconstructionExposesTheFrozenV3Surface)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());

    const auto section_begin = source.indexOf(QStringLiteral("colorReconstructionEnabled"));
    const auto section_end = source.indexOf(QStringLiteral("qsTr(\"Color Zones\")"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editColorReconstruction")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionPrecedenceIndex")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionThreshold")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionSpatial")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionRange")));
    EXPECT_TRUE(section.contains(QStringLiteral("colorReconstructionHueDegrees")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"minimum\": 50")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"maximum\": 150")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"maximum\": 1000")));
    EXPECT_TRUE(section.contains(QStringLiteral("\"maximum\": 50")));
    EXPECT_TRUE(section.contains(QStringLiteral("from: 0")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 360")));
    EXPECT_TRUE(section.contains(QStringLiteral("precedenceIndex === 2")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorReconstruction\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(section.contains(QStringLiteral("picker"), Qt::CaseInsensitive));
    EXPECT_FALSE(section.contains(QStringLiteral("GTK"), Qt::CaseInsensitive));

    const auto monochrome = source.indexOf(QStringLiteral("qsTr(\"Monochrome\")"));
    const auto split_toning = source.indexOf(QStringLiteral("qsTr(\"Split Toning\")"));
    const auto advanced = source.indexOf(QStringLiteral("qsTr(\"Color · Advanced\")"));
    const auto harmonizer = source.indexOf(QStringLiteral("colorHarmonizerEnabled"));
    const auto reconstruction = source.indexOf(QStringLiteral("colorReconstructionEnabled"));
    ASSERT_GE(monochrome, 0);
    ASSERT_GE(split_toning, 0);
    ASSERT_GE(advanced, 0);
    ASSERT_GE(harmonizer, 0);
    ASSERT_GE(reconstruction, 0);
    EXPECT_LT(split_toning, monochrome);
    EXPECT_LT(monochrome, advanced);
    EXPECT_LT(advanced, harmonizer);
    EXPECT_LT(harmonizer, reconstruction);
}

TEST(StudioQmlContract, SharpenExposesAmountRadiusAndThresholdFromOnePresenter)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto begin = source.indexOf(QStringLiteral("title: qsTr(\"Sharpen\")"));
    const auto end = source.indexOf(QStringLiteral("title: qsTr(\"Clarity\")"), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editSharpen")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editSharpenRadius")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editSharpenThreshold")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"sharpen\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"sharpenRadius\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"sharpenThreshold\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 8")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 100")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetValue: 0.5")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, TextureIsPrimaryDetailControlWithCollapsedAdvancedScale)
{
    StudioPresenter presenter;
    const auto state = presenter.editTexture();
    EXPECT_DOUBLE_EQ(state.value(QStringLiteral("strength")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(state.value(QStringLiteral("detailThreshold")).toDouble(), 0.2);
    EXPECT_EQ(state.value(QStringLiteral("iterations")).toLongLong(), 1);

    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto detail = source.indexOf(QStringLiteral("sectionId: \"detail\""));
    const auto texture = source.indexOf(QStringLiteral("title: qsTr(\"Texture\")"), detail);
    const auto sharpen = source.indexOf(QStringLiteral("title: qsTr(\"Sharpen\")"), detail);
    ASSERT_GE(detail, 0);
    ASSERT_GT(texture, detail);
    ASSERT_GT(sharpen, texture);
    const auto section = source.mid(texture, sharpen - texture);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editTexture.strength")));
    EXPECT_TRUE(section.contains(QStringLiteral("from: -100")));
    EXPECT_TRUE(section.contains(QStringLiteral("to: 100")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"texture\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Texture · more\")")));
    EXPECT_TRUE(section.contains(QStringLiteral("expanded: false")));
    EXPECT_TRUE(section.contains(QStringLiteral("editTexture.detailThreshold")));
    EXPECT_TRUE(section.contains(QStringLiteral("editTexture.iterations")));
    EXPECT_TRUE(section.contains(QStringLiteral("textureDetailThreshold")));
    EXPECT_TRUE(section.contains(QStringLiteral("textureIterations")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, DehazeExposesStrengthDistanceAndAdaptiveScale)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto begin = source.indexOf(QStringLiteral("title: qsTr(\"Dehaze\")"));
    const auto end = source.indexOf(QStringLiteral("sectionId: \"detail\""), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editDehaze")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editDehazeDistance")));
    EXPECT_TRUE(section.contains(QStringLiteral("root.presenter.editDehazeAdaptive")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"dehaze\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"dehazeDistance\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(\"dehazeAdaptive\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Adaptive window scale\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, OutputDitherUsesPresenterMethodsWithoutQmlPixelMath)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputDitherEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputDitherMethod\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editOutputDither.methodChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputDitherMethodIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputDitherDamping")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"outputDither\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("Auto dithers integer exports")));
    EXPECT_FALSE(source.contains(QStringLiteral("7.0 / 16.0")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"canvasEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: canvasEnabledBox")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editCanvasEnabled")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Enlarge Canvas\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Enable enlarged canvas\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("visible: canvasEnabledBox.checked")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editCanvas.colorChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("canvasColorIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"canvas\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputFrameEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editOutputFrame.basisChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputFrameLineOffset")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"outputFrame\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"watermarkEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"watermarkText\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editWatermark.alignmentChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("setDevelopText(\"watermarkText\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"watermark\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorZonesEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editColorZones.selectByChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorZonesChroma")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorZonesHueInterpolationIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"colorZones\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"monochromeEnabled\"")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("root.presenter.editMonochromeFilter[modelData.key]")));
    EXPECT_TRUE(source.contains(QStringLiteral("monochromeHighlights")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"monochrome\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"splitToningEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editSplitToning.shadowSaturation")));
    EXPECT_TRUE(source.contains(QStringLiteral("splitHighlightSaturation")));
    EXPECT_TRUE(source.contains(QStringLiteral("splitCompress")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"splitToning\")")));
}

TEST(StudioQmlContract, RawSectionExposesSensorAwareDemosaicAndWaveletDenoise)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto raw = source.indexOf(QStringLiteral("sectionId: \"raw\""));
    ASSERT_GE(raw, 0);
    EXPECT_GT(source.indexOf(QStringLiteral("qsTr(\"Auto — RCD / Markesteijn 3\")"), raw), raw);
    EXPECT_GT(source.indexOf(QStringLiteral("qsTr(\"PPG — Bayer compatibility\")"), raw), raw);
    EXPECT_GT(source.indexOf(QStringLiteral("qsTr(\"Markesteijn 1 — X-Trans fast\")"), raw), raw);
    EXPECT_GT(source.indexOf(QStringLiteral("qsTr(\"Markesteijn 3 — X-Trans quality\")"), raw),
              raw);
    EXPECT_GT(source.indexOf(QStringLiteral("setDevelopNumber(\"demosaicModeIndex\""), raw), raw);
    EXPECT_GT(source.indexOf(QStringLiteral("previewDevelopNumber(\"rawDenoiseThreshold\""), raw),
              raw);
    EXPECT_GT(source.indexOf(QStringLiteral("selectedMediaType === \"image/x-raw\""), raw), raw);
}

TEST(StudioQmlContract, LightPresentsWhiteBalanceAndCommonControlsBeforeSpecializedSettings)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto light_begin = source.indexOf(QStringLiteral("sectionId: \"light\""));
    const auto light_end = source.indexOf(QStringLiteral("sectionId: \"curves\""), light_begin);
    ASSERT_GE(light_begin, 0);
    ASSERT_GT(light_end, light_begin);
    const auto light = source.mid(light_begin, light_end - light_begin);

    const auto white_balance = light.indexOf(QStringLiteral("sectionId: \"whiteBalance\""));
    const auto white_balance_mode =
        light.indexOf(QStringLiteral("setDevelopNumber(\"whiteBalanceMode\""));
    const auto exposure = light.indexOf(QStringLiteral("previewDevelopNumber(\"exposure\""));
    const auto sigmoid_contrast =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"sigmoidContrast\""));
    const auto raster_contrast = light.indexOf(QStringLiteral("previewDevelopNumber(\"contrast\""));
    const auto highlights = light.indexOf(QStringLiteral("previewDevelopNumber(\"highlights\""));
    const auto shadows = light.indexOf(QStringLiteral("previewDevelopNumber(\"shadows\""));
    const auto whites = light.indexOf(QStringLiteral("previewDevelopNumber(\"whites\""));
    const auto blacks = light.indexOf(QStringLiteral("previewDevelopNumber(\"blacks\""));
    const auto exposure_mode = light.indexOf(QStringLiteral("setDevelopNumber(\"exposureMode\""));
    const auto exposure_black =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"exposureBlack\""));
    const auto deflicker =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"exposureDeflickerPercentile\""));
    const auto sigmoid_heading =
        light.indexOf(QStringLiteral("qsTr(\"Sigmoid Display · Standard SDR\")"));
    const auto sigmoid_skew = light.indexOf(QStringLiteral("previewDevelopNumber(\"sigmoidSkew\""));
    const auto preserve_hue =
        light.indexOf(QStringLiteral("previewDevelopNumber(\"sigmoidHuePreservation\""));
    const auto gamma = light.indexOf(QStringLiteral("previewDevelopNumber(\"gamma\""));
    const auto rgb_levels = light.indexOf(QStringLiteral("qsTr(\"RGB levels\")"));

    ASSERT_GE(white_balance, 0);
    ASSERT_GE(white_balance_mode, 0);
    ASSERT_GE(exposure, 0);
    ASSERT_GE(sigmoid_contrast, 0);
    ASSERT_GE(raster_contrast, 0);
    ASSERT_GE(highlights, 0);
    ASSERT_GE(shadows, 0);
    ASSERT_GE(whites, 0);
    ASSERT_GE(blacks, 0);
    ASSERT_GE(exposure_mode, 0);
    ASSERT_GE(exposure_black, 0);
    ASSERT_GE(deflicker, 0);
    ASSERT_GE(sigmoid_heading, 0);
    ASSERT_GE(sigmoid_skew, 0);
    ASSERT_GE(preserve_hue, 0);
    ASSERT_GE(gamma, 0);
    ASSERT_GE(rgb_levels, 0);

    EXPECT_LT(white_balance, white_balance_mode);
    EXPECT_LT(white_balance_mode, exposure);
    EXPECT_LT(exposure, sigmoid_contrast);
    EXPECT_LT(exposure, raster_contrast);
    EXPECT_LT(sigmoid_contrast, highlights);
    EXPECT_LT(raster_contrast, highlights);
    EXPECT_LT(highlights, shadows);
    EXPECT_LT(shadows, whites);
    EXPECT_LT(whites, blacks);
    EXPECT_LT(blacks, exposure_mode);
    EXPECT_LT(exposure_mode, exposure_black);
    EXPECT_LT(exposure_black, deflicker);
    EXPECT_LT(deflicker, sigmoid_heading);
    EXPECT_LT(sigmoid_heading, sigmoid_skew);
    EXPECT_LT(sigmoid_skew, preserve_hue);
    EXPECT_LT(preserve_hue, gamma);
    EXPECT_LT(gamma, rgb_levels);
}

TEST(StudioQmlContract, DevelopPanelUsesDefaultGradingStackWithoutBuryingColorEq)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    const auto white_balance = source.indexOf(QStringLiteral("sectionId: \"whiteBalance\""));
    const auto light = source.indexOf(QStringLiteral("sectionId: \"light\""));
    const auto curves = source.indexOf(QStringLiteral("sectionId: \"curves\""));
    const auto color_eq = source.indexOf(QStringLiteral("sectionId: \"colorEqualizer\""));
    const auto color = source.indexOf(QStringLiteral("sectionId: \"color\""));
    const auto geometry = source.indexOf(QStringLiteral("sectionId: \"geometry\""));
    const auto graduated = source.indexOf(QStringLiteral("sectionId: \"graduated\""));
    ASSERT_GE(white_balance, 0);
    ASSERT_GE(light, 0);
    ASSERT_GE(curves, 0);
    ASSERT_GE(color_eq, 0);
    ASSERT_GE(color, 0);
    ASSERT_GE(geometry, 0);
    ASSERT_GE(graduated, 0);
    EXPECT_LT(light, white_balance);
    EXPECT_LT(white_balance, curves);
    EXPECT_LT(light, curves);
    EXPECT_LT(curves, color_eq);
    EXPECT_LT(color_eq, color);
    EXPECT_LT(color, geometry);
    EXPECT_LT(geometry, graduated);
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color Equalizer\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Graduated ND\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("Graduated ND / Color EQ")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceShadowsWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceMidtonesWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceHighlightsWheel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("hueField: \"colorBalanceShadowsHue\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("chromaField: \"colorBalanceShadowsChroma\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color Balance RGB · more\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color · Advanced\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorEqChannel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorEqBand\" + modelData.index")));
    EXPECT_TRUE(source.contains(QStringLiteral("editColorEqBands")));
    EXPECT_TRUE(source.contains(QStringLiteral("satField")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamily\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveChannel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveEditor\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Curves\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Monotonic\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamilyRgb\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveFamilyTone\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curvePointMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"curveParametricMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"resetActiveCurve\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Channel\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Parametric regions\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Curve settings\")")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("curveControls.rgbFamily ? \"rgbCurve\" : \"toneCurve\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("histogramMode")));
    EXPECT_TRUE(source.contains(QStringLiteral("previewCurve")));
    EXPECT_TRUE(source.contains(QStringLiteral("rgbCurveShadows")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Camera Calibration\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("Aqua")));
    EXPECT_TRUE(source.contains(QStringLiteral("Purple")));
    EXPECT_TRUE(source.contains(QStringLiteral("vignetteMidpoint")));
    EXPECT_TRUE(source.contains(QStringLiteral("vignetteCenterX")));
    EXPECT_TRUE(source.contains(QStringLiteral("vignetteCenterY")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Luminance denoise\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Color denoise\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("denoiseChroma")));
    const auto detail_section = source.indexOf(QStringLiteral("sectionId: \"detail\""));
    const auto raw_section = source.indexOf(QStringLiteral("sectionId: \"raw\""));
    const auto luma_denoise = source.indexOf(QStringLiteral("qsTr(\"Luminance denoise\")"));
    ASSERT_GE(detail_section, 0);
    ASSERT_GE(raw_section, 0);
    ASSERT_GE(luma_denoise, 0);
    EXPECT_GT(luma_denoise, detail_section);
    EXPECT_LT(luma_denoise, raw_section);
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"whiteBalancePickActive\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("setWhiteBalancePickActive")));
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("pickWhiteBalance")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("whiteBalancePickActive")));
    QFile wheel(QStringLiteral(RAVO_STUDIO_COLOR_GRADE_WHEEL_QML));
    ASSERT_TRUE(wheel.open(QIODevice::ReadOnly | QIODevice::Text))
        << wheel.errorString().toStdString();
    const auto wheel_source = QString::fromUtf8(wheel.readAll());
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("previewDevelopNumbers")));
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("setDevelopNumbers")));
    EXPECT_FALSE(wheel_source.contains(QStringLiteral("OpenCL")));

    auto curve_editor_path = QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML);
    curve_editor_path.replace(QStringLiteral("DevelopPanel.qml"),
                              QStringLiteral("ToneCurveEditor.qml"));
    QFile curve_editor(curve_editor_path);
    ASSERT_TRUE(curve_editor.open(QIODevice::ReadOnly | QIODevice::Text))
        << curve_editor.errorString().toStdString();
    const auto curve_source = QString::fromUtf8(curve_editor.readAll());
    EXPECT_TRUE(curve_source.contains(QStringLiteral("qsTr(\"Input\")")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("qsTr(\"Output\")")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("property color curveColor")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("property bool showRegionSplits")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("Qt.CrossCursor")));
    EXPECT_TRUE(curve_source.contains(QStringLiteral("root.displayPoints")));
}

TEST(StudioQmlContract, RetouchAuthorsOrderedRegionsThroughCommandBoundary)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("id: retouchEditor")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.presenter.editRetouch.regions")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.commands.addRetouchRegion")));
    EXPECT_TRUE(source.contains(QStringLiteral("root.commands.removeRetouchRegion")));
    EXPECT_TRUE(source.contains(QStringLiteral("[\"clone\", \"heal\", \"blur\", \"fill\"]")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"blurType\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"fillMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"sourceX\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Add retouch region\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("apply_retouch")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editAddRetouchRegion")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editRemoveRetouchRegion")));
}

TEST(StudioQmlContract, DevelopSlidersPublishUserEditsBeforeRelease)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto panel_source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(panel_source.contains(QStringLiteral("onValueEdited: function (value)")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("onValueEdited: if (")));
    EXPECT_FALSE(panel_source.contains(
        QStringLiteral("onValueChanged: if (root.liveReady && root.commands)")));
    EXPECT_TRUE(panel_source.contains(QStringLiteral("onValueCommitted: function (value)")));
    EXPECT_TRUE(panel_source.contains(QStringLiteral("previewDevelopNumber")));
    EXPECT_FALSE(panel_source.contains(QStringLiteral("onValueChanged: retouchEditor.")));

    QFile wheel(QStringLiteral(RAVO_STUDIO_COLOR_GRADE_WHEEL_QML));
    ASSERT_TRUE(wheel.open(QIODevice::ReadOnly | QIODevice::Text))
        << wheel.errorString().toStdString();
    const auto wheel_source = QString::fromUtf8(wheel.readAll());
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("onValueEdited: function (value)")));
    EXPECT_FALSE(wheel_source.contains(QStringLiteral("onValueEdited: if (")));
    EXPECT_FALSE(wheel_source.contains(
        QStringLiteral("onValueChanged: if (root.liveReady && root.commands)")));
    EXPECT_TRUE(wheel_source.contains(QStringLiteral("onValueCommitted: function (value)")));
}

TEST(StudioQmlContract, DevelopSectionsFollowLightroomEditOrder)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Undo\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Reset all\")")));
    const QStringList order{
        QStringLiteral("light"),
        QStringLiteral("curves"),
        QStringLiteral("colorEqualizer"),
        QStringLiteral("color"),
        QStringLiteral("primaries"),
        QStringLiteral("geometry"),
        QStringLiteral("toneEqual"),
        QStringLiteral("graduated"),
        QStringLiteral("effects"),
        QStringLiteral("detail"),
        QStringLiteral("raw"),
        QStringLiteral("calibration"),
        QStringLiteral("inputProfile"),
        QStringLiteral("profileGamma"),
        QStringLiteral("outputProfile"),
    };
    qsizetype cursor = source.indexOf(QStringLiteral("component DevelopSection"));
    ASSERT_GE(cursor, 0);
    for (const auto &id : order)
    {
        const auto needle = QStringLiteral("sectionId: \"%1\"").arg(id);
        const auto found = source.indexOf(needle, cursor);
        ASSERT_GE(found, 0) << id.toStdString();
        EXPECT_GT(found, cursor) << id.toStdString();
        cursor = found + needle.size();
    }
    const auto light = source.indexOf(QStringLiteral("sectionId: \"light\""));
    const auto white_balance = source.indexOf(QStringLiteral("sectionId: \"whiteBalance\""), light);
    const auto curves = source.indexOf(QStringLiteral("sectionId: \"curves\""), light);
    ASSERT_GE(light, 0);
    ASSERT_GT(white_balance, light);
    ASSERT_GT(curves, white_balance);
}

TEST(StudioQmlContract, GeometryCropToolbarUsesIconsAndAspectLock)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Rotate L\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Flip H\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("RotateCcw.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("RotateCw.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("FlipHorizontal.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("FlipVertical.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("Lock.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("Unlock.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("AbstractButton.IconOnly")));
    EXPECT_TRUE(source.contains(QStringLiteral("setCropAspect(checked ? \"locked\" : \"free\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Lock aspect ratio\")")));
    const auto geometry_begin = source.indexOf(QStringLiteral("sectionId: \"geometry\""));
    const auto geometry_end =
        source.indexOf(QStringLiteral("sectionId: \"toneEqual\""), geometry_begin);
    ASSERT_GE(geometry_begin, 0);
    ASSERT_GT(geometry_end, geometry_begin);
    const auto geometry = source.mid(geometry_begin, geometry_end - geometry_begin);
    EXPECT_TRUE(geometry.contains(QStringLiteral("Layout.fillWidth: true")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("qsTr(\"Angle\")")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"straighten\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"perspectiveVertical\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"perspectiveHorizontal\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("\"field\": \"perspectiveShear\"")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("root.commands.autoPerspective(modelData.mode)")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("perspectiveConstrainCrop")));
    EXPECT_TRUE(geometry.contains(QStringLiteral("perspectiveInterpolationIndex")));
}

TEST(StudioQmlContract, DevelopReviewToolbarOffersSynchronizedBeforeAfterComparison)
{
    QFile review(
        QStringLiteral(RAVO_REPOSITORY_ROOT "/Ravo/desktop/qml/gallery/GalleryReviewBar.qml"));
    ASSERT_TRUE(review.open(QIODevice::ReadOnly | QIODevice::Text))
        << review.errorString().toStdString();
    const auto review_source = QString::fromUtf8(review.readAll());
    const auto comparison_button = review_source.indexOf(QStringLiteral("id: comparisonButton"));
    const auto rating_control = review_source.indexOf(QStringLiteral("RatingControl"));
    ASSERT_GE(comparison_button, 0);
    ASSERT_GT(rating_control, comparison_button);
    EXPECT_TRUE(
        review_source.contains(QStringLiteral("objectName: \"beforeAfterComparisonButton\"")));
    EXPECT_TRUE(review_source.contains(QStringLiteral("visible: root.developOpen")));
    EXPECT_TRUE(review_source.contains(
        QStringLiteral("action: root.commands ? root.commands.comparison : null")));
    EXPECT_TRUE(review_source.contains(QStringLiteral("text: qsTr(\"Y|Y\")")));
    EXPECT_TRUE(review_source.contains(QStringLiteral("tooltipText: action ? action.text : \"\"")));

    QFile main_qml(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main_qml.open(QIODevice::ReadOnly | QIODevice::Text))
        << main_qml.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main_qml.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("comparisonReady")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("studio.comparisonBeforeUrl")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: comparisonBeforeImage")));
    EXPECT_TRUE(
        main_source.contains(QStringLiteral("return window.comparisonReady ? width * 2 : width")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("text: qsTr(\"Before\")")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("text: qsTr(\"After\")")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("!studio.comparisonActive")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("property alias comparison")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("root.ids.editComparison")));
}

TEST(StudioQmlContract, EditLeftRailShowsHistoryInsteadOfLibraryFolders)
{
    QFile library(QStringLiteral(RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML));
    ASSERT_TRUE(library.open(QIODevice::ReadOnly | QIODevice::Text))
        << library.errorString().toStdString();
    const auto library_source = QString::fromUtf8(library.readAll());
    EXPECT_TRUE(library_source.contains(QStringLiteral("DevelopPresetPanel")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("DevelopHistoryPanel")));
    const auto preset_panel = library_source.indexOf(QStringLiteral("DevelopPresetPanel"));
    const auto history_panel = library_source.indexOf(QStringLiteral("DevelopHistoryPanel"));
    ASSERT_GE(preset_panel, 0);
    ASSERT_GE(history_panel, 0);
    EXPECT_LT(preset_panel, history_panel);
    EXPECT_TRUE(library_source.contains(QStringLiteral("developOpen")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("visible: !root.developOpen")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("id: zoomModeBar")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("Layout.preferredWidth: 1")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qsTr(\"Fit\")")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qsTr(\"Fill\")")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("qsTr(\"1:1\")")));
    EXPECT_TRUE(
        library_source.contains(QStringLiteral("Layout.preferredWidth: ControlState.borderThin")));

    QFile history(QStringLiteral(RAVO_STUDIO_DEVELOP_HISTORY_PANEL_QML));
    ASSERT_TRUE(history.open(QIODevice::ReadOnly | QIODevice::Text))
        << history.errorString().toStdString();
    const auto history_source = QString::fromUtf8(history.readAll());
    EXPECT_TRUE(history_source.contains(QStringLiteral("recipeHistory")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("activeHistoryId")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("activeHistorySeq")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("restoreHistory")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("createSnapshot")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("createSnapshot(\"\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("renameSnapshot")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("onDoubleClicked")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("commitRename")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("historyEntries")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Original\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("\"id\": 0")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("snapshotEntries")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("id: snapshotList")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Snapshots\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("kind === \"snapshot\"")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Undo\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Redo\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Reset all\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("beforeAfter")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Copy Parameters\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("qsTr(\"Paste Parameters\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("objectName: \"copyParametersButton\"")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("objectName: \"pasteParametersButton\"")));
    EXPECT_FALSE(history_source.contains(QStringLiteral("qsTr(\"Paste Light\")")));
    EXPECT_FALSE(history_source.contains(QStringLiteral("qsTr(\"Paste Color\")")));
    EXPECT_FALSE(history_source.contains(QStringLiteral("pasteEditsSection")));
    EXPECT_LT(history_source.indexOf(QStringLiteral("qsTr(\"Snapshot\")")),
              history_source.indexOf(QStringLiteral("qsTr(\"Copy Parameters\")")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("Layout.preferredWidth: 1")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("copyParameters")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("pasteParameters")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("hasCopiedParameters")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("modifiedParameterChoices")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoRenameSnapshot")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editCopyParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editCopyParametersSelected")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPasteParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editSetNumbers")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPickWhiteBalance")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editSetWhiteBalancePick")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("previewDevelopNumbers(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("setDevelopNumbers(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("copySelectedParameters(fields)")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.editPickWhiteBalance")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("pickWhiteBalance(x, y)")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("maximumLineCount: 1")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("entryText")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("inactive")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("textColor")));
    EXPECT_TRUE(history_source.contains(QStringLiteral("disabledTextColor")));
}

TEST(StudioCommands, LockingCropAspectKeepsCurrentRatio)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.cropAspect(), QStringLiteral("free"));
    EXPECT_NEAR(presenter.cropAspectRatio(), 0.0, 1e-12);
    presenter.setCropAspect(QStringLiteral("locked"));
    EXPECT_EQ(presenter.cropAspect(), QStringLiteral("locked"));
    EXPECT_NEAR(presenter.cropAspectRatio(), 1.0, 1e-6);
    presenter.setCropAspect(QStringLiteral("free"));
    EXPECT_EQ(presenter.cropAspect(), QStringLiteral("free"));
    EXPECT_NEAR(presenter.cropAspectRatio(), 0.0, 1e-12);
}

TEST(StudioCommands, BuiltinRegistryIsCompleteAndConflictFree)
{
    EXPECT_TRUE(StudioCommandController::validateBuiltinDefinitions().isEmpty());
}

TEST(StudioCommands, CopyDebugTextRequiresSelectionOrPresetPath)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const auto photo_copy = ids.value(QStringLiteral("photoCopyInfo")).toString();
    const auto parameters_copy = ids.value(QStringLiteral("photoCopyParameters")).toString();
    const auto preset_copy = ids.value(QStringLiteral("presetCopyInfo")).toString();
    ASSERT_EQ(photo_copy, QStringLiteral("studio.photo.copy_info"));
    ASSERT_EQ(parameters_copy, QStringLiteral("studio.photo.copy_parameters"));
    ASSERT_EQ(preset_copy, QStringLiteral("studio.preset.copy_info"));

    const auto photo_action = controller.action(photo_copy);
    EXPECT_FALSE(photo_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(photo_action.value(QStringLiteral("disabledReason")).toString().isEmpty());
    const auto photo_rejected = controller.executeAction(photo_copy, QStringLiteral("control"));
    EXPECT_FALSE(photo_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(photo_rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));

    const auto parameters_action = controller.action(parameters_copy);
    EXPECT_FALSE(parameters_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(parameters_action.value(QStringLiteral("disabledReason")).toString().isEmpty());
    const auto parameters_rejected =
        controller.executeAction(parameters_copy, QStringLiteral("control"));
    EXPECT_FALSE(parameters_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(parameters_rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));

    const auto preset_rejected =
        controller.executeCommand(preset_copy, QString{}, QStringLiteral("control"));
    EXPECT_FALSE(preset_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(preset_rejected.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));
}

TEST(StudioCommands, PresetDeleteRequiresCurrentPathBoundConfirmation)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const QString request_id = ids.value(QStringLiteral("presetDelete")).toString();
    const QString confirmed_id = ids.value(QStringLiteral("presetDeleteConfirmed")).toString();
    ASSERT_EQ(request_id, QStringLiteral("studio.preset.request_delete"));
    ASSERT_EQ(confirmed_id, QStringLiteral("studio.preset.delete"));

    QString presentation_id;
    QVariant presentation_argument;
    QObject::connect(&controller, &StudioCommandController::presentationCommandRequested,
                     &controller,
                     [&](const QString &id, const QVariant &argument)
                     {
                         presentation_id = id;
                         presentation_argument = argument;
                     });
    const QVariantMap preset{{QStringLiteral("path"), QStringLiteral("/tmp/Warm.xmp")},
                             {QStringLiteral("name"), QStringLiteral("Warm")}};
    const auto requested = controller.executeCommand(request_id, preset, QStringLiteral("control"));
    ASSERT_TRUE(requested.value(QStringLiteral("accepted")).toBool());
    ASSERT_EQ(presentation_id, request_id);
    const auto presented = presentation_argument.toMap();
    const QString token = presented.value(QStringLiteral("token")).toString();
    ASSERT_FALSE(token.isEmpty());
    EXPECT_EQ(presented.value(QStringLiteral("path")).toString(),
              preset.value(QStringLiteral("path")).toString());

    const QVariantMap wrong_path{{QStringLiteral("token"), token},
                                 {QStringLiteral("path"), QStringLiteral("/tmp/Other.xmp")}};
    const auto changed =
        controller.executeCommand(confirmed_id, wrong_path, QStringLiteral("control"));
    EXPECT_FALSE(changed.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(changed.value(QStringLiteral("code")).toString(), QStringLiteral("invalid_argument"));

    controller.cancelPendingConfirmation(token);
    const QVariantMap canceled{
        {QStringLiteral("token"), token},
        {QStringLiteral("path"), preset.value(QStringLiteral("path")).toString()}};
    const auto after_cancel =
        controller.executeCommand(confirmed_id, canceled, QStringLiteral("control"));
    EXPECT_FALSE(after_cancel.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(after_cancel.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto invalid_rename = controller.executeCommand(
        ids.value(QStringLiteral("presetRename")).toString(),
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/Warm.xmp")}},
        QStringLiteral("control"));
    EXPECT_FALSE(invalid_rename.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(invalid_rename.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));
}

TEST(StudioCommands, CropToolShortcutIsRAndDoesNotRequireEditMode)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto crop = controller.ids().value(QStringLiteral("editCropTool")).toString();
    ASSERT_FALSE(crop.isEmpty());
    bool found_r = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() != crop)
            continue;
        found_r = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(), QStringLiteral("R"));
    }
    EXPECT_TRUE(found_r);
    const auto spec = controller.action(crop);
    EXPECT_FALSE(spec.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(spec.value(QStringLiteral("disabledReason")).toString(),
              QCoreApplication::translate("StudioCommands", "Open a library first."));
}

TEST(StudioLocalization, CompiledChineseCatalogTranslatesDesktopContexts)
{
    ensure_qt_core();
    QTranslator translator;
    ASSERT_TRUE(
        translator.load(QStringLiteral(RAVO_STUDIO_TRANSLATION_DIR "/RavoStudio_zh_CN.qm")));
    ASSERT_TRUE(QCoreApplication::installTranslator(&translator));

    EXPECT_EQ(QCoreApplication::translate("SettingsPage", "Language"), QStringLiteral("语言"));
    EXPECT_EQ(QCoreApplication::translate("StudioCommands", "Open a library first."),
              QStringLiteral("请先打开图库。"));
    EXPECT_EQ(QCoreApplication::translate("StudioPresenter", "Library opened."),
              QStringLiteral("图库已打开。"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "RGB Primaries"),
              QStringLiteral("RGB 原色"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Unbreak input profile"),
              QStringLiteral("修正输入配置文件"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color look-up table · D50 Lab"),
              QStringLiteral("颜色查找表 · D50 Lab"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color Correction · D50 Lab"),
              QStringLiteral("色彩校正 · D50 Lab"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Allow extended chroma"),
              QStringLiteral("允许扩展色度"));
    EXPECT_EQ(QCoreApplication::translate("DevelopHistoryPanel", "Copy Parameters"),
              QStringLiteral("复制参数"));
    EXPECT_EQ(QCoreApplication::translate("DevelopHistoryPanel", "Paste Parameters"),
              QStringLiteral("粘贴参数"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog", "Format"), QStringLiteral("格式"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog", "Continue"),
              QStringLiteral("继续"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog", "Automatic"),
              QStringLiteral("自动"));
    EXPECT_EQ(QCoreApplication::translate("ExportOptionsDialog",
                                          "Write grayscale when the image is neutral"),
              QStringLiteral("图像为中性时写入灰度"));
    EXPECT_EQ(QCoreApplication::translate("StudioCommands", "Export path must be a string."),
              QStringLiteral("导出路径必须是字符串。"));
    EXPECT_EQ(QCoreApplication::translate("StudioExport",
                                          "Export path suffix does not match the selected format"),
              QStringLiteral("文件扩展名与所选导出格式不匹配。"));
    EXPECT_EQ(QCoreApplication::translate("StudioExport", "JPEG quality must be between 5 and 100"),
              QStringLiteral("JPEG 质量必须在 5 到 100 之间"));

    QCoreApplication::removeTranslator(&translator);
}

TEST(StudioLocalization, EveryManifestCatalogActivates)
{
    ensure_qt_core();
    StudioLanguageManager manager(QStringList{QStringLiteral(RAVO_STUDIO_TRANSLATION_DIR)});
    ASSERT_TRUE(manager.initialize(QStringLiteral("en_US"))) << manager.lastError().toStdString();
    for (const auto &language : manager.supportedLanguages())
    {
        ASSERT_TRUE(manager.initialize(language))
            << language.toStdString() << ": " << manager.lastError().toStdString();
        EXPECT_EQ(manager.language(), language);
    }
}

TEST(StudioCommands, CommandPaletteUsesQtPortablePrimaryModifierPolicy)
{
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("macos")),
              QStringLiteral("Ctrl+Shift+P"));
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("windows")),
              QStringLiteral("Ctrl+Shift+P"));
    EXPECT_EQ(StudioCommandController::paletteShortcutForPlatform(QStringLiteral("linux")),
              QStringLiteral("Ctrl+Shift+P"));

#ifdef Q_OS_MACOS
    const auto native =
        QKeySequence::fromString(
            StudioCommandController::paletteShortcutForPlatform(QStringLiteral("macos")),
            QKeySequence::PortableText)
            .toString(QKeySequence::NativeText);
    EXPECT_TRUE(native.contains(QChar(0x2318))) << native.toStdString();
#endif
}

TEST(StudioCommands, FuzzySearchSupportsPrefixesSubsequencesAndMultipleTokens)
{
    const auto exact = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("show command"));
    const auto subsequence = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("scpal"));
    const auto missing = StudioCommandController::fuzzyScore(
        QStringLiteral("Show Command Palette"), QStringLiteral("View"),
        {QStringLiteral("commands"), QStringLiteral("search")},
        QStringLiteral("studio.window.show_command_palette"), QStringLiteral("export raw"));

    EXPECT_GT(exact, subsequence);
    EXPECT_GE(subsequence, 0);
    EXPECT_EQ(missing, -1);
}

TEST(StudioCommands, FuzzySearchNormalizesCaseWidthAndDiacritics)
{
    EXPECT_GE(StudioCommandController::fuzzyScore(
                  QStringLiteral("Réglages"), QStringLiteral("Window"),
                  {QStringLiteral("preferences")}, QStringLiteral("studio.window.show_settings"),
                  QStringLiteral("REGLAGES")),
              0);
}

TEST(StudioCommands, ControllerRevalidatesStateAndRejectsInvalidDispatch)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();

    const auto import_action =
        controller.action(ids.value(QStringLiteral("libraryImportFiles")).toString());
    EXPECT_FALSE(import_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(import_action.value(QStringLiteral("disabledReason")).toString().isEmpty());

    const auto invalid_path =
        controller.executeCommand(ids.value(QStringLiteral("libraryCreatePath")).toString(),
                                  QString{}, QStringLiteral("control"));
    EXPECT_FALSE(invalid_path.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(invalid_path.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto unexpected_argument =
        controller.executeCommand(ids.value(QStringLiteral("windowCommandPalette")).toString(), 1,
                                  QStringLiteral("keyboard"));
    EXPECT_FALSE(unexpected_argument.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unexpected_argument.value(QStringLiteral("code")).toString(),
              QStringLiteral("invalid_argument"));

    const auto opened = controller.executeAction(
        ids.value(QStringLiteral("windowCommandPalette")).toString(), QStringLiteral("keyboard"));
    EXPECT_TRUE(opened.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(controller.paletteOpen());

    controller.setPaletteOpen(false);
    bool found_palette_shortcut = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() !=
            ids.value(QStringLiteral("windowCommandPalette")).toString())
            continue;
        found_palette_shortcut = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(),
                  QStringLiteral("Ctrl+Shift+P"));
        EXPECT_TRUE(entry.value(QStringLiteral("enabled")).toBool());
    }
    EXPECT_TRUE(found_palette_shortcut);
}

TEST(StudioCommands, ExportWriteRevalidatesCatalogAndRejectsLegacyFilterPayload)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const auto export_write = ids.value(QStringLiteral("libraryExportWrite")).toString();
    const auto export_batch_write = ids.value(QStringLiteral("libraryExportBatchWrite")).toString();
    const auto export_open = ids.value(QStringLiteral("libraryExport")).toString();
    EXPECT_FALSE(export_batch_write.isEmpty());

    const auto open_action = controller.action(export_open);
    EXPECT_FALSE(open_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(open_action.value(QStringLiteral("disabledReason")).toString().isEmpty());

    const auto unavailable = controller.executeCommand(
        export_write,
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/out.jpg")},
                    {QStringLiteral("format"), QStringLiteral("jpeg")},
                    {QStringLiteral("options"),
                     QVariantMap{{QStringLiteral("quality"), 95},
                                 {QStringLiteral("jpegSubsampling"), QStringLiteral("auto")}}}},
        QStringLiteral("control"));
    EXPECT_FALSE(unavailable.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unavailable.value(QStringLiteral("code")).toString(), QStringLiteral("unavailable"));

    const auto legacy_filter = controller.executeCommand(
        export_write,
        QVariantMap{{QStringLiteral("path"), QStringLiteral("/tmp/out.jpg")},
                    {QStringLiteral("filter"), QStringLiteral("JPEG (*.jpg *.jpeg)")}},
        QStringLiteral("control"));
    EXPECT_FALSE(legacy_filter.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(legacy_filter.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));

    const auto unavailable_batch = controller.executeCommand(
        export_batch_write,
        QVariantMap{{QStringLiteral("directory"), QStringLiteral("/tmp")},
                    {QStringLiteral("filenameTemplate"), QStringLiteral("{stem}-{sequence}{ext}")},
                    {QStringLiteral("format"), QStringLiteral("png")},
                    {QStringLiteral("options"), QVariantMap{}}},
        QStringLiteral("control"));
    EXPECT_FALSE(unavailable_batch.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(unavailable_batch.value(QStringLiteral("code")).toString(),
              QStringLiteral("unavailable"));
}

TEST(StudioPresenterTest, ExportPresentationCatalogExposesCanonicalDefaults)
{
    ensure_qt_core();
    StudioPresenter presenter;
    const auto formats = presenter.exportFormatChoices();
    ASSERT_EQ(formats.size(), 4);
    EXPECT_EQ(formats.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(formats.at(3).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("original"));
    const auto defaults = presenter.exportDefaultOptions();
    EXPECT_EQ(defaults.value(QStringLiteral("format")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(defaults.value(QStringLiteral("quality")).toInt(), 95);
    EXPECT_EQ(defaults.value(QStringLiteral("jpegSubsampling")).toString(), QStringLiteral("auto"));
    EXPECT_EQ(defaults.value(QStringLiteral("pngBitDepth")).toString(), QStringLiteral("8"));
    EXPECT_EQ(defaults.value(QStringLiteral("pngCompression")).toInt(), 5);
    EXPECT_EQ(defaults.value(QStringLiteral("tiffSampleType")).toString(), QStringLiteral("uint8"));
    EXPECT_EQ(defaults.value(QStringLiteral("tiffCompression")).toString(),
              QStringLiteral("deflate_predictor"));
    EXPECT_EQ(defaults.value(QStringLiteral("tiffCompressionLevel")).toInt(), 6);
    EXPECT_FALSE(defaults.value(QStringLiteral("tiffGrayscaleIfNeutral")).toBool());
    EXPECT_EQ(defaults.value(QStringLiteral("tiffResolutionDpi")).toInt(), 300);
    EXPECT_EQ(defaults.value(QStringLiteral("metadataMode")).toString(), QStringLiteral("full"));
    const auto metadata_modes = presenter.exportMetadataModeChoices();
    ASSERT_EQ(metadata_modes.size(), 3);
    EXPECT_EQ(metadata_modes.at(1).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("no-location"));
    const auto bounds = presenter.exportOptionBounds();
    EXPECT_EQ(bounds.value(QStringLiteral("jpegQualityMin")).toInt(), 5);
    EXPECT_EQ(bounds.value(QStringLiteral("tiffResolutionDpiMax")).toInt(), 9600);
}

TEST(StudioPresenterTest, OutputDitherPresentationOwnsAllFrozenMethods)
{
    ensure_qt_core();
    StudioPresenter presenter;
    const auto dither = presenter.editOutputDither();
    EXPECT_FALSE(dither.value(QStringLiteral("present")).toBool());
    EXPECT_FALSE(dither.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(dither.value(QStringLiteral("methodIndex")).toInt(), 10);
    EXPECT_FALSE(dither.value(QStringLiteral("dampingVisible")).toBool());
    const auto choices = dither.value(QStringLiteral("methodChoices")).toList();
    ASSERT_EQ(choices.size(), static_cast<qsizetype>(kOutputDitherMethodCount));
    EXPECT_EQ(choices.front().toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("random"));
    EXPECT_EQ(choices.back().toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("posterize_8"));
    const auto canvas = presenter.editCanvas();
    EXPECT_FALSE(presenter.editCanvasEnabled());
    EXPECT_FALSE(canvas.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(canvas.value(QStringLiteral("colorChoices")).toList().size(), 5);
    const auto frame = presenter.editOutputFrame();
    EXPECT_FALSE(frame.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(frame.value(QStringLiteral("orientationChoices")).toList().size(), 3);
    EXPECT_EQ(frame.value(QStringLiteral("basisChoices")).toList().size(), 5);
    const auto watermark = presenter.editWatermark();
    EXPECT_FALSE(watermark.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(watermark.value(QStringLiteral("text")).toString(), QStringLiteral("RAVO"));
    EXPECT_EQ(watermark.value(QStringLiteral("alignmentChoices")).toList().size(), 9);
    const auto zones = presenter.editColorZones();
    EXPECT_FALSE(zones.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(zones.value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(zones.value(QStringLiteral("selectByChoices")).toList().size(), 3);
    EXPECT_EQ(zones.value(QStringLiteral("interpolationChoices")).toList().size(), 3);
    const auto monochrome = presenter.editMonochromeFilter();
    EXPECT_FALSE(monochrome.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(monochrome.value(QStringLiteral("size")).toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(monochrome.value(QStringLiteral("mix")).toDouble(), 1.0);
    const auto split = presenter.editSplitToning();
    EXPECT_FALSE(split.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(split.value(QStringLiteral("shadowSaturation")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(split.value(QStringLiteral("compress")).toDouble(), 33.0);
}

TEST(StudioPresenterTest, CatalogRecoveryCommandsBackupVerifyRestoreAndRebuild)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("recovery-photo.png"));
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(70, 120, 180));
    ASSERT_TRUE(image.save(photo, "PNG"));
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    const QString backup = directory.filePath(QStringLiteral("library.ravobackup"));
    const QString restored = directory.filePath(QStringLiteral("restored.sqlite"));
    const QString scheduled_backups = directory.filePath(QStringLiteral("scheduled-backups"));
    ASSERT_TRUE(QDir().mkdir(scheduled_backups));

    StudioPresenter presenter;
    StudioCommandController controller(presenter);
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

    const auto ids = controller.ids();
    EXPECT_EQ(ids.value(QStringLiteral("libraryRecoveryStatus")).toString(),
              QStringLiteral("studio.library.recovery_status"));
    EXPECT_EQ(ids.value(QStringLiteral("libraryBackupRestorePaths")).toString(),
              QStringLiteral("studio.library.backup_restore_paths"));
    EXPECT_EQ(ids.value(QStringLiteral("libraryPreviewRebuildSelected")).toString(),
              QStringLiteral("studio.library.preview_rebuild_selected"));
    EXPECT_EQ(ids.value(QStringLiteral("libraryBackupSchedulePath")).toString(),
              QStringLiteral("studio.library.backup_schedule_path"));

    auto status =
        controller.executeCommand(ids.value(QStringLiteral("libraryRecoveryStatus")).toString(), {},
                                  QStringLiteral("control"));
    ASSERT_TRUE(status.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.recoveryPendingCount(), 0);

    auto created =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupCreatePath")).toString(),
                                  backup, QStringLiteral("control"));
    ASSERT_TRUE(created.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(QFileInfo::exists(backup + QStringLiteral("/manifest.json")));

    auto verified =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupVerifyPath")).toString(),
                                  backup, QStringLiteral("control"));
    ASSERT_TRUE(verified.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty());
    const QVariantMap schedule{{QStringLiteral("directory"), scheduled_backups},
                               {QStringLiteral("intervalMinutes"), 15},
                               {QStringLiteral("retentionCount"), 2}};
    auto scheduled =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupSchedulePath")).toString(),
                                  schedule, QStringLiteral("control"));
    ASSERT_TRUE(scheduled.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    auto schedule_status = presenter.backupScheduleStatus();
    EXPECT_TRUE(schedule_status.value(QStringLiteral("loaded")).toBool());
    EXPECT_TRUE(schedule_status.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(schedule_status.value(QStringLiteral("intervalMinutes")).typeId(),
              QMetaType::LongLong);
    EXPECT_EQ(schedule_status.value(QStringLiteral("lastSuccessUnixMs")).typeId(),
              QMetaType::LongLong);
    EXPECT_EQ(schedule_status.value(QStringLiteral("nextRunUnixMs")).typeId(), QMetaType::LongLong);
    EXPECT_EQ(schedule_status.value(QStringLiteral("retentionCount")).toInt(), 2);

    auto run_schedule =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupScheduleRun")).toString(),
                                  {}, QStringLiteral("control"));
    ASSERT_TRUE(run_schedule.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    schedule_status = presenter.backupScheduleStatus();
    EXPECT_GT(schedule_status.value(QStringLiteral("lastSuccessUnixMs")).toLongLong(), 0);
    EXPECT_GT(schedule_status.value(QStringLiteral("lastBackupBytes")).toULongLong(), 0U);

    auto disable_schedule = controller.executeCommand(
        ids.value(QStringLiteral("libraryBackupScheduleDisable")).toString(), {},
        QStringLiteral("control"));
    ASSERT_TRUE(disable_schedule.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    EXPECT_FALSE(presenter.backupScheduleStatus().value(QStringLiteral("enabled")).toBool());

    const QVariantMap restore_paths{{QStringLiteral("backup"), backup},
                                    {QStringLiteral("catalog"), restored}};
    auto restored_command =
        controller.executeCommand(ids.value(QStringLiteral("libraryBackupRestorePaths")).toString(),
                                  restore_paths, QStringLiteral("control"));
    ASSERT_TRUE(restored_command.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(QFileInfo::exists(restored));
    EXPECT_TRUE(QFileInfo(restored + QStringLiteral(".ravo/sidecars")).isDir());

    auto rebuilt = controller.executeCommand(
        ids.value(QStringLiteral("libraryPreviewRebuildSelected")).toString(), {},
        QStringLiteral("control"));
    ASSERT_TRUE(rebuilt.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.errorText().isEmpty());
    const auto cancel_action =
        controller.action(ids.value(QStringLiteral("libraryCancelOperation")).toString());
    EXPECT_FALSE(cancel_action.value(QStringLiteral("enabled")).toBool());
}

TEST(StudioPresenterTest, MissingFolderRelinkUsesStableIdentityAndCommandOwnedDialog)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString original = directory.filePath(QStringLiteral("original-root"));
    const QString replacement = directory.filePath(QStringLiteral("replacement-root"));
    ASSERT_TRUE(QDir().mkdir(original));
    const QString photo = QDir(original).filePath(QStringLiteral("photo.png"));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(60, 110, 160));
    ASSERT_TRUE(image.save(photo, "PNG"));
    QFile source(photo);
    ASSERT_TRUE(source.open(QIODevice::ReadOnly));
    const auto source_hash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256);
    source.close();
    const QString catalog = directory.filePath(QStringLiteral("library.sqlite"));
    QString folder_id;
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return presenter.visibleCount() == 1 && !presenter.importWorkActive() &&
                       !presenter.busy();
            }));
        for (int row = 0; row < presenter.folders()->rowCount(); ++row)
        {
            const auto index = presenter.folders()->index(row, 0);
            if (presenter.folders()->data(index, FolderListModel::DisplayNameRole).toString() ==
                QStringLiteral("original-root"))
                folder_id =
                    presenter.folders()->data(index, FolderListModel::FolderIdRole).toString();
        }
        ASSERT_FALSE(folder_id.isEmpty());
    }
    ASSERT_TRUE(QDir().rename(original, replacement));

    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    presenter.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    bool missing = false;
    for (int row = 0; row < presenter.folders()->rowCount(); ++row)
    {
        const auto index = presenter.folders()->index(row, 0);
        if (presenter.folders()->data(index, FolderListModel::FolderIdRole).toString() == folder_id)
            missing = presenter.folders()->data(index, FolderListModel::MissingRole).toBool();
    }
    EXPECT_TRUE(missing);
    const auto ids = controller.ids();
    EXPECT_EQ(ids.value(QStringLiteral("libraryFolderRelinkPath")).toString(),
              QStringLiteral("studio.library.folder_relink_path"));
    const QVariantMap relink{{QStringLiteral("folderId"), folder_id},
                             {QStringLiteral("directory"), replacement}};
    const auto command =
        controller.executeCommand(ids.value(QStringLiteral("libraryFolderRelinkPath")).toString(),
                                  relink, QStringLiteral("control"));
    ASSERT_TRUE(command.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(
        wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); }, 30000))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(wait_until([&] { return presenter.visibleCount() == 1; }));
    bool found = false;
    for (int row = 0; row < presenter.folders()->rowCount(); ++row)
    {
        const auto index = presenter.folders()->index(row, 0);
        if (presenter.folders()->data(index, FolderListModel::FolderIdRole).toString() != folder_id)
            continue;
        found = true;
        EXPECT_FALSE(presenter.folders()->data(index, FolderListModel::MissingRole).toBool());
        EXPECT_EQ(presenter.folders()->data(index, FolderListModel::DisplayNameRole).toString(),
                  QStringLiteral("replacement-root"));
    }
    EXPECT_TRUE(found);
    QFile moved(QDir(replacement).filePath(QStringLiteral("photo.png")));
    ASSERT_TRUE(moved.open(QIODevice::ReadOnly));
    EXPECT_EQ(QCryptographicHash::hash(moved.readAll(), QCryptographicHash::Sha256), source_hash);
}

TEST(StudioPresenterTest, ImportCancellationStopsUndispatchedItemsAtItemBoundary)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QStringList photos;
    std::vector<QByteArray> hashes;
    for (int index = 0; index < 3; ++index)
    {
        const auto path = directory.filePath(QStringLiteral("import-%1.png").arg(index));
        QImage image(64, 48, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40 + index * 30, 90, 150));
        ASSERT_TRUE(image.save(path, "PNG"));
        photos.push_back(path);
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        hashes.push_back(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256));
    }
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    bool cancelled = false;
    QObject::connect(&presenter, &StudioPresenter::libraryWorkChanged, &presenter,
                     [&]
                     {
                         if (!cancelled && presenter.importWorkActive() &&
                             presenter.importWorkCompleted() == 1)
                         {
                             cancelled = true;
                             presenter.cancelCatalogOperation();
                         }
                     });
    presenter.importFilePaths(photos);
    ASSERT_TRUE(wait_until([&] { return cancelled && !presenter.importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.visibleCount(), 1);
    EXPECT_TRUE(presenter.statusText().contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    for (int index = 0; index < photos.size(); ++index)
    {
        QFile file(photos[index]);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256),
                  hashes[static_cast<std::size_t>(index)]);
    }
}

TEST(StudioQmlContract, ExportOptionsDialogExposesEveryFormatWithoutCodecParsing)
{
    QFile dialog(QStringLiteral(RAVO_STUDIO_EXPORT_OPTIONS_QML));
    ASSERT_TRUE(dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << dialog.errorString().toStdString();
    const auto source = QString::fromUtf8(dialog.readAll());

    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"ExportOptionsDialog\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Format\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Filename template\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"exportFilenameTemplate\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.selectedCount > 1")));
    EXPECT_TRUE(source.contains(QStringLiteral("{stem}-{sequence}{ext}")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Quality\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Subsampling\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Bit depth\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Compression\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Sample type\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Compression level\")")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("qsTr(\"Write grayscale when the image is neutral\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Resolution (dpi)\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Metadata privacy\")")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("qsTr(\"Original copy writes the exact source bytes")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Cancel\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Continue\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.exportFormatChoices()")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.exportDefaultOptions()")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.exportOptionBounds()")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetFromPresenter()")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportAccepted")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportCanceled")));
    EXPECT_TRUE(source.contains(QStringLiteral("Accessible.name")));
    EXPECT_TRUE(source.contains(QStringLiteral("Keys.onEscapePressed")));
    EXPECT_TRUE(source.contains(QStringLiteral("tiffCompressionId !== \"none\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"jpegQuality\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"jpegSubsampling\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"pngBitDepth\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"pngCompression\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffSampleType\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffCompression\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffCompressionLevel\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffGrayscaleIfNeutral\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"tiffResolutionDpi\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"metadataMode\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"metadataMode\": metadataModeId")));
    EXPECT_FALSE(source.contains(QStringLiteral("parse_export")));
    EXPECT_FALSE(source.contains(QStringLiteral("export_format_from_ui")));
    EXPECT_FALSE(source.contains(QStringLiteral("JpegExportOptions")));
    EXPECT_FALSE(source.contains(QStringLiteral("toLowerCase()")));
    EXPECT_FALSE(source.contains(QStringLiteral("Math.round")));
    EXPECT_FALSE(source.contains(QStringLiteral("property double jpegQuality: 95")));
    EXPECT_FALSE(source.contains(QStringLiteral("property double tiffResolutionDpi: 300")));
}

TEST(StudioQmlContract, CropOverlayShowsWhenCropToolActivates)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    const auto overlay = source.indexOf(QStringLiteral("CropOverlay"));
    ASSERT_GE(overlay, 0);
    const auto visible = source.indexOf(QStringLiteral("visible:"), overlay);
    ASSERT_GE(visible, 0);
    const auto visible_line =
        source.mid(visible, source.indexOf(QLatin1Char('\n'), visible) - visible);
    EXPECT_TRUE(visible_line.contains(QStringLiteral("cropToolActive")));
    EXPECT_TRUE(visible_line.contains(QStringLiteral("studio.previewUrl")));
    EXPECT_TRUE(visible_line.contains(QStringLiteral("photoPlane.width")));
    EXPECT_FALSE(visible_line.contains(QStringLiteral("cropGuideReady")));
    EXPECT_TRUE(source.contains(QStringLiteral("rotation: 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("straighten: studio.editStraighten")));
    EXPECT_FALSE(source.contains(
        QStringLiteral("cropToolActive && studio.cropGuideReady ? studio.editStraighten : 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("photoItem: photoPlane")));
    EXPECT_TRUE(source.contains(QStringLiteral("sourceWidth: studio.selectedWorkingWidth")));
    EXPECT_TRUE(source.contains(QStringLiteral("sourceHeight: studio.selectedWorkingHeight")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("minShortEdgePixels: studio.cropMinShortEdgePixels")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("minShortEdgeFraction: studio.cropMinShortEdgeFraction")));
    EXPECT_TRUE(source.contains(QStringLiteral("onTapped: window.showPhotoMenu()")));
    EXPECT_FALSE(source.contains(QStringLiteral("onClicked: window.showPhotoMenu()")));
}

TEST(StudioQmlContract, PhotoNavigationPansClampsAndResetsOnlyOnOwnedStateChanges)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("property string viewportAssetId")));
    EXPECT_TRUE(source.contains(QStringLiteral("function centerPhotoViewport()")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("window.viewportAssetId !== studio.selectedAssetId")));
    EXPECT_TRUE(source.contains(QStringLiteral("function onZoomChanged()")));
    EXPECT_TRUE(source.contains(QStringLiteral("function onBrowseModeChanged()")));
    EXPECT_TRUE(source.contains(QStringLiteral("scroller.contentX = maxX / 2")));
    EXPECT_TRUE(source.contains(QStringLiteral("scroller.contentY = maxY / 2")));
    EXPECT_TRUE(source.contains(QStringLiteral("boundsBehavior: Flickable.StopAtBounds")));
    EXPECT_TRUE(source.contains(QStringLiteral("function seekNavigatorViewport(nx, ny)")));
    EXPECT_TRUE(source.contains(QStringLiteral("Math.min(maxX")));
    EXPECT_TRUE(source.contains(QStringLiteral("Math.min(maxY")));
    EXPECT_TRUE(source.contains(QStringLiteral("WheelHandler")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.viewAdjustZoom")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.viewToggleActualSize")));
    EXPECT_TRUE(source.contains(QStringLiteral("photoInspectEnabled")));
    EXPECT_TRUE(source.contains(QStringLiteral("cropToolActive")));
    EXPECT_TRUE(source.contains(QStringLiteral("function togglePhotoInspectZoom(stagePos)")));
    EXPECT_TRUE(source.contains(QStringLiteral("function applyPhotoViewportAfterZoom()")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.previewViewportWidth")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.previewViewportHeight")));
    EXPECT_TRUE(source.contains(QStringLiteral("previewPlaceholderReady")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: previewPlaceholderImage")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.selectedThumbnailUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("source: studio.previewUrl")));
    EXPECT_FALSE(source.contains(QStringLiteral("previewImage.implicitWidth")));
    EXPECT_FALSE(source.contains(QStringLiteral("previewImage.implicitHeight")));
    EXPECT_TRUE(source.contains(QStringLiteral("function beginInspectZoomAnimation()")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: inspectZoomAnim")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectStageLockW")));
    EXPECT_TRUE(source.contains(QStringLiteral("inspectAnimScale")));
    EXPECT_TRUE(source.contains(QStringLiteral("transform: Scale")));
    EXPECT_TRUE(source.contains(QStringLiteral(
        "cursorShape: studio.whiteBalancePickActive ? Qt.CrossCursor : Qt.BlankCursor")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: magnifierCursor")));
    EXPECT_TRUE(source.contains(QStringLiteral("onDoubleTapped")));
    EXPECT_TRUE(source.contains(QStringLiteral("openGallery(\"grid\")")));
}

TEST(StudioQmlContract, MainExportUsesTwoStepExplicitFormatPayload)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("ExportOptionsDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("pendingExportFormat")));
    EXPECT_TRUE(source.contains(QStringLiteral("pendingExportOptions")));
    EXPECT_TRUE(source.contains(QStringLiteral("pendingExportFilenameTemplate")));
    EXPECT_TRUE(source.contains(QStringLiteral("libraryExportBatchWrite")));
    EXPECT_TRUE(source.contains(QStringLiteral("Select Batch Export Folder")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"directory\": folderPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"filenameTemplate\": filenameTemplate")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"format\": format")));
    EXPECT_TRUE(source.contains(QStringLiteral("\"options\": options")));
    EXPECT_TRUE(source.contains(QStringLiteral("onFileRejected: window.clearPendingExport()")));
    EXPECT_TRUE(source.contains(QStringLiteral("exportOptionsDialog.visible")));
    EXPECT_FALSE(source.contains(QStringLiteral("\"filter\": selectedFilter")));
    EXPECT_FALSE(source.contains(QStringLiteral("JPEG (*.jpg *.jpeg)\", \"PNG (*.png)\"")));
}

TEST(StudioQmlContract, CatalogRecoveryUsesCommandOwnedDialogsProgressAndCancellation)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupCreateDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupVerifyDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupRestoreSourceDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupRestoreDestinationDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupCreatePath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupVerifyPath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupRestorePaths")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupScheduleDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: backupScheduleFolderDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryBackupSchedulePath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("id: folderRelinkDialog")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("ids.libraryFolderRelinkPath")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("\"backup\": backup")));
    EXPECT_TRUE(main_source.contains(QStringLiteral("\"catalog\": filePath")));

    QFile library(QStringLiteral(RAVO_STUDIO_LIBRARY_SIDE_PANEL_QML));
    ASSERT_TRUE(library.open(QIODevice::ReadOnly | QIODevice::Text))
        << library.errorString().toStdString();
    const auto library_source = QString::fromUtf8(library.readAll());
    EXPECT_TRUE(library_source.contains(QStringLiteral("catalogOperationActive")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("catalogOperationStage")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("catalogOperationCompleted")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("libraryCancelOperation")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("backupScheduleStatus")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("Last verified: %1 · %2")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("libraryFolderRelink")));
    EXPECT_TRUE(library_source.contains(QStringLiteral("missing — click to locate")));

    QFile schedule(QStringLiteral(RAVO_STUDIO_BACKUP_SCHEDULE_QML));
    ASSERT_TRUE(schedule.open(QIODevice::ReadOnly | QIODevice::Text))
        << schedule.errorString().toStdString();
    const auto schedule_source = QString::fromUtf8(schedule.readAll());
    EXPECT_TRUE(schedule_source.contains(QStringLiteral("backupScheduleInterval")));
    EXPECT_TRUE(schedule_source.contains(QStringLiteral("backupScheduleRetention")));
    EXPECT_TRUE(schedule_source.contains(QStringLiteral("Choose Folder…")));
}

TEST(StudioQmlContract, GalleryRequestsSparsePagesFromVisibleDelegates)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("studio.libraryHasMore")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.loadNextLibraryPage()")));
    EXPECT_TRUE(source.contains(QStringLiteral("studio.ensureLibraryRow(tile.index)")));
    EXPECT_TRUE(source.contains(QStringLiteral("onAssetIdChanged")));
    EXPECT_TRUE(source.contains(QStringLiteral("cacheBuffer: cellHeight")));
    EXPECT_FALSE(source.contains(QStringLiteral("cacheBuffer: cellHeight * 8")));
}

TEST(StudioQmlContract, LibraryFilterBarUsesCanonicalQueryCommands)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto main_source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(main_source.contains(QStringLiteral("LibraryFilterBar")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("qsTr(\"Search photos\")")));
    EXPECT_FALSE(main_source.contains(QStringLiteral("qsTr(\"Clear filters\")")));

    QFile bar(QStringLiteral(RAVO_STUDIO_LIBRARY_FILTER_BAR_QML));
    ASSERT_TRUE(bar.open(QIODevice::ReadOnly | QIODevice::Text)) << bar.errorString().toStdString();
    const auto source = QString::fromUtf8(bar.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Search photos\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("setTextFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setMediaFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("setEditFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.mediaFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("presenter.editFilter")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Capture time\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"File size\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("setRatingExact")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Unrated\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Add filter\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("function extraOpen(id)")));
    EXPECT_TRUE(source.contains(QStringLiteral("function addExtra(id)")));
    EXPECT_TRUE(source.contains(QStringLiteral("function removeExtra(id)")));
    EXPECT_TRUE(source.contains(QStringLiteral("qrc:/GeoControls/icons/Plus.svg")));
    EXPECT_TRUE(source.contains(QStringLiteral("qrc:/GeoControls/icons/Close.svg")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetTextFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetMediaFilter")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.librarySetEditFilter")));
}

TEST(StudioQmlContract, RecipeStyleUsesExplicitSaveAndApplyFileCommands)
{
    QFile main(QStringLiteral(RAVO_STUDIO_MAIN_QML));
    ASSERT_TRUE(main.open(QIODevice::ReadOnly | QIODevice::Text))
        << main.errorString().toStdString();
    const auto source = QString::fromUtf8(main.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("id: styleSaveDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: styleApplyDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("*.rstyle.json")));
    EXPECT_TRUE(source.contains(QStringLiteral("*.xmp")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.styleSavePath")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.styleApplyPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("id === ids.styleSave")));
    EXPECT_TRUE(source.contains(QStringLiteral("id === ids.styleApply")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: presetImportDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImport")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImportPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: parameterSelectionDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetSave")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetSaveSelected")));
    EXPECT_TRUE(source.contains(QStringLiteral("id === ids.editCopyParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.editCopyParametersSelected")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: presetRenameDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetRenamePath")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: presetDeleteDialog")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetDeleteConfirmed")));
    EXPECT_TRUE(source.contains(QStringLiteral("cancelPendingConfirmation(token)")));
    EXPECT_FALSE(source.contains(QStringLiteral("darktable_style")));
}

TEST(StudioQmlContract, DevelopPresetPanelSitsAboveHistoryAndImportsThroughCommands)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_DEVELOP_PRESET_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Presets\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetImportButton\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetSaveButton\"")));
    EXPECT_LT(source.indexOf(QStringLiteral("objectName: \"presetImportButton\"")),
              source.indexOf(QStringLiteral("objectName: \"presetSaveButton\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"presetList\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("editPresets")));
    EXPECT_TRUE(source.contains(QStringLiteral("modifiedParameterChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetImport")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetSave")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetApplyPath")));
    EXPECT_TRUE(source.contains(QStringLiteral("acceptedButtons: Qt.LeftButton | Qt.RightButton")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetCopyInfo")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetRename")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.presetDelete")));
    EXPECT_TRUE(source.contains(QStringLiteral("Chrome.StudioContextMenu")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Copy Preset Info\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Rename…\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Delete…\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("ravo.debug.preset")));
    EXPECT_FALSE(source.contains(QStringLiteral("OpenCL")));

    QFile rename_dialog(QStringLiteral(RAVO_STUDIO_PRESET_RENAME_DIALOG_QML));
    ASSERT_TRUE(rename_dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << rename_dialog.errorString().toStdString();
    const auto rename_source = QString::fromUtf8(rename_dialog.readAll());
    EXPECT_TRUE(rename_source.contains(QStringLiteral("DialogShell")));
    EXPECT_TRUE(rename_source.contains(QStringLiteral("objectName: \"presetRenameField\"")));
    EXPECT_TRUE(rename_source.contains(QStringLiteral("signal renameAccepted")));

    QFile save_dialog(QStringLiteral(RAVO_STUDIO_PRESET_SAVE_DIALOG_QML));
    ASSERT_TRUE(save_dialog.open(QIODevice::ReadOnly | QIODevice::Text))
        << save_dialog.errorString().toStdString();
    const auto save_source = QString::fromUtf8(save_dialog.readAll());
    EXPECT_TRUE(save_source.contains(QStringLiteral("objectName: \"ParameterSelectionDialog\"")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("objectName: \"presetParameterList\"")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("signal saveAccepted")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("signal copyAccepted")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("openForCopy")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("qsTr(\"Copy Parameters\")")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("\"included\": false")));
    EXPECT_TRUE(save_source.contains(QStringLiteral("selectedCount > 0")));
}

TEST(StudioQmlContract, PhotoContextMenuCopiesPresenterOwnedDebugText)
{
    QFile menu(QStringLiteral(RAVO_STUDIO_PHOTO_CONTEXT_MENU_QML));
    ASSERT_TRUE(menu.open(QIODevice::ReadOnly | QIODevice::Text))
        << menu.errorString().toStdString();
    const auto source = QString::fromUtf8(menu.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("copyParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("pasteParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("copyPhotoInfo")));
    EXPECT_TRUE(source.contains(QStringLiteral("copyPhotoParameters")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"viewPhotoMenuItem\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"editPhotoMenuItem\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("action: root.commands.loupe")));
    EXPECT_TRUE(source.contains(QStringLiteral("action: root.commands.develop")));
    EXPECT_TRUE(source.contains(QStringLiteral("StudioContextMenu")));
    EXPECT_TRUE(source.contains(QStringLiteral("StudioContextMenuItem")));
    EXPECT_LT(source.indexOf(QStringLiteral("copyParameters")),
              source.indexOf(QStringLiteral("copyPhotoInfo")));
    EXPECT_LT(source.indexOf(QStringLiteral("copyPhotoInfo")),
              source.indexOf(QStringLiteral("copyPhotoParameters")));
    EXPECT_FALSE(source.contains(QStringLiteral("ravo.debug.photo")));
    EXPECT_FALSE(source.contains(QStringLiteral("ravo.debug.parameters")));

    QFile shared_item(QStringLiteral(RAVO_STUDIO_CONTEXT_MENU_ITEM_QML));
    ASSERT_TRUE(shared_item.open(QIODevice::ReadOnly | QIODevice::Text))
        << shared_item.errorString().toStdString();
    const auto shared_source = QString::fromUtf8(shared_item.readAll());
    EXPECT_TRUE(shared_source.contains(QStringLiteral("id: checkmark")));
    EXPECT_TRUE(shared_source.contains(QStringLiteral("root.checkable && root.checked")));
    EXPECT_TRUE(shared_source.contains(QStringLiteral("indicator: Item")));

    QFile actions(QStringLiteral(RAVO_STUDIO_ACTIONS_QML));
    ASSERT_TRUE(actions.open(QIODevice::ReadOnly | QIODevice::Text))
        << actions.errorString().toStdString();
    const auto action_source = QString::fromUtf8(actions.readAll());
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoCopyInfo")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("copyPhotoInfo")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("ids.photoCopyParameters")));
    EXPECT_TRUE(action_source.contains(QStringLiteral("copyPhotoParameters")));
}

TEST(StudioQmlContract, ScopePanelExposesFiveEngineOwnedModesWithoutPixelMath)
{
    QFile panel(QStringLiteral(RAVO_STUDIO_SCOPE_PANEL_QML));
    ASSERT_TRUE(panel.open(QIODevice::ReadOnly | QIODevice::Text))
        << panel.errorString().toStdString();
    const auto source = QString::fromUtf8(panel.readAll());
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Histogram\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Waveform\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Parade\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Vectorscope\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Split\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeWaveformUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeParadeUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeVectorscopeUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("scopeSplitUrl")));
    EXPECT_TRUE(source.contains(QStringLiteral("ids.viewSetScopeMode")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: scopeModeButton")));
    EXPECT_TRUE(source.contains(QStringLiteral("anchors.left: plot.left")));
    EXPECT_TRUE(source.contains(QStringLiteral("anchors.top: plot.top")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: scopeModeMenu")));
    EXPECT_TRUE(source.contains(QStringLiteral("modeId: \"histogram\"")));
    EXPECT_FALSE(source.contains(QStringLiteral("SegmentedControl")));
    EXPECT_FALSE(source.contains(QStringLiteral("srgb_to_linear")));
    EXPECT_FALSE(source.contains(QStringLiteral("rgb_to_d50_uv")));
}

} // namespace
} // namespace ravo
