#include <gtest/gtest.h>
#include <QColorSpace>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>

#include "ravo/desktop/studio_presenter.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/recipe/recipe.h"
#include "ravo/foundation/log.h"
#include "studio_test_support.h"

namespace ravo
{
using namespace studio_test_support;

TEST(LocalAdjustmentWorkspaceTest, DrawAdjustDoneAndReopenKeepGlobalAndLocalSeparate)
{
    ensure_qt_core();
    init_logging("ravo-local-adjustment-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto photo = directory.filePath(QStringLiteral("photo.png"));
    const auto catalog = directory.filePath(QStringLiteral("library.sqlite"));
    QImage image(128, 96, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 110, 150));
    ASSERT_TRUE(image.save(photo, "PNG"));
    QString asset_id, mask_id;
    {
        StudioPresenter presenter;
        presenter.createCatalogFromPath(catalog);
        ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
        presenter.importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                       !presenter.busy();
            }));
        presenter.setBrowseMode(QStringLiteral("develop"));
        ASSERT_TRUE(wait_until(
            [&] { return !presenter.previewLoading() && !presenter.previewUrl().isEmpty(); }));
        asset_id = presenter.selectedAssetId();
        presenter.setDevelopNumber(QStringLiteral("exposure"), 0.8);
        ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }));
        StudioCommandController commands(presenter);
        ASSERT_TRUE(
            commands.applyLocalAdjustment(QStringLiteral("create"), {{QStringLiteral("kind"), 4}}));
        mask_id = presenter.activeLocalId();
        ASSERT_FALSE(mask_id.isEmpty());
        EXPECT_DOUBLE_EQ(presenter.editExposure(), 0);
        QVariantMap arguments{{QStringLiteral("id"), mask_id},
                              {QStringLiteral("asset"), asset_id},
                              {QStringLiteral("x"), 0.2},
                              {QStringLiteral("y"), 0.25},
                              {QStringLiteral("handle"), QStringLiteral("draw")}};
        auto began = commands.localAdjustment(QStringLiteral("gesture_begin"), arguments);
        ASSERT_TRUE(began.value(QStringLiteral("ok")).toBool())
            << presenter.errorText().toStdString();
        arguments.remove(QStringLiteral("handle"));
        arguments.insert(QStringLiteral("token"), began.value(QStringLiteral("token")));
        arguments.insert(QStringLiteral("x"), 0.7);
        arguments.insert(QStringLiteral("y"), 0.75);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("gesture_end"), arguments));
        ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }));
        presenter.setDevelopNumber(QStringLiteral("exposure"), -0.65);
        ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }));
        EXPECT_DOUBLE_EQ(presenter.editExposure(), -0.65);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
        ASSERT_TRUE(
            wait_until([&] { return !presenter.localEditing() && !presenter.previewLoading(); }));
        EXPECT_DOUBLE_EQ(presenter.editExposure(), 0.8);
        EXPECT_EQ(presenter.localAdjustments().size(), 1);
        auto live = StudioLiveSessionController::create(presenter, commands);
        ASSERT_TRUE(live);
        QElapsedTimer quiet;
        quiet.start();
        std::string previous_revision;
        ASSERT_TRUE(wait_until(
            [&]
            {
                const auto observed = live.value()->snapshot();
                const auto revision = observed.find("revision")->number_if()->text;
                if (revision != previous_revision || *observed.find("busy")->boolean_if())
                {
                    previous_revision = revision;
                    quiet.restart();
                }
                return quiet.elapsed() >= 150;
            }));
        const auto state = live.value()->snapshot();
        const auto selection = state.find("selection");
        const auto recipe_state = state.find("recipe");
        ASSERT_NE(selection, nullptr);
        ASSERT_NE(recipe_state, nullptr);
        const QStringList mask_command{
            QStringLiteral("--json"),
            QStringLiteral("studio"),
            QStringLiteral("mask"),
            QStringLiteral("--session-id"),
            QString::fromStdString(live.value()->descriptor().session_id),
            QStringLiteral("--asset-id"),
            asset_id,
            QStringLiteral("--action"),
            QStringLiteral("select"),
            QStringLiteral("--arguments"),
            QString::fromStdString(
                serialize_json(JsonValue::Object{{"id", mask_id.toStdString()}})),
            QStringLiteral("--expect-session-revision"),
            QString::fromStdString(state.find("revision")->number_if()->text),
            QStringLiteral("--expect-selection-revision"),
            QString::fromStdString(selection->find("revision")->number_if()->text),
            QStringLiteral("--expect-recipe-revision"),
            QString::fromStdString(recipe_state->find("revision")->number_if()->text)};
        const auto selected = run_cli_process(mask_command);
        ASSERT_EQ(selected.exit_code, 0) << selected.standard_output.constData();
        EXPECT_TRUE(presenter.localEditing());
        const auto stale_scope = run_cli_process(mask_command);
        EXPECT_NE(stale_scope.exit_code, 0);
        EXPECT_DOUBLE_EQ(presenter.editExposure(), -0.65);
        EXPECT_FALSE(commands.applyLocalAdjustment(
            QStringLiteral("delete"), {{QStringLiteral("id"), QStringLiteral("missing")}}));
        EXPECT_EQ(presenter.localAdjustments().size(), 1);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
        ASSERT_TRUE(
            wait_until([&] { return !presenter.localEditing() && !presenter.previewLoading(); }))
            << "local=" << presenter.localEditing() << " pending=" << presenter.localDonePending()
            << " preview=" << presenter.previewLoading()
            << " error=" << presenter.errorText().toStdString();
    }
    const auto listed = run_cli_process({QStringLiteral("--json"), QStringLiteral("catalog"),
                                         QStringLiteral("mask"), QStringLiteral("--catalog"),
                                         catalog, QStringLiteral("--asset-id"), asset_id,
                                         QStringLiteral("--action"), QStringLiteral("list")});
    ASSERT_EQ(listed.exit_code, 0)
        << listed.standard_error.constData() << listed.standard_output.constData();
    auto data = cli_data(listed.standard_output);
    ASSERT_TRUE(data);
    const auto *recipe_value = data.value().find("recipe");
    ASSERT_NE(recipe_value, nullptr);
    auto recipe = parse_recipe_json(serialize_json(*recipe_value));
    ASSERT_TRUE(recipe);
    auto params = develop_from_recipe(recipe.value());
    ASSERT_TRUE(params);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 0.8);
    auto local = local_adjustment_develop(params.value(), mask_id.toStdString());
    ASSERT_TRUE(local);
    EXPECT_DOUBLE_EQ(local.value().exposure_ev, -0.65);
    const auto stale = run_cli_process(
        {QStringLiteral("--json"), QStringLiteral("catalog"), QStringLiteral("mask"),
         QStringLiteral("--catalog"), catalog, QStringLiteral("--asset-id"), asset_id,
         QStringLiteral("--action"), QStringLiteral("delete"), QStringLiteral("--id"), mask_id,
         QStringLiteral("--expect-revision"), QStringLiteral("0")});
    EXPECT_NE(stale.exit_code, 0);
}

