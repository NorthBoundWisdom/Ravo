#include <gtest/gtest.h>
#include <cmath>
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
        presenter.imports()->importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                       !presenter.busy();
            }));
        presenter.setBrowseMode(QStringLiteral("develop"));
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.inspect()->previewLoading() &&
                       !presenter.inspect()->previewUrl().isEmpty();
            }));
        asset_id = presenter.selectedAssetId();
        presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), 0.8);
        ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }));
        StudioCommandController commands(presenter);
        ASSERT_TRUE(
            commands.applyLocalAdjustment(QStringLiteral("create"), {{QStringLiteral("kind"), 4}}));
        mask_id = presenter.develop()->activeLocalId();
        ASSERT_FALSE(mask_id.isEmpty());
        EXPECT_DOUBLE_EQ(presenter.develop()->editExposure(), 0);
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
        ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }));
        const auto geometry = presenter.develop()->localMaskGeometry();
        ASSERT_FALSE(geometry.isEmpty());
        const auto outline = geometry.front().toMap();
        EXPECT_TRUE(outline.value("closed").toBool());
        EXPECT_EQ(outline.value("points").toList().size(), 128);
        const auto handles = presenter.develop()->localMaskHandles();
        ASSERT_EQ(handles.size(), 4);
        const auto first_point = outline.value("points").toList().front().toMap();
        EXPECT_NEAR(first_point.value("x").toDouble(), handles[1].toMap().value("x").toDouble(),
                    1e-9);
        EXPECT_NEAR(first_point.value("y").toDouble(), handles[1].toMap().value("y").toDouble(),
                    1e-9);
        presenter.develop()->setMaskOverlay(QStringLiteral("local"), false);
        EXPECT_EQ(presenter.develop()->localMaskGeometry(), geometry);
        EXPECT_EQ(presenter.develop()->localMaskHandles(), handles);
        presenter.develop()->setMaskOverlay(QStringLiteral("local"), true);
        presenter.develop()->previewDevelopNumber(QStringLiteral("exposure"), -0.5);
        EXPECT_TRUE(presenter.develop()->maskOverlayVisible());
        EXPECT_FALSE(presenter.develop()->maskOverlayActive());
        EXPECT_EQ(presenter.develop()->localMaskGeometry(), geometry);
        presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), -0.65);
        ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }));
        EXPECT_FALSE(presenter.develop()->maskOverlayActive());
        EXPECT_EQ(presenter.inspect()->previewImage(), presenter.inspect()->previewBaseImage());
        ASSERT_TRUE(wait_until([&] { return presenter.develop()->maskOverlayActive(); }));
        ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }));
        EXPECT_DOUBLE_EQ(presenter.develop()->editExposure(), -0.65)
            << presenter.errorText().toStdString();
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.develop()->localEditing() &&
                       !presenter.inspect()->previewLoading();
            }));
        EXPECT_DOUBLE_EQ(presenter.develop()->editExposure(), 0.8);
        EXPECT_EQ(presenter.develop()->localAdjustments().size(), 1);
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
        const auto *scope = state.find("editing_scope");
        ASSERT_NE(scope, nullptr);
        ASSERT_NE(scope->find("overlay_requested"), nullptr);
        ASSERT_NE(scope->find("overlay_active"), nullptr);
        EXPECT_EQ(*scope->find("overlay_requested")->boolean_if(),
                  presenter.develop()->maskOverlayVisible());
        EXPECT_EQ(*scope->find("overlay_active")->boolean_if(),
                  presenter.develop()->maskOverlayActive());
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
        EXPECT_TRUE(presenter.develop()->localEditing());
        const auto stale_scope = run_cli_process(mask_command);
        EXPECT_NE(stale_scope.exit_code, 0);
        EXPECT_DOUBLE_EQ(presenter.develop()->editExposure(), -0.65);
        EXPECT_FALSE(commands.applyLocalAdjustment(
            QStringLiteral("delete"), {{QStringLiteral("id"), QStringLiteral("missing")}}));
        EXPECT_EQ(presenter.develop()->localAdjustments().size(), 1);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.develop()->localEditing() &&
                       !presenter.inspect()->previewLoading();
            }))
            << "local=" << presenter.develop()->localEditing()
            << " pending=" << presenter.develop()->localDonePending()
            << " preview=" << presenter.inspect()->previewLoading()
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

