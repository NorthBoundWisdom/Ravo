import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ListView {
    id: root
    required property var folderModel
    signal folderChosen(string path)

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
                Layout.preferredWidth: Fonts.size16
                Layout.preferredHeight: Fonts.size16
                Layout.alignment: Qt.AlignVCenter
                visible: folderRow.hasChildren
                CustomLabel {
                    anchors.centerIn: parent
                    text: folderRow.collapsed ? "▸" : "▾"
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: if (root.folderModel)
                        root.folderModel.toggleCollapsed(folderRow.path)
                }
            }
            Item {
                Layout.preferredWidth: Fonts.size16
                Layout.preferredHeight: 1
                visible: !folderRow.hasChildren
            }
            CustomLabel {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                elide: Text.ElideRight
                text: folderRow.displayName
                color: folderRow.errorText.length > 0 ? Theme.errorColor : Theme.textColor
            }
        }

        MouseArea {
            id: folderMouse
            anchors.fill: parent
            anchors.leftMargin: Fonts.size4 + folderRow.depth * Fonts.size20 + Fonts.size16
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (root.folderModel)
                    root.folderModel.selectFolder(folderRow.path);
                root.folderChosen(folderRow.path);
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
