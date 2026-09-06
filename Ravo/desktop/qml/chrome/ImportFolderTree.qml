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
                Layout.preferredWidth: Fonts.listItemHeight
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignVCenter
                CustomLabel {
                    anchors.centerIn: parent
                    visible: folderRow.hasChildren
                    text: folderRow.collapsed ? "▸" : "▾"
                }
                MouseArea {
                    objectName: "importFolderExpand"
                    anchors.fill: parent
                    enabled: folderRow.hasChildren
                    cursorShape: Qt.PointingHandCursor
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
                    onClicked: {
                        const chosenPath = folderRow.path;
                        if (root.folderModel)
                            root.folderModel.activateFolder(chosenPath);
                        root.folderChosen(chosenPath);
                    }
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
