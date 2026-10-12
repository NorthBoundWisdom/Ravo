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
#include "ravo/desktop/studio_display_presentation.h"
#include "ravo/desktop/folder_list_model.h"
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/import_candidate_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_develop_internal.h"
#include "studio_language_manager.h"

#include "studio_test_support.h"
#include "interactive_perf_report.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

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
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.develop()->modifiedParameterChoices().isEmpty());

    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), 0.75);
    presenter.develop()->setDevelopNumber(QStringLiteral("saturation"), 0.4);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9 &&
                   std::abs(presenter.develop()->editSaturation() - 0.4) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    const auto candidates = presenter.develop()->modifiedParameterChoices();
    const auto has_field = [&candidates](const QString &field)
    {
        return std::any_of(
            candidates.cbegin(), candidates.cend(), [&field](const QVariant &entry)
            { return entry.toMap().value(QStringLiteral("field")).toString() == field; });
    };
    EXPECT_TRUE(has_field(QStringLiteral("exposure")));
    EXPECT_TRUE(has_field(QStringLiteral("saturation")));

    presenter.develop()->copyParametersSelected(QVariantList{QStringLiteral("exposure")});
    ASSERT_TRUE(presenter.develop()->hasCopiedParameters());
    EXPECT_EQ(presenter.statusText(),
              QCoreApplication::translate("StudioPresenter", "Parameters copied."));

    presenter.develop()->savePreset(QStringLiteral("Exposure only"),
                                    QVariantList{QStringLiteral("exposure")});
    ASSERT_TRUE(presenter.errorText().isEmpty()) << presenter.errorText().toStdString();
    ASSERT_EQ(presenter.develop()->editPresets().size(), 1);
    const QString preset_path =
        presenter.develop()->editPresets().front().toMap().value(QStringLiteral("path")).toString();
    QFile preset_file(preset_path);
    ASSERT_TRUE(preset_file.open(QIODevice::ReadOnly)) << preset_file.errorString().toStdString();
    auto style = parse_recipe_style_json(preset_file.readAll().toStdString());
    ASSERT_TRUE(style) << style.error().message;
    EXPECT_EQ(style.value().schema_version, kRecipeStyleSelectedSchemaVersion);
    EXPECT_EQ(style.value().selected_fields, (std::vector<std::string>{"exposure"}));

    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), -0.25);
    presenter.develop()->setDevelopNumber(QStringLiteral("saturation"), -0.3);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() + 0.25) < 1e-9 &&
                   std::abs(presenter.develop()->editSaturation() + 0.3) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.develop()->pasteParameters();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editSaturation(), -0.3, 1e-9);
    EXPECT_EQ(presenter.statusText(),
              QCoreApplication::translate("StudioPresenter", "Parameters pasted."));

    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), -0.25);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() + 0.25) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.develop()->applyStyleFromPath(preset_path);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editSaturation(), -0.3, 1e-9);

    presenter.develop()->savePreset(QStringLiteral("Exposure only"),
                                    QVariantList{QStringLiteral("exposure")});
    EXPECT_EQ(
        presenter.errorText(),
        QCoreApplication::translate("StudioPresenter", "A preset with that name already exists."));
    presenter.develop()->savePreset(QStringLiteral("Stale selection"),
                                    QVariantList{QStringLiteral("colorContrast")});
    EXPECT_EQ(presenter.errorText(),
              QCoreApplication::translate("StudioPresenter",
                                          "The selected parameters are no longer modified."));

    presenter.develop()->copyParametersSelected(QVariantList{QStringLiteral("colorContrast")});
    EXPECT_TRUE(presenter.develop()->hasCopiedParameters());
    EXPECT_EQ(presenter.errorText(),
              QCoreApplication::translate("StudioPresenter",
                                          "The selected parameters are no longer modified."));
    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), -0.1);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() + 0.1) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.develop()->pasteParameters();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editSaturation(), -0.3, 1e-9);
}

