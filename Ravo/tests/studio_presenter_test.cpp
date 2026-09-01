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
#include "ravo/desktop/library_set_list_model.h"
#include "ravo/desktop/studio_command_controller.h"
#include "ravo/desktop/studio_live_session_controller.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop_mask.h"
#include "ravo/recipe/style.h"
#include "studio_debug_info.h"
#include "studio_language_manager.h"
#include "studio_qml_test_support.h"

#include "studio_test_support.h"

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
    presenter.importFilePaths({first_photo, second_photo});
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
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
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
    presenter.copyParametersSelected(QVariantList{QStringLiteral("exposure")});
    ASSERT_TRUE(presenter.hasCopiedParameters());

    presenter.selectAsset(second_id);
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until([&] { return !presenter.previewLoading(); }))
        << presenter.errorText().toStdString();
    presenter.setDevelopNumber(QStringLiteral("saturation"), -0.3);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.previewLoading() && std::abs(presenter.editSaturation() + 0.3) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    presenter.toggleAssetSelected(first_id);
    ASSERT_EQ(presenter.selectedCount(), 2);
    presenter.pasteParametersToSelection();
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.catalogOperationActive() && !presenter.previewLoading() &&
                   std::abs(presenter.editExposure() - 0.75) < 1e-9;
        }))
        << presenter.errorText().toStdString();
    EXPECT_NEAR(presenter.editSaturation(), 0.4, 1e-9);
    EXPECT_FALSE(presenter.canUndo());
    EXPECT_EQ(
        presenter.statusText(),
        QCoreApplication::translate("StudioPresenter", "Parameters applied to the selection."));

    presenter.selectAsset(second_id);
    presenter.setBrowseMode(QStringLiteral("develop"));
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
    EXPECT_EQ(presenter.scopeMode(), QStringLiteral("parade"));
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

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
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editColorChecker.patchCount > 0")));
    EXPECT_FALSE(source.contains(QStringLiteral("panel.hasSelection && count > 0")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"colorChecker\")")));
}

