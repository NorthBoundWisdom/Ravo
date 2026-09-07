// Behavioral host: filename template TextField + production ImportCandidateGrid.
// Used to prove passive candidate publication does not steal text focus.
import QtQuick
import QtQuick.Controls

Item {
    id: root
    width: 900
    height: 600
    property var importCandidates
    property url productionGridUrl
    property alias templateField: filenameTemplate
    property alias gridHost: host

    function focusFilenameTemplate() {
        filenameTemplate.forceActiveFocus();
        filenameTemplate.selectAll();
    }

    function focusCandidateGrid() {
        if (host.item)
            host.item.focusGrid();
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