TEST(StudioPresenterTest, SelectAllVisibleSelectsLoadedPhotosAndKeepsPrimary)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage image(48, 32, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(30, 60, 90));
    const QString first_photo = directory.filePath(QStringLiteral("a.png"));
    const QString second_photo = directory.filePath(QStringLiteral("b.png"));
    ASSERT_TRUE(image.save(first_photo, "PNG"));
    image.fill(QColor(90, 60, 30));
    ASSERT_TRUE(image.save(second_photo, "PNG"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.selectAllVisible();
    EXPECT_EQ(presenter.selectedCount(), 0);

    presenter.imports()->importFilePaths({first_photo, second_photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    const QString first_id = presenter.assets()->assetIdAt(0);
    const QString second_id = presenter.assets()->assetIdAt(1);
    ASSERT_FALSE(first_id.isEmpty());
    ASSERT_FALSE(second_id.isEmpty());
    presenter.selectAsset(second_id);
    ASSERT_EQ(presenter.selectedCount(), 1);
    presenter.selectAllVisible();
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    EXPECT_EQ(presenter.selectedCount(), 2);
    EXPECT_EQ(presenter.selectedAssetId(), second_id);
    EXPECT_TRUE(presenter.isAssetSelected(first_id));
    EXPECT_TRUE(presenter.isAssetSelected(second_id));
    presenter.selectAllVisible();
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    EXPECT_EQ(presenter.selectedCount(), 2);
    EXPECT_EQ(presenter.selectedAssetId(), second_id);
}

TEST(StudioPresenterTest, PasteParametersToSelectionOverlaysClipboardAndClearsSessionUndo)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    QImage image(96, 64, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(90, 120, 170));
    const QString first_photo = directory.filePath(QStringLiteral("one.png"));
    const QString second_photo = directory.filePath(QStringLiteral("two.png"));
    ASSERT_TRUE(image.save(first_photo, "PNG"));
    image.fill(QColor(170, 120, 90));
    ASSERT_TRUE(image.save(second_photo, "PNG"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->importFilePaths({first_photo, second_photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    const QString first_id = presenter.assets()->assetIdAt(0);
    const QString second_id = presenter.assets()->assetIdAt(1);
    ASSERT_FALSE(first_id.isEmpty());
    ASSERT_FALSE(second_id.isEmpty());
    presenter.selectAsset(first_id);
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }))
        << presenter.errorText().toStdString();
    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), 0.75);
    presenter.develop()->setDevelopNumber(QStringLiteral("saturation"), 0.4);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9 &&
                   std::abs(presenter.develop()->editSaturation() - 0.4) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.develop()->copyParametersSelected(QVariantList{QStringLiteral("exposure")});
    ASSERT_TRUE(presenter.develop()->hasCopiedParameters());

    presenter.selectAsset(second_id);
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }))
        << presenter.errorText().toStdString();
    presenter.develop()->setDevelopNumber(QStringLiteral("saturation"), -0.3);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editSaturation() + 0.3) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.toggleAssetSelected(first_id);
    ASSERT_EQ(presenter.selectedCount(), 2);
    presenter.develop()->pasteParametersToSelection();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.catalogOperationActive() && !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editSaturation(), 0.4, 1e-9);
    EXPECT_FALSE(presenter.develop()->canUndo());
    EXPECT_EQ(
        presenter.statusText(),
        QCoreApplication::translate("StudioPresenter", "Parameters applied to the selection."));

    presenter.selectAsset(second_id);
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   std::abs(presenter.develop()->editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editSaturation(), -0.3, 1e-9);
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
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    bool saw_initial_live = false;
    QObject::connect(presenter.inspect(), &StudioInspectPresenter::previewChanged, &presenter,
                     [&]
                     {
                         const auto url = presenter.inspect()->previewUrl();
                         saw_initial_live =
                             saw_initial_live || (url.scheme() == QLatin1String("image") &&
                                                  url.path() == QLatin1String("/live"));
                     });
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   presenter.inspect()->previewUrl().isLocalFile();
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(saw_initial_live);
    const QSize settled_viewport(presenter.inspect()->previewViewportWidth(),
                                 presenter.inspect()->previewViewportHeight());
    EXPECT_EQ(settled_viewport, presenter.inspect()->previewImage().size());
    EXPECT_EQ(std::max(settled_viewport.width(), settled_viewport.height()), 1600);

    bool saw_uncommitted_live = false;
    QSize uncommitted_live_image_size;
    QSize uncommitted_live_viewport;
    QObject::connect(
        presenter.inspect(), &StudioInspectPresenter::previewChanged, &presenter,
        [&]
        {
            const auto url = presenter.inspect()->previewUrl();
            if (url.scheme() == QLatin1String("image") && url.path() == QLatin1String("/live"))
            {
                saw_uncommitted_live = true;
                uncommitted_live_image_size = presenter.inspect()->previewImage().size();
                uncommitted_live_viewport = QSize(presenter.inspect()->previewViewportWidth(),
                                                  presenter.inspect()->previewViewportHeight());
            }
        });
    presenter.develop()->previewDevelopNumber(QStringLiteral("exposure"), 0.25);
    ASSERT_TRUE(
        wait_until([&] { return saw_uncommitted_live && !presenter.inspect()->previewLoading(); }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.develop()->editExposure(), 0.25, 1e-9);
    EXPECT_EQ(uncommitted_live_image_size, settled_viewport);
    EXPECT_EQ(uncommitted_live_viewport, settled_viewport);

    bool saw_live = false;
    bool saw_settled = false;
    bool live_viewport_stayed_settled = false;
    QObject::connect(
        presenter.inspect(), &StudioInspectPresenter::previewChanged, &presenter,
        [&]
        {
            const auto url = presenter.inspect()->previewUrl();
            if (url.scheme() == QLatin1String("image") && url.path() == QLatin1String("/live"))
            {
                saw_live = true;
                live_viewport_stayed_settled =
                    QSize(presenter.inspect()->previewViewportWidth(),
                          presenter.inspect()->previewViewportHeight()) == settled_viewport;
            }
            saw_settled = saw_live && url.isLocalFile();
        });
    presenter.develop()->applyStyleFromPath(style_path);
    ASSERT_TRUE(wait_until([&] { return saw_live && saw_settled; }))
        << presenter.errorText().toStdString()
        << " url=" << presenter.inspect()->previewUrl().toString().toStdString();
    EXPECT_FALSE(presenter.inspect()->previewLoading());
    EXPECT_NEAR(presenter.develop()->editExposure(), 1.0, 1e-9);
    EXPECT_TRUE(live_viewport_stayed_settled);
    EXPECT_EQ(QSize(presenter.inspect()->previewViewportWidth(),
                    presenter.inspect()->previewViewportHeight()),
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
    presenter.imports()->importFilePaths({photo});
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
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewImage().isNull() &&
                   !presenter.inspect()->previewUrl().isEmpty();
        }))
        << presenter.errorText().toStdString();

    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), 0.5);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   presenter.inspect()->previewUrl().isLocalFile() &&
                   std::abs(presenter.develop()->editExposure() - 0.5) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    const QImage first_after = presenter.inspect()->previewImage();
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
            return presenter.develop()->comparisonActive() &&
                   !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->comparisonBeforeUrl().isEmpty() &&
                   !presenter.inspect()->comparisonBeforeImage().isNull();
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(commands.action(comparison_action).value(QStringLiteral("checked")).toBool());
    const QImage before = presenter.inspect()->comparisonBeforeImage();
    const QImage after = presenter.inspect()->previewImage();
    ASSERT_EQ(before.size(), after.size());
    const QPoint center(before.width() / 2, before.height() / 2);
    EXPECT_NE(before.pixelColor(center), after.pixelColor(center));
    EXPECT_EQ(first_after.pixelColor(center), after.pixelColor(center));

    presenter.develop()->setDevelopNumber(QStringLiteral("exposure"), 1.0);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   presenter.inspect()->previewUrl().isLocalFile() &&
                   std::abs(presenter.develop()->editExposure() - 1.0) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.develop()->comparisonActive());
    EXPECT_EQ(before.pixelColor(center),
              presenter.inspect()->comparisonBeforeImage().pixelColor(center));
    EXPECT_NE(after.pixelColor(center), presenter.inspect()->previewImage().pixelColor(center));

    const auto deactivated = commands.executeAction(comparison_action, QStringLiteral("control"));
    ASSERT_TRUE(deactivated.value(QStringLiteral("accepted")).toBool());
    EXPECT_FALSE(presenter.develop()->comparisonActive());
    EXPECT_TRUE(presenter.inspect()->comparisonBeforeUrl().isEmpty());
    EXPECT_TRUE(presenter.inspect()->comparisonBeforeImage().isNull());
    EXPECT_FALSE(commands.action(comparison_action).value(QStringLiteral("checked")).toBool());

    ASSERT_TRUE(commands.executeAction(comparison_action, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(commands.executeAction(comparison_action, QStringLiteral("control"))
                    .value(QStringLiteral("accepted"))
                    .toBool());
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }));
    EXPECT_FALSE(presenter.develop()->comparisonActive());
    EXPECT_TRUE(presenter.inspect()->comparisonBeforeUrl().isEmpty());
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
    presenter.imports()->importFilePaths({photo});
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
            return !presenter.inspect()->previewLoading() &&
                   presenter.inspect()->previewUrl().isLocalFile();
        }))
        << presenter.errorText().toStdString();

    std::vector<qulonglong> published_revisions;
    std::vector<qulonglong> timed_revisions;
    QUrl last_url = presenter.inspect()->previewUrl();
    QTimer updates;
    updates.setTimerType(Qt::PreciseTimer);
    updates.setInterval(1);
    QObject::connect(presenter.develop(), &StudioDevelopPresenter::interactivePreviewPublished,
                     &updates,
                     [&](const qulonglong revision, const qlonglong intent_to_image_us)
                     {
                         EXPECT_GT(intent_to_image_us, 0);
                         timed_revisions.push_back(revision);
                         updates.stop();
                     });
    QObject::connect(presenter.inspect(), &StudioInspectPresenter::previewChanged, &updates,
                     [&]
                     {
                         const QUrl current = presenter.inspect()->previewUrl();
                         if (current == last_url || current.scheme() != QLatin1String("image") ||
                             current.path() != QLatin1String("/live"))
                         {
                             return;
                         }
                         last_url = current;
                         published_revisions.push_back(
                             QUrlQuery(current).queryItemValue(QStringLiteral("r")).toULongLong());
                     });

    presenter.develop()->previewDevelopNumber(QStringLiteral("exposure"), -0.2);
    constexpr int kIntentCount = 40;
    for (int index = 1; index < kIntentCount - 1; ++index)
    {
        presenter.develop()->previewDevelopNumber(QStringLiteral("exposure"),
                                                  -0.2 + static_cast<double>(index) * 0.02);
    }
    const double latest_exposure = -0.2 + static_cast<double>(kIntentCount - 1) * 0.02;
    // A finite quiet burst may legitimately supersede its first frame. Keep
    // changed intents flowing until progress is observed, then require a
    // distinct final value that was never sent by the progress stream.
    int stream_index = 0;
    QObject::connect(&updates, &QTimer::timeout, &updates,
                     [&]
                     {
                         presenter.develop()->previewDevelopNumber(
                             QStringLiteral("exposure"),
                             -0.2 + static_cast<double>(stream_index) * 0.02);
                         stream_index = (stream_index + 1) % (kIntentCount - 1);
                     });
    updates.start();
    ASSERT_TRUE(wait_until([&] { return !published_revisions.empty(); }, 10000))
        << presenter.errorText().toStdString();
    updates.stop();
    const auto progress_frames = published_revisions.size();
    presenter.develop()->previewDevelopNumber(QStringLiteral("exposure"), latest_exposure);
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

    EXPECT_GE(published_revisions.size(), 2U);
    EXPECT_GT(published_revisions.size(), progress_frames);
    EXPECT_EQ(timed_revisions, published_revisions);
    for (std::size_t index = 1; index < published_revisions.size(); ++index)
        EXPECT_LT(published_revisions[index - 1], published_revisions[index]);
    EXPECT_NEAR(presenter.develop()->editExposure(), latest_exposure, 1e-9);

    const auto state = live.value()->snapshot();
    const auto *preview = state.find("preview");
    ASSERT_NE(preview, nullptr);
    const auto *profile = preview->find("color_profile");
    const auto *digest = preview->find("pixel_sha256");
    ASSERT_NE(profile, nullptr);
    ASSERT_NE(profile->string_if(), nullptr);
    ASSERT_NE(digest, nullptr);
    ASSERT_NE(digest->string_if(), nullptr);
    const QImage displayed = presenter.inspect()->previewImage();
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

    StudioDisplayPresentation display;
    StudioPresenter presenter;
    ASSERT_TRUE(display.valid());
    presenter.bindDisplayPresentation(&display);
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
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewImage().isNull();
        },
        30000))
        << presenter.errorText().toStdString();

    const double baseline = presenter.develop()->editExposure();
    const double sweep_center = std::clamp(baseline, -2.9, 3.9);
    std::vector<std::int64_t> elapsed_us;
    elapsed_us.reserve(runs);
    for (std::size_t run = 0U; run < runs + 2U; ++run)
    {
        const double offset = static_cast<double>(static_cast<int>(run % 7U) - 3) * 0.01;
        const QUrl previous = presenter.inspect()->previewUrl();
        QElapsedTimer timer;
        QEventLoop event_loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        std::optional<std::int64_t> published_us;
        QObject::connect(&timeout, &QTimer::timeout, &event_loop, &QEventLoop::quit);
        const auto connection = QObject::connect(
            presenter.inspect(), &StudioInspectPresenter::previewChanged, &presenter,
            [&]
            {
                const QUrl current = presenter.inspect()->previewUrl();
                if (!published_us.has_value() && current != previous &&
                    current.scheme() == QLatin1String("image") &&
                    current.path() == QLatin1String("/live") &&
                    !presenter.inspect()->previewImage().isNull())
                {
                    published_us = timer.nsecsElapsed() / 1000;
                    event_loop.quit();
                }
            });
        timer.start();
        presenter.develop()->previewDevelopNumber(QStringLiteral("exposure"),
                                                  sweep_center + offset);
        timeout.start(5000);
        if (!published_us.has_value())
        {
            event_loop.exec();
        }
        QObject::disconnect(connection);
        ASSERT_TRUE(published_us.has_value()) << presenter.errorText().toStdString();
        if (run >= 2U)
            elapsed_us.push_back(*published_us);
    }
    const auto stats = interactive_perf_report::summarize(elapsed_us);
    std::cerr << "studio_interactive_runs=" << runs << " intent_to_publish_min_us=" << stats.min
              << " intent_to_publish_median_us=" << stats.p50
              << " intent_to_publish_p90_us=" << stats.p90
              << " intent_to_publish_max_us=" << stats.max << '\n';
    {
        interactive_perf_report::CaseMeta meta;
        meta.case_id = "develop_intent_to_publish_with_display";
        meta.path = "gallery_viewer_develop";
        meta.unit = "us";
        meta.cache_state = "warm";
        meta.source_kind = presenter.selectedMediaType().toStdString();
        meta.warmups = 2;
        meta.recorded_samples = runs;
        meta.asset_id = asset_id;
        meta.catalog_path = catalog_path;
        interactive_perf_report::emit_case(meta, elapsed_us);
    }
    if (const char *budget = std::getenv("RAVO_INTERACTIVE_PERF_P90_BUDGET_MS"))
    {
        EXPECT_LE(stats.p90, std::stoll(budget) * 1000);
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
    const auto preview_matches_current = [&]
    {
        const auto state = live.value()->snapshot();
        const auto *preview = state.find("preview");
        if (preview == nullptr || preview->object_if() == nullptr)
            return false;
        const auto *matches = preview->find("matches_current_recipe");
        return matches != nullptr && matches->boolean_if() != nullptr && *matches->boolean_if();
    };
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
    // Cached settled pixels may be published through the live image provider.
    // Start the burst only after the displayed frame matches the loaded recipe.
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewImage().isNull() && preview_matches_current();
        },
        30000))
        << presenter.errorText().toStdString();

    const double baseline = presenter.develop()->editExposure();
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
    QUrl last_url = presenter.inspect()->previewUrl();
    QObject::connect(&intent_timer, &QTimer::timeout, &event_loop,
                     [&]
                     {
                         const auto now_us = timer.nsecsElapsed() / 1000;
                         if (!first_intent_us)
                             first_intent_us = now_us;
                         presenter.develop()->previewDevelopNumber(
                             QStringLiteral("exposure"),
                             burst_start + static_cast<double>(sent) * 0.001);
                         last_intent_us = now_us;
                         ++sent;
                         if (sent == intents)
                             intent_timer.stop();
                     });
    QObject::connect(presenter.inspect(), &StudioInspectPresenter::previewChanged, &event_loop,
                     [&]
                     {
                         const QUrl current = presenter.inspect()->previewUrl();
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
    EXPECT_FALSE(presenter.develop()->canUndo());
    EXPECT_FALSE(presenter.develop()->canRedo());
    presenter.develop()->restoreHistory(0);
    presenter.develop()->undoEdit();
    presenter.develop()->redoEdit();
    EXPECT_FALSE(presenter.develop()->canUndo());
    EXPECT_FALSE(presenter.develop()->canRedo());
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
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->previewLoading(); }))
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
    EXPECT_NEAR(presenter.develop()->editExposure(), 0.0, 1e-9);
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
    auto saved = writer.develop().save_develop(asset_id, params);
    ASSERT_TRUE(saved) << saved.error().message;
    ASSERT_TRUE(writer.close());

    ASSERT_TRUE(wait_until(
        [&]
        {
            presenter.pollCatalogRevision();
            return std::abs(presenter.develop()->editExposure() - 1.0) < 1e-6;
        }))
        << presenter.errorText().toStdString()
        << " exposure=" << presenter.develop()->editExposure();
    EXPECT_TRUE(presenter.selectedHasEdits());
    EXPECT_FALSE(presenter.develop()->canUndo());
}

