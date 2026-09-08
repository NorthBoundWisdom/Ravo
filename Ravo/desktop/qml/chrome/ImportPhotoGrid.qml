pragma Translator: ImportPage

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importPhotoGrid"
    required property var presenter
    property alias selectionAnchor: candidateGrid.selectionAnchor
    property real preferredCell: 180
    property string sourceIdentity: presenter.importSourceRoot
    onSourceIdentityChanged: candidateGrid.selectionAnchor = -1

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

    ImportCandidateGrid {
        id: candidateGrid
        anchors.fill: parent
        anchors.margins: Fonts.size8
        candidates: root.presenter.importCandidates
        interactionLocked: root.presenter.importWorkActive
        preferredCell: root.preferredCell
        showVerticalScrollBar: true
        thumbnailDemandPublisher: root.presenter
        accessibleName: qsTr("Import candidates")
        accessibleDescription: qsTr("Use arrow keys to navigate, Shift to select a range, Control or Command to preserve the selection, and Space to check or uncheck.")

        cellDelegate: Item {
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
            required property bool thumbnailLoading
            readonly property bool inViewport: y + height >= candidateGrid.contentY && y <= candidateGrid.contentY + candidateGrid.height
            readonly property bool keyboardCurrent: index === candidateGrid.currentIndex && candidateGrid.grid.activeFocus
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
                    running: candidateDelegate.thumbnailLoading && candidateDelegate.eligible
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
                    onClicked: candidateGrid.applyCheckAt(index)
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
                        candidateGrid.applyMouseSelection(index, mouse.modifiers);
                    }
                }
            }
        }
    }
}
