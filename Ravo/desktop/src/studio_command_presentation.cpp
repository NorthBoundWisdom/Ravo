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

namespace ravo
{
using namespace command_internal;

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
        {QStringLiteral("libraryRecoveryStatus"), QLatin1String(command::kLibraryRecoveryStatus)},
        {QStringLiteral("libraryRecoverySync"), QLatin1String(command::kLibraryRecoverySync)},
        {QStringLiteral("libraryBackupCreate"), QLatin1String(command::kLibraryBackupCreate)},
        {QStringLiteral("libraryBackupCreatePath"),
         QLatin1String(command::kLibraryBackupCreatePath)},
        {QStringLiteral("libraryBackupVerify"), QLatin1String(command::kLibraryBackupVerify)},
        {QStringLiteral("libraryBackupVerifyPath"),
         QLatin1String(command::kLibraryBackupVerifyPath)},
        {QStringLiteral("libraryBackupRestore"), QLatin1String(command::kLibraryBackupRestore)},
        {QStringLiteral("libraryBackupRestorePaths"),
         QLatin1String(command::kLibraryBackupRestorePaths)},
        {QStringLiteral("libraryBackupSchedule"), QLatin1String(command::kLibraryBackupSchedule)},
        {QStringLiteral("libraryBackupSchedulePath"),
         QLatin1String(command::kLibraryBackupSchedulePath)},
        {QStringLiteral("libraryBackupScheduleDisable"),
         QLatin1String(command::kLibraryBackupScheduleDisable)},
        {QStringLiteral("libraryBackupScheduleRun"),
         QLatin1String(command::kLibraryBackupScheduleRun)},
        {QStringLiteral("libraryPreviewRebuildSelected"),
         QLatin1String(command::kLibraryPreviewRebuildSelected)},
        {QStringLiteral("libraryPreviewRebuildAll"),
         QLatin1String(command::kLibraryPreviewRebuildAll)},
        {QStringLiteral("libraryCancelOperation"), QLatin1String(command::kLibraryCancelOperation)},
        {QStringLiteral("librarySetTagFilter"), QLatin1String(command::kLibrarySetTagFilter)},
        {QStringLiteral("librarySetRatingFilter"), QLatin1String(command::kLibrarySetRatingFilter)},
        {QStringLiteral("libraryToggleColorFilter"),
         QLatin1String(command::kLibraryToggleColorFilter)},
        {QStringLiteral("librarySetRejectFilter"), QLatin1String(command::kLibrarySetRejectFilter)},
        {QStringLiteral("librarySetTextFilter"), QLatin1String(command::kLibrarySetTextFilter)},
        {QStringLiteral("librarySetMediaFilter"), QLatin1String(command::kLibrarySetMediaFilter)},
        {QStringLiteral("librarySetEditFilter"), QLatin1String(command::kLibrarySetEditFilter)},
        {QStringLiteral("librarySetCameraFilter"), QLatin1String(command::kLibrarySetCameraFilter)},
        {QStringLiteral("librarySetLensFilter"), QLatin1String(command::kLibrarySetLensFilter)},
        {QStringLiteral("librarySetLensNameFilter"),
         QLatin1String(command::kLibrarySetLensNameFilter)},
        {QStringLiteral("librarySetCaptureDateFilter"),
         QLatin1String(command::kLibrarySetCaptureDateFilter)},
        {QStringLiteral("librarySetLocationFilter"),
         QLatin1String(command::kLibrarySetLocationFilter)},
        {QStringLiteral("librarySetSort"), QLatin1String(command::kLibrarySetSort)},
        {QStringLiteral("libraryClearFilters"), QLatin1String(command::kLibraryClearFilters)},
        {QStringLiteral("librarySelectFolder"), QLatin1String(command::kLibrarySelectFolder)},
        {QStringLiteral("librarySelectLastImport"),
         QLatin1String(command::kLibrarySelectLastImport)},
        {QStringLiteral("librarySelectSet"), QLatin1String(command::kLibrarySelectSet)},
        {QStringLiteral("libraryCreateManualSet"), QLatin1String(command::kLibraryCreateManualSet)},
        {QStringLiteral("libraryCreateSmartSet"), QLatin1String(command::kLibraryCreateSmartSet)},
        {QStringLiteral("libraryRenameSet"), QLatin1String(command::kLibraryRenameSet)},
        {QStringLiteral("libraryDeleteSet"), QLatin1String(command::kLibraryDeleteSet)},
        {QStringLiteral("libraryAddSelectionToSet"),
         QLatin1String(command::kLibraryAddSelectionToSet)},
        {QStringLiteral("libraryRemoveSelectionFromSet"),
         QLatin1String(command::kLibraryRemoveSelectionFromSet)},
        {QStringLiteral("libraryToggleStackCollapse"),
         QLatin1String(command::kLibraryToggleStackCollapse)},
        {QStringLiteral("libraryFolderRelink"), QLatin1String(command::kLibraryFolderRelink)},
        {QStringLiteral("libraryFolderRelinkPath"),
         QLatin1String(command::kLibraryFolderRelinkPath)},
        {QStringLiteral("libraryRevealFolder"), QLatin1String(command::kLibraryRevealFolder)},
        {QStringLiteral("libraryRemoveFolder"),
         QLatin1String(command::kLibraryRequestRemoveFolder)},
        {QStringLiteral("libraryRemoveFolderConfirmed"),
         QLatin1String(command::kLibraryRemoveFolder)},
        {QStringLiteral("photoSelect"), QLatin1String(command::kPhotoSelect)},
        {QStringLiteral("photoSelectAll"), QLatin1String(command::kPhotoSelectAll)},
        {QStringLiteral("photoRate"), QLatin1String(command::kPhotoSetRating)},
        {QStringLiteral("photoColor"), QLatin1String(command::kPhotoSetColor)},
        {QStringLiteral("photoSetTags"), QLatin1String(command::kPhotoSetTags)},
        {QStringLiteral("photoSetMetadata"), QLatin1String(command::kPhotoSetMetadata)},
        {QStringLiteral("photoRefreshMetadata"), QLatin1String(command::kPhotoRefreshMetadata)},
        {QStringLiteral("photoCreateSnapshot"), QLatin1String(command::kPhotoCreateSnapshot)},
        {QStringLiteral("photoRenameSnapshot"), QLatin1String(command::kPhotoRenameSnapshot)},
        {QStringLiteral("photoRestoreHistory"), QLatin1String(command::kPhotoRestoreHistory)},
        {QStringLiteral("photoPick"), QLatin1String(command::kPhotoTogglePick)},
        {QStringLiteral("photoReject"), QLatin1String(command::kPhotoToggleReject)},
        {QStringLiteral("photoUnflag"), QLatin1String(command::kPhotoCullUnflag)},
        {QStringLiteral("photoRemove"), QLatin1String(command::kPhotoRequestRemove)},
        {QStringLiteral("photoRemoveConfirmed"), QLatin1String(command::kPhotoRemove)},
        {QStringLiteral("photoRemoveFromDisk"), QLatin1String(command::kPhotoRequestDelete)},
        {QStringLiteral("photoRemoveFromDiskConfirmed"), QLatin1String(command::kPhotoDelete)},
        {QStringLiteral("photoPrevious"), QLatin1String(command::kPhotoPrevious)},
        {QStringLiteral("photoNext"), QLatin1String(command::kPhotoNext)},
        {QStringLiteral("photoCopyInfo"), QLatin1String(command::kPhotoCopyInfo)},
        {QStringLiteral("photoCopyParameters"), QLatin1String(command::kPhotoCopyParameters)},
        {QStringLiteral("photoRevealInFileManager"),
         QLatin1String(command::kPhotoRevealInFileManager)},
        {QStringLiteral("photoEditIn"), QLatin1String(command::kPhotoEditIn)},
        {QStringLiteral("photoEditInPrepare"), QLatin1String(command::kPhotoEditInPrepare)},
        {QStringLiteral("photoEditInCheckReturned"),
         QLatin1String(command::kPhotoEditInCheckReturned)},
        {QStringLiteral("photoEditInClearSession"),
         QLatin1String(command::kPhotoEditInClearSession)},
        {QStringLiteral("photoEditInAbandon"), QLatin1String(command::kPhotoEditInAbandon)},
        {QStringLiteral("photoEditInReopen"), QLatin1String(command::kPhotoEditInReopen)},
        {QStringLiteral("photoEditInRefreshStatus"),
         QLatin1String(command::kPhotoEditInRefreshStatus)},
        {QStringLiteral("photoOfflineEditRefreshStatus"),
         QLatin1String(command::kPhotoOfflineEditRefreshStatus)},
        {QStringLiteral("photoOfflineEditReconnect"),
         QLatin1String(command::kPhotoOfflineEditReconnect)},
        {QStringLiteral("photoAiProposal"), QLatin1String(command::kPhotoAiProposal)},
        {QStringLiteral("photoAiPropose"), QLatin1String(command::kPhotoAiPropose)},
        {QStringLiteral("photoAiProposalSelect"), QLatin1String(command::kPhotoAiProposalSelect)},
        {QStringLiteral("photoAiProposalRefresh"), QLatin1String(command::kPhotoAiProposalRefresh)},
        {QStringLiteral("photoAiProposalApply"), QLatin1String(command::kPhotoAiProposalApply)},
        {QStringLiteral("photoAiProposalReject"), QLatin1String(command::kPhotoAiProposalReject)},
        {QStringLiteral("photoAiProposalCancel"), QLatin1String(command::kPhotoAiProposalCancel)},
        {QStringLiteral("viewGrid"), QLatin1String(command::kViewGrid)},
        {QStringLiteral("viewLoupe"), QLatin1String(command::kViewLoupe)},
        {QStringLiteral("viewDevelop"), QLatin1String(command::kViewDevelop)},
        {QStringLiteral("viewSurvey"), QLatin1String(command::kViewSurvey)},
        {QStringLiteral("viewBurstCompare"), QLatin1String(command::kViewBurstCompare)},
        {QStringLiteral("viewBurstComparePrevious"),
         QLatin1String(command::kViewBurstComparePrevious)},
        {QStringLiteral("viewBurstCompareNext"), QLatin1String(command::kViewBurstCompareNext)},
        {QStringLiteral("photoCreateVersion"), QLatin1String(command::kPhotoCreateVersion)},
        {QStringLiteral("photoStackSelection"), QLatin1String(command::kPhotoStackSelection)},
        {QStringLiteral("photoUnstack"), QLatin1String(command::kPhotoUnstack)},
        {QStringLiteral("photoSetStackPick"), QLatin1String(command::kPhotoSetStackPick)},
        {QStringLiteral("viewFit"), QLatin1String(command::kViewFit)},
        {QStringLiteral("viewFill"), QLatin1String(command::kViewFill)},
        {QStringLiteral("viewActual"), QLatin1String(command::kViewActual)},
        {QStringLiteral("viewToggleActualSize"), QLatin1String(command::kViewToggleActualSize)},
        {QStringLiteral("viewSetZoomMode"), QLatin1String(command::kViewSetZoomMode)},
        {QStringLiteral("viewAdjustZoom"), QLatin1String(command::kViewAdjustZoom)},
        {QStringLiteral("viewSetThumbnailSize"), QLatin1String(command::kViewSetThumbnailSize)},
        {QStringLiteral("viewSetScopeMode"), QLatin1String(command::kViewSetScopeMode)},
        {QStringLiteral("viewPhotoInfo"), QLatin1String(command::kViewTogglePhotoInfo)},
        {QStringLiteral("editUndo"), QLatin1String(command::kEditUndo)},
        {QStringLiteral("editRedo"), QLatin1String(command::kEditRedo)},
        {QStringLiteral("editCopyParameters"), QLatin1String(command::kEditCopyParameters)},
        {QStringLiteral("editCopyParametersSelected"),
         QLatin1String(command::kEditCopyParametersSelected)},
        {QStringLiteral("editPasteParameters"), QLatin1String(command::kEditPasteParameters)},
        {QStringLiteral("editPasteParametersToSelection"),
         QLatin1String(command::kEditPasteParametersToSelection)},
        {QStringLiteral("editResetAll"), QLatin1String(command::kEditResetAll)},
        {QStringLiteral("editResetSection"), QLatin1String(command::kEditResetSection)},
        {QStringLiteral("editSetSectionEnabled"), QLatin1String(command::kEditSetSectionEnabled)},
        {QStringLiteral("editResetControl"), QLatin1String(command::kEditResetControl)},
        {QStringLiteral("editSetNumber"), QLatin1String(command::kEditSetNumber)},
        {QStringLiteral("editSetNumbers"), QLatin1String(command::kEditSetNumbers)},
        {QStringLiteral("editPickWhiteBalance"), QLatin1String(command::kEditPickWhiteBalance)},
        {QStringLiteral("editSetWhiteBalancePick"),
         QLatin1String(command::kEditSetWhiteBalancePick)},
        {QStringLiteral("editPlaceMask"), QLatin1String(command::kEditPlaceMask)},
        {QStringLiteral("editSetMaskPlace"), QLatin1String(command::kEditSetMaskPlace)},
        {QStringLiteral("editAssistParametricMask"),
         QLatin1String(command::kEditAssistParametricMask)},
        {QStringLiteral("editSetMaskParametricAssist"),
         QLatin1String(command::kEditSetMaskParametricAssist)},
        {QStringLiteral("editSetText"), QLatin1String(command::kEditSetText)},
        {QStringLiteral("editSetToneCurve"), QLatin1String(command::kEditSetToneCurve)},
        {QStringLiteral("editAddRetouchRegion"), QLatin1String(command::kEditAddRetouchRegion)},
        {QStringLiteral("editRemoveRetouchRegion"),
         QLatin1String(command::kEditRemoveRetouchRegion)},
        {QStringLiteral("editSetCrop"), QLatin1String(command::kEditSetCrop)},
        {QStringLiteral("editSetCropAspect"), QLatin1String(command::kEditSetCropAspect)},
        {QStringLiteral("editAutoPerspective"), QLatin1String(command::kEditAutoPerspective)},
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
        {QStringLiteral("presetSave"), QLatin1String(command::kPresetSave)},
        {QStringLiteral("presetSaveSelected"), QLatin1String(command::kPresetSaveSelected)},
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
bool StudioCommandController::photoInfoVisible() const noexcept
{
    return photo_info_visible_;
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