TEST(StudioPresenterTest, ScopeModeOwnsAllAcceptedDiagnosticsAndRejectsFutureState)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.inspect()->scopeMode(), QStringLiteral("parade"));
    for (const auto &mode : {QStringLiteral("waveform"), QStringLiteral("parade"),
                             QStringLiteral("vectorscope"), QStringLiteral("split")})
    {
        presenter.inspect()->setScopeMode(mode);
        EXPECT_EQ(presenter.inspect()->scopeMode(), mode);
    }
    presenter.inspect()->setScopeMode(QStringLiteral("future"));
    EXPECT_EQ(presenter.inspect()->scopeMode(), QStringLiteral("histogram"));
    EXPECT_TRUE(presenter.inspect()->scopeParadeUrl().isEmpty());
    EXPECT_TRUE(presenter.inspect()->scopeWaveformUrl().isEmpty());
    EXPECT_TRUE(presenter.inspect()->scopeVectorscopeUrl().isEmpty());
    EXPECT_TRUE(presenter.inspect()->scopeSplitUrl().isEmpty());
}

TEST(StudioQmlContract, ThreeDimensionalLutUsesTypedPresenterAndGenericDevelopIntents)
{
    StudioPresenter presenter;
    const auto state = presenter.develop()->editLut3d();
    EXPECT_FALSE(state.value(QStringLiteral("present")).toBool());
    EXPECT_FALSE(state.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(state.value(QStringLiteral("hasFile")).toBool());
    EXPECT_EQ(state.value(QStringLiteral("spaceChoices")).toList().size(), 6);
    EXPECT_EQ(state.value(QStringLiteral("interpolationChoices")).toList().size(), 2);
}

TEST(StudioPresenterTest, MaskPlacePreviewMapsThroughCropAndRejectsGeometry)
{
    using studio_develop_internal::map_mask_place_preview;
    using studio_develop_internal::mask_place_geometry_allowed;
    DevelopParams params;
    EXPECT_TRUE(mask_place_geometry_allowed(params));
    auto identity = map_mask_place_preview(params, 0.25, 0.75);
    ASSERT_TRUE(identity) << identity.error().message;
    EXPECT_DOUBLE_EQ(identity.value().first, 0.25);
    EXPECT_DOUBLE_EQ(identity.value().second, 0.75);

    params.crop_x = 0.10;
    params.crop_y = 0.20;
    params.crop_width = 0.50;
    params.crop_height = 0.40;
    auto cropped = map_mask_place_preview(params, 0.5, 0.5);
    ASSERT_TRUE(cropped) << cropped.error().message;
    EXPECT_DOUBLE_EQ(cropped.value().first, 0.35);
    EXPECT_DOUBLE_EQ(cropped.value().second, 0.40);

    params.straighten_degrees = 5.0;
    EXPECT_FALSE(mask_place_geometry_allowed(params));
    auto rejected = map_mask_place_preview(params, 0.5, 0.5);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "mask_place_geometry_unavailable");

    params.straighten_degrees = 0.0;
    auto invalid = map_mask_place_preview(params, -0.1, 0.5);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_mask_place_preview");
}

