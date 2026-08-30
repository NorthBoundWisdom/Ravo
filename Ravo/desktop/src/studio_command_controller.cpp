#include "ravo/desktop/studio_command_controller.h"

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

namespace ravo
{
namespace
{

namespace command
{
inline constexpr auto kLibraryCreate = "studio.library.create";
inline constexpr auto kLibraryCreatePath = "studio.library.create_path";
inline constexpr auto kLibraryOpen = "studio.library.open";
inline constexpr auto kLibraryOpenPath = "studio.library.open_path";
inline constexpr auto kLibraryImportFiles = "studio.library.import_files";
inline constexpr auto kLibraryImportPaths = "studio.library.import_paths";
inline constexpr auto kLibraryImportFolder = "studio.library.import_folder";
inline constexpr auto kLibraryImportFolderPath = "studio.library.import_folder_path";
inline constexpr auto kLibraryExport = "studio.library.export";
inline constexpr auto kLibraryExportWrite = "studio.library.export_write";
inline constexpr auto kLibraryExportBatchWrite = "studio.library.export_batch_write";
inline constexpr auto kLibrarySetTagFilter = "studio.library.set_tag_filter";
inline constexpr auto kLibrarySetRatingFilter = "studio.library.set_rating_filter";
inline constexpr auto kLibraryToggleColorFilter = "studio.library.toggle_color_filter";
inline constexpr auto kLibrarySetRejectFilter = "studio.library.set_reject_filter";
inline constexpr auto kLibrarySetTextFilter = "studio.library.set_text_filter";
inline constexpr auto kLibrarySetMediaFilter = "studio.library.set_media_filter";
inline constexpr auto kLibrarySetEditFilter = "studio.library.set_edit_filter";
inline constexpr auto kLibrarySetSort = "studio.library.set_sort";
inline constexpr auto kLibraryClearFilters = "studio.library.clear_filters";
inline constexpr auto kLibrarySelectFolder = "studio.library.select_folder";
inline constexpr auto kPhotoSelect = "studio.photo.select";
inline constexpr auto kPhotoSetRating = "studio.photo.set_rating";
inline constexpr auto kPhotoSetColor = "studio.photo.set_color";
inline constexpr auto kPhotoSetTags = "studio.photo.set_tags";
inline constexpr auto kPhotoSetMetadata = "studio.photo.set_metadata";
inline constexpr auto kPhotoRefreshMetadata = "studio.photo.refresh_metadata";
inline constexpr auto kPhotoCreateSnapshot = "studio.photo.create_snapshot";
inline constexpr auto kPhotoRenameSnapshot = "studio.photo.rename_snapshot";
inline constexpr auto kPhotoRestoreHistory = "studio.photo.restore_history";
inline constexpr auto kPhotoToggleReject = "studio.photo.toggle_reject";
inline constexpr auto kPhotoRequestRemove = "studio.photo.request_remove";
inline constexpr auto kPhotoRemove = "studio.photo.remove";
inline constexpr auto kPhotoRequestDelete = "studio.photo.request_delete_from_disk";
inline constexpr auto kPhotoDelete = "studio.photo.delete_from_disk";
inline constexpr auto kPhotoPrevious = "studio.photo.previous";
inline constexpr auto kPhotoNext = "studio.photo.next";
inline constexpr auto kPhotoCopyInfo = "studio.photo.copy_info";
inline constexpr auto kPhotoCopyParameters = "studio.photo.copy_parameters";
inline constexpr auto kViewGrid = "studio.view.show_grid";
inline constexpr auto kViewLoupe = "studio.view.show_loupe";
inline constexpr auto kViewDevelop = "studio.view.show_develop";
inline constexpr auto kViewFit = "studio.view.fit";
inline constexpr auto kViewFill = "studio.view.fill";
inline constexpr auto kViewActual = "studio.view.actual_size";
inline constexpr auto kViewToggleActualSize = "studio.view.toggle_actual_size";
inline constexpr auto kViewSetZoomMode = "studio.view.set_zoom_mode";
inline constexpr auto kViewAdjustZoom = "studio.view.adjust_zoom";
inline constexpr auto kViewSetThumbnailSize = "studio.view.set_thumbnail_size";
inline constexpr auto kViewSetScopeMode = "studio.view.set_scope_mode";
inline constexpr auto kEditUndo = "studio.edit.undo";
inline constexpr auto kEditRedo = "studio.edit.redo";
inline constexpr auto kEditCopyEdits = "studio.edit.copy_edits";
inline constexpr auto kEditPasteEdits = "studio.edit.paste_edits";
inline constexpr auto kEditPasteEditsSection = "studio.edit.paste_edits_section";
inline constexpr auto kEditSetNumbers = "studio.edit.set_numbers";
inline constexpr auto kEditPickWhiteBalance = "studio.edit.pick_white_balance";
inline constexpr auto kEditSetWhiteBalancePick = "studio.edit.set_white_balance_pick";
inline constexpr auto kEditResetAll = "studio.edit.reset_all";
inline constexpr auto kEditResetSection = "studio.edit.reset_section";
inline constexpr auto kEditSetSectionEnabled = "studio.edit.set_section_enabled";
inline constexpr auto kEditResetControl = "studio.edit.reset_control";
inline constexpr auto kEditSetNumber = "studio.edit.set_number";
inline constexpr auto kEditSetText = "studio.edit.set_text";
inline constexpr auto kEditSetToneCurve = "studio.edit.set_tone_curve";
inline constexpr auto kEditAddRetouchRegion = "studio.edit.add_retouch_region";
inline constexpr auto kEditRemoveRetouchRegion = "studio.edit.remove_retouch_region";
inline constexpr auto kEditSetCrop = "studio.edit.set_crop";
inline constexpr auto kEditSetCropAspect = "studio.edit.set_crop_aspect";
inline constexpr auto kEditRotateLeft = "studio.edit.rotate_left";
inline constexpr auto kEditRotateRight = "studio.edit.rotate_right";
inline constexpr auto kEditFlipHorizontal = "studio.edit.flip_horizontal";
inline constexpr auto kEditFlipVertical = "studio.edit.flip_vertical";
inline constexpr auto kEditCropTool = "studio.edit.toggle_crop_tool";
inline constexpr auto kEditBeforeAfter = "studio.edit.toggle_before_after";
inline constexpr auto kEditComparison = "studio.edit.toggle_comparison";
inline constexpr auto kStyleSave = "studio.style.save";
inline constexpr auto kStyleSavePath = "studio.style.save_path";
inline constexpr auto kStyleApply = "studio.style.apply";
inline constexpr auto kStyleApplyPath = "studio.style.apply_path";
inline constexpr auto kPresetImport = "studio.preset.import";
inline constexpr auto kPresetImportPath = "studio.preset.import_path";
inline constexpr auto kPresetApplyPath = "studio.preset.apply_path";
inline constexpr auto kPresetCopyInfo = "studio.preset.copy_info";
inline constexpr auto kPresetRename = "studio.preset.rename";
inline constexpr auto kPresetRenamePath = "studio.preset.rename_path";
inline constexpr auto kPresetRequestDelete = "studio.preset.request_delete";
inline constexpr auto kPresetDelete = "studio.preset.delete";
inline constexpr auto kWindowSettings = "studio.window.show_settings";
inline constexpr auto kWindowAssistant = "studio.window.toggle_assistant";
inline constexpr auto kWindowClose = "studio.window.close";
inline constexpr auto kWindowQuit = "studio.window.quit";
inline constexpr auto kWindowAbout = "studio.window.show_about";
inline constexpr auto kWindowPalette = "studio.window.show_command_palette";
inline constexpr auto kWindowDismiss = "studio.window.dismiss";
} // namespace command

namespace action_id
{
inline constexpr auto kPreviousRange = "studio.photo.previous_range";
inline constexpr auto kNextRange = "studio.photo.next_range";
inline constexpr auto kRating0 = "studio.photo.rating_0";
inline constexpr auto kRating1 = "studio.photo.rating_1";
inline constexpr auto kRating2 = "studio.photo.rating_2";
inline constexpr auto kRating3 = "studio.photo.rating_3";
inline constexpr auto kRating4 = "studio.photo.rating_4";
inline constexpr auto kRating5 = "studio.photo.rating_5";
inline constexpr auto kColorNone = "studio.photo.color_none";
inline constexpr auto kColorRed = "studio.photo.color_red";
inline constexpr auto kColorYellow = "studio.photo.color_yellow";
inline constexpr auto kColorGreen = "studio.photo.color_green";
inline constexpr auto kColorBlue = "studio.photo.color_blue";
inline constexpr auto kColorPurple = "studio.photo.color_purple";
} // namespace action_id

enum class Condition
{
    kAlways,
    kCatalogOpen,
    kCatalogReady,
    kSelection,
    kReadySelection,
    kNonGrid,
    kDevelop,
    kDevelopSelection,
    kCanUndo,
    kCanRedo,
    kCanPasteEdits,
    kCanDelete
};

struct ShortcutSpec
{
    QString sequence;
    bool requires_non_text_input = false;
};

struct ActionSpec
{
    QString id;
    QString command_id;
    QString title;
    QString category;
    QStringList keywords;
    QString menu_path;
    int order = 0;
    bool palette_visible = true;
    bool has_argument = false;
    QVariant argument;
    QVector<ShortcutSpec> shortcuts;
};

using Validator = std::function<QString(const QVariant &)>;
using Handler = std::function<void(const QVariant &, const QString &)>;

struct CommandSpec
{
    QString id;
    Condition condition = Condition::kAlways;
    Validator validator;
    Handler handler;
};

struct State
{
    bool enabled = true;
    QString reason;
};

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
    QT_TRANSLATE_NOOP("StudioCommands", "Copy edits first."),
    QT_TRANSLATE_NOOP("StudioCommands", "The selected originals cannot be deleted."),
    QT_TRANSLATE_NOOP("StudioCommands", "Command unavailable in the current context."),
    QT_TRANSLATE_NOOP("StudioCommands", "Unreject"),
    QT_TRANSLATE_NOOP("StudioCommands", "Done Cropping"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown action: %1"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown command: %1"),
    QT_TRANSLATE_NOOP("StudioCommands", "Unknown command source: %1")};

QString primary_key(const QString &key, const bool shift = false, const bool alt = false)
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
            QLatin1String(command::kViewGrid),
            QLatin1String(command::kViewLoupe),
            QLatin1String(command::kViewDevelop),
            QLatin1String(command::kViewFit),
            QLatin1String(command::kViewFill),
            QLatin1String(command::kViewActual),
            QLatin1String(command::kViewToggleActualSize),
            QLatin1String(command::kViewSetZoomMode),
            QLatin1String(command::kViewAdjustZoom),
            QLatin1String(command::kViewSetThumbnailSize),
            QLatin1String(command::kViewSetScopeMode),
            QLatin1String(command::kEditUndo),
            QLatin1String(command::kEditRedo),
            QLatin1String(command::kEditCopyEdits),
            QLatin1String(command::kEditPasteEdits),
            QLatin1String(command::kEditPasteEditsSection),
            QLatin1String(command::kEditResetAll),
            QLatin1String(command::kEditResetSection),
            QLatin1String(command::kEditSetSectionEnabled),
            QLatin1String(command::kEditResetControl),
            QLatin1String(command::kEditSetNumber),
            QLatin1String(command::kEditSetNumbers),
            QLatin1String(command::kEditPickWhiteBalance),
            QLatin1String(command::kEditSetWhiteBalancePick),
            QLatin1String(command::kEditSetText),
            QLatin1String(command::kEditSetToneCurve),
            QLatin1String(command::kEditAddRetouchRegion),
            QLatin1String(command::kEditRemoveRetouchRegion),
            QLatin1String(command::kEditSetCrop),
            QLatin1String(command::kEditSetCropAspect),
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
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Import Photos...")), file,
        {QStringLiteral("files"), QStringLiteral("photos")}, QStringLiteral("file.transfer"), 10,
        true, {key(primary_key(QStringLiteral("I")))});
    add(command::kLibraryImportFolder, command::kLibraryImportFolder,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Import Folder...")), file,
        {QStringLiteral("directory"), QStringLiteral("photos")}, QStringLiteral("file.transfer"),
        20, true, {key(primary_key(QStringLiteral("I"), true))});
    add(command::kLibraryExport, command::kLibraryExport,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Export Selected...")), file,
        {QStringLiteral("save"), QStringLiteral("render")}, QStringLiteral("file.transfer"), 30,
        true, {key(primary_key(QStringLiteral("E"), true))});
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
    add(command::kEditCopyEdits, command::kEditCopyEdits,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Copy Edits")), edit,
        {QStringLiteral("history"), QStringLiteral("clipboard"), QStringLiteral("paste")},
        QStringLiteral("edit.history"), 30, true,
        {key(primary_key(QStringLiteral("C"), true), true)});
    add(command::kEditPasteEdits, command::kEditPasteEdits,
        QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Paste Edits")), edit,
        {QStringLiteral("history"), QStringLiteral("clipboard"), QStringLiteral("copy")},
        QStringLiteral("edit.history"), 40, true,
        {key(primary_key(QStringLiteral("V"), false, true), true)});
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

