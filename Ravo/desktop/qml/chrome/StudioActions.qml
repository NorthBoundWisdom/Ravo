import QtQuick
import QtQuick.Controls

// Single QML command table. Every menu, shortcut, context menu, and control
// invokes run(), which is the only QML path into presenter.executeCommand.
Item {
    id: root
    width: 0
    height: 0

    property var presenter
    property var windowHost

    readonly property bool catalogReady: presenter && presenter.catalogOpen && !presenter.busy
    readonly property bool interactive: windowHost && !windowHost.settingsOpen
    readonly property bool developOpen: presenter && presenter.browseMode === "develop"
    readonly property bool hasSelection: presenter && presenter.selectedAssetId.length > 0
    readonly property bool catalogOpen: presenter && presenter.catalogOpen

    readonly property QtObject ids: QtObject {
        readonly property string libraryCreate: "library.create"
        readonly property string libraryOpen: "library.open"
        readonly property string libraryImportFiles: "library.importFiles"
        readonly property string libraryImportFolder: "library.importFolder"
        readonly property string libraryExport: "library.export"
        readonly property string libraryExportWrite: "library.exportWrite"
        readonly property string photoSelect: "photo.select"
        readonly property string photoRate: "photo.rate"
        readonly property string photoColor: "photo.color"
        readonly property string photoSetTags: "photo.setTags"
        readonly property string photoSetMetadata: "photo.setMetadata"
        readonly property string photoCreateSnapshot: "photo.createSnapshot"
        readonly property string photoRestoreHistory: "photo.restoreHistory"
        readonly property string librarySetTagFilter: "library.setTagFilter"
        readonly property string photoReject: "photo.reject"
        readonly property string photoRemove: "photo.remove"
        readonly property string photoRemoveFromDisk: "photo.removeFromDisk"
        readonly property string photoPrevious: "photo.previous"
        readonly property string photoNext: "photo.next"
        readonly property string viewGrid: "view.grid"
        readonly property string viewLoupe: "view.loupe"
        readonly property string viewDevelop: "view.develop"
        readonly property string viewFit: "view.fit"
        readonly property string viewFill: "view.fill"
        readonly property string viewActual: "view.actual"
        readonly property string editUndo: "edit.undo"
        readonly property string editRedo: "edit.redo"
        readonly property string editResetAll: "edit.resetAll"
        readonly property string editResetSection: "edit.resetSection"
        readonly property string editResetControl: "edit.resetControl"
        readonly property string editSetNumber: "edit.setNumber"
        readonly property string editSetToneCurve: "edit.setToneCurve"
        readonly property string editSetCrop: "edit.setCrop"
        readonly property string editSetCropAspect: "edit.setCropAspect"
        readonly property string editRotateLeft: "edit.rotateLeft"
        readonly property string editRotateRight: "edit.rotateRight"
        readonly property string editFlipHorizontal: "edit.flipHorizontal"
        readonly property string editFlipVertical: "edit.flipVertical"
        readonly property string editCropTool: "edit.cropTool"
        readonly property string editBeforeAfter: "edit.beforeAfter"
        readonly property string windowSettings: "window.settings"
        readonly property string windowClose: "window.close"
        readonly property string windowQuit: "window.quit"
        readonly property string windowAbout: "window.about"
    }

    property alias createLibrary: createLibraryAction
    property alias openLibrary: openLibraryAction
    property alias importPhotos: importPhotosAction
    property alias importFolder: importFolderAction
    property alias exportPhoto: exportPhotoAction
    property alias closeWindow: closeWindowAction
    property alias preferences: preferencesAction
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
    property alias previousPhoto: previousAction
    property alias nextPhoto: nextAction
    property alias reject: rejectAction
    property alias resetEdits: resetEditsAction
    property alias rotateLeft: rotateLeftAction
    property alias rotateRight: rotateRightAction
    property alias cropTool: cropToolAction
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

    function run(id, argument) {
        if (!root.presenter)
            return;
        if (argument === undefined)
            root.presenter.executeCommand(id);
        else
            root.presenter.executeCommand(id, argument);
    }

    function handlePhotoClick(assetId, mouse) {
        const right = mouse && mouse.button === Qt.RightButton;
        const shift = mouse && (mouse.modifiers & Qt.ShiftModifier);
        const toggle = mouse && (mouse.modifiers & Qt.ControlModifier);
        if (right && root.presenter && root.presenter.isAssetSelected(assetId) && !shift && !toggle) {
            // Keep the current multi-selection when opening the context menu.
        } else if (shift)
            root.run(root.ids.photoSelect, {
                "id": assetId,
                "mode": "range"
            });
        else if (toggle)
            root.run(root.ids.photoSelect, {
                "id": assetId,
                "mode": "toggle"
            });
        else
            root.run(root.ids.photoSelect, {
                "id": assetId,
                "mode": "single"
            });
        if (right && root.windowHost)
            root.windowHost.showPhotoMenu();
    }

    function handlePhotoDoubleClick(assetId) {
        root.run(root.ids.photoSelect, assetId);
        root.run(root.ids.viewLoupe);
    }

    function openGallery(preferredMode) {
        if (preferredMode === "loupe" && root.hasSelection)
            root.run(root.ids.viewLoupe);
        else
            root.run(root.ids.viewGrid);
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
        root.run(root.ids.photoCreateSnapshot, label);
    }

    function restoreHistory(id) {
        root.run(root.ids.photoRestoreHistory, id);
    }

    function setTagFilter(value) {
        root.run(root.ids.librarySetTagFilter, value);
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

    function resetSection(section) {
        root.run(root.ids.editResetSection, section);
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

    function toggleCropTool() {
        root.run(root.ids.editCropTool);
    }

    Action {
        id: createLibraryAction
        text: qsTr("Create Library…")
        shortcut: StandardKey.New
        onTriggered: root.run(root.ids.libraryCreate)
    }
    Action {
        id: openLibraryAction
        text: qsTr("Open Library…")
        shortcut: StandardKey.Open
        onTriggered: root.run(root.ids.libraryOpen)
    }
    Action {
        id: importPhotosAction
        text: qsTr("Import Photos…")
        shortcut: "Ctrl+I"
        enabled: root.catalogReady
        onTriggered: root.run(root.ids.libraryImportFiles)
    }
    Action {
        id: importFolderAction
        text: qsTr("Import Folder…")
        shortcut: "Ctrl+Shift+I"
        enabled: root.catalogReady
        onTriggered: root.run(root.ids.libraryImportFolder)
    }
    Action {
        id: exportPhotoAction
        text: qsTr("Export Photo…")
        shortcut: "Ctrl+Shift+E"
        enabled: root.catalogReady && root.hasSelection
        onTriggered: root.run(root.ids.libraryExport)
    }
    Action {
        id: closeWindowAction
        text: qsTr("Close")
        shortcut: StandardKey.Close
        onTriggered: root.run(root.ids.windowClose)
    }
    Action {
        id: preferencesAction
        text: qsTr("Settings…")
        shortcut: StandardKey.Preferences
        onTriggered: root.run(root.ids.windowSettings)
    }
    Action {
        id: quitAction
        text: qsTr("Quit Ravo Studio")
        shortcut: StandardKey.Quit
        onTriggered: root.run(root.ids.windowQuit)
    }
    Action {
        id: undoAction
        text: qsTr("Undo")
        shortcut: StandardKey.Undo
        enabled: root.interactive && root.developOpen && root.presenter && root.presenter.canUndo
        onTriggered: root.run(root.ids.editUndo)
    }
    Action {
        id: redoAction
        text: qsTr("Redo")
        shortcut: StandardKey.Redo
        enabled: root.interactive && root.developOpen && root.presenter && root.presenter.canRedo
        onTriggered: root.run(root.ids.editRedo)
    }
    Action {
        id: gridAction
        text: qsTr("Gallery")
        shortcut: "Ctrl+1"
        enabled: root.interactive && root.catalogOpen
        onTriggered: root.run(root.ids.viewGrid)
    }
    Action {
        id: loupeAction
        text: qsTr("Loupe")
        shortcut: "Ctrl+2"
        enabled: root.interactive && root.catalogOpen && root.hasSelection
        onTriggered: root.run(root.ids.viewLoupe)
    }
    Action {
        id: developAction
        text: qsTr("Edit")
        shortcut: "Ctrl+3"
        enabled: root.interactive && root.catalogOpen
        onTriggered: root.run(root.ids.viewDevelop)
    }
    Action {
        id: fitAction
        text: qsTr("Fit")
        shortcut: "Ctrl+0"
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        onTriggered: root.run(root.ids.viewFit)
    }
    Action {
        id: fillAction
        text: qsTr("Fill")
        shortcut: "Ctrl+9"
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        onTriggered: root.run(root.ids.viewFill)
    }
    Action {
        id: actualSizeAction
        text: qsTr("Actual Size")
        shortcut: "Ctrl+Alt+0"
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        onTriggered: root.run(root.ids.viewActual)
    }
    Action {
        id: beforeAfterAction
        text: qsTr("Before / After")
        shortcut: "\\"
        enabled: root.interactive && root.developOpen
        onTriggered: root.run(root.ids.editBeforeAfter)
    }
    Action {
        id: previousAction
        text: qsTr("Previous Photo")
        shortcut: "Left"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.run(root.ids.photoPrevious)
    }
    Action {
        id: nextAction
        text: qsTr("Next Photo")
        shortcut: "Right"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.run(root.ids.photoNext)
    }
    Shortcut {
        enabled: root.interactive && root.hasSelection
        sequence: "Shift+Left"
        onActivated: root.run(root.ids.photoPrevious, "range")
    }
    Shortcut {
        enabled: root.interactive && root.hasSelection
        sequence: "Shift+Right"
        onActivated: root.run(root.ids.photoNext, "range")
    }
    Action {
        id: rejectAction
        text: root.presenter && root.presenter.selectedRejected ? qsTr("Unreject") : qsTr("Reject")
        shortcut: "X"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.run(root.ids.photoReject)
    }
    Action {
        id: resetEditsAction
        text: qsTr("Reset All Edits")
        shortcut: "Ctrl+Shift+R"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: root.run(root.ids.editResetAll)
    }
    Action {
        id: cropToolAction
        text: root.presenter && root.presenter.cropToolActive ? qsTr("Done Cropping") : qsTr("Crop & Rotate")
        shortcut: "R"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: root.toggleCropTool()
    }
    Action {
        id: rotateLeftAction
        text: qsTr("Rotate Left")
        shortcut: "Ctrl+["
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: root.run(root.ids.editRotateLeft)
    }
    Action {
        id: rotateRightAction
        text: qsTr("Rotate Right")
        shortcut: "Ctrl+]"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: root.run(root.ids.editRotateRight)
    }
    Action {
        id: flipHorizontalAction
        text: qsTr("Flip Horizontal")
        shortcut: "Ctrl+Shift+H"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: root.run(root.ids.editFlipHorizontal)
    }
    Action {
        id: flipVerticalAction
        text: qsTr("Flip Vertical")
        shortcut: "Ctrl+Shift+V"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: root.run(root.ids.editFlipVertical)
    }
    Action {
        id: aboutAction
        text: qsTr("About Ravo Studio")
        onTriggered: root.run(root.ids.windowAbout)
    }
    Action {
        id: rating0Action
        text: qsTr("Rating 0")
        shortcut: "0"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setRating(0)
    }
    Action {
        id: rating1Action
        text: qsTr("Rating 1")
        shortcut: "1"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setRating(1)
    }
    Action {
        id: rating2Action
        text: qsTr("Rating 2")
        shortcut: "2"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setRating(2)
    }
    Action {
        id: rating3Action
        text: qsTr("Rating 3")
        shortcut: "3"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setRating(3)
    }
    Action {
        id: rating4Action
        text: qsTr("Rating 4")
        shortcut: "4"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setRating(4)
    }
    Action {
        id: rating5Action
        text: qsTr("Rating 5")
        shortcut: "5"
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setRating(5)
    }
    Action {
        id: colorNoneAction
        text: qsTr("No Color")
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setColorLabel("none")
    }
    Action {
        id: colorRedAction
        text: qsTr("Red")
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setColorLabel("red")
    }
    Action {
        id: colorYellowAction
        text: qsTr("Yellow")
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setColorLabel("yellow")
    }
    Action {
        id: colorGreenAction
        text: qsTr("Green")
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setColorLabel("green")
    }
    Action {
        id: colorBlueAction
        text: qsTr("Blue")
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setColorLabel("blue")
    }
    Action {
        id: colorPurpleAction
        text: qsTr("Purple")
        enabled: root.interactive && root.hasSelection
        onTriggered: root.setColorLabel("purple")
    }
    Action {
        id: removeAction
        text: qsTr("Remove from Catalog…")
        shortcut: StandardKey.Delete
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.windowHost)
            root.windowHost.askRemovePhoto()
    }
    Action {
        id: removeFromDiskAction
        text: qsTr("Delete from Disk…")
        enabled: root.interactive && root.hasSelection && root.presenter && root.presenter.canDeleteFromDisk
        onTriggered: if (root.windowHost)
            root.windowHost.askDeleteFromDisk()
    }

    Shortcut {
        enabled: root.interactive && root.catalogOpen
        sequence: "G"
        onActivated: gridAction.trigger()
    }
    Shortcut {
        enabled: root.interactive && root.catalogOpen
        sequence: "E"
        onActivated: loupeAction.trigger()
    }
    Shortcut {
        enabled: root.interactive && root.catalogOpen
        sequence: "D"
        onActivated: developAction.trigger()
    }
    Shortcut {
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        sequence: "F"
        onActivated: fitAction.trigger()
    }
    Shortcut {
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        sequence: "Shift+1"
        onActivated: actualSizeAction.trigger()
    }
    Shortcut {
        enabled: root.interactive && root.developOpen
        sequence: "Z"
        onActivated: undoAction.trigger()
    }
    Shortcut {
        enabled: root.interactive && root.developOpen
        sequence: "Shift+Z"
        onActivated: redoAction.trigger()
    }
    Shortcut {
        enabled: removeAction.enabled
        sequence: "Backspace"
        onActivated: removeAction.trigger()
    }
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (root.windowHost && root.windowHost.settingsOpen)
                root.windowHost.settingsOpen = false;
            else
                root.run(root.ids.viewGrid);
        }
    }
    Shortcut {
        enabled: root.interactive
        sequences: ["Return", "Enter"]
        onActivated: loupeAction.trigger()
    }
}