TEST(StudioPresenterTest, ParametricAssistThresholdsFromSampleAndHistogram)
{
    auto plain = parametric_thresholds_from_histogram_assist(0.5, nullptr);
    ASSERT_TRUE(plain) << plain.error().message;
    EXPECT_NEAR(plain.value()[0], 0.5 - 32.0 / 255.0, 1e-9);
    EXPECT_NEAR(plain.value()[3], 0.5 + 32.0 / 255.0, 1e-9);
    EXPECT_LE(plain.value()[0], plain.value()[1]);
    EXPECT_LE(plain.value()[1], plain.value()[2]);
    EXPECT_LE(plain.value()[2], plain.value()[3]);

    auto invalid = parametric_thresholds_from_histogram_assist(-0.1, nullptr);
    ASSERT_FALSE(invalid);
    EXPECT_EQ(invalid.error().context.at("reason"), "invalid_parametric_assist_sample");

    std::array<std::uint32_t, 256> bins{};
    bins[100] = 50;
    bins[101] = 40;
    bins[102] = 30;
    bins[99] = 20;
    auto assisted = parametric_thresholds_from_histogram_assist(100.0 / 255.0, &bins);
    ASSERT_TRUE(assisted) << assisted.error().message;
    EXPECT_NEAR(assisted.value()[0], 99.0 / 255.0, 1e-9);
    EXPECT_NEAR(assisted.value()[3], 102.0 / 255.0, 1e-9);

    EXPECT_NEAR(normalized_display_mask_channel(255, 0, 0, 1), 1.0, 1e-12);
    EXPECT_NEAR(normalized_display_mask_channel(0, 255, 0, 2), 1.0, 1e-12);
    EXPECT_NEAR(normalized_display_mask_channel(0, 0, 255, 3), 1.0, 1e-12);
    EXPECT_NEAR(normalized_display_mask_channel(255, 255, 255, 0), 1.0, 1e-9);
}