QString no_argument(const QVariant &argument)
{
    return argument.isValid() && !argument.isNull() ?
               QStringLiteral("This command takes no argument.") :
               QString{};
}

QString non_empty_string(const QVariant &argument)
{
    return argument.metaType().id() != QMetaType::QString ||
                   argument.toString().trimmed().isEmpty() ?
               QStringLiteral("A non-empty string is required.") :
               QString{};
}

QString map_argument(const QVariant &argument)
{
    return argument.canConvert<QVariantMap>() ? QString{} :
                                                QStringLiteral("An object argument is required.");
}

QString list_argument(const QVariant &argument)
{
    if (!argument.canConvert<QVariantList>() && !argument.canConvert<QStringList>())
        return QStringLiteral("A non-empty list of paths is required.");
    const auto values = argument.toList().isEmpty() ? QVariantList{} : argument.toList();
    if (!values.isEmpty())
    {
        for (const auto &value : values)
            if (value.metaType().id() != QMetaType::QString || value.toString().trimmed().isEmpty())
                return QStringLiteral("Every import path must be a non-empty string.");
        return {};
    }
    const auto strings = argument.toStringList();
    if (strings.isEmpty())
        return QStringLiteral("A non-empty list of paths is required.");
    for (const auto &value : strings)
        if (value.trimmed().isEmpty())
            return QStringLiteral("Every import path must be a non-empty string.");
    return {};
}

bool numeric_argument(const QVariant &argument)
{
    switch (argument.metaType().id())
    {
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Float:
    case QMetaType::Double:
        return true;
    default:
        return false;
    }
}

QString finite_number(const QVariant &argument, const QString &name)
{
    return numeric_argument(argument) && std::isfinite(argument.toDouble()) ?
               QString{} :
               tr_command(QString::fromUtf8(
                              QT_TRANSLATE_NOOP("StudioCommands", "%1 must be a finite number.")))
                   .arg(name);
}

QString required_fields(const QVariant &argument, const QStringList &fields)
{
    const auto map_error = map_argument(argument);
    if (!map_error.isEmpty())
        return map_error;
    const auto values = argument.toMap();
    for (const auto &field : fields)
        if (!values.contains(field))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Missing command argument field: %1.")))
                .arg(field);
    return {};
}

QString preset_identity_argument(const QVariant &argument)
{
    const auto error = required_fields(argument, {QStringLiteral("path"), QStringLiteral("name")});
    if (!error.isEmpty())
        return error;
    const auto values = argument.toMap();
    static const QSet<QString> allowed{QStringLiteral("path"), QStringLiteral("name")};
    for (auto it = values.constBegin(); it != values.constEnd(); ++it)
    {
        if (!allowed.contains(it.key()))
            return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                  "StudioCommands", "Unknown command argument field: %1.")))
                .arg(it.key());
    }
    const auto path = values.value(QStringLiteral("path"));
    const auto name = values.value(QStringLiteral("name"));
    return path.metaType().id() == QMetaType::QString && !path.toString().trimmed().isEmpty() &&
                   name.metaType().id() == QMetaType::QString &&
                   !name.toString().trimmed().isEmpty() ?
               QString{} :
               QStringLiteral("Preset path and name must be non-empty strings.");
}

QString one_of(const QVariant &argument, const QSet<QString> &values, const QString &name)
{
    return argument.metaType().id() == QMetaType::QString && values.contains(argument.toString()) ?
               QString{} :
               tr_command(
                   QString::fromUtf8(QT_TRANSLATE_NOOP("StudioCommands", "Unknown %1 value.")))
                   .arg(name);
}

QStringList strings_from(const QVariant &argument)
{
    auto result = argument.toStringList();
    if (!result.isEmpty())
        return result;
    for (const auto &value : argument.toList())
        result.push_back(value.toString());
    return result;
}

QVariantMap accepted()
{
    return {{QStringLiteral("accepted"), true},
            {QStringLiteral("code"), QStringLiteral("accepted")},
            {QStringLiteral("message"), QString{}}};
}

QVariantMap rejected(const QString &code, const QString &message)
{
    return {{QStringLiteral("accepted"), false},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}};
}

template <typename Range>
QSet<QString> string_set(const Range &values)
{
    QSet<QString> result;
    for (const auto &value : values)
        result.insert(value);
    return result;
}

} // namespace

struct StudioCommandController::Impl
{
    QHash<QString, CommandSpec> commands;
    QVector<ActionSpec> actions = builtin_actions();
    qulonglong confirmation_revision = 0;
    QString pending_confirmation_command;
    QString pending_confirmation_token;
    std::vector<std::string> pending_confirmation_assets;
    QVariant pending_confirmation_argument;
};