TEST(LocalAdjustmentWorkspaceTest, GeometricGuidesFollowRotationAndRemainWithoutOverlay)
{
    ensure_qt_core();
    init_logging("ravo-local-geometry-tests");
    QTemporaryDir directory;
    const auto photo = directory.filePath(QStringLiteral("guides.png"));
    QImage image(128, 96, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(80, 110, 150));
    ASSERT_TRUE(image.save(photo));
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("guides.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.selectedAssetId().isEmpty() && !presenter.busy(); }));
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewUrl().isEmpty() &&
                   presenter.develop()->state().develop_loaded_;
        }));
    presenter.develop()->rotateRight();
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }))
        << presenter.errorText().toStdString();
    StudioCommandController commands(presenter);
    for (const int kind : {2, 3, 4, 7, 8})
    {
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("create"),
                                                  {{QStringLiteral("kind"), kind}}));
        EXPECT_TRUE(presenter.develop()->localMaskGeometry().isEmpty());
        QVariantMap args{{"id", presenter.develop()->activeLocalId()},
                         {"asset", presenter.selectedAssetId()},
                         {"x", 0.3},
                         {"y", 0.3},
                         {"handle", "draw"}};
        auto began = commands.localAdjustment(QStringLiteral("gesture_begin"), args);
        ASSERT_TRUE(began.value("ok").toBool()) << presenter.errorText().toStdString();
        args.remove("handle");
        args.insert("token", began.value("token"));
        args.insert("x", 0.7);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("gesture_update"), args));
        args.insert("y", 0.7);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("gesture_end"), args));
        ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }));
        const auto guides = presenter.develop()->localMaskGeometry();
        ASSERT_FALSE(guides.isEmpty()) << kind;
        if (kind == 2)
        {
            EXPECT_EQ(guides.size(), 3);
            EXPECT_FALSE(guides[1].toMap().value("closed").toBool());
        }
        else if (kind != 8)
            EXPECT_TRUE(guides.front().toMap().value("closed").toBool());
        for (const auto &guide : guides)
            for (const auto &point : guide.toMap().value("points").toList())
            {
                EXPECT_TRUE(std::isfinite(point.toMap().value("x").toDouble()));
                EXPECT_TRUE(std::isfinite(point.toMap().value("y").toDouble()));
            }
        presenter.develop()->setMaskOverlay(QStringLiteral("local"), false);
        EXPECT_EQ(presenter.develop()->localMaskGeometry(), guides);
        ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
        ASSERT_TRUE(wait_until(
            [&]
            {
                return !presenter.develop()->localEditing() &&
                       !presenter.inspect()->previewLoading();
            }));
        EXPECT_TRUE(presenter.develop()->localMaskGeometry().isEmpty());
    }
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
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(
        wait_until([&] { return !presenter.selectedAssetId().isEmpty() && !presenter.busy(); }));
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewUrl().isEmpty();
        }));
    StudioCommandController commands(presenter);
    ASSERT_TRUE(
        commands.applyLocalAdjustment(QStringLiteral("create"), {{QStringLiteral("kind"), 8}}));
    EXPECT_FALSE(
        commands.applyLocalAdjustment(QStringLiteral("gesture_begin"),
                                      {{QStringLiteral("id"), presenter.develop()->activeLocalId()},
                                       {QStringLiteral("asset"), presenter.selectedAssetId()},
                                       {QStringLiteral("x"), -1},
                                       {QStringLiteral("y"), 0.5},
                                       {QStringLiteral("handle"), QStringLiteral("draw")}}));
    ASSERT_TRUE(commands.applyLocalAdjustment(QStringLiteral("done"), {}));
    EXPECT_FALSE(presenter.develop()->localEditing());
    EXPECT_TRUE(presenter.develop()->localAdjustments().isEmpty());
}
} // namespace ravo