TEST(StudioPresenterTest, ParametricAssistGeometryMatchesMaskPlaceFailClosed)
{
    using studio_develop_internal::mask_place_geometry_allowed;
    DevelopParams params;
    EXPECT_TRUE(mask_place_geometry_allowed(params));
    params.canvas_enabled = true;
    EXPECT_FALSE(mask_place_geometry_allowed(params));
    params.canvas_enabled = false;
    params.perspective_vertical = 0.2;
    EXPECT_FALSE(mask_place_geometry_allowed(params));
}

TEST(StudioPresenterTest, ParametricAssistAuthorizedEverydayConsumersOnly)
{
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kColorBalanceRgb));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kExposure));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kRgbCurve));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kToneCurve));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kHighlights));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kShadows));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kWhites));
    EXPECT_TRUE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kBlacks));
    EXPECT_FALSE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kColorHarmonizer));
    EXPECT_FALSE(develop_mask_parametric_assist_allowed(DevelopMaskTarget::kGraduatedNd));
}

TEST(StudioQmlContract, TextureIsPrimaryDetailControlWithCollapsedAdvancedScale)
{
    StudioPresenter presenter;
    const auto state = presenter.develop()->editTexture();
    EXPECT_DOUBLE_EQ(state.value(QStringLiteral("strength")).toDouble(), 0.0);
    EXPECT_DOUBLE_EQ(state.value(QStringLiteral("detailThreshold")).toDouble(), 0.2);
    EXPECT_EQ(state.value(QStringLiteral("iterations")).toLongLong(), 1);
}