StudioCommandController::StudioCommandController(StudioPresenter &presenter, QObject *parent)
    : QObject(parent)
    , presenter_(presenter)
    , impl_(std::make_unique<Impl>())
{
    const auto add =
        [this](const char *id, const Condition condition, Validator validator, Handler handler)
    {
        const auto key = QLatin1String(id);
        if (impl_->commands.contains(key))
            qFatal("Duplicate Studio command: %s", id);
        impl_->commands.insert(key, {key, condition, std::move(validator), std::move(handler)});
    };
    const auto present = [this](const char *id, const QVariant &argument)
    { emit presentationCommandRequested(QLatin1String(id), argument); };
    const auto request_confirmation = [this](const char *request_id, const char *confirmed_id)
    {
        ++impl_->confirmation_revision;
        impl_->pending_confirmation_command = QLatin1String(confirmed_id);
        impl_->pending_confirmation_token = QStringLiteral("%1:%2")
                                                .arg(QLatin1String(confirmed_id))
                                                .arg(impl_->confirmation_revision);
        impl_->pending_confirmation_assets = presenter_.selected_asset_ids();
        impl_->pending_confirmation_argument.clear();
        emit presentationCommandRequested(QLatin1String(request_id),
                                          impl_->pending_confirmation_token);
    };
    const auto confirmation_validator = [this](const char *confirmed_id, const QVariant &argument)
    {
        if (argument.metaType().id() != QMetaType::QString ||
            impl_->pending_confirmation_command != QLatin1String(confirmed_id) ||
            impl_->pending_confirmation_token != argument.toString())
            return QStringLiteral("A current confirmation token is required.");
        if (impl_->pending_confirmation_assets != presenter_.selected_asset_ids())
            return QStringLiteral("The photo selection changed after confirmation was requested.");
        return QString{};
    };
    const auto request_preset_confirmation =
        [this](const char *request_id, const char *confirmed_id, const QVariant &argument)
    {
        ++impl_->confirmation_revision;
        impl_->pending_confirmation_command = QLatin1String(confirmed_id);
        impl_->pending_confirmation_token = QStringLiteral("%1:%2")
                                                .arg(QLatin1String(confirmed_id))
                                                .arg(impl_->confirmation_revision);
        impl_->pending_confirmation_assets.clear();
        impl_->pending_confirmation_argument = argument;
        auto presentation = argument.toMap();
        presentation.insert(QStringLiteral("token"), impl_->pending_confirmation_token);
        emit presentationCommandRequested(QLatin1String(request_id), presentation);
    };
    const auto preset_confirmation_validator =
        [this](const char *confirmed_id, const QVariant &argument)
    {
        const auto field_error =
            required_fields(argument, {QStringLiteral("token"), QStringLiteral("path")});
        if (!field_error.isEmpty())
            return field_error;
        const auto fields = argument.toMap();
        static const QSet<QString> allowed{QStringLiteral("token"), QStringLiteral("path")};
        for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        {
            if (!allowed.contains(it.key()))
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                      "StudioCommands", "Unknown command argument field: %1.")))
                    .arg(it.key());
        }
        const auto token = fields.value(QStringLiteral("token"));
        const auto path = fields.value(QStringLiteral("path"));
        if (token.metaType().id() != QMetaType::QString ||
            path.metaType().id() != QMetaType::QString ||
            impl_->pending_confirmation_command != QLatin1String(confirmed_id) ||
            impl_->pending_confirmation_token != token.toString())
            return QStringLiteral("A current confirmation token is required.");
        const auto pending = impl_->pending_confirmation_argument.toMap();
        if (pending.value(QStringLiteral("path")).toString() != path.toString())
            return QStringLiteral("The preset changed after confirmation was requested.");
        return QString{};
    };
    const auto clear_confirmation = [this]()
    {
        impl_->pending_confirmation_command.clear();
        impl_->pending_confirmation_token.clear();
        impl_->pending_confirmation_assets.clear();
        impl_->pending_confirmation_argument.clear();
    };

    add(command::kLibraryCreate, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryCreate, argument); });
    add(command::kLibraryCreatePath, Condition::kAlways, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.createCatalogFromPath(argument.toString()); });
    add(command::kLibraryOpen, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryOpen, argument); });
    add(command::kLibraryOpenPath, Condition::kAlways, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.openCatalogFromPath(argument.toString()); });
    add(command::kLibraryImportFiles, Condition::kCatalogReady, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryImportFiles, argument); });
    add(command::kLibraryImportPaths, Condition::kCatalogReady, list_argument,
        [this](const QVariant &argument, const QString &)
        { presenter_.importFilePaths(strings_from(argument)); });
    add(command::kLibraryImportFolder, Condition::kCatalogReady, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryImportFolder, argument); });
    add(command::kLibraryImportFolderPath, Condition::kCatalogReady, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.importFolderFromPath(argument.toString()); });
    add(command::kLibraryExport, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kLibraryExport, argument); });
    add(
        command::kLibraryExportWrite, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            const auto fields =
                required_fields(argument, {QStringLiteral("path"), QStringLiteral("format"),
                                           QStringLiteral("options")});
            if (!fields.isEmpty())
                return fields;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{QStringLiteral("path"), QStringLiteral("format"),
                                               QStringLiteral("options")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            {
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            }
            static const QSet<QString> formats{QStringLiteral("jpeg"), QStringLiteral("png"),
                                               QStringLiteral("tiff"), QStringLiteral("original")};
            const auto format_error =
                one_of(values.value(QStringLiteral("format")), formats, QStringLiteral("format"));
            if (!format_error.isEmpty())
                return format_error;
            const auto path = values.value(QStringLiteral("path"));
            if (path.metaType().id() != QMetaType::QString)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export path must be a string.")));
            if (path.toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export path must not be empty.")));
            const auto options = values.value(QStringLiteral("options"));
            if (options.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export options must be an object.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.exportSelectedToPath(fields.value(QStringLiteral("path")).toString(),
                                            fields.value(QStringLiteral("format")).toString(),
                                            fields.value(QStringLiteral("options")).toMap());
        });
    add(
        command::kEditSetText, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("name"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Develop text control name must not be empty.")));
            return fields.value(QStringLiteral("value")).metaType().id() == QMetaType::QString ?
                       QString{} :
                       tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                           "StudioCommands", "Develop text value must be text.")));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setDevelopText(fields.value(QStringLiteral("name")).toString(),
                                      fields.value(QStringLiteral("value")).toString());
        });
    add(
        command::kLibraryExportBatchWrite, Condition::kReadySelection,
        [](const QVariant &argument)
        {
            const auto fields = required_fields(
                argument, {QStringLiteral("directory"), QStringLiteral("filenameTemplate"),
                           QStringLiteral("format"), QStringLiteral("options")});
            if (!fields.isEmpty())
                return fields;
            const auto values = argument.toMap();
            static const QSet<QString> allowed{QStringLiteral("directory"),
                                               QStringLiteral("filenameTemplate"),
                                               QStringLiteral("format"), QStringLiteral("options")};
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            {
                if (!allowed.contains(it.key()))
                    return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                                          "StudioCommands", "Unknown command argument field: %1.")))
                        .arg(it.key());
            }
            static const QSet<QString> formats{QStringLiteral("jpeg"), QStringLiteral("png"),
                                               QStringLiteral("tiff"), QStringLiteral("original")};
            const auto format_error =
                one_of(values.value(QStringLiteral("format")), formats, QStringLiteral("format"));
            if (!format_error.isEmpty())
                return format_error;
            const auto directory = values.value(QStringLiteral("directory"));
            if (directory.metaType().id() != QMetaType::QString)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export directory must be a string.")));
            if (directory.toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export directory must not be empty.")));
            const auto filename_template = values.value(QStringLiteral("filenameTemplate"));
            if (filename_template.metaType().id() != QMetaType::QString)
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Export filename template must be a string.")));
            if (filename_template.toString().trimmed().isEmpty())
                return tr_command(QString::fromUtf8(QT_TRANSLATE_NOOP(
                    "StudioCommands", "Export filename template must not be empty.")));
            const auto options = values.value(QStringLiteral("options"));
            if (options.metaType().id() != QMetaType::QVariantMap)
                return tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Export options must be an object.")));
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.exportSelectedToDirectory(
                fields.value(QStringLiteral("directory")).toString(),
                fields.value(QStringLiteral("filenameTemplate")).toString(),
                fields.value(QStringLiteral("format")).toString(),
                fields.value(QStringLiteral("options")).toMap());
        });
    add(command::kStyleSave, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kStyleSave, argument); });
    add(command::kStyleSavePath, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.saveStyleToPath(argument.toString()); });
    add(command::kStyleApply, Condition::kDevelopSelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kStyleApply, argument); });
    add(command::kStyleApplyPath, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.applyStyleFromPath(argument.toString()); });
    add(command::kPresetImport, Condition::kReadySelection, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPresetImport, argument); });
    add(command::kPresetImportPath, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.importPresetFromPath(argument.toString()); });
    add(command::kPresetApplyPath, Condition::kReadySelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.applyStyleFromPath(argument.toString()); });
    add(command::kPresetCopyInfo, Condition::kCatalogOpen, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.copyPresetDebugInfo(argument.toString()); });
    add(command::kPresetRename, Condition::kCatalogOpen, preset_identity_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kPresetRename, argument); });
    add(command::kPresetRenamePath, Condition::kCatalogOpen, preset_identity_argument,
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.renamePreset(fields.value(QStringLiteral("path")).toString(),
                                    fields.value(QStringLiteral("name")).toString());
        });
    add(command::kPresetRequestDelete, Condition::kCatalogOpen, preset_identity_argument,
        [request_preset_confirmation](const QVariant &argument, const QString &)
        {
            request_preset_confirmation(command::kPresetRequestDelete, command::kPresetDelete,
                                        argument);
        });
    add(
        command::kPresetDelete, Condition::kCatalogOpen,
        [preset_confirmation_validator](const QVariant &argument)
        { return preset_confirmation_validator(command::kPresetDelete, argument); },
        [this, clear_confirmation](const QVariant &argument, const QString &)
        {
            const QString path = argument.toMap().value(QStringLiteral("path")).toString();
            clear_confirmation();
            presenter_.deletePreset(path);
        });
    add(
        command::kLibrarySetTagFilter, Condition::kCatalogOpen, [](const QVariant &)
        { return QString{}; }, [this](const QVariant &argument, const QString &)
        { presenter_.setTagFilter(argument.toString()); });
    add(
        command::kLibrarySetRatingFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("mode"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            static const QSet<QString> modes{QStringLiteral("any"), QStringLiteral("min"),
                                             QStringLiteral("exact")};
            if (!modes.contains(fields.value(QStringLiteral("mode")).toString()))
                return QStringLiteral("Unknown rating filter mode.");
            const auto value = fields.value(QStringLiteral("value"));
            const double number = value.toDouble();
            return numeric_argument(value) && std::isfinite(number) &&
                           std::floor(number) == number && number >= 0.0 && number <= 5.0 ?
                       QString{} :
                       QStringLiteral("Rating filter value must be an integer from 0 to 5.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setRatingFilter(fields.value(QStringLiteral("mode")).toString(),
                                       fields.value(QStringLiteral("value")).toInt());
        });
    add(
        command::kLibraryToggleColorFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("red"), QStringLiteral("yellow"),
                                              QStringLiteral("green"), QStringLiteral("blue"),
                                              QStringLiteral("purple")};
            return one_of(argument, values, QStringLiteral("color filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.toggleColorFilter(argument.toString()); });
    add(
        command::kLibrarySetRejectFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("include"), QStringLiteral("exclude"),
                                              QStringLiteral("only")};
            return one_of(argument, values, QStringLiteral("reject filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setRejectFilter(argument.toString()); });
    add(
        command::kLibrarySetTextFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::QString ?
                       QString{} :
                       QStringLiteral("Library text filter must be a string.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setFilterText(argument.toString()); });
    add(
        command::kLibrarySetMediaFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("any"), QStringLiteral("raw"),
                                              QStringLiteral("jpeg"), QStringLiteral("png"),
                                              QStringLiteral("tiff")};
            return one_of(argument, values, QStringLiteral("media filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setMediaFilter(argument.toString()); });
    add(
        command::kLibrarySetEditFilter, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("any"), QStringLiteral("edited"),
                                              QStringLiteral("unedited")};
            return one_of(argument, values, QStringLiteral("edit filter"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setEditFilter(argument.toString()); });
    add(
        command::kLibrarySetSort, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("field"), QStringLiteral("direction")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            static const QSet<QString> sort_fields{
                QStringLiteral("imported"), QStringLiteral("captured"), QStringLiteral("name"),
                QStringLiteral("rating"), QStringLiteral("size")};
            static const QSet<QString> directions{QStringLiteral("asc"), QStringLiteral("desc")};
            if (!sort_fields.contains(fields.value(QStringLiteral("field")).toString()))
                return QStringLiteral("Unknown library sort field.");
            return directions.contains(fields.value(QStringLiteral("direction")).toString()) ?
                       QString{} :
                       QStringLiteral("Unknown library sort direction.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setSort(fields.value(QStringLiteral("field")).toString(),
                               fields.value(QStringLiteral("direction")).toString());
        });
    add(command::kLibraryClearFilters, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.clearFilters(); });
    add(
        command::kLibrarySelectFolder, Condition::kCatalogOpen, [](const QVariant &)
        { return QString{}; }, [this](const QVariant &argument, const QString &)
        { presenter_.selectFolder(argument.toString()); });

    add(
        command::kPhotoSelect, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            if (argument.metaType().id() == QMetaType::QString)
                return argument.toString().trimmed().isEmpty() ?
                           QStringLiteral("An asset ID is required.") :
                           QString{};
            const auto error = required_fields(argument, {QStringLiteral("id")});
            return !error.isEmpty() || argument.toMap()
                                           .value(QStringLiteral("id"))
                                           .toString()
                                           .trimmed()
                                           .isEmpty() ?
                       QStringLiteral("An asset ID is required.") :
                       QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            QString asset_id = argument.toString();
            QString mode = QStringLiteral("single");
            const auto fields = argument.toMap();
            if (!fields.isEmpty())
            {
                asset_id = fields.value(QStringLiteral("id")).toString();
                if (fields.contains(QStringLiteral("modifiers")))
                {
                    const auto modifiers = Qt::KeyboardModifiers(static_cast<unsigned int>(
                        fields.value(QStringLiteral("modifiers")).toInt()));
                    if (modifiers.testFlag(Qt::ShiftModifier))
                        mode = QStringLiteral("range");
                    else if (modifiers.testFlag(Qt::ControlModifier) ||
                             modifiers.testFlag(Qt::MetaModifier))
                        mode = QStringLiteral("toggle");
                }
                else
                    mode =
                        fields.value(QStringLiteral("mode"), QStringLiteral("single")).toString();
            }
            if (mode == QLatin1String("range"))
                presenter_.selectAssetRange(asset_id);
            else if (mode == QLatin1String("toggle"))
                presenter_.toggleAssetSelected(asset_id);
            else
                presenter_.selectAsset(asset_id);
        });
    add(
        command::kPhotoSetRating, Condition::kSelection,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 0.0 && value <= 5.0 ?
                       QString{} :
                       QStringLiteral("Rating must be an integer between 0 and 5.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setRating(argument.toInt()); });
    add(
        command::kPhotoSetColor, Condition::kSelection,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("none"),   QStringLiteral("red"),
                                              QStringLiteral("yellow"), QStringLiteral("green"),
                                              QStringLiteral("blue"),   QStringLiteral("purple")};
            return values.contains(argument.toString()) ? QString{} :
                                                          QStringLiteral("Unknown color label.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setColorLabel(argument.toString()); });
    add(
        command::kPhotoSetTags, Condition::kSelection, [](const QVariant &) { return QString{}; },
        [this](const QVariant &argument, const QString &)
        { presenter_.setAssetTags(argument.toString()); });
    add(
        command::kPhotoSetMetadata, Condition::kSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("name"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            static const QSet<QString> names{QStringLiteral("title"), QStringLiteral("description"),
                                             QStringLiteral("creator"),
                                             QStringLiteral("copyright")};
            return names.contains(argument.toMap().value(QStringLiteral("name")).toString()) ?
                       QString{} :
                       QStringLiteral("Unknown writable metadata field.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setMetadataField(fields.value(QStringLiteral("name")).toString(),
                                        fields.value(QStringLiteral("value")).toString());
        });
    add(command::kPhotoRefreshMetadata, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.refreshSelectedMetadata(); });
    add(
        command::kPhotoCreateSnapshot, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            if (!argument.isValid() || argument.metaType().id() == QMetaType::QString)
                return QString{};
            return QStringLiteral("Snapshot label must be a string.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.createSnapshot(argument.toString()); });
    add(
        command::kPhotoRenameSnapshot, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("id"), QStringLiteral("label")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            const auto id = fields.value(QStringLiteral("id"));
            const double number = id.toDouble();
            if (!(numeric_argument(id) && std::isfinite(number) && std::floor(number) == number &&
                  number >= 0.0))
                return QStringLiteral("A non-negative integer history ID is required.");
            if (fields.value(QStringLiteral("label")).metaType().id() != QMetaType::QString)
                return QStringLiteral("Snapshot label must be a string.");
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.renameSnapshot(fields.value(QStringLiteral("id")).toInt(),
                                      fields.value(QStringLiteral("label")).toString());
        });
    add(
        command::kPhotoRestoreHistory, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 0.0 ?
                       QString{} :
                       QStringLiteral("A non-negative integer history ID is required.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.restoreHistory(argument.toInt()); });
    add(command::kPhotoToggleReject, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleRejected(); });
    add(command::kPhotoCopyInfo, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.copySelectedPhotoDebugInfo(); });
    add(command::kPhotoCopyParameters, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.copySelectedPhotoParametersDebugInfo(); });
    add(command::kPhotoRequestRemove, Condition::kSelection, no_argument,
        [request_confirmation](const QVariant &, const QString &)
        { request_confirmation(command::kPhotoRequestRemove, command::kPhotoRemove); });
    add(
        command::kPhotoRemove, Condition::kSelection,
        [confirmation_validator](const QVariant &argument)
        { return confirmation_validator(command::kPhotoRemove, argument); },
        [this, clear_confirmation](const QVariant &, const QString &)
        {
            clear_confirmation();
            presenter_.remove_selected_from_catalog();
        });
    add(command::kPhotoRequestDelete, Condition::kCanDelete, no_argument,
        [request_confirmation](const QVariant &, const QString &)
        { request_confirmation(command::kPhotoRequestDelete, command::kPhotoDelete); });
    add(
        command::kPhotoDelete, Condition::kCanDelete,
        [confirmation_validator](const QVariant &argument)
        { return confirmation_validator(command::kPhotoDelete, argument); },
        [this, clear_confirmation](const QVariant &, const QString &)
        {
            clear_confirmation();
            presenter_.remove_selected_from_disk();
        });
    const auto navigation_validator = [](const QVariant &argument)
    {
        return !argument.isValid() || argument.isNull() ||
                       argument.toString() == QLatin1String("range") ?
                   QString{} :
                   QStringLiteral("Navigation argument must be 'range'.");
    };
    add(command::kPhotoPrevious, Condition::kSelection, navigation_validator,
        [this](const QVariant &argument, const QString &)
        {
            if (argument.toString() != QLatin1String("range"))
                presenter_.selectPrevious();
            else if (const int row = presenter_.selectedIndex(); row > 0)
                presenter_.selectAssetRange(presenter_.assets()->assetIdAt(row - 1));
        });
    add(command::kPhotoNext, Condition::kSelection, navigation_validator,
        [this](const QVariant &argument, const QString &)
        {
            if (argument.toString() != QLatin1String("range"))
                presenter_.selectNext();
            else if (const int row = presenter_.selectedIndex();
                     row >= 0 && row + 1 < presenter_.assets()->rowCount())
                presenter_.selectAssetRange(presenter_.assets()->assetIdAt(row + 1));
        });

    add(command::kViewGrid, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.returnToGrid(); });
    add(command::kViewLoupe, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openLoupe(); });
    add(command::kViewDevelop, Condition::kCatalogOpen, no_argument,
        [this](const QVariant &, const QString &) { presenter_.openDevelop(); });
    add(command::kViewFit, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setZoomMode(QStringLiteral("fit")); });
    add(command::kViewFill, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setZoomMode(QStringLiteral("fill")); });
    add(command::kViewActual, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &)
        { presenter_.setZoomMode(QStringLiteral("actual")); });
    add(command::kViewToggleActualSize, Condition::kNonGrid, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleActualSize(); });
    add(
        command::kViewSetZoomMode, Condition::kNonGrid,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("fit"), QStringLiteral("fill"),
                                              QStringLiteral("actual")};
            return one_of(argument, values, QStringLiteral("zoom mode"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setZoomMode(argument.toString()); });
    add(
        command::kViewAdjustZoom, Condition::kNonGrid, [](const QVariant &argument)
        { return finite_number(argument, QStringLiteral("Zoom delta")); },
        [this](const QVariant &argument, const QString &)
        { presenter_.adjustZoom(argument.toInt()); });
    add(
        command::kViewSetThumbnailSize, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 120.0 && value <= 320.0 ?
                       QString{} :
                       QStringLiteral("Thumbnail size must be an integer between 120 and 320.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setThumbnailSize(argument.toInt()); });
    add(
        command::kViewSetScopeMode, Condition::kCatalogOpen,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{
                QStringLiteral("histogram"), QStringLiteral("waveform"), QStringLiteral("parade"),
                QStringLiteral("vectorscope"), QStringLiteral("split")};
            return one_of(argument, values, QStringLiteral("scope mode"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setScopeMode(argument.toString()); });

    add(command::kEditUndo, Condition::kCanUndo, no_argument,
        [this](const QVariant &, const QString &) { presenter_.undoEdit(); });
    add(command::kEditRedo, Condition::kCanRedo, no_argument,
        [this](const QVariant &, const QString &) { presenter_.redoEdit(); });
    add(command::kEditCopyEdits, Condition::kSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.copyEdits(); });
    add(command::kEditPasteEdits, Condition::kCanPasteEdits, no_argument,
        [this](const QVariant &, const QString &) { presenter_.pasteEdits(); });
    add(
        command::kEditPasteEditsSection, Condition::kCanPasteEdits,
        [](const QVariant &argument)
        {
            static const QSet<QString> values{QStringLiteral("all"), QStringLiteral("light"),
                                              QStringLiteral("color")};
            return one_of(argument, values, QStringLiteral("edit section"));
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.pasteEditsSection(argument.toString()); });
    add(command::kEditResetAll, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.resetAllEdits(); });
    add(command::kEditResetSection, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.resetSection(argument.toString()); });
    add(
        command::kEditSetSectionEnabled, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("section"), QStringLiteral("enabled")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("section")).toString().trimmed().isEmpty())
                return QStringLiteral("Develop section name must not be empty.");
            const auto enabled = fields.value(QStringLiteral("enabled"));
            return enabled.metaType().id() == QMetaType::Bool || enabled.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("Develop section enabled must be boolean.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.setSectionEffectEnabled(fields.value(QStringLiteral("section")).toString(),
                                               fields.value(QStringLiteral("enabled")).toBool());
        });
    add(command::kEditResetControl, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.resetControl(argument.toString()); });
    add(
        command::kEditSetNumber, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("name"), QStringLiteral("value")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            if (fields.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                return QStringLiteral("Develop control name must not be empty.");
            return finite_number(fields.value(QStringLiteral("value")),
                                 QStringLiteral("Develop value"));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const auto name = fields.value(QStringLiteral("name")).toString();
            const double value = fields.value(QStringLiteral("value")).toDouble();
            if (!std::isfinite(value))
            {
                presenter_.setError(tr_command(QString::fromUtf8(
                    QT_TRANSLATE_NOOP("StudioCommands", "Develop value must be finite."))));
                return;
            }
            if (fields.value(QStringLiteral("live")).toBool())
                presenter_.previewDevelopNumber(name, value);
            else
                presenter_.setDevelopNumber(name, value);
        });
    add(
        command::kEditSetNumbers, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error = required_fields(argument, {QStringLiteral("fields")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap().value(QStringLiteral("fields")).toMap();
            if (fields.isEmpty())
                return QStringLiteral("Develop fields must not be empty.");
            for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
            {
                if (it.key().trimmed().isEmpty())
                    return QStringLiteral("Develop control name must not be empty.");
                const auto number_error =
                    finite_number(it.value(), QStringLiteral("Develop value"));
                if (!number_error.isEmpty())
                    return number_error;
            }
            return QString{};
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto payload = argument.toMap();
            const auto fields = payload.value(QStringLiteral("fields")).toMap();
            if (payload.value(QStringLiteral("live")).toBool())
                presenter_.previewDevelopNumbers(fields);
            else
                presenter_.setDevelopNumbers(fields);
        });
    add(
        command::kEditPickWhiteBalance, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error =
                required_fields(argument, {QStringLiteral("x"), QStringLiteral("y")});
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            const auto x_error =
                finite_number(fields.value(QStringLiteral("x")), QStringLiteral("White-balance X"));
            if (!x_error.isEmpty())
                return x_error;
            return finite_number(fields.value(QStringLiteral("y")),
                                 QStringLiteral("White-balance Y"));
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            presenter_.pickWhiteBalance(fields.value(QStringLiteral("x")).toDouble(),
                                        fields.value(QStringLiteral("y")).toDouble());
        });
    add(
        command::kEditSetWhiteBalancePick, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            return argument.metaType().id() == QMetaType::Bool || argument.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("White-balance pick state must be boolean.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.setWhiteBalancePickActive(argument.toBool()); });
    add(
        command::kEditSetToneCurve, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const auto error = required_fields(argument, {QStringLiteral("points")});
            if (!error.isEmpty())
                return error;
            return argument.toMap().value(QStringLiteral("points")).canConvert<QVariantList>() ?
                       QString{} :
                       QStringLiteral("Tone curve points must be a list.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const auto family = fields.value(QStringLiteral("family")).toString();
            const int channel = fields.value(QStringLiteral("channel"), 0).toInt();
            const auto points = fields.value(QStringLiteral("points")).toList();
            if (fields.value(QStringLiteral("live")).toBool())
            {
                if (family.isEmpty())
                    presenter_.previewToneCurve(points);
                else
                    presenter_.previewCurvePoints(family, channel, points);
            }
            else if (family.isEmpty())
            {
                presenter_.setToneCurve(points);
            }
            else
            {
                presenter_.setCurvePoints(family, channel, points);
            }
        });
    add(
        command::kEditAddRetouchRegion, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            return required_fields(
                argument,
                {QStringLiteral("mode"), QStringLiteral("centerX"), QStringLiteral("centerY"),
                 QStringLiteral("radius"), QStringLiteral("feather"), QStringLiteral("opacity"),
                 QStringLiteral("sourceX"), QStringLiteral("sourceY"), QStringLiteral("blurType"),
                 QStringLiteral("blurRadius"), QStringLiteral("fillMode"), QStringLiteral("fillR"),
                 QStringLiteral("fillG"), QStringLiteral("fillB"),
                 QStringLiteral("fillBrightness")});
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.addRetouchRegion(argument.toMap()); });
    add(
        command::kEditRemoveRetouchRegion, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const double value = argument.toDouble();
            return numeric_argument(argument) && std::isfinite(value) &&
                           std::floor(value) == value && value >= 0.0 &&
                           value <= static_cast<double>(std::numeric_limits<int>::max()) ?
                       QString{} :
                       QStringLiteral("Retouch region index must be a non-negative integer.");
        },
        [this](const QVariant &argument, const QString &)
        { presenter_.removeRetouchRegion(argument.toInt()); });
    add(
        command::kEditSetCrop, Condition::kDevelopSelection,
        [](const QVariant &argument)
        {
            const QStringList names{QStringLiteral("x"), QStringLiteral("y"),
                                    QStringLiteral("width"), QStringLiteral("height")};
            const auto error = required_fields(argument, names);
            if (!error.isEmpty())
                return error;
            const auto fields = argument.toMap();
            for (const auto &name : names)
            {
                const auto number_error = finite_number(fields.value(name), name);
                if (!number_error.isEmpty())
                    return number_error;
            }
            return fields.value(QStringLiteral("width")).toDouble() > 0.0 &&
                           fields.value(QStringLiteral("height")).toDouble() > 0.0 ?
                       QString{} :
                       QStringLiteral("Crop width and height must be positive.");
        },
        [this](const QVariant &argument, const QString &)
        {
            const auto fields = argument.toMap();
            const double x = fields.value(QStringLiteral("x")).toDouble();
            const double y = fields.value(QStringLiteral("y")).toDouble();
            const double width = fields.value(QStringLiteral("width")).toDouble();
            const double height = fields.value(QStringLiteral("height")).toDouble();
            if (fields.value(QStringLiteral("live")).toBool())
                presenter_.previewCropRect(x, y, width, height);
            else
                presenter_.setCropRect(x, y, width, height);
        });
    add(command::kEditSetCropAspect, Condition::kDevelopSelection, non_empty_string,
        [this](const QVariant &argument, const QString &)
        { presenter_.setCropAspect(argument.toString()); });
    add(command::kEditRotateLeft, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rotateLeft(); });
    add(command::kEditRotateRight, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.rotateRight(); });
    add(command::kEditFlipHorizontal, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.flipHorizontal(); });
    add(command::kEditFlipVertical, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.flipVertical(); });
    add(
        command::kEditCropTool, Condition::kSelection,
        [](const QVariant &argument)
        {
            return !argument.isValid() || argument.isNull() || argument.canConvert<bool>() ?
                       QString{} :
                       QStringLiteral("Crop state must be boolean.");
        },
        [this](const QVariant &argument, const QString &source)
        {
            presenter_.openDevelop();
            if (argument.isValid())
            {
                presenter_.setCropToolActive(argument.toBool());
                return;
            }
            // R always enters crop. Menu/button still toggle so Done can exit.
            presenter_.setCropToolActive(
                source == QLatin1String("keyboard") ? true : !presenter_.cropToolActive());
        });
    add(command::kEditBeforeAfter, Condition::kDevelop, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleBeforeAfter(); });
    add(command::kEditComparison, Condition::kDevelopSelection, no_argument,
        [this](const QVariant &, const QString &) { presenter_.toggleComparison(); });

    add(command::kWindowSettings, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowSettings, argument); });
    add(command::kWindowAssistant, Condition::kAlways, no_argument,
        [this](const QVariant &, const QString &) { setAssistantOpen(!assistant_open_); });
    add(command::kWindowClose, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowClose, argument); });
    add(command::kWindowQuit, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowQuit, argument); });
    add(command::kWindowAbout, Condition::kAlways, no_argument,
        [present](const QVariant &argument, const QString &)
        { present(command::kWindowAbout, argument); });
    add(command::kWindowPalette, Condition::kAlways, no_argument,
        [this](const QVariant &, const QString &) { setPaletteOpen(true); });
    add(command::kWindowDismiss, Condition::kAlways, no_argument,
        [this, present](const QVariant &argument, const QString &)
        {
            if (settings_open_)
                present(command::kWindowDismiss, argument);
            else if (assistant_open_)
                setAssistantOpen(false);
            else if (presenter_.catalogOpen())
                presenter_.returnToGrid();
        });

    const auto errors = validateBuiltinDefinitions();
    if (!errors.isEmpty())
        qFatal("Studio command registry validation failed: %s",
               errors.join(QLatin1String("; ")).toUtf8().constData());
    if (string_set(impl_->commands.keys()) != string_set(command_ids()))
        qFatal("Studio command handlers do not cover the builtin command registry");

    const auto changed = [this]() { refresh(); };
    connect(&presenter_, &StudioPresenter::catalogChanged, this, changed);
    connect(&presenter_, &StudioPresenter::busyChanged, this, changed);
    connect(&presenter_, &StudioPresenter::libraryWorkChanged, this, changed);
    connect(&presenter_, &StudioPresenter::selectionChanged, this, changed);
    connect(&presenter_, &StudioPresenter::browseModeChanged, this, changed);
    connect(&presenter_, &StudioPresenter::zoomChanged, this, changed);
    connect(&presenter_, &StudioPresenter::editChanged, this, changed);
    connect(&presenter_, &StudioPresenter::copiedEditsChanged, this, changed);
}

