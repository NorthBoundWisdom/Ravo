#include <gtest/gtest.h>
#include <QDir>
#include <QTemporaryDir>
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;

TEST(StudioBackupSettingsTest, SettingsSaveRunDisableAndReopenUseCatalogPolicy)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto catalog = directory.filePath("library.sqlite");
    const auto backups = directory.filePath("backups");
    ASSERT_TRUE(QDir().mkdir(backups));
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    auto *settings = commands.backupSettings();
    EXPECT_FALSE(settings->canApply());
    presenter.createCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until([&] { return settings->loaded() && !presenter.busy(); }));
    commands.setSettingsOpen(true);
    EXPECT_TRUE(commands.selectSettingsSection("backup"));
    EXPECT_FALSE(commands.selectSettingsSection("unknown"));
    EXPECT_FALSE(commands.action(commands.ids().value("libraryImportFiles").toString())
                     .value("enabled")
                     .toBool());
    ASSERT_TRUE(settings->canEdit());
    settings->setDirectory(backups);
    settings->setIntervalMinutes(37);
    settings->setRetentionCount(3);
    settings->setEnabled(true);
    ASSERT_TRUE(settings->apply());
    ASSERT_TRUE(wait_until([&] { return !settings->saving() && !presenter.busy(); }));
    EXPECT_FALSE(settings->dirty());
    EXPECT_EQ(presenter.backupScheduleStatus().value("intervalMinutes").toInt(), 37);
    ASSERT_TRUE(settings->runNow());
    EXPECT_FALSE(settings->canApply());
    EXPECT_FALSE(settings->canEdit());
    ASSERT_TRUE(wait_until([&] { return !presenter.catalogOperationActive() && !presenter.busy(); },
                           30000));
    EXPECT_GT(presenter.backupScheduleStatus().value("lastBackupBytes").toULongLong(), 0U);
    EXPECT_FALSE(settings->dirty());
    settings->setEnabled(false);
    ASSERT_TRUE(settings->apply());
    ASSERT_TRUE(wait_until([&] { return !settings->saving() && !presenter.busy(); }));
    const auto result =
        run_cli_process({"catalog", "backup-policy", "--catalog", catalog, "--json"});
    EXPECT_EQ(result.exit_code, 0) << result.standard_error.toStdString();
    auto policy = cli_data(result.standard_output);
    ASSERT_TRUE(policy) << policy.error().message;
    EXPECT_EQ(policy.value().find("interval_minutes")->number_if()->text, "37");
    EXPECT_FALSE(*policy.value().find("enabled")->boolean_if());
    StudioPresenter reopened;
    reopened.openCatalogFromPath(catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return reopened.catalogOpen() && !reopened.busy() &&
                   reopened.backupScheduleStatus().value("loaded").toBool();
        }));
    EXPECT_FALSE(reopened.backupScheduleStatus().value("enabled").toBool());
    EXPECT_EQ(reopened.backupScheduleStatus().value("intervalMinutes").toInt(), 37);
    EXPECT_EQ(reopened.backupScheduleStatus().value("retentionCount").toInt(), 3);
}

TEST(StudioBackupSettingsTest, DisabledConfigurationAndConflictsAreObservableThroughCli)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ScopedEnvironmentVariable runtime("RAVO_LIVE_CONTROL_DIR",
                                      directory.filePath("control").toUtf8());
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    auto live = StudioLiveSessionController::create(presenter, commands);
    ASSERT_TRUE(live) << live.error().message;
    const auto session = QString::fromStdString(live.value()->descriptor().session_id);
    presenter.createCatalogFromPath(directory.filePath("library.sqlite"));
    auto *settings = commands.backupSettings();
    ASSERT_TRUE(wait_until([&] { return settings->loaded() && !presenter.busy(); }));
    commands.setSettingsOpen(true);
    commands.selectSettingsSection("backup");
    settings->setIntervalMinutes(43);
    ASSERT_TRUE(settings->apply());
    ASSERT_TRUE(wait_until([&] { return !settings->saving() && !presenter.busy(); }));
    EXPECT_FALSE(settings->dirty());
    EXPECT_EQ(presenter.backupScheduleStatus().value("intervalMinutes").toInt(), 43);
    settings->setIntervalMinutes(57);
    presenter.configureBackupSchedule({}, 61, 4, false);
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    EXPECT_TRUE(settings->dirty());
    EXPECT_FALSE(settings->canApply());
    EXPECT_EQ(settings->intervalMinutes(), 57);
    const auto state_result =
        run_cli_process({"studio", "state", "--session-id", session, "--json"});
    ASSERT_EQ(state_result.exit_code, 0) << state_result.standard_error.toStdString();
    auto state = cli_data(state_result.standard_output);
    ASSERT_TRUE(state) << state.error().message;
    const auto *snapshot = state.value().find("settings");
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(*snapshot->find("schema")->string_if(), "ravo.studio.settings/v1");
    EXPECT_EQ(*snapshot->find("section")->string_if(), "backup");
    EXPECT_TRUE(*snapshot->find("backup")->find("conflict")->boolean_if());
    EXPECT_EQ(snapshot->find("backup")->find("interval_minutes")->number_if()->text, "57");
    EXPECT_EQ(state_result.standard_output.indexOf("api_key"), -1);
    settings->reload();
    EXPECT_EQ(settings->intervalMinutes(), 61);
    EXPECT_EQ(settings->retentionCount(), 4);
    EXPECT_FALSE(settings->dirty());
}

TEST(StudioBackupSettingsTest, FailedSavePreservesDraftAndLateFolderSelectionRejects)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    auto *settings = commands.backupSettings();
    presenter.createCatalogFromPath(directory.filePath("first.sqlite"));
    ASSERT_TRUE(wait_until([&] { return settings->loaded() && !presenter.busy(); }));
    commands.setSettingsOpen(true);
    settings->setEnabled(true);
    settings->setDirectory(directory.filePath("missing"));
    settings->setIntervalMinutes(47);
    ASSERT_TRUE(settings->apply());
    ASSERT_TRUE(wait_until([&] { return !settings->saving() && !presenter.busy(); }));
    EXPECT_TRUE(settings->dirty());
    EXPECT_FALSE(settings->lastError().isEmpty());
    EXPECT_EQ(settings->intervalMinutes(), 47);
    EXPECT_FALSE(presenter.backupScheduleStatus().value("enabled").toBool());
    settings->reload();
    settings->setIntervalMinutes(1);
    EXPECT_FALSE(settings->apply());
    EXPECT_TRUE(settings->dirty());
    EXPECT_FALSE(settings->lastError().isEmpty());
    settings->reload();
    ASSERT_TRUE(settings->beginDirectorySelection());
    presenter.createCatalogFromPath(directory.filePath("second.sqlite"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogPath() == directory.filePath("second.sqlite") &&
                   !presenter.busy();
        }));
    EXPECT_FALSE(settings->acceptDirectory(directory.path()));
    EXPECT_TRUE(settings->directory().isEmpty());
    EXPECT_FALSE(settings->dirty());
    const QVariantMap invalid{{"directory", directory.path()},
                              {"intervalMinutes", 15},
                              {"retentionCount", 2},
                              {"enabled", "true"}};
    EXPECT_FALSE(
        commands
            .executeCommand(commands.ids().value("libraryBackupSchedulePath").toString(), invalid)
            .value("accepted")
            .toBool());
}
} // namespace ravo