TEST(StudioPresenterTest, ImportCandidateModelKeepsNamesWhenUpdatingInspection)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    ImportCandidate first;
    first.source_path = "/tmp/one.png";
    first.display_name = "one.png";
    ImportCandidate second;
    second.source_path = "/tmp/two.png";
    second.display_name = "two.png";
    model.setCandidates({first, second});
    EXPECT_EQ(model.rowCount(), 2);
    EXPECT_EQ(model.selectedCount(), 2);
    EXPECT_FALSE(model.inspected(0));
    ImportCandidate inspected = first;
    inspected.duplicate = true;
    inspected.media_type = "image/png";
    model.updateCandidate(0, inspected);
    EXPECT_TRUE(model.inspected(0));
    EXPECT_FALSE(model.inspected(1));
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_EQ(model.data(model.index(0, 0), ImportCandidateListModel::DisplayNameRole).toString(),
              QStringLiteral("one.png"));
    EXPECT_TRUE(model.data(model.index(0, 0), ImportCandidateListModel::DuplicateRole).toBool());
    QImage image(8, 8, QImage::Format_RGB888);
    image.fill(Qt::red);
    model.setThumbnail(1, image);
    EXPECT_FALSE(model.thumbnail(1).isNull());
    EXPECT_TRUE(model.thumbnail(0).isNull());
}