StudioCommandController::~StudioCommandController() = default;

QVariantMap StudioCommandController::ids() const
{
    return {
        {QStringLiteral("libraryCreate"), QLatin1String(command::kLibraryCreate)},
        {QStringLiteral("libraryCreatePath"), QLatin1String(command::kLibraryCreatePath)},
        {QStringLiteral("libraryOpen"), QLatin1String(command::kLibraryOpen)},
        {QStringLiteral("libraryOpenPath"), QLatin1String(command::kLibraryOpenPath)},
        {QStringLiteral("libraryImportFiles"), QLatin1String(command::kLibraryImportFiles)},
        {QStringLiteral("libraryImportPaths"), QLatin1String(command::kLibraryImportPaths)},
        {QStringLiteral("libraryImportFolder"), QLatin1String(command::kLibraryImportFolder)},
        {QStringLiteral("libraryImportFolderPath"),
         QLatin1String(command::kLibraryImportFolderPath)},
        {QStringLiteral("libraryExport"), QLatin1String(command::kLibraryExport)},
        {QStringLiteral("libraryExportWrite"), QLatin1String(command::kLibraryExportWrite)},
        {QStringLiteral("libraryExportBatchWrite"),
         QLatin1String(command::kLibraryExportBatchWrite)},
        {QStringLiteral("librarySetTagFilter"), QLatin1String(command::kLibrarySetTagFilter)},
        {QStringLiteral("librarySetRatingFilter"), QLatin1String(command::kLibrarySetRatingFilter)},
        {QStringLiteral("libraryToggleColorFilter"),
         QLatin1String(command::kLibraryToggleColorFilter)},
        {QStringLiteral("librarySetRejectFilter"), QLatin1String(command::kLibrarySetRejectFilter)},
        {QStringLiteral("librarySetTextFilter"), QLatin1String(command::kLibrarySetTextFilter)},
        {QStringLiteral("librarySetMediaFilter"), QLatin1String(command::kLibrarySetMediaFilter)},
        {QStringLiteral("librarySetEditFilter"), QLatin1String(command::kLibrarySetEditFilter)},
        {QStringLiteral("librarySetSort"), QLatin1String(command::kLibrarySetSort)},
        {QStringLiteral("libraryClearFilters"), QLatin1String(command::kLibraryClearFilters)},
        {QStringLiteral("librarySelectFolder"), QLatin1String(command::kLibrarySelectFolder)},
        {QStringLiteral("photoSelect"), QLatin1String(command::kPhotoSelect)},
        {QStringLiteral("photoRate"), QLatin1String(command::kPhotoSetRating)},
        {QStringLiteral("photoColor"), QLatin1String(command::kPhotoSetColor)},
        {QStringLiteral("photoSetTags"), QLatin1String(command::kPhotoSetTags)},
        {QStringLiteral("photoSetMetadata"), QLatin1String(command::kPhotoSetMetadata)},
        {QStringLiteral("photoRefreshMetadata"), QLatin1String(command::kPhotoRefreshMetadata)},
        {QStringLiteral("photoCreateSnapshot"), QLatin1String(command::kPhotoCreateSnapshot)},
        {QStringLiteral("photoRenameSnapshot"), QLatin1String(command::kPhotoRenameSnapshot)},
        {QStringLiteral("photoRestoreHistory"), QLatin1String(command::kPhotoRestoreHistory)},
        {QStringLiteral("photoReject"), QLatin1String(command::kPhotoToggleReject)},
        {QStringLiteral("photoRemove"), QLatin1String(command::kPhotoRequestRemove)},
        {QStringLiteral("photoRemoveConfirmed"), QLatin1String(command::kPhotoRemove)},
        {QStringLiteral("photoRemoveFromDisk"), QLatin1String(command::kPhotoRequestDelete)},
        {QStringLiteral("photoRemoveFromDiskConfirmed"), QLatin1String(command::kPhotoDelete)},
        {QStringLiteral("photoPrevious"), QLatin1String(command::kPhotoPrevious)},
        {QStringLiteral("photoNext"), QLatin1String(command::kPhotoNext)},
        {QStringLiteral("photoCopyInfo"), QLatin1String(command::kPhotoCopyInfo)},
        {QStringLiteral("photoCopyParameters"), QLatin1String(command::kPhotoCopyParameters)},
        {QStringLiteral("viewGrid"), QLatin1String(command::kViewGrid)},
        {QStringLiteral("viewLoupe"), QLatin1String(command::kViewLoupe)},
        {QStringLiteral("viewDevelop"), QLatin1String(command::kViewDevelop)},
        {QStringLiteral("viewFit"), QLatin1String(command::kViewFit)},
        {QStringLiteral("viewFill"), QLatin1String(command::kViewFill)},
        {QStringLiteral("viewActual"), QLatin1String(command::kViewActual)},
        {QStringLiteral("viewToggleActualSize"), QLatin1String(command::kViewToggleActualSize)},
        {QStringLiteral("viewSetZoomMode"), QLatin1String(command::kViewSetZoomMode)},
        {QStringLiteral("viewAdjustZoom"), QLatin1String(command::kViewAdjustZoom)},
        {QStringLiteral("viewSetThumbnailSize"), QLatin1String(command::kViewSetThumbnailSize)},
        {QStringLiteral("viewSetScopeMode"), QLatin1String(command::kViewSetScopeMode)},
        {QStringLiteral("editUndo"), QLatin1String(command::kEditUndo)},
        {QStringLiteral("editRedo"), QLatin1String(command::kEditRedo)},
        {QStringLiteral("editCopyEdits"), QLatin1String(command::kEditCopyEdits)},
        {QStringLiteral("editPasteEdits"), QLatin1String(command::kEditPasteEdits)},
        {QStringLiteral("editPasteEditsSection"), QLatin1String(command::kEditPasteEditsSection)},
        {QStringLiteral("editResetAll"), QLatin1String(command::kEditResetAll)},
        {QStringLiteral("editResetSection"), QLatin1String(command::kEditResetSection)},
        {QStringLiteral("editSetSectionEnabled"), QLatin1String(command::kEditSetSectionEnabled)},
        {QStringLiteral("editResetControl"), QLatin1String(command::kEditResetControl)},
        {QStringLiteral("editSetNumber"), QLatin1String(command::kEditSetNumber)},
        {QStringLiteral("editSetNumbers"), QLatin1String(command::kEditSetNumbers)},
        {QStringLiteral("editPickWhiteBalance"), QLatin1String(command::kEditPickWhiteBalance)},
        {QStringLiteral("editSetWhiteBalancePick"),
         QLatin1String(command::kEditSetWhiteBalancePick)},
        {QStringLiteral("editSetText"), QLatin1String(command::kEditSetText)},
        {QStringLiteral("editSetToneCurve"), QLatin1String(command::kEditSetToneCurve)},
        {QStringLiteral("editAddRetouchRegion"), QLatin1String(command::kEditAddRetouchRegion)},
        {QStringLiteral("editRemoveRetouchRegion"),
         QLatin1String(command::kEditRemoveRetouchRegion)},
        {QStringLiteral("editSetCrop"), QLatin1String(command::kEditSetCrop)},
        {QStringLiteral("editSetCropAspect"), QLatin1String(command::kEditSetCropAspect)},
        {QStringLiteral("editRotateLeft"), QLatin1String(command::kEditRotateLeft)},
        {QStringLiteral("editRotateRight"), QLatin1String(command::kEditRotateRight)},
        {QStringLiteral("editFlipHorizontal"), QLatin1String(command::kEditFlipHorizontal)},
        {QStringLiteral("editFlipVertical"), QLatin1String(command::kEditFlipVertical)},
        {QStringLiteral("editCropTool"), QLatin1String(command::kEditCropTool)},
        {QStringLiteral("editBeforeAfter"), QLatin1String(command::kEditBeforeAfter)},
        {QStringLiteral("editComparison"), QLatin1String(command::kEditComparison)},
        {QStringLiteral("styleSave"), QLatin1String(command::kStyleSave)},
        {QStringLiteral("styleSavePath"), QLatin1String(command::kStyleSavePath)},
        {QStringLiteral("styleApply"), QLatin1String(command::kStyleApply)},
        {QStringLiteral("styleApplyPath"), QLatin1String(command::kStyleApplyPath)},
        {QStringLiteral("presetImport"), QLatin1String(command::kPresetImport)},
        {QStringLiteral("presetImportPath"), QLatin1String(command::kPresetImportPath)},
        {QStringLiteral("presetApplyPath"), QLatin1String(command::kPresetApplyPath)},
        {QStringLiteral("presetCopyInfo"), QLatin1String(command::kPresetCopyInfo)},
        {QStringLiteral("presetRename"), QLatin1String(command::kPresetRename)},
        {QStringLiteral("presetRenamePath"), QLatin1String(command::kPresetRenamePath)},
        {QStringLiteral("presetDelete"), QLatin1String(command::kPresetRequestDelete)},
        {QStringLiteral("presetDeleteConfirmed"), QLatin1String(command::kPresetDelete)},
        {QStringLiteral("windowSettings"), QLatin1String(command::kWindowSettings)},
        {QStringLiteral("windowAssistant"), QLatin1String(command::kWindowAssistant)},
        {QStringLiteral("windowClose"), QLatin1String(command::kWindowClose)},
        {QStringLiteral("windowQuit"), QLatin1String(command::kWindowQuit)},
        {QStringLiteral("windowAbout"), QLatin1String(command::kWindowAbout)},
        {QStringLiteral("windowCommandPalette"), QLatin1String(command::kWindowPalette)},
        {QStringLiteral("windowDismiss"), QLatin1String(command::kWindowDismiss)},
        {QStringLiteral("actionRating0"), QLatin1String(action_id::kRating0)},
        {QStringLiteral("actionRating1"), QLatin1String(action_id::kRating1)},
        {QStringLiteral("actionRating2"), QLatin1String(action_id::kRating2)},
        {QStringLiteral("actionRating3"), QLatin1String(action_id::kRating3)},
        {QStringLiteral("actionRating4"), QLatin1String(action_id::kRating4)},
        {QStringLiteral("actionRating5"), QLatin1String(action_id::kRating5)},
        {QStringLiteral("actionColorNone"), QLatin1String(action_id::kColorNone)},
        {QStringLiteral("actionColorRed"), QLatin1String(action_id::kColorRed)},
        {QStringLiteral("actionColorYellow"), QLatin1String(action_id::kColorYellow)},
        {QStringLiteral("actionColorGreen"), QLatin1String(action_id::kColorGreen)},
        {QStringLiteral("actionColorBlue"), QLatin1String(action_id::kColorBlue)},
        {QStringLiteral("actionColorPurple"), QLatin1String(action_id::kColorPurple)}};
}

