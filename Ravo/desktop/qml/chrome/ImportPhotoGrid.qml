pragma Translator: ImportPage

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importPhotoGrid"
    required property var presenter
    property int selectionAnchor: -1
    property real preferredCell: 180
    property string sourceIdentity: presenter.importSourceRoot
    onSourceIdentityChanged: selectionAnchor = -1

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
            root.presenter.importCandidates.highlightRange(root.selectionAnchor, bounded, additive);
        else if (!additive) {
            root.presenter.importCandidates.highlightExclusive(bounded);
            root.selectionAnchor = bounded;
        }
    }

    Layout.fillWidth: true
    Layout.fillHeight: true
    color: Theme.windowColor

    BusyIndicator {
        anchors.centerIn: parent
        running: root.presenter.importScanActive && candidateGrid.count === 0
        visible: running
    }

    CustomLabel {
        anchors.centerIn: parent
        visible: !root.presenter.importScanActive && candidateGrid.count === 0
        text: root.presenter.importSourceRoot.length ? qsTr("No supported photos found") : qsTr("Choose a source folder")
        color: Theme.placeholderTextColor
    }

    GridView {
        id: candidateGrid
        anchors.fill: parent
        anchors.margins: Fonts.size8
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
        model: root.presenter.importCandidates
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Import candidates")
        Accessible.description: qsTr("Use arrow keys to navigate, Shift to select a range, Control or Command to preserve the selection, and Space to check or uncheck.")

        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AlwaysOn
            implicitWidth: 10
        }

        onCountChanged: root.initializeKeyboardFocus()
        onVisibleChanged: if (visible) {
            root.initializeKeyboardFocus();
            forceActiveFocus();
        }

        Keys.priority: Keys.BeforeItem
        Keys.onPressed: function (event) {
            if (root.presenter.importWorkActive)
                return;

            root.initializeKeyboardFocus();
            const additive = (event.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
            const extend = (event.modifiers & Qt.ShiftModifier) !== 0;

            if (additive && event.key === Qt.Key_A) {
                root.presenter.importCandidates.highlightAll();
                event.accepted = true;
                return;
            }
            if (event.key === Qt.Key_Space && candidateGrid.currentIndex >= 0) {
                root.presenter.importCandidates.applyCheck(candidateGrid.currentIndex);
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
            id: candidateDelegate
            required property int index
            required property string sourcePath
            required property string displayName
            required property string mediaType
            required property int pixelWidth
            required property int pixelHeight
            required property bool selected
            required property bool highlighted
            required property bool eligible
            required property bool duplicate
            required property url thumbnailUrl
            required property string errorText
            required property bool inspected
            readonly property bool inViewport: y + height >= candidateGrid.contentY && y <= candidateGrid.contentY + candidateGrid.height
            readonly property bool keyboardCurrent: index === candidateGrid.currentIndex && candidateGrid.activeFocus
            onInViewportChanged: if (inViewport)
                root.presenter.ensureImportThumbnail(index)
            onSourcePathChanged: root.presenter.ensureImportThumbnail(index)
            onThumbnailUrlChanged: if (inViewport && thumbnailUrl.toString().length === 0)
                root.presenter.ensureImportThumbnail(index)
            enabled: eligible
            opacity: duplicate ? 0.45 : 1
            width: candidateGrid.cellWidth
            height: candidateGrid.cellHeight
            Component.onCompleted: root.presenter.ensureImportThumbnail(index)
            onIndexChanged: root.presenter.ensureImportThumbnail(index)

            Rectangle {
                anchors.fill: parent
                anchors.margins: Fonts.size3
                ToolTip.visible: candidateMouse.containsMouse && errorText.length > 0
                ToolTip.text: errorText
                color: Theme.imageSurroundColor
                border.width: highlighted || candidateDelegate.keyboardCurrent ? ControlState.borderFocus : ControlState.borderThin
                border.color: highlighted || candidateDelegate.keyboardCurrent ? Theme.highlightColor : Theme.dividerColor

                Image {
                    anchors.fill: parent
                    anchors.margins: 3
                    source: thumbnailUrl
                    fillMode: Image.PreserveAspectFit
                    asynchronous: false
                    visible: thumbnailUrl.toString().length > 0
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    width: 28
                    height: 28
                    running: !candidateDelegate.inspected && candidateDelegate.eligible
                    visible: running
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 28
                    color: "#aa000000"

                    CustomLabel {
                        anchors.centerIn: parent
                        width: parent.width - 8
                        text: displayName
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideMiddle
                    }
                }

                CustomCheckBox {
                    objectName: "importCandidateCheckBox"
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 4
                    indicatorSize: Math.max(24, Fonts.size24)
                    width: Math.max(32, Fonts.scaledUiSize(32))
                    height: width
                    checked: selected
                    enabled: eligible
                    onClicked: {
                        candidateGrid.currentIndex = index;
                        root.selectionAnchor = index;
                        root.presenter.importCandidates.applyCheck(index);
                        candidateGrid.forceActiveFocus();
                    }
                }

                CustomLabel {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    visible: !eligible
                    text: duplicate ? qsTr("Duplicate photo") : qsTr("Unavailable")
                    color: duplicate ? Theme.disabledTextColor : Theme.warningColor
                }

                MouseArea {
                    id: candidateMouse
                    hoverEnabled: true
                    anchors.fill: parent
                    z: -1

                    onPressed: function (mouse) {
                        preventStealing = (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.MetaModifier)) !== 0;
                    }
                    onReleased: preventStealing = false
                    onCanceled: preventStealing = false
                    onClicked: function (mouse) {
                        const additive = (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
                        const extend = (mouse.modifiers & Qt.ShiftModifier) !== 0;
                        const previousAnchor = root.selectionAnchor >= 0 ? root.selectionAnchor : (candidateGrid.currentIndex >= 0 ? candidateGrid.currentIndex : index);
                        candidateGrid.currentIndex = index;
                        if (extend) {
                            root.selectionAnchor = previousAnchor;
                            root.presenter.importCandidates.highlightRange(root.selectionAnchor, index, additive);
                        } else if (additive) {
                            root.presenter.importCandidates.highlightToggle(index);
                            root.selectionAnchor = index;
                        } else {
                            root.presenter.importCandidates.highlightExclusive(index);
                            root.selectionAnchor = index;
                        }
                        candidateGrid.positionViewAtIndex(index, GridView.Contain);
                        candidateGrid.forceActiveFocus();
                    }
                }
            }
        }
    }
}