TEST(StudioPresenterTest, ImportCandidateModelHighlightsAndAppliesCheckToSelection)
{
    ensure_qt_core();
    ImportCandidateListModel model;
    ImportCandidate first;
    first.source_path = "/tmp/one.png";
    first.display_name = "one.png";
    ImportCandidate second;
    second.source_path = "/tmp/two.png";
    second.display_name = "two.png";
    ImportCandidate third;
    third.source_path = "/tmp/three.png";
    third.display_name = "three.png";
    model.setCandidates({first, second, third});
    EXPECT_EQ(model.selectedCount(), 3);
    EXPECT_FALSE(model.highlighted(0));
    model.highlightExclusive(0);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_FALSE(model.highlighted(1));
    model.highlightToggle(2);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(2));
    model.applyCheck(0);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_FALSE(model.data(model.index(0, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_TRUE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    EXPECT_FALSE(model.data(model.index(2, 0), ImportCandidateListModel::SelectedRole).toBool());
    model.applyCheck(1);
    EXPECT_EQ(model.selectedCount(), 0);
    EXPECT_FALSE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    model.applyCheck(1);
    EXPECT_EQ(model.selectedCount(), 1);
    EXPECT_TRUE(model.data(model.index(1, 0), ImportCandidateListModel::SelectedRole).toBool());
    model.highlightRange(0, 1, false);
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_FALSE(model.highlighted(2));
    model.highlightAll();
    EXPECT_TRUE(model.highlighted(0));
    EXPECT_TRUE(model.highlighted(1));
    EXPECT_TRUE(model.highlighted(2));
    model.setAllSelected(true);
    model.applyCheck(0);
    EXPECT_EQ(model.selectedCount(), 0);
}

TEST(StudioPresenterTest, ImportWorkspacePublishesNamedPlaceholdersBeforeThumbnails)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source_dir = QDir(directory.path()).filePath(QStringLiteral("source"));
    ASSERT_TRUE(QDir().mkpath(source_dir));
    const QStringList names{QStringLiteral("alpha.png"), QStringLiteral("beta.png"),
                            QStringLiteral("gamma.png")};
    for (const auto &name : names)
    {
        QImage image(48, 32, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40 + static_cast<int>(names.indexOf(name)), 80, 120));
        ASSERT_TRUE(image.save(QDir(source_dir).filePath(name), "PNG"));
    }

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->openImportPage();
    presenter.imports()->setImportSourceRoot(source_dir);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() &&
                   presenter.imports()->importCandidates()->rowCount() == 3;
        },
        30000))
        << presenter.errorText().toStdString();
    auto *candidates = presenter.imports()->importCandidates();
    EXPECT_EQ(candidates->selectedCount(), 3);
    for (int row = 0; row < 3; ++row)
    {
        EXPECT_EQ(
            candidates->data(candidates->index(row, 0), ImportCandidateListModel::DisplayNameRole)
                .toString(),
            names.at(row));
        EXPECT_FALSE(candidates->inspected(row));
        EXPECT_TRUE(candidates->thumbnail(row).isNull());
    }
    presenter.imports()->ensureImportThumbnail(0);
    ASSERT_TRUE(wait_until(
        [&] { return candidates->inspected(0) && !candidates->thumbnail(0).isNull(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(
        candidates->data(candidates->index(0, 0), ImportCandidateListModel::EligibleRole).toBool());
    EXPECT_EQ(candidates->data(candidates->index(0, 0), ImportCandidateListModel::DisplayNameRole)
                  .toString(),
              QStringLiteral("alpha.png"));
}

TEST(StudioPresenterTest, ImportWorkspacePublishesGalleryPlaceholdersWhenImportStarts)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source_dir = QDir(directory.path()).filePath(QStringLiteral("source"));
    ASSERT_TRUE(QDir().mkpath(source_dir));
    const QStringList names{QStringLiteral("alpha.png"), QStringLiteral("beta.png"),
                            QStringLiteral("gamma.png")};
    for (const auto &name : names)
    {
        QImage image(48, 32, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(QColor(40 + static_cast<int>(names.indexOf(name)), 80, 120));
        ASSERT_TRUE(image.save(QDir(source_dir).filePath(name), "PNG"));
    }

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->openImportPage();
    presenter.imports()->setImportMode(QStringLiteral("add"));
    presenter.imports()->setImportSourceRoot(source_dir);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importScanActive() &&
                   presenter.imports()->importCandidates()->rowCount() == 3;
        },
        30000))
        << presenter.errorText().toStdString();
    presenter.imports()->startPlannedImport();
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.imports()->importPageOpen() && presenter.visibleCount() == 3; },
        30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(presenter.lastImportSelected());
    for (int row = 0; row < 3; ++row)
    {
        EXPECT_EQ(presenter.assets()
                      ->data(presenter.assets()->index(row, 0), AssetListModel::DisplayNameRole)
                      .toString(),
                  names.at(row));
    }
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.imports()->importPreflightActive() &&
                   !presenter.imports()->importWorkActive();
        },
        30000))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.lastImportCount(), 3);
    EXPECT_EQ(presenter.visibleCount(), 3);
    EXPECT_FALSE(presenter.selectedAssetId().isEmpty());
}

TEST(StudioPresenterTest, FolderListModelDoesNotResetWhenStructureIsUnchanged)
{
    ensure_qt_core();
    FolderListModel model;
    int resets = 0;
    int changes = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&] { ++resets; });
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&] { ++changes; });

    std::vector<FolderRecord> folders(2);
    folders[0].display_name = "All Photographs";
    folders[0].asset_count = 2;
    folders[1].id = "fld_a";
    folders[1].uri = "file:///tmp/pictures";
    folders[1].display_name = "pictures";
    folders[1].depth = 1;
    folders[1].asset_count = 2;
    model.setFolders(folders);
    ASSERT_EQ(model.rowCount(), 2);
    ASSERT_EQ(resets, 1);

    model.setFolders(folders);
    EXPECT_EQ(resets, 1);
    EXPECT_EQ(changes, 0);

    folders[1].asset_count = 3;
    model.setFolders(folders);
    EXPECT_EQ(resets, 1);
    ASSERT_GE(changes, 1);
    EXPECT_EQ(model.data(model.index(1, 0), FolderListModel::AssetCountRole).toInt(), 3);

    FolderRecord extra;
    extra.id = "fld_b";
    extra.uri = "file:///tmp/pictures/trip";
    extra.display_name = "trip";
    extra.depth = 2;
    extra.asset_count = 1;
    folders.push_back(extra);
    model.setFolders(folders);
    EXPECT_EQ(resets, 2);
    EXPECT_EQ(model.rowCount(), 3);
}