namespace
{
State resolve_state(const StudioPresenter &presenter, const Condition condition,
                    const bool settings_open)
{
    if (settings_open && condition != Condition::kAlways)
        return {false, tr_command(QStringLiteral("Close Settings to use this command."))};
    const bool catalog_open = presenter.catalogOpen();
    const bool selection = !presenter.selectedAssetId().isEmpty();
    const bool ready = catalog_open && !presenter.busy() && !presenter.importWorkActive();
    switch (condition)
    {
    case Condition::kAlways:
        return {};
    case Condition::kCatalogOpen:
        return catalog_open ? State{} :
                              State{false, tr_command(QStringLiteral("Open a library first."))};
    case Condition::kCatalogReady:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return ready ? State{} :
                       State{false, tr_command(QStringLiteral("Wait for library work to finish."))};
    case Condition::kSelection:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return selection ? State{} :
                           State{false, tr_command(QStringLiteral("Select a photo first."))};
    case Condition::kReadySelection:
        if (!ready)
            return {false, !catalog_open ?
                               tr_command(QStringLiteral("Open a library first.")) :
                               tr_command(QStringLiteral("Wait for library work to finish."))};
        return selection ? State{} :
                           State{false, tr_command(QStringLiteral("Select a photo first."))};
    case Condition::kNonGrid:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        return presenter.browseMode() != QLatin1String("grid") ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Open a photo first."))};
    case Condition::kDevelop:
        return presenter.browseMode() == QLatin1String("develop") ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Open Edit first."))};
    case Condition::kDevelopSelection:
        if (!selection)
            return {false, tr_command(QStringLiteral("Select a photo first."))};
        return presenter.browseMode() == QLatin1String("develop") ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Open Edit first."))};
    case Condition::kCanUndo:
        return presenter.browseMode() == QLatin1String("develop") && presenter.canUndo() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Nothing to undo."))};
    case Condition::kCanRedo:
        return presenter.browseMode() == QLatin1String("develop") && presenter.canRedo() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Nothing to redo."))};
    case Condition::kCanPasteEdits:
        if (!catalog_open)
            return {false, tr_command(QStringLiteral("Open a library first."))};
        if (!selection)
            return {false, tr_command(QStringLiteral("Select a photo first."))};
        return presenter.hasCopiedEdits() ?
                   State{} :
                   State{false, tr_command(QStringLiteral("Copy edits first."))};
    case Condition::kCanDelete:
        return presenter.canDeleteFromDisk() ?
                   State{} :
                   State{false,
                         tr_command(QStringLiteral("The selected originals cannot be deleted."))};
    }
    return {false, tr_command(QStringLiteral("Command unavailable in the current context."))};
}
} // namespace

