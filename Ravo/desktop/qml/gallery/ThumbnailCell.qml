import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Item {
    id: root

    property url thumbnailUrl
    property string thumbnailState: "pending"
    property string importState: ""
    property int rating: 0
    property string colorLabel: "none"
    property bool rejected: false
    property bool hasEdits: false
    property bool selected: false
    property bool current: false
    property int sequenceNumber: 0
    property string displayName: ""
    property string mediaType: ""
    property int pixelWidth: 0
    property int pixelHeight: 0
    property bool compact: false
    property var swatchColor: function (name) {
        return Theme.midColor;
    }

    signal clicked(int button, int modifiers)
    signal doubleClicked

    readonly property bool missing: importState === "missing" || thumbnailState === "missing"
    readonly property string kindLabel: {
        if (mediaType === "image/x-raw")
            return "RAW";
        if (mediaType === "image/jpeg")
            return "JPEG";
        if (mediaType === "image/png")
            return "PNG";
        if (mediaType === "image/tiff")
            return "TIFF";
        if (mediaType === "image/webp")
            return "WEBP";
        if (mediaType === "image/gif")
            return "GIF";
        if (mediaType === "image/bmp")
            return "BMP";
        return "";
    }
    readonly property string sizeLabel: (pixelWidth > 0 && pixelHeight > 0) ? (pixelWidth + "×" + pixelHeight) : ""
    readonly property string starText: rating > 0 ? "★".repeat(rating) : ""
    readonly property color starColor: "#ffca56"
    readonly property color chromeTextColor: "#f5f5f5"
    readonly property color chromeMetaColor: "#ececec"

    Rectangle {
        id: frame
        anchors.fill: parent
        anchors.margins: root.compact ? 0 : Fonts.size2
        color: Theme.imageSurroundColor
        border.width: root.current || root.selected ? 2 : 1
        border.color: root.current ? Theme.selectedBorderColor : (root.selected ? Theme.selectedSecondaryBorderColor : Theme.dividerColor)
        clip: true

        Image {
            id: photo
            anchors.fill: parent
            anchors.margins: 2
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
            source: root.thumbnailUrl
            visible: root.thumbnailUrl.toString().length > 0
        }

        CustomLabel {
            anchors.centerIn: parent
            visible: root.thumbnailUrl.toString().length === 0
            text: {
                if (root.thumbnailState === "failed")
                    return qsTr("Failed");
                if (root.missing)
                    return qsTr("Missing");
                return qsTr("Loading…");
            }
            color: root.chromeTextColor
            font.pixelSize: root.compact ? Fonts.size10 : Fonts.size12
        }

        Item {
            id: chrome
            anchors.fill: photo
            visible: photo.visible && photo.status === Image.Ready

            readonly property real gutterX: (photo.paintedWidth > 0) ? Math.max(0, (width - photo.paintedWidth) / 2) : 0
            readonly property real gutterY: (photo.paintedHeight > 0) ? Math.max(0, (height - photo.paintedHeight) / 2) : 0
            readonly property bool topBand: gutterY >= (root.compact ? 11 : 13)
            readonly property bool bottomBand: gutterY >= (root.compact ? 11 : 13)
            readonly property bool sideBand: gutterX >= 14 && !topBand

            Rectangle {
                visible: sequenceLabel.visible && !chrome.topBand && !chrome.sideBand
                x: Math.max(0, sequenceLabel.x - 2)
                y: Math.max(0, sequenceLabel.y - 1)
                width: sequenceLabel.implicitWidth + 4
                height: sequenceLabel.implicitHeight + 2
                radius: 2
                color: "#99000000"
            }

            CustomLabel {
                id: sequenceLabel
                x: 1
                y: chrome.topBand ? Math.max(0, (chrome.gutterY - implicitHeight) / 2) : (chrome.sideBand ? chrome.gutterY + 1 : 1)
                text: root.sequenceNumber > 0 ? String(root.sequenceNumber) : ""
                visible: text.length > 0
                font.pixelSize: root.compact ? Fonts.size10 : Fonts.size12
                font.bold: true
                color: root.chromeTextColor
            }

            CustomLabel {
                id: metaLabel
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: chrome.topBand ? Math.max(0, (chrome.gutterY - implicitHeight) / 2) : 1
                anchors.rightMargin: flagRow.width > 0 ? flagRow.width + 4 : 1
                visible: text.length > 0 && (chrome.topBand || chrome.sideBand)
                text: {
                    if (chrome.topBand && !root.compact && root.sizeLabel.length > 0)
                        return root.kindLabel.length > 0 ? (root.kindLabel + "  " + root.sizeLabel) : root.sizeLabel;
                    return root.kindLabel;
                }
                font.pixelSize: root.compact ? Fonts.size10 : Fonts.size12
                color: root.chromeMetaColor
            }

            Rectangle {
                id: bottomScrim
                anchors.fill: bottomBar
                visible: bottomBar.visible && !chrome.bottomBand
                color: "#99000000"
            }

            RowLayout {
                id: bottomBar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 3
                anchors.rightMargin: 3
                anchors.bottomMargin: chrome.bottomBand ? Math.max(1, Math.round((chrome.gutterY - implicitHeight) / 2)) : 2
                spacing: Fonts.size4
                visible: root.starText.length > 0 || root.displayName.length > 0 || root.colorLabel !== "none"

                CustomLabel {
                    id: starLabel
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: implicitWidth
                    text: root.starText
                    visible: text.length > 0
                    font.pixelSize: root.compact ? Fonts.size10 : Fonts.size12
                    color: root.starColor
                }

                CustomLabel {
                    id: nameLabel
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    Layout.minimumWidth: 0
                    text: root.displayName
                    visible: text.length > 0
                    elide: Text.ElideMiddle
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: root.compact ? Fonts.size10 : Fonts.size12
                    color: root.chromeTextColor
                }

                Rectangle {
                    id: swatch
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: width
                    Layout.preferredHeight: height
                    width: root.compact ? 8 : 10
                    height: width
                    radius: width / 2
                    visible: root.colorLabel !== "none"
                    color: root.swatchColor(root.colorLabel)
                }
            }

            Row {
                id: flagRow
                spacing: 3
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.topMargin: chrome.topBand ? Math.max(0, (chrome.gutterY - implicitHeight) / 2) : 2
                anchors.rightMargin: 2

                Rectangle {
                    visible: root.missing
                    width: root.compact ? 8 : 54
                    height: root.compact ? 8 : 16
                    radius: root.compact ? 4 : 3
                    color: "#c47b16"
                    CustomLabel {
                        anchors.centerIn: parent
                        visible: !root.compact
                        text: qsTr("Missing")
                        color: "#ffffff"
                        font.pixelSize: Fonts.size10
                    }
                }

                Rectangle {
                    visible: root.rejected
                    width: root.compact ? 8 : 48
                    height: root.compact ? 8 : 16
                    radius: root.compact ? 4 : 3
                    color: "#aa3333"
                    CustomLabel {
                        anchors.centerIn: parent
                        visible: !root.compact
                        text: qsTr("Reject")
                        color: "#ffffff"
                        font.pixelSize: Fonts.size10
                    }
                }

                Rectangle {
                    visible: root.hasEdits
                    width: root.compact ? 8 : 40
                    height: root.compact ? 8 : 16
                    radius: root.compact ? 4 : 3
                    color: Theme.textColor
                    CustomLabel {
                        anchors.centerIn: parent
                        visible: !root.compact
                        text: qsTr("Edit")
                        color: Theme.windowColor
                        font.pixelSize: Fonts.size10
                    }
                }
            }
        }

        MouseArea {
            id: clickArea
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: false
            property int pressModifiers: Qt.NoModifier

            onPressed: function (mouse) {
                pressModifiers = mouse.modifiers;
                // Keep modifier clicks from turning into a grid/film-strip flick.
                preventStealing = (mouse.modifiers & (Qt.ShiftModifier | Qt.ControlModifier | Qt.MetaModifier)) !== 0;
            }
            onReleased: preventStealing = false
            onCanceled: preventStealing = false
            onClicked: function (mouse) {
                const modifiers = mouse.modifiers !== 0 ? mouse.modifiers : pressModifiers;
                root.clicked(mouse.button, modifiers);
            }
            onDoubleClicked: root.doubleClicked()
        }
    }
}