TEST(StudioPresenterTest, SelectingFolderDoesNotResetFolderTree)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString pictures = QDir(directory.path()).filePath(QStringLiteral("Pictures"));
    const QString trip = QDir(pictures).filePath(QStringLiteral("trip"));
    ASSERT_TRUE(QDir().mkpath(trip));
    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 90, 130));
    ASSERT_TRUE(image.save(QDir(pictures).filePath(QStringLiteral("a.png")), "PNG"));
    image.fill(QColor(130, 90, 40));
    ASSERT_TRUE(image.save(QDir(trip).filePath(QStringLiteral("b.png")), "PNG"));

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->importFilePaths({QDir(pictures).filePath(QStringLiteral("a.png")),
                                          QDir(trip).filePath(QStringLiteral("b.png"))});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 2 && !presenter.imports()->importWorkActive() &&
                   !presenter.busy() && presenter.folders()->rowCount() >= 3;
        },
        30000))
        << presenter.errorText().toStdString();

    QString pictures_uri;
    QString trip_uri;
    for (int row = 0; row < presenter.folders()->rowCount(); ++row)
    {
        const auto index = presenter.folders()->index(row, 0);
        const auto name =
            presenter.folders()->data(index, FolderListModel::DisplayNameRole).toString();
        const auto uri =
            presenter.folders()->data(index, FolderListModel::FolderUriRole).toString();
        if (name == QStringLiteral("Pictures"))
            pictures_uri = uri;
        else if (name == QStringLiteral("trip"))
            trip_uri = uri;
    }
    ASSERT_FALSE(pictures_uri.isEmpty());
    ASSERT_FALSE(trip_uri.isEmpty());

    int tree_resets = 0;
    QObject::connect(presenter.folders(), &QAbstractItemModel::modelReset, [&] { ++tree_resets; });
    const int tree_rows = presenter.folders()->rowCount();
    presenter.selectFolder(trip_uri);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedFolderUri() == trip_uri && presenter.visibleCount() == 1 &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(tree_resets, 0);
    EXPECT_EQ(presenter.folders()->rowCount(), tree_rows);

    presenter.selectFolder(QString());
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.selectedFolderUri().isEmpty() && presenter.visibleCount() == 2 &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();
    EXPECT_EQ(tree_resets, 0);
    EXPECT_EQ(presenter.folders()->rowCount(), tree_rows);

    presenter.folders()->toggleCollapsed(pictures_uri);
    EXPECT_GE(tree_resets, 1);
    EXPECT_LT(presenter.folders()->rowCount(), tree_rows);
}

TEST(StudioPresenterTest, ActualSizeInspectRoiFollowsLiveDevelopWithoutPan)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo =
        QDir(QString::fromUtf8(RAVO_REPOSITORY_ROOT))
            .filePath(QStringLiteral("Ravo/tests/fixtures/frozen/images/mire1.cr2"));
    ASSERT_TRUE(QFileInfo::exists(photo)) << photo.toStdString();

    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        },
        30000))
        << presenter.errorText().toStdString();
    presenter.openDevelop();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() &&
                   !presenter.inspect()->previewUrl().isEmpty();
        },
        30000))
        << presenter.errorText().toStdString();
    presenter.inspect()->setZoomMode(QStringLiteral("actual"));
    presenter.inspect()->requestInspectRoi(0.25, 0.25, 0.2, 0.15);
    ASSERT_TRUE(wait_until([&] { return !presenter.inspect()->inspectRoiUrl().isEmpty(); }, 30000))
        << presenter.errorText().toStdString();
    const QUrl first_roi = presenter.inspect()->inspectRoiUrl();
    ASSERT_FALSE(presenter.inspect()->inspectRoiImage().isNull());
    presenter.develop()->previewDevelopNumbers(QVariantMap{{QStringLiteral("exposure"), 0.75}});
    ASSERT_TRUE(
        wait_until([&] { return presenter.inspect()->inspectRoiUrl() != first_roi; }, 30000))
        << presenter.errorText().toStdString() << " first=" << first_roi.toString().toStdString()
        << " current=" << presenter.inspect()->inspectRoiUrl().toString().toStdString();
    EXPECT_TRUE(
        presenter.inspect()->inspectRoiUrl().toString().contains(QStringLiteral("inspectRoi")));
    EXPECT_FALSE(presenter.inspect()->inspectRoiImage().isNull());
    presenter.inspect()->adjustZoom(120);
    EXPECT_EQ(presenter.inspect()->zoomMode(), QStringLiteral("custom"));
    EXPECT_TRUE(presenter.inspect()->inspectRoiUrl().isEmpty());
    EXPECT_TRUE(presenter.inspect()->inspectRoiImage().isNull());
    EXPECT_EQ(presenter.inspect()->gpuRoiNativeSurface(), 0U);
    presenter.inspect()->setZoomMode(QStringLiteral("fit"));
    EXPECT_TRUE(presenter.inspect()->inspectRoiUrl().isEmpty());
    EXPECT_TRUE(presenter.inspect()->inspectRoiImage().isNull());
}

} // namespace
} // namespace ravo
