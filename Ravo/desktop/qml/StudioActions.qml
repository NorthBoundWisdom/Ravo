import QtQuick
import QtQuick.Controls

// Command table for menus and shortcuts.
// Ctrl in sequences is portable: Command on macOS, Ctrl on Windows/Linux (MuseScore convention).
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

    property alias createLibrary: createLibraryAction
    property alias openLibrary: openLibraryAction
    property alias importPhotos: importPhotosAction
    property alias importFolder: importFolderAction
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
    property alias about: aboutAction
    property alias rating0: rating0Action
    property alias rating1: rating1Action
    property alias rating2: rating2Action
    property alias rating3: rating3Action
    property alias rating4: rating4Action
    property alias rating5: rating5Action

    Action {
        id: createLibraryAction
        text: qsTr("Create Library…")
        shortcut: StandardKey.New
        onTriggered: if (root.windowHost) root.windowHost.openCreateLibraryDialog()
    }
    Action {
        id: openLibraryAction
        text: qsTr("Open Library…")
        shortcut: StandardKey.Open
        onTriggered: if (root.windowHost) root.windowHost.openOpenLibraryDialog()
    }
    Action {
        id: importPhotosAction
        text: qsTr("Import Photos…")
        shortcut: "Ctrl+I"
        enabled: root.catalogReady
        onTriggered: if (root.windowHost) root.windowHost.openImportDialog()
    }
    Action {
        id: importFolderAction
        text: qsTr("Import Folder…")
        shortcut: "Ctrl+Shift+I"
        enabled: root.catalogReady
        onTriggered: if (root.windowHost) root.windowHost.openImportFolderDialog()
    }
    Action {
        id: closeWindowAction
        text: qsTr("Close")
        shortcut: StandardKey.Close
        onTriggered: if (root.windowHost) root.windowHost.close()
    }
    Action {
        id: preferencesAction
        text: qsTr("Settings…")
        shortcut: StandardKey.Preferences
        onTriggered: if (root.windowHost) root.windowHost.settingsOpen = true
    }
    Action {
        id: quitAction
        text: qsTr("Quit Ravo Studio")
        shortcut: StandardKey.Quit
        onTriggered: Qt.quit()
    }
    Action {
        id: undoAction
        text: qsTr("Undo")
        shortcut: StandardKey.Undo
        enabled: root.interactive && root.developOpen && root.presenter && root.presenter.canUndo
        onTriggered: if (root.presenter) root.presenter.undoEdit()
    }
    Action {
        id: redoAction
        text: qsTr("Redo")
        shortcut: StandardKey.Redo
        enabled: root.interactive && root.developOpen && root.presenter && root.presenter.canRedo
        onTriggered: if (root.presenter) root.presenter.redoEdit()
    }
    Action {
        id: gridAction
        text: qsTr("Grid")
        shortcut: "Ctrl+1"
        enabled: root.interactive && root.presenter && root.presenter.catalogOpen
        onTriggered: if (root.presenter) root.presenter.returnToGrid()
    }
    Action {
        id: loupeAction
        text: qsTr("Loupe")
        shortcut: "Ctrl+2"
        enabled: root.interactive && root.presenter && root.presenter.catalogOpen
        onTriggered: if (root.presenter) root.presenter.openLoupe()
    }
    Action {
        id: developAction
        text: qsTr("Edit")
        shortcut: "Ctrl+3"
        enabled: root.interactive && root.presenter && root.presenter.catalogOpen
        onTriggered: if (root.presenter) root.presenter.openDevelop()
    }
    Action {
        id: fitAction
        text: qsTr("Fit")
        shortcut: "Ctrl+0"
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        onTriggered: if (root.presenter) root.presenter.setZoomMode("fit")
    }
    Action {
        id: fillAction
        text: qsTr("Fill")
        shortcut: "Ctrl+9"
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        onTriggered: if (root.presenter) root.presenter.setZoomMode("fill")
    }
    Action {
        id: actualSizeAction
        text: qsTr("Actual Size")
        shortcut: "Ctrl+Alt+0"
        enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"
        onTriggered: if (root.presenter) root.presenter.setZoomMode("actual")
    }
    Action {
        id: beforeAfterAction
        text: qsTr("Before / After")
        shortcut: "\\"
        enabled: root.interactive && root.developOpen
        onTriggered: if (root.presenter) root.presenter.toggleBeforeAfter()
    }
    Action {
        id: previousAction
        text: qsTr("Previous Photo")
        shortcut: "Left"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.selectPrevious()
    }
    Action {
        id: nextAction
        text: qsTr("Next Photo")
        shortcut: "Right"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.selectNext()
    }
    Action {
        id: rejectAction
        text: qsTr("Reject")
        shortcut: "X"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.toggleRejected()
    }
    Action {
        id: resetEditsAction
        text: qsTr("Reset All Edits")
        shortcut: "Ctrl+Shift+R"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.resetAllEdits()
    }
    Action {
        id: rotateLeftAction
        text: qsTr("Rotate Left")
        shortcut: "Ctrl+["
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.rotateLeft()
    }
    Action {
        id: rotateRightAction
        text: qsTr("Rotate Right")
        shortcut: "Ctrl+]"
        enabled: root.interactive && root.developOpen && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.rotateRight()
    }
    Action {
        id: aboutAction
        text: qsTr("About Ravo Studio")
        onTriggered: if (root.windowHost) root.windowHost.openAboutDialog()
    }
    Action {
        id: rating0Action
        text: qsTr("Rating 0")
        shortcut: "0"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.setRating(0)
    }
    Action {
        id: rating1Action
        text: qsTr("Rating 1")
        shortcut: "1"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.setRating(1)
    }
    Action {
        id: rating2Action
        text: qsTr("Rating 2")
        shortcut: "2"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.setRating(2)
    }
    Action {
        id: rating3Action
        text: qsTr("Rating 3")
        shortcut: "3"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.setRating(3)
    }
    Action {
        id: rating4Action
        text: qsTr("Rating 4")
        shortcut: "4"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.setRating(4)
    }
    Action {
        id: rating5Action
        text: qsTr("Rating 5")
        shortcut: "5"
        enabled: root.interactive && root.hasSelection
        onTriggered: if (root.presenter) root.presenter.setRating(5)
    }

    // Additional Lightroom-style keys; menus show the Ctrl/⌘ sequences above.
    Shortcut { enabled: root.interactive && root.presenter && root.presenter.catalogOpen; sequence: "G"; onActivated: gridAction.trigger() }
    Shortcut { enabled: root.interactive && root.presenter && root.presenter.catalogOpen; sequence: "E"; onActivated: loupeAction.trigger() }
    Shortcut { enabled: root.interactive && root.presenter && root.presenter.catalogOpen; sequence: "D"; onActivated: developAction.trigger() }
    Shortcut { enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"; sequence: "F"; onActivated: fitAction.trigger() }
    Shortcut { enabled: root.interactive && root.presenter && root.presenter.browseMode !== "grid"; sequence: "Shift+1"; onActivated: actualSizeAction.trigger() }
    Shortcut { enabled: root.interactive && root.developOpen; sequence: "Z"; onActivated: undoAction.trigger() }
    Shortcut { enabled: root.interactive && root.developOpen; sequence: "Shift+Z"; onActivated: redoAction.trigger() }
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (root.windowHost && root.windowHost.settingsOpen)
                root.windowHost.settingsOpen = false
            else if (root.presenter)
                root.presenter.returnToGrid()
        }
    }
    Shortcut { enabled: root.interactive; sequences: ["Return", "Enter"]; onActivated: loupeAction.trigger() }
}