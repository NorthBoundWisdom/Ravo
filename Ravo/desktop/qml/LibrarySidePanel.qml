import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    color: Theme.contentSurfaceColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        CustomLabel {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.topMargin: Fonts.size12
            Layout.bottomMargin: Fonts.size8
            text: qsTr("Library")
            font.bold: true
        }

        ListView {
            id: folderList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            clip: true
            spacing: Fonts.size2
            boundsBehavior: Flickable.StopAtBounds
            model: root.presenter ? root.presenter.folders : null

            delegate: Item {
                id: folderRow
                required property string folderUri
                required property string displayName
                required property int depth
                required property int assetCount

                width: ListView.view.width
                height: Fonts.listItemHeight
                clip: true

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: {
                        if (root.presenter && folderRow.folderUri === root.presenter.selectedFolderUri)
                            return Theme.railSurfaceColor
                        if (folderMouse.containsMouse)
                            return Theme.buttonHoveredColor
                        return "transparent"
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Fonts.size8 + Math.max(0, folderRow.depth) * Fonts.size12
                        anchors.rightMargin: Fonts.size8
                        spacing: Fonts.size8

                        CustomLabel {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            elide: Text.ElideRight
                            wrapMode: Text.NoWrap
                            maximumLineCount: 1
                            text: folderRow.folderUri.length === 0 ? qsTr("All Photographs") : folderRow.displayName
                            color: Theme.textColor
                        }
                        CustomLabel {
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            Layout.preferredWidth: implicitWidth
                            text: String(folderRow.assetCount)
                            color: Theme.placeholderTextColor
                        }
                    }

                    MouseArea {
                        id: folderMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (root.presenter) root.presenter.selectFolder(folderRow.folderUri)
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.dividerColor
    }
}
