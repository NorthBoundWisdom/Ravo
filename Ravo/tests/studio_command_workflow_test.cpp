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

#include "studio_test_support.h"

namespace ravo
{
namespace
{
using namespace studio_test_support;

TEST(StudioCommands, LockingCropAspectKeepsCurrentRatio)
{
    ensure_qt_core();
    StudioPresenter presenter;
    EXPECT_EQ(presenter.develop()->cropAspect(), QStringLiteral("free"));
    EXPECT_NEAR(presenter.develop()->cropAspectRatio(), 0.0, 1e-12);
    presenter.develop()->setCropAspect(QStringLiteral("locked"));
    EXPECT_EQ(presenter.develop()->cropAspect(), QStringLiteral("locked"));
    EXPECT_NEAR(presenter.develop()->cropAspectRatio(), 1.0, 1e-6);
    presenter.develop()->setCropAspect(QStringLiteral("free"));
    EXPECT_EQ(presenter.develop()->cropAspect(), QStringLiteral("free"));
    EXPECT_NEAR(presenter.develop()->cropAspectRatio(), 0.0, 1e-12);
}

TEST(StudioCommands, BuiltinRegistryIsCompleteAndConflictFree)
{
    const auto errors = StudioCommandController::validateBuiltinDefinitions();
    EXPECT_TRUE(errors.isEmpty()) << errors.join(QLatin1Char('\n')).toStdString();
}

TEST(StudioCommands, MenuEntriesExposeTitledRowsForMenuBarPaths)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const QStringList paths = {QStringLiteral("file.library"),   QStringLiteral("file.transfer"),
                               QStringLiteral("file.recovery"),  QStringLiteral("file.window"),
                               QStringLiteral("edit.history"),   QStringLiteral("edit.reset"),
                               QStringLiteral("view.mode"),      QStringLiteral("view.zoom"),
                               QStringLiteral("view.compare"),   QStringLiteral("view.commands"),
                               QStringLiteral("photo.navigate"), QStringLiteral("photo.transform"),
                               QStringLiteral("photo.rating"),   QStringLiteral("photo.color"),
                               QStringLiteral("photo.review"),   QStringLiteral("photo.delete"),
                               QStringLiteral("help.about")};
    for (const auto &path : paths)
    {
        const auto entries = controller.menuEntries(path);
        ASSERT_FALSE(entries.isEmpty()) << path.toStdString();
        for (const auto &entry : entries)
        {
            const auto map = entry.toMap();
            EXPECT_FALSE(map.value(QStringLiteral("actionId")).toString().isEmpty())
                << path.toStdString();
            EXPECT_FALSE(map.value(QStringLiteral("title")).toString().isEmpty())
                << path.toStdString() << " "
                << map.value(QStringLiteral("actionId")).toString().toStdString();
        }
    }
    EXPECT_GE(controller.menuEntries(QStringLiteral("file.recovery")).size(), 11);
}

TEST(StudioCommands, CopyDebugTextRequiresSelectionOrPresetPath)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    const auto photo_copy = ids.value(QStringLiteral("photoCopyInfo")).toString();
    const auto parameters_copy = ids.value(QStringLiteral("photoCopyParameters")).toString();
    const auto reveal = ids.value(QStringLiteral("photoRevealInFileManager")).toString();
    const auto preset_copy = ids.value(QStringLiteral("presetCopyInfo")).toString();
    ASSERT_EQ(photo_copy, QStringLiteral("studio.photo.copy_info"));
    ASSERT_EQ(parameters_copy, QStringLiteral("studio.photo.copy_parameters"));
    ASSERT_EQ(reveal, QStringLiteral("studio.photo.reveal_in_file_manager"));
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