QVariantMap StudioCommandController::action(const QString &action_id) const
{
    const auto found = std::find_if(impl_->actions.cbegin(), impl_->actions.cend(),
                                    [&action_id](const ActionSpec &candidate)
                                    { return candidate.id == action_id; });
    if (found == impl_->actions.cend())
        return {};
    const auto command_found = impl_->commands.constFind(found->command_id);
    if (command_found == impl_->commands.cend())
        return {};
    const auto state = resolve_state(presenter_, command_found->condition, settings_open_);
    QString title = tr_command(found->title);
    if (found->id == QLatin1String(command::kPhotoToggleReject) && presenter_.selectedRejected())
        title = tr_command(QStringLiteral("Unreject"));
    if (found->id == QLatin1String(command::kEditCropTool) && presenter_.cropToolActive())
        title = tr_command(QStringLiteral("Done Cropping"));

    bool checkable = false;
    bool checked = false;
    if (found->id == QLatin1String(command::kViewGrid) ||
        found->id == QLatin1String(command::kViewLoupe) ||
        found->id == QLatin1String(command::kViewDevelop))
    {
        checkable = true;
        checked = (found->id == QLatin1String(command::kViewGrid) &&
                   presenter_.browseMode() == QLatin1String("grid")) ||
                  (found->id == QLatin1String(command::kViewLoupe) &&
                   presenter_.browseMode() == QLatin1String("loupe")) ||
                  (found->id == QLatin1String(command::kViewDevelop) &&
                   presenter_.browseMode() == QLatin1String("develop"));
    }
    else if (found->id == QLatin1String(command::kViewFit) ||
             found->id == QLatin1String(command::kViewFill) ||
             found->id == QLatin1String(command::kViewActual))
    {
        checkable = true;
        checked = (found->id == QLatin1String(command::kViewFit) &&
                   presenter_.zoomMode() == QLatin1String("fit")) ||
                  (found->id == QLatin1String(command::kViewFill) &&
                   presenter_.zoomMode() == QLatin1String("fill")) ||
                  (found->id == QLatin1String(command::kViewActual) &&
                   presenter_.zoomMode() == QLatin1String("actual"));
    }
    else if (found->command_id == QLatin1String(command::kPhotoSetRating))
    {
        checkable = true;
        checked = found->argument.toInt() == presenter_.selectedRating();
    }
    else if (found->command_id == QLatin1String(command::kPhotoSetColor))
    {
        checkable = true;
        checked = found->argument.toString() == presenter_.selectedColorLabel();
    }
    else if (found->id == QLatin1String(command::kPhotoToggleReject))
    {
        checkable = true;
        checked = presenter_.selectedRejected();
    }
    else if (found->id == QLatin1String(command::kEditCropTool))
    {
        checkable = true;
        checked = presenter_.cropToolActive();
    }
    else if (found->id == QLatin1String(command::kEditBeforeAfter))
    {
        checkable = true;
        checked = presenter_.beforeAfter();
    }
    else if (found->id == QLatin1String(command::kEditComparison))
    {
        checkable = true;
        checked = presenter_.comparisonActive();
    }
    else if (found->id == QLatin1String(command::kWindowAssistant))
    {
        checkable = true;
        checked = assistant_open_;
    }
    const QString shortcut =
        found->shortcuts.isEmpty() ? QString{} : native_key(found->shortcuts.front().sequence);
    return {{QStringLiteral("actionId"), found->id},
            {QStringLiteral("commandId"), found->command_id},
            {QStringLiteral("title"), title},
            {QStringLiteral("category"), tr_command(found->category)},
            {QStringLiteral("keywords"), found->keywords},
            {QStringLiteral("menuPath"), found->menu_path},
            {QStringLiteral("enabled"), state.enabled},
            {QStringLiteral("disabledReason"), state.reason},
            {QStringLiteral("checkable"), checkable},
            {QStringLiteral("checked"), checked},
            {QStringLiteral("executing"), false},
            {QStringLiteral("shortcutText"), shortcut}};
}

