import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ListView {
    id: root
    required property var folderModel
    signal folderChosen(string path)

    function chooseFolder(chosenPath) {
        // Activation may synchronously replace every row. Finish forwarding the
        // choice in this stable tree context, never in the destroyed delegate.
        if (root.folderModel)
            root.folderModel.activateFolder(chosenPath);
        root.folderChosen(chosenPath);
    }

    clip: true
    boundsBehavior: Flickable.StopAtBounds
    spacing: 0
    model: root.folderModel
    Connections {
        target: root.folderModel
        function onFolderRevealed(row) {
            root.positionViewAtIndex(row, ListView.Contain);
        }
    }

    delegate: Item {
        id: folderRow
        required property string path
        required property string displayName
        required property int depth
        required property bool hasChildren
        required property bool collapsed
        required property bool listingPending
        required property bool selected
        required property string errorText
        required property int index

        width: ListView.view.width
        height: Fonts.listItemHeight

        Rectangle {
            anchors.fill: parent
            color: folderRow.selected ? Theme.buttonHoveredColor : folderMouse.containsMouse ? Theme.buttonHoveredColor : "transparent"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Fonts.size4 + folderRow.depth * Fonts.size20
            anchors.rightMargin: Fonts.size8
            spacing: Fonts.size4

            Item {
                Layout.preferredWidth: Fonts.listItemHeight
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                ToolButton {
                    objectName: "importFolderExpand"
                    anchors.fill: parent
                    enabled: folderRow.hasChildren && !folderRow.listingPending
                    visible: folderRow.hasChildren
                    padding: 0
                    text: folderRow.listingPending ? "…" : folderRow.collapsed ? "▸" : "▾"
                    contentItem: CustomLabel {
                        text: parent.text
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        color: parent.hovered ? Theme.buttonHoveredColor : "transparent"
                    }
                    ToolTip.visible: hovered && folderRow.errorText.length > 0
                    ToolTip.text: folderRow.errorText
                    onClicked: if (root.folderModel)
                        root.folderModel.toggleCollapsed(folderRow.path)
                }
            }
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumWidth: 0
                CustomLabel {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    text: folderRow.displayName
                    color: folderRow.errorText.length > 0 ? Theme.errorColor : Theme.textColor
                }
                MouseArea {
                    id: folderMouse
                    objectName: "importFolderChoose"
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.chooseFolder(folderRow.path)
                }
            }
        }
    }

    CustomLabel {
        anchors.centerIn: parent
        visible: root.count === 0
        text: qsTr("No folders")
        color: Theme.placeholderTextColor
    }
}
