// Composition host: filename template text + production candidate grid + command textInputActive binding.
// Mirrors Main.qml textInputActive ownership without loading the full Studio shell.
import QtQuick
import QtQuick.Controls
import QtQuick.Window

Item {
    id: root
    width: 900
    height: 600

    property var importCandidates
    property var commandController
    property url productionGridUrl
    property alias templateField: filenameTemplate
    property alias gridHost: host

    readonly property bool textInputActive: {
        const item = Window.window ? Window.window.activeFocusItem : null;
        return !!(item && (item instanceof TextInput || item instanceof TextEdit));
    }

    onTextInputActiveChanged: syncTextInputActive()
    onCommandControllerChanged: syncTextInputActive()

    function syncTextInputActive() {
        if (root.commandController)
            root.commandController.textInputActive = root.textInputActive;
    }

    function focusFilenameTemplateUnselected() {
        filenameTemplate.forceActiveFocus();
        filenameTemplate.cursorPosition = filenameTemplate.text.length;
        filenameTemplate.deselect();
        syncTextInputActive();
    }

    function focusFilenameTemplateAndSelectAll() {
        filenameTemplate.forceActiveFocus();
        filenameTemplate.selectAll();
        syncTextInputActive();
    }

    function focusCandidateGrid() {
        if (host.item)
            host.item.focusGrid();
        syncTextInputActive();
    }

    TextField {
        id: filenameTemplate
        objectName: "importFilenameTemplate"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        height: 36
        text: "{date}_{stem}_{seq}"
        selectByMouse: true
        onActiveFocusChanged: root.syncTextInputActive()
    }

    Loader {
        id: host
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: filenameTemplate.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 8
        source: root.productionGridUrl
        onLoaded: {
            item.candidates = root.importCandidates;
            item.preferredCell = 180;
            item.cellDelegate = minimalCell;
        }
    }

    Component {
        id: minimalCell
        Item {
            required property int index
            width: GridView.view ? GridView.view.cellWidth : 0
            height: GridView.view ? GridView.view.cellHeight : 0
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                color: "#444444"
            }
        }
    }

    onImportCandidatesChanged: if (host.item)
        host.item.candidates = root.importCandidates
}
