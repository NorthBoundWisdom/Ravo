// Offscreen StudioImportWorkspace keyboard harness.
// Mirrors ImportPhotoGrid.qml Keys.onPressed / moveKeyboardFocus without GeoControls.
import QtQuick

Item {
    id: root
    width: 800
    height: 600
    property var importCandidates
    property bool importWorkActive: false
    property int selectionAnchor: -1
    property real preferredCell: 180

    function fittedGridCell(availableWidth, preferred) {
        const inner = Math.max(120, availableWidth - 14);
        return inner / Math.max(1, Math.floor(inner / preferred));
    }

    function initializeKeyboardFocus() {
        if (candidateGrid.count <= 0) {
            candidateGrid.currentIndex = -1;
            root.selectionAnchor = -1;
            return;
        }
        if (candidateGrid.currentIndex < 0 || candidateGrid.currentIndex >= candidateGrid.count)
            candidateGrid.currentIndex = 0;
        if (root.selectionAnchor < 0 || root.selectionAnchor >= candidateGrid.count)
            root.selectionAnchor = candidateGrid.currentIndex;
    }

    function keyboardColumnCount() {
        return Math.max(1, Math.floor(candidateGrid.width / Math.max(1, candidateGrid.cellWidth)));
    }

    function keyboardPageStep() {
        const rows = Math.max(1, Math.floor(candidateGrid.height / Math.max(1, candidateGrid.cellHeight)));
        return rows * root.keyboardColumnCount();
    }

    function moveKeyboardFocus(target, extend, additive) {
        root.initializeKeyboardFocus();
        if (candidateGrid.currentIndex < 0)
            return;
        const previous = candidateGrid.currentIndex;
        const bounded = Math.max(0, Math.min(candidateGrid.count - 1, target));
        if (root.selectionAnchor < 0 || root.selectionAnchor >= candidateGrid.count)
            root.selectionAnchor = previous;
        candidateGrid.currentIndex = bounded;
        candidateGrid.positionViewAtIndex(bounded, GridView.Contain);
        if (extend)
            root.importCandidates.highlightRange(root.selectionAnchor, bounded, additive);
        else if (!additive) {
            root.importCandidates.highlightExclusive(bounded);
            root.selectionAnchor = bounded;
        }
    }

    function focusCandidateGrid() {
        candidateGrid.forceActiveFocus();
    }

    GridView {
        id: candidateGrid
        objectName: "importCandidateKeyboardGrid"
        anchors.fill: parent
        anchors.margins: 8
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        keyNavigationEnabled: false
        activeFocusOnTab: true
        currentIndex: -1
        highlightFollowsCurrentItem: false
        cellWidth: root.fittedGridCell(width, root.preferredCell)
        cellHeight: cellWidth
        cacheBuffer: cellHeight
        model: root.importCandidates

        onCountChanged: root.initializeKeyboardFocus()

        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function (event) {
            if (root.importWorkActive)
                return;

            root.initializeKeyboardFocus();
            const additive = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
            const extend = (event.modifiers & Qt.ShiftModifier) !== 0;

            if (event.key === Qt.Key_Space && candidateGrid.currentIndex >= 0) {
                root.importCandidates.applyCheck(candidateGrid.currentIndex);
                event.accepted = true;
                return;
            }

            let target = candidateGrid.currentIndex;
            if (event.key === Qt.Key_Left)
                target -= 1;
            else if (event.key === Qt.Key_Right)
                target += 1;
            else if (event.key === Qt.Key_Up)
                target -= root.keyboardColumnCount();
            else if (event.key === Qt.Key_Down)
                target += root.keyboardColumnCount();
            else if (event.key === Qt.Key_Home)
                target = 0;
            else if (event.key === Qt.Key_End)
                target = candidateGrid.count - 1;
            else if (event.key === Qt.Key_PageUp)
                target -= root.keyboardPageStep();
            else if (event.key === Qt.Key_PageDown)
                target += root.keyboardPageStep();
            else
                return;

            root.moveKeyboardFocus(target, extend, additive);
            event.accepted = true;
        }

        delegate: Item {
            required property int index
            width: candidateGrid.cellWidth
            height: candidateGrid.cellHeight
            Rectangle {
                anchors.fill: parent
                anchors.margins: 2
                color: "#333333"
                border.width: index === candidateGrid.currentIndex ? 2 : 0
                border.color: "#66ccff"
            }
        }
    }
}
