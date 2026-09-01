import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property string displayName: ""
    property string captureSummary: ""
    property string mediaType: ""
    property string dimensions: ""
    property string fileSize: ""
    property bool compact: false

    readonly property string mediaLabel: {
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
        return mediaType;
    }
    readonly property string fileSummary: {
        const parts = [];
        if (root.mediaLabel.length > 0)
            parts.push(root.mediaLabel);
        if (dimensions.length > 0)
            parts.push(dimensions);
        if (!compact && fileSize.length > 0)
            parts.push(fileSize);
        return parts.join(" · ");
    }
    readonly property string compactSummary: captureSummary.length > 0 ? captureSummary : fileSummary

    objectName: "photoInformationOverlay"
    implicitWidth: compact ? Fonts.scaledUiSize(260) : Fonts.scaledUiSize(640)
    implicitHeight: information.implicitHeight + (compact ? Fonts.size8 : Fonts.size16)
    width: Math.min(implicitWidth, parent ? Math.max(0, parent.width - Fonts.size16) : implicitWidth)
    height: implicitHeight
    radius: 4
    color: Qt.rgba(0, 0, 0, 0.72)
    Accessible.role: Accessible.StaticText
    Accessible.name: displayName

    ColumnLayout {
        id: information
        anchors.fill: parent
        anchors.margins: root.compact ? Fonts.size4 : Fonts.size8
        spacing: Fonts.size2

        CustomLabel {
            Layout.fillWidth: true
            text: root.displayName
            color: "#ffffff"
            font.bold: true
            font.pixelSize: root.compact ? Fonts.size12 : Fonts.size16
            wrapMode: root.compact ? Text.NoWrap : Text.Wrap
            elide: root.compact ? Text.ElideMiddle : Text.ElideNone
        }

        CustomLabel {
            Layout.fillWidth: true
            visible: text.length > 0
            text: root.compact ? root.compactSummary : root.captureSummary
            color: "#f2f2f2"
            font.pixelSize: root.compact ? Fonts.size10 : Fonts.size14
            wrapMode: root.compact ? Text.NoWrap : Text.Wrap
            elide: root.compact ? Text.ElideRight : Text.ElideNone
        }

        CustomLabel {
            Layout.fillWidth: true
            visible: !root.compact && text.length > 0
            text: root.fileSummary
            color: "#d8d8d8"
            font.pixelSize: Fonts.size14
            wrapMode: Text.Wrap
            elide: Text.ElideNone
        }
    }
}