    const auto reveal_action = controller.action(reveal);
    EXPECT_FALSE(reveal_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(reveal_action.value(QStringLiteral("disabledReason")).toString().isEmpty());
    const auto reveal_rejected = controller.executeAction(reveal, QStringLiteral("control"));
    EXPECT_FALSE(reveal_rejected.value(QStringLiteral("accepted")).toBool());
    EXPECT_EQ(reveal_rejected.value(QStringLiteral("code")).toString(),
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

TEST(StudioCommands, ReturnConfirmsCropToolAndKeepsDevelopCrop)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString photo = directory.filePath(QStringLiteral("crop-enter.png"));
    QImage image(640, 480, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    ASSERT_TRUE(image.save(photo, "PNG"));

    StudioPresenter presenter;
    StudioCommandController controller(presenter);
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

    presenter.develop()->setCropToolActive(true);
    ASSERT_TRUE(presenter.develop()->cropToolActive());
    presenter.develop()->previewCropRect(0.15, 0.20, 0.55, 0.50);
    const double crop_x = presenter.develop()->editCropX();
    const double crop_y = presenter.develop()->editCropY();
    const double crop_width = presenter.develop()->editCropWidth();
    const double crop_height = presenter.develop()->editCropHeight();
    ASSERT_GT(crop_width, 0.0);
    ASSERT_GT(crop_height, 0.0);
    ASSERT_LT(crop_width, 0.999);
    ASSERT_LT(crop_height, 0.999);

    const auto loupe = QStringLiteral("studio.view.confirm_or_loupe");
    bool found_return = false;
    bool found_enter = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() != loupe)
            continue;
        const auto sequence = entry.value(QStringLiteral("sequence")).toString();
        found_return = found_return || sequence == QStringLiteral("Return");
        found_enter = found_enter || sequence == QStringLiteral("Enter");
    }
    EXPECT_TRUE(found_return);
    EXPECT_TRUE(found_enter);

    const auto confirmed = controller.executeAction(loupe, QStringLiteral("keyboard"));
    EXPECT_TRUE(confirmed.value(QStringLiteral("accepted")).toBool());
    EXPECT_FALSE(presenter.develop()->cropToolActive());
    EXPECT_EQ(presenter.browseMode(), QStringLiteral("develop"));
    EXPECT_NEAR(presenter.develop()->editCropX(), crop_x, 1e-6);
    EXPECT_NEAR(presenter.develop()->editCropY(), crop_y, 1e-6);
    EXPECT_NEAR(presenter.develop()->editCropWidth(), crop_width, 1e-6);
    EXPECT_NEAR(presenter.develop()->editCropHeight(), crop_height, 1e-6);

    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() && !presenter.busy() &&
                   presenter.develop()->canUndo();
        }))
        << presenter.errorText().toStdString();
    const QString asset_id = presenter.selectedAssetId();
    presenter.setBrowseMode(QStringLiteral("grid"));
    presenter.selectAsset(QString{});
    presenter.selectAsset(asset_id);
    presenter.setBrowseMode(QStringLiteral("develop"));
    ASSERT_TRUE(wait_until(
        [&]
        {
            return !presenter.inspect()->previewLoading() && !presenter.busy() &&
                   std::abs(presenter.develop()->editCropWidth() - crop_width) < 1e-6;
        }))
        << presenter.errorText().toStdString();
    EXPECT_FALSE(presenter.develop()->cropToolActive());
    EXPECT_NEAR(presenter.develop()->editCropX(), crop_x, 1e-6);
    EXPECT_NEAR(presenter.develop()->editCropY(), crop_y, 1e-6);
    EXPECT_NEAR(presenter.develop()->editCropHeight(), crop_height, 1e-6);
}

