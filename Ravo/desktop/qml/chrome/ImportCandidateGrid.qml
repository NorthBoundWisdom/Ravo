import QtQuick
import QtQuick.Controls

Item {
    id: root
    objectName: "importCandidateGrid"

    property var candidates
    property bool interactionLocked: false
    property int selectionAnchor: -1
    property real preferredCell: 180
    property Component cellDelegate
    property string accessibleName
    property string accessibleDescription
    property bool showVerticalScrollBar: false
    property var thumbnailDemandPublisher

    property alias grid: candidateGrid
    property alias currentIndex: candidateGrid.currentIndex
    property alias count: candidateGrid.count
    property alias cellWidth: candidateGrid.cellWidth
    property alias cellHeight: candidateGrid.cellHeight
    property alias contentY: candidateGrid.contentY
    property alias contentHeight: candidateGrid.contentHeight
    property alias contentWidth: candidateGrid.contentWidth
    property alias visibleArea: candidateGrid.visibleArea

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
            root.candidates.highlightRange(root.selectionAnchor, bounded, additive);
        else if (!additive) {
            root.candidates.highlightExclusive(bounded);
            root.selectionAnchor = bounded;
        }
    }

    function focusGrid() {
        candidateGrid.forceActiveFocus();
    }

    function publishViewportDemand() {
        if (!thumbnailDemandPublisher || typeof thumbnailDemandPublisher.setImportThumbnailViewportDemand !== "function")
            return;
        const rows = [];
        const first = Math.max(0, Math.floor(candidateGrid.contentY / Math.max(1, candidateGrid.cellHeight)) * root.keyboardColumnCount());
        const visibleCount = Math.ceil(candidateGrid.height / Math.max(1, candidateGrid.cellHeight)) * root.keyboardColumnCount() + root.keyboardColumnCount();
        for (let i = 0; i < visibleCount; ++i) {
            const row = first + i;
            if (row >= 0 && row < candidateGrid.count)
                rows.push(row);
        }
        const current = candidateGrid.currentIndex;
        thumbnailDemandPublisher.setImportThumbnailViewportDemand(rows, 2, current);
    }

    function applyMouseSelection(index, modifiers) {
        const additive = (modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
        const extend = (modifiers & Qt.ShiftModifier) !== 0;
        const previousAnchor = root.selectionAnchor >= 0 ? root.selectionAnchor : (candidateGrid.currentIndex >= 0 ? candidateGrid.currentIndex : index);
        candidateGrid.currentIndex = index;
        if (extend) {
            root.selectionAnchor = previousAnchor;
            root.candidates.highlightRange(root.selectionAnchor, index, additive);
        } else if (additive) {
            root.candidates.highlightToggle(index);
            root.selectionAnchor = index;
        } else {
            root.candidates.highlightExclusive(index);
            root.selectionAnchor = index;
        }
        candidateGrid.positionViewAtIndex(index, GridView.Contain);
        candidateGrid.forceActiveFocus();
    }

    function applyCheckAt(index) {
        candidateGrid.currentIndex = index;
        root.selectionAnchor = index;
        root.candidates.applyCheck(index);
        candidateGrid.forceActiveFocus();
    }

    GridView {
        id: candidateGrid
        objectName: "importCandidateKeyboardGrid"
        anchors.fill: parent
        visible: count > 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        pixelAligned: true
        keyNavigationEnabled: false
        activeFocusOnTab: true
        currentIndex: -1
        highlightFollowsCurrentItem: false
        cellWidth: root.fittedGridCell(width, root.preferredCell)
        cellHeight: cellWidth
        cacheBuffer: cellHeight
        model: root.candidates
        delegate: root.cellDelegate
        Accessible.role: Accessible.List
        Accessible.name: root.accessibleName
        Accessible.description: root.accessibleDescription

        ScrollBar.vertical: ScrollBar {
            policy: root.showVerticalScrollBar ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
            implicitWidth: 10
            visible: root.showVerticalScrollBar
        }

        onCountChanged: {
            root.initializeKeyboardFocus();
            root.publishViewportDemand();
        }
        onCellWidthChanged: root.publishViewportDemand()
        onCellHeightChanged: root.publishViewportDemand()
        onContentYChanged: root.publishViewportDemand()
        onHeightChanged: root.publishViewportDemand()
        onWidthChanged: root.publishViewportDemand()
        onVisibleChanged: if (visible) {
            // Passive focus comes from focusGrid()/click/Tab, not passive count.
            root.initializeKeyboardFocus();
        }

        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function (event) {
            if (root.interactionLocked)
                return;

            root.initializeKeyboardFocus();
            const additive = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
            const extend = (event.modifiers & Qt.ShiftModifier) !== 0;

            if (event.key === Qt.Key_Space && candidateGrid.currentIndex >= 0) {
                root.candidates.applyCheck(candidateGrid.currentIndex);
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
    }
}
