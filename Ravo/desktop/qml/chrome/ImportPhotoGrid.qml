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
    Shortcut {
        sequence: StandardKey.SelectAll
        enabled: root.visible && candidateGrid.activeFocus && !root.presenter.importPreflightActive
        onActivated: root.presenter.importCandidates.highlightAll()
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
        text: root.presenter.importSourceRoot.length ? (root.presenter.importDuplicateCount > 0 ? qsTr("No new photos") : qsTr("No supported photos found")) : qsTr("Choose a source folder")
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
        highlightFollowsCurrentItem: false
        cellWidth: root.fittedGridCell(width, root.preferredCell)
        cellHeight: cellWidth
        cacheBuffer: cellHeight
        model: root.presenter.importCandidates
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AlwaysOn
            implicitWidth: 10
        }
        onVisibleChanged: if (visible)
            forceActiveFocus()
        Keys.onPressed: function (event) {
            if (root.presenter.importWorkActive)
                return;
            if (event.key === Qt.Key_Space && root.selectionAnchor >= 0) {
                root.presenter.importCandidates.applyCheck(root.selectionAnchor);
                event.accepted = true;
            }
        }
        delegate: Item {
            id: candidateDelegate
            required property int index
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
                border.width: highlighted ? ControlState.borderFocus : ControlState.borderThin
                border.color: highlighted ? Theme.highlightColor : Theme.dividerColor
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
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 4
                    checked: selected
                    enabled: eligible
                    onClicked: root.presenter.importCandidates.applyCheck(index)
                }
                CustomLabel {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 6
                    visible: !eligible
                    text: qsTr("Unavailable")
                    color: Theme.warningColor
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
                        if ((mouse.modifiers & Qt.ShiftModifier) !== 0 && root.selectionAnchor >= 0)
                            root.presenter.importCandidates.highlightRange(root.selectionAnchor, index, additive);
                        else if (additive)
                            root.presenter.importCandidates.highlightToggle(index);
                        else
                            root.presenter.importCandidates.highlightExclusive(index);
                        root.selectionAnchor = index;
                        candidateGrid.forceActiveFocus();
                    }
                }
            }
        }
    }
}
