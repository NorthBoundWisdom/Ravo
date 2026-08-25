import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    color: Theme.toolbarSurfaceColor

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: Theme.dividerColor
        z: 1
    }

    ListView {
        id: strip
        anchors.fill: parent
        anchors.margins: Fonts.size4
        anchors.topMargin: Fonts.size4 + 1
        orientation: ListView.Horizontal
        clip: true
        spacing: Fonts.size4
        model: root.presenter ? root.presenter.assets : null
        currentIndex: root.presenter ? root.presenter.selectedIndex : -1
        highlightMoveDuration: 0
        onCurrentIndexChanged: {
            if (currentIndex >= 0)
                positionViewAtIndex(currentIndex, ListView.Contain);
        }

        delegate: Item {
            required property string assetId
            required property url thumbnailUrl
            required property string importState
            required property string thumbnailState
            required property int rating
            required property bool rejected
            required property bool selected
            width: 88
            height: strip.height
            Component.onCompleted: if (root.presenter)
                root.presenter.ensureThumbnail(assetId)

            Rectangle {
                anchors.fill: parent
                color: Theme.pageSurfaceColor
                border.width: selected || (root.presenter && assetId === root.presenter.selectedAssetId) ? 2 : 1
                border.color: root.presenter && assetId === root.presenter.selectedAssetId ? Theme.selectedBorderColor : (selected ? Theme.selectedSecondaryBorderColor : Theme.dividerColor)
                Image {
                    anchors.fill: parent
                    anchors.margins: 2
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: false
                    source: thumbnailUrl
                }
                CustomLabel {
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    anchors.margins: 3
                    text: rating > 0 ? "★".repeat(rating) : ""
                    font.pixelSize: Fonts.size10
                    color: "#ffca56"
                }
                Rectangle {
                    visible: importState === "missing" || thumbnailState === "missing" || rejected
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 4
                    width: 8
                    height: 8
                    radius: 4
                    color: rejected ? "#aa3333" : "#c47b16"
                }
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: function (mouse) {
                        if (root.commands)
                            root.commands.handlePhotoClick(assetId, mouse);
                    }
                    onDoubleClicked: if (root.commands)
                        root.commands.handlePhotoDoubleClick(assetId)
                }
            }
        }
    }
}