TEST(StudioCommands, SelectAllShortcutSelectsLoadedPhotosAndYieldsToTextInput)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto action_id = controller.ids().value(QStringLiteral("photoSelectAll")).toString();
    ASSERT_EQ(action_id, QStringLiteral("studio.photo.select_all"));

    const auto closed = controller.action(action_id);
    EXPECT_FALSE(closed.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(closed.value(QStringLiteral("disabledReason")).toString(),
              QCoreApplication::translate("StudioCommands", "Open a library first."));
    EXPECT_EQ(closed.value(QStringLiteral("shortcutText")).toString(),
              QKeySequence::fromString(QStringLiteral("Ctrl+A"), QKeySequence::PortableText)
                  .toString(QKeySequence::NativeText));

    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    const auto empty = controller.action(action_id);
    EXPECT_FALSE(empty.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(empty.value(QStringLiteral("disabledReason")).toString(),
              QCoreApplication::translate("StudioCommands", "No photos to select."));

    QImage image(32, 24, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(QColor(40, 80, 120));
    const QString first_photo = directory.filePath(QStringLiteral("one.png"));
    const QString second_photo = directory.filePath(QStringLiteral("two.png"));
    const QString third_photo = directory.filePath(QStringLiteral("three.png"));
    ASSERT_TRUE(image.save(first_photo, "PNG"));
    image.fill(QColor(120, 80, 40));
    ASSERT_TRUE(image.save(second_photo, "PNG"));
    image.fill(QColor(80, 120, 40));
    ASSERT_TRUE(image.save(third_photo, "PNG"));
    presenter.imports()->importFilePaths({first_photo, second_photo, third_photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 3 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy();
        }))
        << presenter.errorText().toStdString();

    const QString first_id = presenter.assets()->assetIdAt(0);
    const QString second_id = presenter.assets()->assetIdAt(1);
    const QString third_id = presenter.assets()->assetIdAt(2);
    ASSERT_FALSE(first_id.isEmpty());
    ASSERT_FALSE(second_id.isEmpty());
    ASSERT_FALSE(third_id.isEmpty());
    presenter.selectAsset(second_id);
    ASSERT_EQ(presenter.selectedCount(), 1);
    ASSERT_EQ(presenter.selectedAssetId(), second_id);

    bool found_shortcut = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() != action_id)
            continue;
        found_shortcut = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(), QStringLiteral("Ctrl+A"));
        EXPECT_TRUE(entry.value(QStringLiteral("enabled")).toBool());
    }
    ASSERT_TRUE(found_shortcut);

    const auto selected = controller.executeAction(action_id, QStringLiteral("keyboard"));
    EXPECT_TRUE(selected.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    EXPECT_EQ(presenter.selectedCount(), 3);
    EXPECT_EQ(presenter.selectedAssetId(), second_id);
    EXPECT_TRUE(presenter.isAssetSelected(first_id));
    EXPECT_TRUE(presenter.isAssetSelected(second_id));
    EXPECT_TRUE(presenter.isAssetSelected(third_id));

    presenter.setBrowseMode(QStringLiteral("loupe"));
    const auto from_loupe = controller.executeAction(action_id, QStringLiteral("keyboard"));
    EXPECT_TRUE(from_loupe.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(wait_until([&] { return !presenter.busy(); }));
    EXPECT_EQ(presenter.selectedCount(), 3);

    controller.setTextInputActive(true);
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() == action_id)
            EXPECT_FALSE(entry.value(QStringLiteral("enabled")).toBool());
    }
}

