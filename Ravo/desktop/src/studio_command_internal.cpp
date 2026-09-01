#include "ravo/desktop/studio_command_controller.h"

#include "studio_command_internal.h"
#include "studio_command_ids.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QHash>
#include <QKeySequence>
#include <QMetaType>
#include <QRegularExpression>
#include <QSet>
#include <QVector>
#include <Qt>

#include "ravo/desktop/asset_list_model.h"
#include "ravo/desktop/studio_presenter.h"
#include "ravo/recipe/develop.h"
#include "studio_qt.h"

namespace ravo::command_internal
{

QString tr_command(const QString &source)
{
    const auto bytes = source.toUtf8();
    return QCoreApplication::translate("StudioCommands", bytes.constData());
}

[[maybe_unused]] const char *const kStudioCommandTranslationSources[] = {
    QT_TRANSLATE_NOOP("StudioCommands", "This command takes no argument."),
    QT_TRANSLATE_NOOP("StudioCommands", "A non-empty string is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "An object argument is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "A non-empty list of paths is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "Every import path must be a non-empty string."),
    QT_TRANSLATE_NOOP("StudioCommands", "%1 must be a finite number."),
    QT_TRANSLATE_NOOP("StudioCommands", "Missing command argument field: %1."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown %1 value."),
    QT_TRANSLATE_NOOP("StudioCommands", "A current confirmation token is required."),
    QT_TRANSLATE_NOOP("StudioCommands",
                      "The photo selection changed after confirmation was requested."),
    QT_TRANSLATE_NOOP("StudioCommands", "Preset path and name must be non-empty strings."),
    QT_TRANSLATE_NOOP("StudioCommands", "The preset changed after confirmation was requested."),
    QT_TRANSLATE_NOOP("StudioCommands", "Export path must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Export directory must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Export filename template must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown rating filter mode."),
    QT_TRANSLATE_NOOP("StudioCommands", "Rating filter value must be an integer from 0 to 5."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown library sort field."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown library sort direction."),
    QT_TRANSLATE_NOOP("StudioCommands", "An asset ID is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "Rating must be an integer between 0 and 5."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown color label."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown writable metadata field."),
    QT_TRANSLATE_NOOP("StudioCommands", "A non-negative integer history ID is required."),
    QT_TRANSLATE_NOOP("StudioCommands", "Snapshot label must be a string."),
    QT_TRANSLATE_NOOP("StudioCommands", "Navigation argument must be 'range'."),
    QT_TRANSLATE_NOOP("StudioCommands", "Thumbnail size must be an integer between 120 and 320."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop control name must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop fields must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "White-balance pick state must be boolean."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop section name must not be empty."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop section enabled must be boolean."),
    QT_TRANSLATE_NOOP("StudioCommands", "Develop value must be finite."),
    QT_TRANSLATE_NOOP("StudioCommands", "Tone curve points must be a list."),
    QT_TRANSLATE_NOOP("StudioCommands", "Crop width and height must be positive."),
    QT_TRANSLATE_NOOP("StudioCommands", "Crop state must be boolean."),
    QT_TRANSLATE_NOOP("StudioCommands", "Close Settings to use this command."),
    QT_TRANSLATE_NOOP("StudioCommands", "Open a library first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Wait for library work to finish."),
    QT_TRANSLATE_NOOP("StudioCommands", "Select a photo first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Open a photo first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Open Edit first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Nothing to undo."),
    QT_TRANSLATE_NOOP("StudioCommands", "Nothing to redo."),
    QT_TRANSLATE_NOOP("StudioCommands", "No modified parameters to copy."),
    QT_TRANSLATE_NOOP("StudioCommands", "Copy parameters first."),
    QT_TRANSLATE_NOOP("StudioCommands", "Select at least two photos first."),
    QT_TRANSLATE_NOOP("StudioCommands", "The selected originals cannot be deleted."),
    QT_TRANSLATE_NOOP("StudioCommands", "No catalog operation is running."),
    QT_TRANSLATE_NOOP("StudioCommands", "Command unavailable in the current context."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unreject"),
    QT_TRANSLATE_NOOP("StudioCommands", "Done Cropping"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown action: %1"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown command: %1"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown command source: %1")};

QString primary_key(const QString &key, const bool shift, const bool alt)
{
    // QKeySequence PortableText uses Ctrl as the cross-platform primary
    // accelerator. Qt renders and dispatches it as Command on macOS.
    QStringList parts{QStringLiteral("Ctrl")};
    if (shift)
        parts.push_back(QStringLiteral("Shift"));
    if (alt)
        parts.push_back(QStringLiteral("Alt"));
    parts.push_back(key);
    return parts.join(QLatin1Char('+'));
}

QString native_key(const QString &portable)
{
    return QKeySequence::fromString(portable, QKeySequence::PortableText)
        .toString(QKeySequence::NativeText);
}

QStringList command_ids()
{
    return {QLatin1String(command::kLibraryCreate),
            QLatin1String(command::kLibraryCreatePath),
            QLatin1String(command::kLibraryOpen),
            QLatin1String(command::kLibraryOpenPath),
            QLatin1String(command::kLibraryImportFiles),
            QLatin1String(command::kLibraryImportPaths),
            QLatin1String(command::kLibraryImportFolder),
            QLatin1String(command::kLibraryImportFolderPath),
            QLatin1String(command::kLibraryExport),
            QLatin1String(command::kLibraryExportWrite),
            QLatin1String(command::kLibraryExportBatchWrite),
            QLatin1String(command::kLibraryRecoveryStatus),
            QLatin1String(command::kLibraryRecoverySync),
            QLatin1String(command::kLibraryBackupCreate),
            QLatin1String(command::kLibraryBackupCreatePath),
            QLatin1String(command::kLibraryBackupVerify),
            QLatin1String(command::kLibraryBackupVerifyPath),
            QLatin1String(command::kLibraryBackupRestore),
            QLatin1String(command::kLibraryBackupRestorePaths),
            QLatin1String(command::kLibraryBackupSchedule),
            QLatin1String(command::kLibraryBackupSchedulePath),
            QLatin1String(command::kLibraryBackupScheduleDisable),
            QLatin1String(command::kLibraryBackupScheduleRun),
            QLatin1String(command::kLibraryPreviewRebuildSelected),
            QLatin1String(command::kLibraryPreviewRebuildAll),
            QLatin1String(command::kLibraryCancelOperation),
            QLatin1String(command::kLibrarySetTagFilter),
            QLatin1String(command::kLibrarySetRatingFilter),
            QLatin1String(command::kLibraryToggleColorFilter),
            QLatin1String(command::kLibrarySetRejectFilter),
            QLatin1String(command::kLibrarySetTextFilter),
            QLatin1String(command::kLibrarySetMediaFilter),
            QLatin1String(command::kLibrarySetEditFilter),
            QLatin1String(command::kLibrarySetSort),
            QLatin1String(command::kLibraryClearFilters),
            QLatin1String(command::kLibrarySelectFolder),
            QLatin1String(command::kLibrarySelectLastImport),
            QLatin1String(command::kLibrarySelectSet),
            QLatin1String(command::kLibraryCreateManualSet),
            QLatin1String(command::kLibraryCreateSmartSet),
            QLatin1String(command::kLibraryRenameSet),
            QLatin1String(command::kLibraryDeleteSet),
            QLatin1String(command::kLibraryAddSelectionToSet),
            QLatin1String(command::kLibraryRemoveSelectionFromSet),
            QLatin1String(command::kLibraryToggleStackCollapse),
            QLatin1String(command::kLibraryFolderRelink),
            QLatin1String(command::kLibraryFolderRelinkPath),
            QLatin1String(command::kPhotoSelect),
            QLatin1String(command::kPhotoSetRating),
            QLatin1String(command::kPhotoSetColor),
            QLatin1String(command::kPhotoSetTags),
            QLatin1String(command::kPhotoSetMetadata),
            QLatin1String(command::kPhotoRefreshMetadata),
            QLatin1String(command::kPhotoCreateSnapshot),
            QLatin1String(command::kPhotoRenameSnapshot),
            QLatin1String(command::kPhotoRestoreHistory),
            QLatin1String(command::kPhotoToggleReject),
            QLatin1String(command::kPhotoRequestRemove),
            QLatin1String(command::kPhotoRemove),
            QLatin1String(command::kPhotoRequestDelete),
            QLatin1String(command::kPhotoDelete),
            QLatin1String(command::kPhotoPrevious),
            QLatin1String(command::kPhotoNext),
            QLatin1String(command::kPhotoCopyInfo),
            QLatin1String(command::kPhotoCopyParameters),
            QLatin1String(command::kPhotoCreateVersion),
            QLatin1String(command::kPhotoStackSelection),
            QLatin1String(command::kPhotoUnstack),
            QLatin1String(command::kPhotoSetStackPick),
            QLatin1String(command::kViewGrid),
            QLatin1String(command::kViewLoupe),
            QLatin1String(command::kViewDevelop),
            QLatin1String(command::kViewSurvey),
            QLatin1String(command::kViewFit),
            QLatin1String(command::kViewFill),
            QLatin1String(command::kViewActual),
            QLatin1String(command::kViewToggleActualSize),
            QLatin1String(command::kViewSetZoomMode),
            QLatin1String(command::kViewAdjustZoom),
            QLatin1String(command::kViewSetThumbnailSize),
            QLatin1String(command::kViewSetScopeMode),
            QLatin1String(command::kViewTogglePhotoInfo),
            QLatin1String(command::kEditUndo),
            QLatin1String(command::kEditRedo),
            QLatin1String(command::kEditCopyParameters),
            QLatin1String(command::kEditCopyParametersSelected),
            QLatin1String(command::kEditPasteParameters),
            QLatin1String(command::kEditPasteParametersToSelection),
            QLatin1String(command::kEditResetAll),
            QLatin1String(command::kEditResetSection),
            QLatin1String(command::kEditSetSectionEnabled),
            QLatin1String(command::kEditResetControl),
            QLatin1String(command::kEditSetNumber),
            QLatin1String(command::kEditSetNumbers),
            QLatin1String(command::kEditPickWhiteBalance),
            QLatin1String(command::kEditSetWhiteBalancePick),
            QLatin1String(command::kEditPlaceMask),
            QLatin1String(command::kEditSetMaskPlace),
            QLatin1String(command::kEditSetText),
            QLatin1String(command::kEditSetToneCurve),
            QLatin1String(command::kEditAddRetouchRegion),
            QLatin1String(command::kEditRemoveRetouchRegion),
            QLatin1String(command::kEditSetCrop),
            QLatin1String(command::kEditSetCropAspect),
            QLatin1String(command::kEditAutoPerspective),
            QLatin1String(command::kEditRotateLeft),
            QLatin1String(command::kEditRotateRight),
            QLatin1String(command::kEditFlipHorizontal),
            QLatin1String(command::kEditFlipVertical),
            QLatin1String(command::kEditCropTool),
            QLatin1String(command::kEditBeforeAfter),
            QLatin1String(command::kEditComparison),
            QLatin1String(command::kStyleSave),
            QLatin1String(command::kStyleSavePath),
            QLatin1String(command::kStyleApply),
            QLatin1String(command::kStyleApplyPath),
            QLatin1String(command::kPresetImport),
            QLatin1String(command::kPresetImportPath),
            QLatin1String(command::kPresetApplyPath),
            QLatin1String(command::kPresetSave),
            QLatin1String(command::kPresetSaveSelected),
            QLatin1String(command::kPresetCopyInfo),
            QLatin1String(command::kPresetRename),
            QLatin1String(command::kPresetRenamePath),
            QLatin1String(command::kPresetRequestDelete),
            QLatin1String(command::kPresetDelete),
            QLatin1String(command::kWindowSettings),
            QLatin1String(command::kWindowAssistant),
            QLatin1String(command::kWindowClose),
            QLatin1String(command::kWindowQuit),
            QLatin1String(command::kWindowAbout),
            QLatin1String(command::kWindowPalette),
            QLatin1String(command::kWindowDismiss)};
}

QVector<ActionSpec> builtin_actions()
{
    QVector<ActionSpec> result;
    const auto add = [&result](const char *id, const char *command_id, const QString &title,
                               const QString &category, QStringList keywords,
                               const QString &menu_path, const int order,
                               const bool palette_visible, QVector<ShortcutSpec> shortcuts = {},
                               const QVariant &argument = {}, const bool has_argument = false)
    {
        result.push_back({QLatin1String(id), QLatin1String(command_id), title, category,
                          std::move(keywords), menu_path, order, palette_visible, has_argument,
                          argument, std::move(shortcuts)});
    };
    const auto key = [](const QString &sequence, const bool non_text = false)
    { return ShortcutSpec{sequence, non_text}; };
    const auto file = QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "File"));
    const auto edit = QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Edit"));
    const auto view = QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "View"));
    const auto photo = QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Photo"));

    add(command::kLibraryCreate, command::kLibraryCreate,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Create Library...")), file,
        {QStringLiteral("new"), QStringLiteral("catalog")}, QStringLiteral("file.library"), 10,
        true, {key(primary_key(QStringLiteral("N")))});
    add(command::kLibraryOpen, command::kLibraryOpen,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Open Library...")), file,
        {QStringLiteral("catalog")}, QStringLiteral("file.library"), 20, true,
        {key(primary_key(QStringLiteral("O")))});
    add(command::kLibraryImportFiles, command::kLibraryImportFiles,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Import...")), file,
        {QStringLiteral("files"), QStringLiteral("photos")}, QStringLiteral("file.transfer"), 10,
        true, {key(primary_key(QStringLiteral("I")))});
    add(command::kLibraryExport, command::kLibraryExport,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Export Selected...")), file,
        {QStringLiteral("save"), QStringLiteral("render")}, QStringLiteral("file.transfer"), 30,
        true, {key(primary_key(QStringLiteral("E"), true))});
    add(command::kLibraryRecoveryStatus, command::kLibraryRecoveryStatus,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Recovery Status")), file,
        {QStringLiteral("sidecar"), QStringLiteral("durability")}, QStringLiteral("file.recovery"),
        10, true);
    add(command::kLibraryRecoverySync, command::kLibraryRecoverySync,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Synchronize Recovery")), file,
        {QStringLiteral("sidecar"), QStringLiteral("retry")}, QStringLiteral("file.recovery"), 20,
        true);
    add(command::kLibraryBackupCreate, command::kLibraryBackupCreate,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Create Catalog Backup...")), file,
        {QStringLiteral("recovery"), QStringLiteral("archive")}, QStringLiteral("file.recovery"),
        30, true);
    add(command::kLibraryBackupVerify, command::kLibraryBackupVerify,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Verify Catalog Backup...")), file,
        {QStringLiteral("recovery"), QStringLiteral("integrity")}, QStringLiteral("file.recovery"),
        40, true);
    add(command::kLibraryBackupRestore, command::kLibraryBackupRestore,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Restore Catalog Backup...")), file,
        {QStringLiteral("recovery"), QStringLiteral("restore")}, QStringLiteral("file.recovery"),
        50, true);
    add(command::kLibraryBackupSchedule, command::kLibraryBackupSchedule,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Enable Scheduled Backups...")), file,
        {QStringLiteral("automatic"), QStringLiteral("retention")}, QStringLiteral("file.recovery"),
        55, true);
    add(command::kLibraryBackupScheduleRun, command::kLibraryBackupScheduleRun,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Run Scheduled Backup Now")), file,
        {QStringLiteral("automatic"), QStringLiteral("backup")}, QStringLiteral("file.recovery"),
        56, true);
    add(command::kLibraryBackupScheduleDisable, command::kLibraryBackupScheduleDisable,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Disable Scheduled Backups")), file,
        {QStringLiteral("automatic"), QStringLiteral("backup")}, QStringLiteral("file.recovery"),
        57, true);
    add(command::kLibraryPreviewRebuildSelected, command::kLibraryPreviewRebuildSelected,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Rebuild Selected Previews")), file,
        {QStringLiteral("cache"), QStringLiteral("thumbnail")}, QStringLiteral("file.recovery"), 60,
        true);
    add(command::kLibraryPreviewRebuildAll, command::kLibraryPreviewRebuildAll,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Rebuild All Previews")), file,
        {QStringLiteral("cache"), QStringLiteral("thumbnail")}, QStringLiteral("file.recovery"), 70,
        true);
    add(command::kLibraryCancelOperation, command::kLibraryCancelOperation,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Cancel Catalog Operation")), file,
        {QStringLiteral("cancel"), QStringLiteral("recovery")}, QStringLiteral("file.recovery"), 80,
        true);
    add(command::kStyleSave, command::kStyleSave,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Save Edits as Style...")), file,
        {QStringLiteral("recipe"), QStringLiteral("preset")}, QStringLiteral("file.style"), 10,
        true);
    add(command::kStyleApply, command::kStyleApply,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Apply Recipe Style...")), file,
        {QStringLiteral("recipe"), QStringLiteral("preset")}, QStringLiteral("file.style"), 20,
        true);
    add(command::kPresetImport, command::kPresetImport,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Import Preset...")), file,
        {QStringLiteral("preset"), QStringLiteral("lightroom"), QStringLiteral("xmp")},
        QStringLiteral("file.style"), 30, true);
    add(command::kWindowClose, command::kWindowClose,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Close Window")), file,
        {QStringLiteral("window")}, QStringLiteral("file.window"), 10, true,
        {key(primary_key(QStringLiteral("W")))});
    add(command::kWindowSettings, command::kWindowSettings,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Settings...")), file,
        {QStringLiteral("preferences"), QStringLiteral("configuration")},
        QStringLiteral("file.window"), 20, true, {key(primary_key(QStringLiteral(",")))});
    add(command::kWindowQuit, command::kWindowQuit,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Quit Ravo Studio")), file,
        {QStringLiteral("exit")}, QStringLiteral("file.window"), 30, true,
        {key(primary_key(QStringLiteral("Q")))});

    add(command::kEditUndo, command::kEditUndo,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Undo")), edit,
        {QStringLiteral("history")}, QStringLiteral("edit.history"), 10, true,
        {key(primary_key(QStringLiteral("Z"))), key(QStringLiteral("Z"), true)});
    add(command::kEditRedo, command::kEditRedo,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Redo")), edit,
        {QStringLiteral("history")}, QStringLiteral("edit.history"), 20, true,
        {key(primary_key(QStringLiteral("Z"), true)), key(QStringLiteral("Shift+Z"), true)});
    add(command::kEditCopyParameters, command::kEditCopyParameters,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Copy Parameters")), edit,
        {QStringLiteral("history"), QStringLiteral("clipboard"), QStringLiteral("paste")},
        QStringLiteral("edit.history"), 30, true,
        {key(primary_key(QStringLiteral("C"), true), true)});
    add(command::kEditPasteParameters, command::kEditPasteParameters,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Paste Parameters")), edit,
        {QStringLiteral("history"), QStringLiteral("clipboard"), QStringLiteral("copy")},
        QStringLiteral("edit.history"), 40, true,
        {key(primary_key(QStringLiteral("V"), false, true), true)});
    add(command::kEditPasteParametersToSelection, command::kEditPasteParametersToSelection,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Paste Parameters to Selection")),
        edit, {QStringLiteral("history"), QStringLiteral("clipboard"), QStringLiteral("sync")},
        QStringLiteral("edit.history"), 45, true,
        {key(primary_key(QStringLiteral("V"), true, true), true)});
    add(command::kEditResetAll, command::kEditResetAll,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Reset All Edits")), edit,
        {QStringLiteral("develop"), QStringLiteral("clear")}, QStringLiteral("edit.reset"), 10,
        true, {key(primary_key(QStringLiteral("R"), true))});

    add(command::kViewGrid, command::kViewGrid,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Gallery")), view,
        {QStringLiteral("grid"), QStringLiteral("library")}, QStringLiteral("view.mode"), 10, true,
        {key(primary_key(QStringLiteral("1"))), key(QStringLiteral("G"), true)});
    add(command::kViewLoupe, command::kViewLoupe,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Loupe")), view,
        {QStringLiteral("photo"), QStringLiteral("viewer")}, QStringLiteral("view.mode"), 20, true,
        {key(primary_key(QStringLiteral("2"))), key(QStringLiteral("E"), true),
         key(QStringLiteral("Return"), true), key(QStringLiteral("Enter"), true)});
    add(command::kViewDevelop, command::kViewDevelop,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Edit")), view,
        {QStringLiteral("develop")}, QStringLiteral("view.mode"), 30, true,
        {key(primary_key(QStringLiteral("3"))), key(QStringLiteral("D"), true)});
    add(command::kViewSurvey, command::kViewSurvey,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Survey")), view,
        {QStringLiteral("compare"), QStringLiteral("cull"), QStringLiteral("n-up")},
        QStringLiteral("view.mode"), 40, true, {key(QStringLiteral("N"), true)});
    add(command::kViewFit, command::kViewFit,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Fit")), view,
        {QStringLiteral("zoom")}, QStringLiteral("view.zoom"), 10, true,
        {key(primary_key(QStringLiteral("0"))), key(QStringLiteral("F"), true)});
    add(command::kViewFill, command::kViewFill,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Fill")), view,
        {QStringLiteral("zoom")}, QStringLiteral("view.zoom"), 20, true,
        {key(primary_key(QStringLiteral("9")))});
    add(command::kViewActual, command::kViewActual,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Actual Size")), view,
        {QStringLiteral("100%"), QStringLiteral("1:1"), QStringLiteral("zoom")},
        QStringLiteral("view.zoom"), 30, true,
        {key(primary_key(QStringLiteral("0"), false, true)), key(QStringLiteral("Shift+1"), true)});
    add(command::kEditBeforeAfter, command::kEditBeforeAfter,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Before / After")), view,
        {QStringLiteral("compare")}, QStringLiteral("view.compare"), 10, true,
        {key(QStringLiteral("\\"), true)});
    add(command::kEditComparison, command::kEditComparison,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Before / After")), view,
        {QStringLiteral("compare"), QStringLiteral("side by side")}, QStringLiteral("view.compare"),
        20, false, {key(QStringLiteral("Y"), true)});
    add(command::kViewTogglePhotoInfo, command::kViewTogglePhotoInfo,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Photo Information")), view,
        {QStringLiteral("metadata"), QStringLiteral("overlay"), QStringLiteral("exif")},
        QStringLiteral("view.compare"), 30, true, {key(QStringLiteral("I"), true)});
    add(command::kWindowPalette, command::kWindowPalette,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Show Command Palette")), view,
        {QStringLiteral("commands"), QStringLiteral("search")}, QStringLiteral("view.commands"), 10,
        false, {key(primary_key(QStringLiteral("P"), true))});
    add(command::kWindowAssistant, command::kWindowAssistant,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Assistant")), view,
        {QStringLiteral("chat"), QStringLiteral("ai"), QStringLiteral("model")},
        QStringLiteral("view.commands"), 20, true, {key(primary_key(QStringLiteral("A"), true))});

    add(command::kPhotoPrevious, command::kPhotoPrevious,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Previous Photo")), photo,
        {QStringLiteral("navigate")}, QStringLiteral("photo.navigate"), 10, true,
        {key(QStringLiteral("Left"), true)});
    add(command::kPhotoNext, command::kPhotoNext,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Next Photo")), photo,
        {QStringLiteral("navigate")}, QStringLiteral("photo.navigate"), 20, true,
        {key(QStringLiteral("Right"), true)});
    add(action_id::kPreviousRange, command::kPhotoPrevious,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Extend Selection to Previous")),
        photo, {QStringLiteral("range"), QStringLiteral("selection")}, {}, 0, false,
        {key(QStringLiteral("Shift+Left"), true)}, QStringLiteral("range"), true);
    add(action_id::kNextRange, command::kPhotoNext,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Extend Selection to Next")), photo,
        {QStringLiteral("range"), QStringLiteral("selection")}, {}, 0, false,
        {key(QStringLiteral("Shift+Right"), true)}, QStringLiteral("range"), true);
    add(command::kEditCropTool, command::kEditCropTool,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Crop & Rotate")), photo,
        {QStringLiteral("crop"), QStringLiteral("straighten")}, QStringLiteral("photo.transform"),
        10, true, {key(QStringLiteral("R"), true)});
    add(command::kEditRotateLeft, command::kEditRotateLeft,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Rotate Left")), photo,
        {QStringLiteral("transform")}, QStringLiteral("photo.transform"), 20, true,
        {key(primary_key(QStringLiteral("[")))});
    add(command::kEditRotateRight, command::kEditRotateRight,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Rotate Right")), photo,
        {QStringLiteral("transform")}, QStringLiteral("photo.transform"), 30, true,
        {key(primary_key(QStringLiteral("]")))});
    add(command::kEditFlipHorizontal, command::kEditFlipHorizontal,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Flip Horizontal")), photo,
        {QStringLiteral("mirror")}, QStringLiteral("photo.transform"), 40, true,
        {key(primary_key(QStringLiteral("H"), true))});
    add(command::kEditFlipVertical, command::kEditFlipVertical,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Flip Vertical")), photo,
        {QStringLiteral("mirror")}, QStringLiteral("photo.transform"), 50, true,
        {key(primary_key(QStringLiteral("V"), true))});

    const char *rating_ids[] = {action_id::kRating0, action_id::kRating1, action_id::kRating2,
                                action_id::kRating3, action_id::kRating4, action_id::kRating5};
    for (int rating = 0; rating <= 5; ++rating)
    {
        add(rating_ids[rating], command::kPhotoSetRating,
            QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Rating %1")).arg(rating), photo,
            {QStringLiteral("stars"), QStringLiteral("review")}, QStringLiteral("photo.rating"),
            rating, true, {key(QString::number(rating), true)}, rating, true);
    }
    struct ColorAction
    {
        const char *id;
        const char *value;
        const char *title;
    };
    const ColorAction colors[] = {
        {action_id::kColorNone, "none", QT_TRANSLATE_NOOP("StudioCommands", "No Color")},
        {action_id::kColorRed, "red", QT_TRANSLATE_NOOP("StudioCommands", "Red")},
        {action_id::kColorYellow, "yellow", QT_TRANSLATE_NOOP("StudioCommands", "Yellow")},
        {action_id::kColorGreen, "green", QT_TRANSLATE_NOOP("StudioCommands", "Green")},
        {action_id::kColorBlue, "blue", QT_TRANSLATE_NOOP("StudioCommands", "Blue")},
        {action_id::kColorPurple, "purple", QT_TRANSLATE_NOOP("StudioCommands", "Purple")}};
    int color_order = 0;
    for (const auto &color : colors)
    {
        add(color.id, command::kPhotoSetColor, QString::fromLatin1(color.title), photo,
            {QStringLiteral("label"), QStringLiteral("review")}, QStringLiteral("photo.color"),
            color_order++, true, {}, QString::fromLatin1(color.value), true);
    }
    add(command::kPhotoToggleReject, command::kPhotoToggleReject,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Reject")), photo,
        {QStringLiteral("review"), QStringLiteral("flag")}, QStringLiteral("photo.review"), 10,
        true, {key(QStringLiteral("X"), true)});
    add(command::kPhotoCopyInfo, command::kPhotoCopyInfo,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Copy Info")), photo,
        {QStringLiteral("identity"), QStringLiteral("debug"), QStringLiteral("clipboard")},
        QStringLiteral("photo.review"), 20, true);
    add(command::kPhotoCopyParameters, command::kPhotoCopyParameters,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Copy Parameters")), photo,
        {QStringLiteral("parameters"), QStringLiteral("recipe"), QStringLiteral("debug"),
         QStringLiteral("clipboard")},
        QStringLiteral("photo.review"), 30, true);
    add(command::kPhotoRequestRemove, command::kPhotoRequestRemove,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Remove from Catalog...")), photo,
        {QStringLiteral("library"), QStringLiteral("delete")}, QStringLiteral("photo.delete"), 10,
        true, {key(QStringLiteral("Delete"), true), key(QStringLiteral("Backspace"), true)});
    add(command::kPhotoRequestDelete, command::kPhotoRequestDelete,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Delete from Disk...")), photo,
        {QStringLiteral("original"), QStringLiteral("permanent")}, QStringLiteral("photo.delete"),
        20, true);
    add(command::kPhotoRefreshMetadata, command::kPhotoRefreshMetadata,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Refresh Capture Metadata")), photo,
        {QStringLiteral("exif"), QStringLiteral("capture"), QStringLiteral("refresh")},
        QStringLiteral("photo.metadata"), 10, true);
    add(command::kPhotoCreateVersion, command::kPhotoCreateVersion,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Create Virtual Copy")), photo,
        {QStringLiteral("version"), QStringLiteral("variant")}, QStringLiteral("photo.library"), 10,
        true);
    add(command::kPhotoStackSelection, command::kPhotoStackSelection,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Stack Photos")), photo,
        {QStringLiteral("group"), QStringLiteral("burst")}, QStringLiteral("photo.library"), 20,
        true);
    add(command::kPhotoUnstack, command::kPhotoUnstack,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Unstack Photos")), photo,
        {QStringLiteral("group"), QStringLiteral("burst")}, QStringLiteral("photo.library"), 30,
        true);
    add(command::kPhotoSetStackPick, command::kPhotoSetStackPick,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Set Stack Pick")), photo,
        {QStringLiteral("pick"), QStringLiteral("stack")}, QStringLiteral("photo.library"), 40,
        true);
    add(command::kLibraryToggleStackCollapse, command::kLibraryToggleStackCollapse,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Collapse Stacks")),
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Library")),
        {QStringLiteral("stack"), QStringLiteral("expand")}, QStringLiteral("library.view"), 10,
        true);
    add(command::kLibraryClearFilters, command::kLibraryClearFilters,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Clear Library Filters")),
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Library")),
        {QStringLiteral("search"), QStringLiteral("show all")}, {}, 0, true);
    add(command::kWindowAbout, command::kWindowAbout,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "About Ravo Studio")),
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Help")), {QStringLiteral("version")},
        QStringLiteral("help.about"), 10, true);
    add(command::kWindowDismiss, command::kWindowDismiss,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Dismiss Current View")),
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Window")),
        {QStringLiteral("escape")}, {}, 0, false, {key(QStringLiteral("Esc"), true)});
    return result;
}

QString normalize_search(const QString &value)
{
    const auto decomposed = value.normalized(QString::NormalizationForm_KD).toCaseFolded();
    QString result;
    result.reserve(decomposed.size());
    for (const auto character : decomposed)
    {
        const auto category = character.category();
        if (category != QChar::Mark_NonSpacing && category != QChar::Mark_SpacingCombining &&
            category != QChar::Mark_Enclosing)
            result.push_back(character);
    }
    return result;
}

int token_score(const QString &source, const QString &raw_token)
{
    const auto haystack = normalize_search(source);
    const auto token = normalize_search(raw_token);
    if (token.isEmpty())
        return 0;
    if (haystack == token)
        return 4000;
    for (qsizetype index = 0; index + token.size() <= haystack.size(); ++index)
    {
        const bool boundary = index == 0 || !haystack.at(index - 1).isLetterOrNumber();
        if (boundary && haystack.mid(index, token.size()) == token)
            return 3000 - static_cast<int>(index);
    }
    const qsizetype contiguous = haystack.indexOf(token);
    if (contiguous >= 0)
        return 2000 - static_cast<int>(contiguous);
    qsizetype cursor = 0;
    qsizetype first = -1;
    qsizetype last = -1;
    for (const auto character : token)
    {
        const qsizetype found = haystack.indexOf(character, cursor);
        if (found < 0)
            return -1;
        if (first < 0)
            first = found;
        last = found;
        cursor = found + 1;
    }
    const qsizetype gaps = std::max<qsizetype>(0, last - first + 1 - token.size());
    return 1000 - static_cast<int>(first) - static_cast<int>(gaps) * 4;
}

} // namespace ravo::command_internal