QVariantList StudioCommandController::menuEntries(const QString &path) const
{
    QVector<const ActionSpec *> selected;
    for (const auto &definition : impl_->actions)
        if (definition.menu_path == path)
            selected.push_back(&definition);
    std::sort(selected.begin(), selected.end(),
              [](const auto *left, const auto *right)
              {
                  return left->order < right->order ||
                         (left->order == right->order && left->id < right->id);
              });
    QVariantList result;
    for (const auto *definition : selected)
        result.push_back(action(definition->id));
    return result;
}

QVariantList StudioCommandController::paletteEntries() const
{
    struct Ranked
    {
        QVariantMap value;
        int score = 0;
        int order = 0;
        QString id;
    };
    QVector<Ranked> ranked;
    for (const auto &definition : impl_->actions)
    {
        if (!definition.palette_visible)
            continue;
        const auto value = action(definition.id);
        const int score = fuzzyScore(value.value(QStringLiteral("title")).toString(),
                                     value.value(QStringLiteral("category")).toString(),
                                     definition.keywords, definition.id, palette_query_);
        if (score >= 0)
            ranked.push_back({value, score, definition.order, definition.id});
    }
    std::sort(ranked.begin(), ranked.end(),
              [this](const Ranked &left, const Ranked &right)
              {
                  if (!palette_query_.trimmed().isEmpty() && left.score != right.score)
                      return left.score > right.score;
                  const auto left_category =
                      left.value.value(QStringLiteral("category")).toString();
                  const auto right_category =
                      right.value.value(QStringLiteral("category")).toString();
                  if (left_category != right_category)
                      return left_category.localeAwareCompare(right_category) < 0;
                  if (left.order != right.order)
                      return left.order < right.order;
                  const auto left_title = left.value.value(QStringLiteral("title")).toString();
                  const auto right_title = right.value.value(QStringLiteral("title")).toString();
                  if (left_title != right_title)
                      return left_title.localeAwareCompare(right_title) < 0;
                  return left.id < right.id;
              });
    QVariantList result;
    for (const auto &entry : ranked)
        result.push_back(entry.value);
    return result;
}

QVariantList StudioCommandController::shortcutEntries() const
{
    QVariantList result;
    for (const auto &definition : impl_->actions)
    {
        const auto state = action(definition.id);
        for (const auto &shortcut : definition.shortcuts)
        {
            const bool enabled = state.value(QStringLiteral("enabled")).toBool() &&
                                 !palette_open_ && !modal_open_ &&
                                 (!shortcut.requires_non_text_input || !text_input_active_);
            result.push_back(
                QVariantMap{{QStringLiteral("actionId"), definition.id},
                            {QStringLiteral("sequence"), shortcut.sequence},
                            {QStringLiteral("nativeText"), native_key(shortcut.sequence)},
                            {QStringLiteral("enabled"), enabled}});
        }
    }
    return result;
}

