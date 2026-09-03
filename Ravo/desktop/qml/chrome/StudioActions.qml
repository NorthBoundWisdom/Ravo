import QtQuick
import QtQuick.Controls

// Thin projection for controls that need named Action objects or argument helpers.
// IDs, metadata, state, shortcuts, and dispatch are owned by the C++ controller.
Item {
    id: root
    width: 0
    height: 0
    required property var controller
    property var presenter
    property var windowHost
    readonly property var ids: controller ? controller.ids : ({})
    readonly property bool hasSelection: presenter && presenter.selectedAssetId.length > 0

    function run(id, argument, source) {
        if (root.controller)
            root.controller.executeCommand(id, argument === undefined ? null : argument, source === undefined ? "control" : source);
    }
    function trigger(actionId, source) {
        if (root.controller)
            root.controller.executeAction(actionId, source === undefined ? "control" : source);
    }
    function handlePhotoClick(assetId, button, modifiers) {
        const mods = Number(modifiers);
        const right = button === Qt.RightButton;
        const shift = (mods & Qt.ShiftModifier) !== 0;
        const toggle = (mods & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
        if (!(right && root.presenter && root.presenter.isAssetSelected(assetId) && !shift && !toggle))
            root.run(root.ids.photoSelect, {
                "id": assetId,
                "modifiers": mods
            });
        if (right && root.windowHost)
            root.windowHost.showPhotoMenu();
    }
    function handlePhotoDoubleClick(assetId) {
        root.run(root.ids.photoSelect, assetId);
        root.trigger(root.ids.viewLoupe);
    }
    function openGallery(preferredMode) {
        if (preferredMode === "survey" && root.presenter && root.presenter.selectedCount >= 2) {
            root.trigger(root.ids.viewSurvey);
            return;
        }
        root.trigger(preferredMode === "loupe" && root.hasSelection ? root.ids.viewLoupe : root.ids.viewGrid);
    }
    function setRating(value) {
        root.run(root.ids.photoRate, value);
    }
    function setColorLabel(value) {
        root.run(root.ids.photoColor, value);
    }
    function setTags(value) {
        root.run(root.ids.photoSetTags, value);
    }
    function setMetadata(name, value) {
        root.run(root.ids.photoSetMetadata, {
            "name": name,
            "value": value
        });
    }
    function createSnapshot(label) {
        root.run(root.ids.photoCreateSnapshot, label === undefined ? "" : label);
    }
    function renameSnapshot(id, label) {
        root.run(root.ids.photoRenameSnapshot, {
            "id": id,
            "label": label
        });
    }
    function restoreHistory(id) {
        root.run(root.ids.photoRestoreHistory, id);
    }
    function setTagFilter(value) {
        root.run(root.ids.librarySetTagFilter, value);
    }
    function setTextFilter(value) {
        root.run(root.ids.librarySetTextFilter, value);
    }
    function setMediaFilter(value) {
        root.run(root.ids.librarySetMediaFilter, value);
    }
    function setEditFilter(value) {
        root.run(root.ids.librarySetEditFilter, value);
    }
    function setCameraFacetFilter(make, model) {
        root.run(root.ids.librarySetCameraFilter, {
            "make": make === undefined ? "" : make,
            "model": model === undefined ? "" : model
        });
    }
    function setLensFacetFilter(value) {
        root.run(root.ids.librarySetLensFilter, value === undefined ? "" : value);
    }
    function setCaptureDateFacetFilter(value) {
        root.run(root.ids.librarySetCaptureDateFilter, value === undefined ? "" : value);
    }
    function setLocationFacetFilter(country, provinceState, city, sublocation) {
        root.run(root.ids.librarySetLocationFilter, {
            "country": country === undefined ? "" : country,
            "province_state": provinceState === undefined ? "" : provinceState,
            "city": city === undefined ? "" : city,
            "sublocation": sublocation === undefined ? "" : sublocation
        });
    }
    function previewDevelopNumber(name, value) {
        root.run(root.ids.editSetNumber, {
            "name": name,
            "value": value,
            "live": true
        });
    }
    function setDevelopNumber(name, value) {
        root.run(root.ids.editSetNumber, {
            "name": name,
            "value": value
        });
    }
    function previewDevelopNumbers(fields) {
        root.run(root.ids.editSetNumbers, {
            "fields": fields,
            "live": true
        });
    }
    function setDevelopNumbers(fields) {
        root.run(root.ids.editSetNumbers, {
            "fields": fields
        });
    }
    function copySelectedParameters(fields) {
        root.run(root.ids.editCopyParametersSelected, fields);
    }
    function pickWhiteBalance(x, y) {
        root.run(root.ids.editPickWhiteBalance, {
            "x": x,
            "y": y
        });
    }
    function setWhiteBalancePickActive(active) {
        root.run(root.ids.editSetWhiteBalancePick, active);
    }
    function placeMask(x, y) {
        root.run(root.ids.editPlaceMask, {
            "x": x,
            "y": y
        });
    }
    function setMaskPlaceActive(active) {
        root.run(root.ids.editSetMaskPlace, active);
    }
    function assistParametricMask(x, y) {
        root.run(root.ids.editAssistParametricMask, {
            "x": x,
            "y": y
        });
    }
    function setMaskParametricAssistActive(active) {
        root.run(root.ids.editSetMaskParametricAssist, active);
    }
    function setDevelopText(name, value) {
        root.run(root.ids.editSetText, {
            "name": name,
            "value": value
        });
    }
    function previewToneCurve(points) {
        root.run(root.ids.editSetToneCurve, {
            "points": points,
            "live": true
        });
    }
    function setToneCurve(points) {
        root.run(root.ids.editSetToneCurve, {
            "points": points
        });
    }
    function previewCurve(family, channel, points) {
        root.run(root.ids.editSetToneCurve, {
            "family": family,
            "channel": channel,
            "points": points,
            "live": true
        });
    }
    function setCurve(family, channel, points) {
        root.run(root.ids.editSetToneCurve, {
            "family": family,
            "channel": channel,
            "points": points
        });
    }
    function addRetouchRegion(region) {
        root.run(root.ids.editAddRetouchRegion, region);
    }
    function removeRetouchRegion(index) {
        root.run(root.ids.editRemoveRetouchRegion, index);
    }
    function resetSection(section) {
        root.run(root.ids.editResetSection, section);
    }
    function setSectionEnabled(section, enabled) {
        root.run(root.ids.editSetSectionEnabled, {
            "section": section,
            "enabled": enabled
        });
    }
    function resetControl(name) {
        root.run(root.ids.editResetControl, name);
    }
    function previewCropRect(x, y, width, height) {
        root.run(root.ids.editSetCrop, {
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "live": true
        });
    }
    function setCropRect(x, y, width, height) {
        root.run(root.ids.editSetCrop, {
            "x": x,
            "y": y,
            "width": width,
            "height": height
        });
    }
    function setCropAspect(aspect) {
        root.run(root.ids.editSetCropAspect, aspect);
    }
    function autoPerspective(mode) {
        root.run(root.ids.editAutoPerspective, mode);
    }
    function toggleCropTool() {
        root.trigger(root.ids.editCropTool);
    }

    component RegisteredAction: Action {
        required property string actionId
        readonly property var spec: {
            const ignoredRevision = root.controller ? root.controller.revision : 0;
            return root.controller ? root.controller.action(actionId) : ({});
        }
        text: spec.title || ""
        enabled: Boolean(spec.enabled)
        checkable: Boolean(spec.checkable)
        checked: Boolean(spec.checked)
        onTriggered: root.trigger(actionId)
    }

    property alias createLibrary: createLibraryAction
    property alias openLibrary: openLibraryAction
    property alias importPhotos: importPhotosAction
    property alias exportPhoto: exportPhotoAction
    property alias closeWindow: closeWindowAction
    property alias preferences: preferencesAction
    property alias assistant: assistantAction
    property alias quitApp: quitAction
    property alias undo: undoAction
    property alias redo: redoAction
    property alias grid: gridAction
    property alias loupe: loupeAction
    property alias develop: developAction
    property alias fit: fitAction
    property alias fill: fillAction
    property alias actualSize: actualSizeAction
    property alias beforeAfter: beforeAfterAction
    property alias comparison: comparisonAction
    property alias previousPhoto: previousAction
    property alias nextPhoto: nextAction
    property alias reject: rejectAction
    property alias resetEdits: resetEditsAction
    property alias copyParameters: copyParametersAction
    property alias pasteParameters: pasteParametersAction
    property alias pasteParametersToSelection: pasteParametersToSelectionAction
    property alias copyPhotoInfo: copyPhotoInfoAction
    property alias copyPhotoParameters: copyPhotoParametersAction
    property alias revealInFileManager: revealInFileManagerAction
    property alias cropTool: cropToolAction
    property alias rotateLeft: rotateLeftAction
    property alias rotateRight: rotateRightAction
    property alias flipHorizontal: flipHorizontalAction
    property alias flipVertical: flipVerticalAction
    property alias about: aboutAction
    property alias rating0: rating0Action
    property alias rating1: rating1Action
    property alias rating2: rating2Action
    property alias rating3: rating3Action
    property alias rating4: rating4Action
    property alias rating5: rating5Action
    property alias colorNone: colorNoneAction
    property alias colorRed: colorRedAction
    property alias colorYellow: colorYellowAction
    property alias colorGreen: colorGreenAction
    property alias colorBlue: colorBlueAction
    property alias colorPurple: colorPurpleAction
    property alias removePhoto: removeAction
    property alias removeFromDisk: removeFromDiskAction

    RegisteredAction {
        id: createLibraryAction
        actionId: root.ids.libraryCreate || ""
    }
    RegisteredAction {
        id: openLibraryAction
        actionId: root.ids.libraryOpen || ""
    }
    RegisteredAction {
        id: importPhotosAction
        actionId: root.ids.libraryImportFiles || ""
    }
    RegisteredAction {
        id: exportPhotoAction
        actionId: root.ids.libraryExport || ""
    }
    RegisteredAction {
        id: closeWindowAction
        actionId: root.ids.windowClose || ""
    }
    RegisteredAction {
        id: preferencesAction
        actionId: root.ids.windowSettings || ""
    }
    RegisteredAction {
        id: assistantAction
        actionId: root.ids.windowAssistant || ""
    }
    RegisteredAction {
        id: quitAction
        actionId: root.ids.windowQuit || ""
    }
    RegisteredAction {
        id: undoAction
        actionId: root.ids.editUndo || ""
    }
    RegisteredAction {
        id: redoAction
        actionId: root.ids.editRedo || ""
    }
    RegisteredAction {
        id: gridAction
        actionId: root.ids.viewGrid || ""
    }
    RegisteredAction {
        id: loupeAction
        actionId: root.ids.viewLoupe || ""
    }
    RegisteredAction {
        id: developAction
        actionId: root.ids.viewDevelop || ""
    }
    RegisteredAction {
        id: fitAction
        actionId: root.ids.viewFit || ""
    }
    RegisteredAction {
        id: fillAction
        actionId: root.ids.viewFill || ""
    }
    RegisteredAction {
        id: actualSizeAction
        actionId: root.ids.viewActual || ""
    }
    RegisteredAction {
        id: beforeAfterAction
        actionId: root.ids.editBeforeAfter || ""
    }
    RegisteredAction {
        id: comparisonAction
        actionId: root.ids.editComparison || ""
    }
    RegisteredAction {
        id: previousAction
        actionId: root.ids.photoPrevious || ""
    }
    RegisteredAction {
        id: nextAction
        actionId: root.ids.photoNext || ""
    }
    RegisteredAction {
        id: rejectAction
        actionId: root.ids.photoReject || ""
    }
    RegisteredAction {
        id: resetEditsAction
        actionId: root.ids.editResetAll || ""
    }
    RegisteredAction {
        id: copyParametersAction
        actionId: root.ids.editCopyParameters || ""
    }
    RegisteredAction {
        id: pasteParametersAction
        actionId: root.ids.editPasteParameters || ""
    }
    RegisteredAction {
        id: pasteParametersToSelectionAction
        actionId: root.ids.editPasteParametersToSelection || ""
    }
    RegisteredAction {
        id: copyPhotoInfoAction
        actionId: root.ids.photoCopyInfo || ""
    }
    RegisteredAction {
        id: copyPhotoParametersAction
        actionId: root.ids.photoCopyParameters || ""
    }
    RegisteredAction {
        id: revealInFileManagerAction
        actionId: root.ids.photoRevealInFileManager || ""
    }
    RegisteredAction {
        id: cropToolAction
        actionId: root.ids.editCropTool || ""
    }
    RegisteredAction {
        id: rotateLeftAction
        actionId: root.ids.editRotateLeft || ""
    }
    RegisteredAction {
        id: rotateRightAction
        actionId: root.ids.editRotateRight || ""
    }
    RegisteredAction {
        id: flipHorizontalAction
        actionId: root.ids.editFlipHorizontal || ""
    }
    RegisteredAction {
        id: flipVerticalAction
        actionId: root.ids.editFlipVertical || ""
    }
    RegisteredAction {
        id: aboutAction
        actionId: root.ids.windowAbout || ""
    }
    RegisteredAction {
        id: rating0Action
        actionId: root.ids.actionRating0 || ""
    }
    RegisteredAction {
        id: rating1Action
        actionId: root.ids.actionRating1 || ""
    }
    RegisteredAction {
        id: rating2Action
        actionId: root.ids.actionRating2 || ""
    }
    RegisteredAction {
        id: rating3Action
        actionId: root.ids.actionRating3 || ""
    }
    RegisteredAction {
        id: rating4Action
        actionId: root.ids.actionRating4 || ""
    }
    RegisteredAction {
        id: rating5Action
        actionId: root.ids.actionRating5 || ""
    }
    RegisteredAction {
        id: colorNoneAction
        actionId: root.ids.actionColorNone || ""
    }
    RegisteredAction {
        id: colorRedAction
        actionId: root.ids.actionColorRed || ""
    }
    RegisteredAction {
        id: colorYellowAction
        actionId: root.ids.actionColorYellow || ""
    }
    RegisteredAction {
        id: colorGreenAction
        actionId: root.ids.actionColorGreen || ""
    }
    RegisteredAction {
        id: colorBlueAction
        actionId: root.ids.actionColorBlue || ""
    }
    RegisteredAction {
        id: colorPurpleAction
        actionId: root.ids.actionColorPurple || ""
    }
    RegisteredAction {
        id: removeAction
        actionId: root.ids.photoRemove || ""
    }
    RegisteredAction {
        id: removeFromDiskAction
        actionId: root.ids.photoRemoveFromDisk || ""
    }
}
