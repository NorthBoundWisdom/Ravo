// Thin StudioImportWorkspace keyboard harness.
// Assembles the production ImportCandidateGrid with a minimal cell delegate.
import QtQuick

Item {
    id: root
    width: 800
    height: 600
    property var importCandidates
    property bool importWorkActive: false
    property url productionGridUrl
    property int selectionAnchor: host.item ? host.item.selectionAnchor : -1

    function keyboardColumnCount() {
        return host.item ? host.item.keyboardColumnCount() : 1;
    }

    function keyboardPageStep() {
        return host.item ? host.item.keyboardPageStep() : 1;
    }

    function focusCandidateGrid() {
        if (host.item)
            host.item.focusGrid();
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
                color: "#333333"
                border.width: GridView.view && index === GridView.view.currentIndex ? 2 : 0
                border.color: "#66ccff"
            }
        }
    }

    Loader {
        id: host
        anchors.fill: parent
        anchors.margins: 8
        source: root.productionGridUrl
        onLoaded: {
            item.candidates = root.importCandidates;
            item.interactionLocked = root.importWorkActive;
            item.preferredCell = 180;
            item.cellDelegate = minimalCell;
            item.selectionAnchorChanged.connect(function () {
                root.selectionAnchor = item.selectionAnchor;
            });
            root.selectionAnchor = item.selectionAnchor;
        }
    }

    onImportCandidatesChanged: if (host.item)
        host.item.candidates = root.importCandidates
    onImportWorkActiveChanged: if (host.item)
        host.item.interactionLocked = root.importWorkActive
}