TEST(StudioCommands, PhotoInformationShortcutTogglesSessionOwnedOverlay)
{
    ensure_qt_core();
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    StudioPresenter presenter;
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }))
        << presenter.errorText().toStdString();
    StudioCommandController controller(presenter);
    const auto action_id = controller.ids().value(QStringLiteral("viewPhotoInfo")).toString();
    ASSERT_EQ(action_id, QStringLiteral("studio.view.toggle_photo_info"));

    bool found_i = false;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() != action_id)
            continue;
        found_i = true;
        EXPECT_EQ(entry.value(QStringLiteral("sequence")).toString(), QStringLiteral("I"));
        EXPECT_TRUE(entry.value(QStringLiteral("enabled")).toBool());
    }
    ASSERT_TRUE(found_i);
    EXPECT_FALSE(controller.photoInfoVisible());
    EXPECT_TRUE(controller.action(action_id).value(QStringLiteral("checkable")).toBool());
    EXPECT_FALSE(controller.action(action_id).value(QStringLiteral("checked")).toBool());

    const auto shown = controller.executeAction(action_id, QStringLiteral("keyboard"));
    EXPECT_TRUE(shown.value(QStringLiteral("accepted")).toBool());
    EXPECT_TRUE(controller.photoInfoVisible());
    EXPECT_TRUE(controller.action(action_id).value(QStringLiteral("checked")).toBool());

    const auto hidden = controller.executeAction(action_id, QStringLiteral("keyboard"));
    EXPECT_TRUE(hidden.value(QStringLiteral("accepted")).toBool());
    EXPECT_FALSE(controller.photoInfoVisible());

    controller.setTextInputActive(true);
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        if (entry.value(QStringLiteral("actionId")).toString() == action_id)
            EXPECT_FALSE(entry.value(QStringLiteral("enabled")).toBool());
    }
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
    EXPECT_EQ(QCoreApplication::translate("StudioCommands", "Photo Information"),
              QStringLiteral("照片信息"));
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
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Presence"), QStringLiteral("鲜艳度"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color Grading"),
              QStringLiteral("颜色分级"));
    EXPECT_EQ(QCoreApplication::translate("DevelopPanel", "Color Mixer"),
              QStringLiteral("颜色混合器"));
    EXPECT_EQ(QCoreApplication::translate("LibrarySidePanel", "Last Imported Photos"),
              QStringLiteral("上次导入的照片"));
    EXPECT_EQ(QCoreApplication::translate("ImportPage", "Build Previews"),
              QStringLiteral("构建预览"));
    EXPECT_EQ(QCoreApplication::translate("ImportPage", "Preserve hierarchy"),
              QStringLiteral("保留目录层级"));
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
        const auto settings_title = QCoreApplication::translate("StudioCommands", "Settings...");
        const auto native_prefix = QCoreApplication::translate("QCocoaMenuItem", "Setting");
        EXPECT_TRUE(settings_title.startsWith(native_prefix, Qt::CaseInsensitive))
            << language.toStdString() << ": " << settings_title.toStdString();
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
    const auto formats = presenter.exports()->exportFormatChoices();
    ASSERT_EQ(formats.size(), 4);
    EXPECT_EQ(formats.at(0).toMap().value(QStringLiteral("id")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(formats.at(3).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("original"));
    const auto defaults = presenter.exports()->exportDefaultOptions();
    EXPECT_EQ(defaults.value(QStringLiteral("format")).toString(), QStringLiteral("jpeg"));
    EXPECT_EQ(defaults.value(QStringLiteral("quality")).toInt(), 95);
    const auto sizing = defaults.value(QStringLiteral("sizing")).toMap();
    EXPECT_EQ(sizing.value(QStringLiteral("mode")).toString(), QStringLiteral("original"));
    EXPECT_EQ(sizing.value(QStringLiteral("longEdge")).toInt(), 2560);
    EXPECT_EQ(sizing.value(QStringLiteral("width")).toInt(), 2560);
    EXPECT_EQ(sizing.value(QStringLiteral("height")).toInt(), 1440);
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
    const auto metadata_modes = presenter.exports()->exportMetadataModeChoices();
    ASSERT_EQ(metadata_modes.size(), 3);
    EXPECT_EQ(metadata_modes.at(1).toMap().value(QStringLiteral("id")).toString(),
              QStringLiteral("no-location"));
    const auto bounds = presenter.exports()->exportOptionBounds();
    EXPECT_EQ(bounds.value(QStringLiteral("jpegQualityMin")).toInt(), 5);
    EXPECT_EQ(bounds.value(QStringLiteral("tiffResolutionDpiMax")).toInt(), 9600);
}

TEST(StudioPresenterTest, OutputDitherPresentationOwnsAllFrozenMethods)
{
    ensure_qt_core();
    StudioPresenter presenter;
    const auto dither = presenter.develop()->editOutputDither();
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
    const auto canvas = presenter.develop()->editCanvas();
    EXPECT_FALSE(presenter.develop()->editCanvasEnabled());
    EXPECT_FALSE(canvas.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(canvas.value(QStringLiteral("colorChoices")).toList().size(), 5);
    const auto frame = presenter.develop()->editOutputFrame();
    EXPECT_FALSE(frame.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(frame.value(QStringLiteral("orientationChoices")).toList().size(), 3);
    EXPECT_EQ(frame.value(QStringLiteral("basisChoices")).toList().size(), 5);
    const auto watermark = presenter.develop()->editWatermark();
    EXPECT_FALSE(watermark.value(QStringLiteral("enabled")).toBool());
    EXPECT_EQ(watermark.value(QStringLiteral("text")).toString(), QStringLiteral("RAVO"));
    EXPECT_EQ(watermark.value(QStringLiteral("alignmentChoices")).toList().size(), 9);
    const auto zones = presenter.develop()->editColorZones();
    EXPECT_FALSE(zones.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(zones.value(QStringLiteral("editable")).toBool());
    EXPECT_EQ(zones.value(QStringLiteral("selectByChoices")).toList().size(), 3);
    EXPECT_EQ(zones.value(QStringLiteral("interpolationChoices")).toList().size(), 3);
    const auto monochrome = presenter.develop()->editMonochromeFilter();
    EXPECT_FALSE(monochrome.value(QStringLiteral("enabled")).toBool());
    EXPECT_DOUBLE_EQ(monochrome.value(QStringLiteral("size")).toDouble(), 2.0);
    EXPECT_DOUBLE_EQ(monochrome.value(QStringLiteral("mix")).toDouble(), 1.0);
    const auto split = presenter.develop()->editSplitToning();
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
    presenter.imports()->importFilePaths({photo});
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.visibleCount() == 1 && !presenter.selectedAssetId().isEmpty() &&
                   !presenter.busy() && !presenter.imports()->importWorkActive() &&
                   !presenter.inspect()->previewLoading();
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
        presenter.imports()->importFilePaths({photo});
        ASSERT_TRUE(wait_until(
            [&]
            {
                return presenter.visibleCount() == 1 && !presenter.imports()->importWorkActive() &&
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
    QObject::connect(presenter.imports(), &StudioImportWorkspace::libraryWorkChanged, &presenter,
                     [&]
                     {
                         if (!cancelled && presenter.imports()->importWorkActive() &&
                             presenter.imports()->importWorkCompleted() == 1)
                         {
                             cancelled = true;
                             presenter.cancelCatalogOperation();
                         }
                     });
    presenter.imports()->importFilePaths(photos);
    ASSERT_TRUE(
        wait_until([&] { return cancelled && !presenter.imports()->importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_EQ(presenter.visibleCount(), 1);
    EXPECT_TRUE(presenter.lastImportAvailable());
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 1);
    EXPECT_TRUE(presenter.statusText().contains(QStringLiteral("cancel"), Qt::CaseInsensitive));
    for (int index = 0; index < photos.size(); ++index)
    {
        QFile file(photos[index]);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        EXPECT_EQ(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256),
                  hashes[static_cast<std::size_t>(index)]);
    }
}

TEST(StudioPresenterTest, ImportKeepsGalleryStableThenPublishesOneLastImportCollection)
{
    ensure_qt_core();
    ravo::init_logging("ravo-desktop-command-tests");
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto make_photo = [&](const QString &name, const QColor &color)
    {
        const QString path = directory.filePath(name);
        QImage image(64, 48, QImage::Format_RGB888);
        image.setColorSpace(QColorSpace(QColorSpace::SRgb));
        image.fill(color);
        EXPECT_TRUE(image.save(path, "PNG"));
        return path;
    };
    const QString baseline = make_photo(QStringLiteral("baseline.png"), QColor(20, 40, 60));
    const QString corrupt = directory.filePath(QStringLiteral("batch-corrupt.png"));
    QFile corrupt_file(corrupt);
    ASSERT_TRUE(corrupt_file.open(QIODevice::WriteOnly));
    ASSERT_EQ(corrupt_file.write("not a png"), 9);
    corrupt_file.close();
    const QStringList batch{make_photo(QStringLiteral("batch-a.png"), QColor(80, 20, 30)),
                            make_photo(QStringLiteral("batch-b.png"), QColor(20, 90, 40)),
                            make_photo(QStringLiteral("batch-c.png"), QColor(30, 40, 120)),
                            corrupt};

    StudioPresenter presenter;
    StudioCommandController commands(presenter);
    presenter.createCatalogFromPath(directory.filePath(QStringLiteral("library.sqlite")));
    ASSERT_TRUE(wait_until([&] { return presenter.catalogOpen() && !presenter.busy(); }));
    presenter.imports()->importFilePaths({baseline});
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    ASSERT_TRUE(presenter.lastImportSelected());
    presenter.selectFolder(QString{});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.lastImportSelected() && presenter.visibleCount() == 1; }));

    int resets_while_importing = 0;
    int inserts_while_importing = 0;
    bool observed_intermediate_progress = false;
    QObject::connect(presenter.assets(), &QAbstractItemModel::modelReset, &presenter,
                     [&]
                     {
                         if (presenter.imports()->importWorkActive())
                             ++resets_while_importing;
                     });
    QObject::connect(presenter.assets(), &QAbstractItemModel::rowsInserted, &presenter,
                     [&]
                     {
                         if (presenter.imports()->importWorkActive())
                             ++inserts_while_importing;
                     });
    QObject::connect(presenter.imports(), &StudioImportWorkspace::libraryWorkChanged, &presenter,
                     [&]
                     {
                         if (presenter.imports()->importWorkActive() &&
                             presenter.imports()->importWorkCompleted() > 0 &&
                             presenter.imports()->importWorkCompleted() <
                                 presenter.imports()->importWorkTotal())
                         {
                             observed_intermediate_progress = true;
                             EXPECT_EQ(presenter.assets()->rowCount(), 1);
                         }
                     });

    presenter.imports()->importFilePaths(batch);
    ASSERT_TRUE(wait_until([&] { return !presenter.imports()->importWorkActive(); }, 30000))
        << presenter.errorText().toStdString();
    EXPECT_TRUE(observed_intermediate_progress);
    EXPECT_EQ(resets_while_importing, 0);
    EXPECT_EQ(inserts_while_importing, 0);
    EXPECT_TRUE(presenter.lastImportAvailable());
    EXPECT_TRUE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 3);
    ASSERT_EQ(presenter.visibleCount(), 3);

    std::vector<QString> names;
    for (int row = 0; row < presenter.assets()->rowCount(); ++row)
    {
        names.push_back(
            presenter.assets()
                ->data(presenter.assets()->index(row, 0), AssetListModel::DisplayNameRole)
                .toString());
    }
    std::ranges::sort(names);
    EXPECT_EQ(names,
              std::vector<QString>({QStringLiteral("batch-a.png"), QStringLiteral("batch-b.png"),
                                    QStringLiteral("batch-c.png")}));

    presenter.selectFolder(QString{});
    ASSERT_TRUE(wait_until(
        [&] { return !presenter.lastImportSelected() && presenter.visibleCount() == 4; }));
    const auto selected_last = commands.executeCommand(
        commands.ids().value(QStringLiteral("librarySelectLastImport")).toString(), {},
        QStringLiteral("control"));
    ASSERT_TRUE(selected_last.value(QStringLiteral("accepted")).toBool());
    ASSERT_TRUE(wait_until(
        [&] { return presenter.lastImportSelected() && presenter.visibleCount() == 3; }));

    const QString next_catalog = directory.filePath(QStringLiteral("next-library.sqlite"));
    presenter.createCatalogFromPath(next_catalog);
    ASSERT_TRUE(wait_until(
        [&]
        {
            return presenter.catalogPath() == next_catalog && !presenter.busy() &&
                   presenter.visibleCount() == 0;
        }));
    EXPECT_FALSE(presenter.lastImportAvailable());
    EXPECT_FALSE(presenter.lastImportSelected());
    EXPECT_EQ(presenter.lastImportCount(), 0);
}

TEST(StudioCommands, EditInCommandsExposePrepareAndCheckReturned)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto ids = controller.ids();
    EXPECT_EQ(ids.value(QStringLiteral("photoEditIn")).toString(),
              QStringLiteral("studio.photo.edit_in"));
    EXPECT_EQ(ids.value(QStringLiteral("photoEditInPrepare")).toString(),
              QStringLiteral("studio.photo.edit_in_prepare"));
    EXPECT_EQ(ids.value(QStringLiteral("photoEditInCheckReturned")).toString(),
              QStringLiteral("studio.photo.edit_in_check_returned"));
    EXPECT_EQ(ids.value(QStringLiteral("photoEditInClearSession")).toString(),
              QStringLiteral("studio.photo.edit_in_clear_session"));
    EXPECT_EQ(ids.value(QStringLiteral("photoEditInAbandon")).toString(),
              QStringLiteral("studio.photo.edit_in_abandon"));
    EXPECT_EQ(ids.value(QStringLiteral("photoEditInReopen")).toString(),
              QStringLiteral("studio.photo.edit_in_reopen"));
    EXPECT_EQ(ids.value(QStringLiteral("photoEditInRefreshStatus")).toString(),
              QStringLiteral("studio.photo.edit_in_refresh_status"));

    const auto edit_in = ids.value(QStringLiteral("photoEditIn")).toString();
    const auto edit_action = controller.action(edit_in);
    EXPECT_FALSE(edit_action.value(QStringLiteral("enabled")).toBool());
    EXPECT_FALSE(edit_action.value(QStringLiteral("disabledReason")).toString().isEmpty());

    const auto prepare = ids.value(QStringLiteral("photoEditInPrepare")).toString();
    const auto rejected = controller.executeCommand(
        prepare, QVariantMap{{QStringLiteral("editorId"), QString{}}}, QStringLiteral("control"));
    EXPECT_FALSE(rejected.value(QStringLiteral("accepted")).toBool());

    const auto defaults = presenter.externalEditorDefaultOptions();
    EXPECT_EQ(defaults.value(QStringLiteral("profile")).toString(), QStringLiteral("srgb"));
    EXPECT_EQ(defaults.value(QStringLiteral("tiffSampleType")).toString(),
              QStringLiteral("uint16"));
    EXPECT_TRUE(defaults.value(QStringLiteral("autoStack")).toBool());
    EXPECT_TRUE(presenter.externalEditorSession().isEmpty());
}

TEST(StudioCommands, CullReviewPickRejectUnflagShortcutsWireToAdr0150)
{
    ensure_qt_core();
    StudioPresenter presenter;
    StudioCommandController controller(presenter);
    const auto pick = controller.ids().value(QStringLiteral("photoPick")).toString();
    const auto reject = controller.ids().value(QStringLiteral("photoReject")).toString();
    const auto unflag = controller.ids().value(QStringLiteral("photoUnflag")).toString();
    ASSERT_EQ(pick, QLatin1String("studio.photo.toggle_pick"));
    ASSERT_EQ(reject, QLatin1String("studio.photo.toggle_reject"));
    ASSERT_EQ(unflag, QLatin1String("studio.photo.cull_unflag"));

    QString pick_seq;
    QString reject_seq;
    QString unflag_seq;
    for (const auto &entry_value : controller.shortcutEntries())
    {
        const auto entry = entry_value.toMap();
        const auto action_id = entry.value(QStringLiteral("actionId")).toString();
        const auto sequence = entry.value(QStringLiteral("sequence")).toString();
        if (action_id == pick)
            pick_seq = sequence;
        else if (action_id == reject)
            reject_seq = sequence;
        else if (action_id == unflag)
            unflag_seq = sequence;
    }
    EXPECT_EQ(pick_seq, QStringLiteral("P"));
    EXPECT_EQ(reject_seq, QStringLiteral("X"));
    EXPECT_EQ(unflag_seq, QStringLiteral("U"));

    for (int rating = 0; rating <= 5; ++rating)
    {
        const auto rate_id = QStringLiteral("studio.photo.rating_%1").arg(rating);
        bool found = false;
        for (const auto &entry_value : controller.shortcutEntries())
        {
            const auto entry = entry_value.toMap();
            if (entry.value(QStringLiteral("actionId")).toString() == rate_id &&
                entry.value(QStringLiteral("sequence")).toString() == QString::number(rating))
            {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << rating;
    }

    EXPECT_EQ(controller.ids().value(QStringLiteral("photoRate")).toString(),
              QLatin1String("studio.photo.set_rating"));
    EXPECT_EQ(controller.ids().value(QStringLiteral("photoColor")).toString(),
              QLatin1String("studio.photo.set_color"));
}

} // namespace
} // namespace ravo