TEST(LocalAdjustmentWorkspaceTest, UnfinishedCreationAndInvalidGestureLeaveNoSavedMask)
{
    ensure_qt_core();
    init_logging("ravo-local-adjustment-tests");
    QTemporaryDir directory;
    QImage image(64, 48, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 110, 150));
    const auto photo = directory.filePath(QStringLiteral("photo.png"));
    ASSERT_TRUE(image.save(photo, "PNG"));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.importFilePaths({photo});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.selectedAssetId().isEmpty() && !presenter.busy(); }));
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.previewLoading() && !presenter.previewUrl().isEmpty(); }));
    StudioCommandController commands(presenter);
    ASSERT_TRUE(
        commands.applyLocalAdjustment(QStringLiteral("create"), {{QStringLiteral("kind"), 8}}));
    EXPECT_FALSE(commands.applyLocalAdjustment(
        QStringLiteral("gesture_begin"), {{QStringLiteral("id"), presenter.activeLocalId()},
                                          {QStringLiteral("asset"), presenter.selectedAssetId()},
                                          {QStringLiteral("x"), -1},
                                          {QStringLiteral("y"), 0.5},
                                          {QStringLiteral("handle"), QStringLiteral("draw")}}));
    ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
    EXPECT_FALSE(presenter.localEditing());
    EXPECT_TRUE(presenter.localAdjustments().isEmpty());
}
} // namespace ravo
