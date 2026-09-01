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

        WheelHandler {
            target: null
            onWheel: function (event) {
                const pixel = event.pixelDelta.x !== 0 ? event.pixelDelta.x : event.pixelDelta.y;
                const angle = event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y;
                const delta = pixel !== 0 ? pixel : angle;
                if (delta === 0) {
                    event.accepted = false;
                    return;
                }
                const maximum = Math.max(0, strip.contentWidth - strip.width);
                const next = Math.max(0, Math.min(maximum, strip.contentX - delta));
                if (next === strip.contentX) {
                    event.accepted = false;
                    return;
                }
                strip.contentX = next;
                event.accepted = true;
            }
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
            required property int versionOrdinal
            required property int stackCount
            required property bool stackPick
            required property bool selected
            required property int pixelWidth
            required property int pixelHeight
            required property int index
            width: Math.max(72, height)
            height: strip.height
            Component.onCompleted: if (root.presenter && stripDelegate.thumbnailState !== "ready")
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
                versionOrdinal: stripDelegate.versionOrdinal
                stackCount: stripDelegate.stackCount
                stackPick: stripDelegate.stackPick
                selected: stripDelegate.selected
                current: root.presenter ? stripDelegate.assetId === root.presenter.selectedAssetId : false
                sequenceNumber: stripDelegate.index + 1
                displayName: stripDelegate.displayName
                mediaType: stripDelegate.mediaType
                pixelWidth: stripDelegate.pixelWidth
                pixelHeight: stripDelegate.pixelHeight
                swatchColor: root.swatchColor
                onClicked: function (button, modifiers) {
                    if (root.commands)
                        root.commands.handlePhotoClick(stripDelegate.assetId, button, modifiers);
                }
                onDoubleClicked: if (root.commands)
                    root.commands.handlePhotoDoubleClick(stripDelegate.assetId)
            }
        }
    }
}
