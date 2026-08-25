import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    property var swatchColor: function (name) {
        return Theme.midColor;
    }
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
            id: stripDelegate
            required property string assetId
            required property string displayName
            required property string mediaType
            required property url thumbnailUrl
            required property string importState
            required property string thumbnailState
            required property int rating
            required property string colorLabel
            required property bool rejected
            required property bool hasEdits
            required property bool selected
            required property int pixelWidth
            required property int pixelHeight
            required property int index
            width: Math.max(72, height)
            height: strip.height
            Component.onCompleted: if (root.presenter)
                root.presenter.ensureThumbnail(stripDelegate.assetId)

            ThumbnailCell {
                anchors.fill: parent
                compact: true
                thumbnailUrl: stripDelegate.thumbnailUrl
                thumbnailState: stripDelegate.thumbnailState
                importState: stripDelegate.importState
                rating: stripDelegate.rating
                colorLabel: stripDelegate.colorLabel
                rejected: stripDelegate.rejected
                hasEdits: stripDelegate.hasEdits
                selected: stripDelegate.selected
                current: root.presenter ? stripDelegate.assetId === root.presenter.selectedAssetId : false
                sequenceNumber: stripDelegate.index + 1
                displayName: stripDelegate.displayName
                mediaType: stripDelegate.mediaType
                pixelWidth: stripDelegate.pixelWidth
                pixelHeight: stripDelegate.pixelHeight
                swatchColor: root.swatchColor
                onClicked: function (mouse) {
                    if (root.commands)
                        root.commands.handlePhotoClick(stripDelegate.assetId, mouse);
                }
                onDoubleClicked: if (root.commands)
                    root.commands.handlePhotoDoubleClick(stripDelegate.assetId)
            }
        }
    }
}