QVariantMap StudioCommandController::executeAction(const QString &action_id, const QString &source)
{
    const auto found = std::find_if(impl_->actions.cbegin(), impl_->actions.cend(),
                                    [&action_id](const ActionSpec &candidate)
                                    { return candidate.id == action_id; });
    if (found == impl_->actions.cend())
    {
        const auto message = tr_command(QStringLiteral("Unknown action: %1")).arg(action_id);
        presenter_.setError(message);
        emit dispatchRejected(action_id, QStringLiteral("unknown_action"), message);
        return rejected(QStringLiteral("unknown_action"), message);
    }
    const auto state = action(action_id);
    if (!state.value(QStringLiteral("enabled")).toBool())
    {
        const auto message = state.value(QStringLiteral("disabledReason")).toString();
        emit dispatchRejected(action_id, QStringLiteral("unavailable"), message);
        return rejected(QStringLiteral("unavailable"), message);
    }
    return executeCommandInternal(found->command_id,
                                  found->has_argument ? found->argument : QVariant{}, source);
}

QVariantMap StudioCommandController::executeCommand(const QString &command_id,
                                                    const QVariant &argument, const QString &source)
{
    return executeCommandInternal(command_id, argument, source);
}

Result<bool>
StudioCommandController::applyDevelopFields(const std::vector<StudioDevelopField> &fields)
{
    if (fields.empty() || fields.size() > 256U)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Live Develop mutation requires 1 through 256 fields");
    }
    if (modal_open_)
    {
        return make_error(ErrorCode::kConflict,
                          "Studio command is unavailable while a modal surface is open",
                          {{"reason", "command_unavailable"}});
    }
    const bool enter_develop = presenter_.browseMode() != QLatin1String("develop");
    const auto state = resolve_state(
        presenter_, enter_develop ? Condition::kReadySelection : Condition::kDevelopSelection,
        settings_open_);
    if (!state.enabled)
    {
        return make_error(ErrorCode::kConflict, state.reason.toUtf8().toStdString(),
                          {{"reason", "command_unavailable"}});
    }
    if (!presenter_.develop_loaded_ || presenter_.busy_ || presenter_.import_work_active_ ||
        presenter_.develop_job_in_flight_ || presenter_.pending_save_ ||
        presenter_.pending_preview_)
    {
        return make_error(ErrorCode::kConflict, "Studio Develop state is busy",
                          {{"reason", "busy"}});
    }

    DevelopParams next = presenter_.develop_;
    std::set<std::string, std::less<>> names;
    for (const auto &field : fields)
    {
        if (field.name.empty() || field.name.size() > 256U || !std::isfinite(field.value))
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Live Develop field name or value is invalid",
                              {{"name", field.name}});
        }
        if (!names.insert(field.name).second)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Live Develop field was specified more than once",
                              {{"name", field.name}});
        }
        auto applied = apply_develop_field_strict(next, field.name, field.value);
        if (!applied)
        {
            auto error = applied.error();
            error.context.insert_or_assign("reason", "invalid_develop_field");
            return error;
        }
    }
    if (enter_develop)
    {
        presenter_.openDevelop();
    }
    return presenter_.mutate_develop(std::move(next), StudioPresenter::DevelopEdit::Commit);
}

void StudioCommandController::cancelPendingConfirmation(const QString &token)
{
    if (token.isEmpty() || impl_->pending_confirmation_token != token)
        return;
    impl_->pending_confirmation_command.clear();
    impl_->pending_confirmation_token.clear();
    impl_->pending_confirmation_assets.clear();
    impl_->pending_confirmation_argument.clear();
}

QVariantMap StudioCommandController::executeCommandInternal(const QString &command_id,
                                                            const QVariant &argument,
                                                            const QString &source)
{
    static const QSet<QString> sources{QStringLiteral("menu"), QStringLiteral("keyboard"),
                                       QStringLiteral("palette"), QStringLiteral("control"),
                                       QStringLiteral("system")};
    const auto found = impl_->commands.constFind(command_id);
    if (found == impl_->commands.cend())
    {
        const auto message = tr_command(QStringLiteral("Unknown command: %1")).arg(command_id);
        presenter_.setError(message);
        emit dispatchRejected(command_id, QStringLiteral("unknown_command"), message);
        return rejected(QStringLiteral("unknown_command"), message);
    }
    if (!sources.contains(source))
    {
        const auto message = tr_command(QStringLiteral("Unknown command source: %1")).arg(source);
        presenter_.setError(message);
        emit dispatchRejected(command_id, QStringLiteral("invalid_source"), message);
        return rejected(QStringLiteral("invalid_source"), message);
    }
    const auto state = resolve_state(presenter_, found->condition, settings_open_);
    if (!state.enabled)
    {
        emit dispatchRejected(command_id, QStringLiteral("unavailable"), state.reason);
        return rejected(QStringLiteral("unavailable"), state.reason);
    }
    const auto validation_error = found->validator ? found->validator(argument) : QString{};
    if (!validation_error.isEmpty())
    {
        const auto message = tr_command(validation_error);
        presenter_.setError(message);
        emit dispatchRejected(command_id, QStringLiteral("invalid_argument"), message);
        return rejected(QStringLiteral("invalid_argument"), message);
    }
    found->handler(argument, source);
    return accepted();
}

QString StudioCommandController::paletteQuery() const
{
    return palette_query_;
}
bool StudioCommandController::paletteOpen() const noexcept
{
    return palette_open_;
}
bool StudioCommandController::textInputActive() const noexcept
{
    return text_input_active_;
}
bool StudioCommandController::settingsOpen() const noexcept
{
    return settings_open_;
}
bool StudioCommandController::assistantOpen() const noexcept
{
    return assistant_open_;
}
bool StudioCommandController::modalOpen() const noexcept
{
    return modal_open_;
}
qulonglong StudioCommandController::revision() const noexcept
{
    return revision_;
}

void StudioCommandController::setPaletteQuery(const QString &query)
{
    if (palette_query_ == query)
        return;
    palette_query_ = query;
    emit paletteQueryChanged();
    refresh();
}

void StudioCommandController::setPaletteOpen(const bool open)
{
    if (palette_open_ == open)
        return;
    palette_open_ = open;
    if (!open && !palette_query_.isEmpty())
    {
        palette_query_.clear();
        emit paletteQueryChanged();
    }
    emit paletteOpenChanged();
    refresh();
}

void StudioCommandController::setTextInputActive(const bool active)
{
    if (text_input_active_ == active)
        return;
    text_input_active_ = active;
    refresh();
}

void StudioCommandController::setSettingsOpen(const bool open)
{
    if (settings_open_ == open)
        return;
    settings_open_ = open;
    refresh();
}

void StudioCommandController::setAssistantOpen(const bool open)
{
    if (assistant_open_ == open)
        return;
    assistant_open_ = open;
    emit assistantOpenChanged();
    refresh();
}

void StudioCommandController::setModalOpen(const bool open)
{
    if (modal_open_ == open)
        return;
    modal_open_ = open;
    refresh();
}

void StudioCommandController::retranslate()
{
    refresh();
}

void StudioCommandController::refresh()
{
    ++revision_;
    emit commandsChanged();
}

QStringList StudioCommandController::validateBuiltinDefinitions()
{
    QStringList errors;
    const QRegularExpression pattern(QStringLiteral("^studio(?:\\.[a-z][a-z0-9_]*){2,}$"));
    QSet<QString> known_commands;
    for (const auto &id : command_ids())
    {
        if (!pattern.match(id).hasMatch())
            errors.push_back(QStringLiteral("Invalid command ID: %1").arg(id));
        if (known_commands.contains(id))
            errors.push_back(QStringLiteral("Duplicate command ID: %1").arg(id));
        known_commands.insert(id);
    }
    QSet<QString> known_actions;
    QHash<QString, QString> shortcut_owners;
    for (const auto &definition : builtin_actions())
    {
        if (!pattern.match(definition.id).hasMatch())
            errors.push_back(QStringLiteral("Invalid action ID: %1").arg(definition.id));
        if (known_actions.contains(definition.id))
            errors.push_back(QStringLiteral("Duplicate action ID: %1").arg(definition.id));
        known_actions.insert(definition.id);
        if (!known_commands.contains(definition.command_id))
            errors.push_back(QStringLiteral("Action %1 targets unknown command %2")
                                 .arg(definition.id, definition.command_id));
        if ((definition.palette_visible || !definition.menu_path.isEmpty()) &&
            (definition.title.trimmed().isEmpty() || definition.category.trimmed().isEmpty()))
            errors.push_back(
                QStringLiteral("Action %1 is missing presentation metadata").arg(definition.id));
        for (const auto &shortcut : definition.shortcuts)
        {
            const auto owner = shortcut_owners.constFind(shortcut.sequence);
            if (owner != shortcut_owners.cend() && owner.value() != definition.id)
                errors.push_back(QStringLiteral("Shortcut conflict: %1 is owned by %2 and %3")
                                     .arg(shortcut.sequence, owner.value(), definition.id));
            else
                shortcut_owners.insert(shortcut.sequence, definition.id);
        }
    }
    return errors;
}

int StudioCommandController::fuzzyScore(const QString &title, const QString &category,
                                        const QStringList &keywords, const QString &command_id,
                                        const QString &query)
{
    const auto tokens = normalize_search(query).split(QRegularExpression(QStringLiteral("\\s+")),
                                                      Qt::SkipEmptyParts);
    if (tokens.isEmpty())
        return 0;
    int total = 0;
    for (const auto &token : tokens)
    {
        int best = token_score(title, token);
        if (best >= 0)
            best += 400;
        for (const auto &keyword : keywords)
        {
            const int score = token_score(keyword, token);
            if (score >= 0)
                best = std::max(best, score + 300);
        }
        const int category_score = token_score(category, token);
        if (category_score >= 0)
            best = std::max(best, category_score + 200);
        const int id_score = token_score(command_id, token);
        if (id_score >= 0)
            best = std::max(best, id_score + 100);
        if (best < 0)
            return -1;
        total += best;
    }
    return total;
}

QString StudioCommandController::paletteShortcutForPlatform(const QString &platform)
{
    const auto normalized = platform.trimmed().toLower();
    if (normalized == QLatin1String("macos") || normalized == QLatin1String("mac") ||
        normalized == QLatin1String("windows") || normalized == QLatin1String("win") ||
        normalized == QLatin1String("linux"))
        return QStringLiteral("Ctrl+Shift+P");
    return {};
}

} // namespace ravo