TEST(StudioQmlContract, ColorCorrectionUsesHardBoundsAndGenericDevelopIntents)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

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
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editColorCorrection")));
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

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
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editColorContrast")));
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

    const auto section_begin = source.indexOf(QStringLiteral("objectName: \"velviaEnabled\""));
    const auto section_end = source.indexOf(QStringLiteral("Color Balance RGB"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editVelviaParams.enabled")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editVelviaParams.strength")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editVelviaParams.bias")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editVelviaParams.masked")));
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

    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    const auto begin = source.indexOf(QStringLiteral("objectName: \"lut3dFile\""));
    const auto end = source.indexOf(QStringLiteral("Color Balance RGB"), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(source.contains(QStringLiteral("QmlFileDialogPage")));
    EXPECT_TRUE(source.contains(QStringLiteral("Cube LUT (*.cube *.CUBE)")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editLut3d.filePath")));
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

    const auto section_begin = source.indexOf(QStringLiteral("colorHarmonizerEnabled"));
    const auto section_end =
        source.indexOf(QStringLiteral("qsTr(\"Color Reconstruction\")"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editColorHarmonizer")));
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
        QStringLiteral("modelData.index < panel.presenter.editColorHarmonizer.customNodeCount")));
    EXPECT_TRUE(section.contains(QStringLiteral("resetControl(\"colorHarmonizer\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
    EXPECT_FALSE(section.contains(QStringLiteral("auto-detect")));
    EXPECT_FALSE(section.contains(QStringLiteral("histogram")));
    EXPECT_FALSE(section.contains(QStringLiteral("picker")));
    EXPECT_FALSE(section.contains(QStringLiteral("harmony guide"), Qt::CaseInsensitive));
    EXPECT_TRUE(section.contains(QStringLiteral("MaskEditor")));
    EXPECT_TRUE(section.contains(QStringLiteral("editColorHarmonizerMask")));
    EXPECT_TRUE(source.contains(QStringLiteral("editColorBalanceRgbMask")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorBalanceRgbMaskEditor\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("// MaskEditor.qml")));
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());

    const auto section_begin = source.indexOf(QStringLiteral("colorReconstructionEnabled"));
    const auto section_end = source.indexOf(QStringLiteral("qsTr(\"Color Zones\")"), section_begin);
    ASSERT_GE(section_begin, 0);
    ASSERT_GT(section_end, section_begin);
    const auto section = source.mid(section_begin, section_end - section_begin);
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editColorReconstruction")));
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
    const auto advanced = source.indexOf(QStringLiteral("qsTr(\"Color · Advanced\")"), monochrome);
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    const auto begin = source.indexOf(QStringLiteral("title: qsTr(\"Sharpen\")"));
    const auto end = source.indexOf(QStringLiteral("title: qsTr(\"Clarity\")"), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editSharpen")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editSharpenRadius")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editSharpenThreshold")));
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

    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    const auto detail = source.indexOf(QStringLiteral("sectionId: \"detail\""));
    const auto texture = source.indexOf(QStringLiteral("title: qsTr(\"Texture\")"), detail);
    const auto sharpen = source.indexOf(QStringLiteral("title: qsTr(\"Sharpen\")"), detail);
    ASSERT_GE(detail, 0);
    ASSERT_GT(texture, detail);
    ASSERT_GT(sharpen, texture);
    const auto section = source.mid(texture, sharpen - texture);
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editTexture.strength")));
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
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    const auto begin = source.indexOf(QStringLiteral("title: qsTr(\"Dehaze\")"));
    const auto end = source.indexOf(QStringLiteral("sectionId: \"detail\""), begin);
    ASSERT_GE(begin, 0);
    ASSERT_GT(end, begin);
    const auto section = source.mid(begin, end - begin);
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editDehaze")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editDehazeDistance")));
    EXPECT_TRUE(section.contains(QStringLiteral("panel.presenter.editDehazeAdaptive")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"dehaze\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("previewDevelopNumber(\"dehazeDistance\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("setDevelopNumber(\"dehazeAdaptive\"")));
    EXPECT_TRUE(section.contains(QStringLiteral("qsTr(\"Adaptive window scale\")")));
    EXPECT_FALSE(section.contains(QStringLiteral("OpenCL")));
}

TEST(StudioQmlContract, OutputDitherUsesPresenterMethodsWithoutQmlPixelMath)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputDitherEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputDitherMethod\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editOutputDither.methodChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputDitherMethodIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputDitherDamping")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"outputDither\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("Auto dithers integer exports")));
    EXPECT_FALSE(source.contains(QStringLiteral("7.0 / 16.0")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"canvasEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("id: canvasEnabledBox")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editCanvasEnabled")));
    EXPECT_TRUE(source.contains(QStringLiteral("qsTr(\"Enlarge Canvas\")")));
    EXPECT_FALSE(source.contains(QStringLiteral("qsTr(\"Enable enlarged canvas\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("visible: canvasEnabledBox.checked")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editCanvas.colorChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("canvasColorIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"canvas\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"outputFrameEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editOutputFrame.basisChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("outputFrameLineOffset")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"outputFrame\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"watermarkEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"watermarkText\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editWatermark.alignmentChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("setDevelopText(\"watermarkText\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"watermark\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"colorZonesEnabled\"")));
    EXPECT_TRUE(source.contains(QStringLiteral("panel.presenter.editColorZones.selectByChoices")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorZonesChroma")));
    EXPECT_TRUE(source.contains(QStringLiteral("colorZonesHueInterpolationIndex")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"colorZones\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"monochromeEnabled\"")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("panel.presenter.editMonochromeFilter[modelData.key]")));
    EXPECT_TRUE(source.contains(QStringLiteral("monochromeHighlights")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"monochrome\")")));
    EXPECT_TRUE(source.contains(QStringLiteral("objectName: \"splitToningEnabled\"")));
    EXPECT_TRUE(
        source.contains(QStringLiteral("panel.presenter.editSplitToning.shadowSaturation")));
    EXPECT_TRUE(source.contains(QStringLiteral("splitHighlightSaturation")));
    EXPECT_TRUE(source.contains(QStringLiteral("splitCompress")));
    EXPECT_TRUE(source.contains(QStringLiteral("resetControl(\"splitToning\")")));
}

TEST(StudioQmlContract, RawSectionExposesSensorAwareDemosaicAndWaveletDenoise)
{
    const auto source = combined_develop_qml_source();
    ASSERT_FALSE(source.isEmpty());
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

} // namespace
} // namespace ravo
