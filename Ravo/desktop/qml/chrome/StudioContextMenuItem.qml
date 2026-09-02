import QtQuick
import QtQuick.Controls
import GeoControls 1.0

MenuItem {
    id: root

    property string displayText: ""
    property bool reserveCheckColumn: checkable
    readonly property string resolvedText: displayText.length > 0 ? displayText : text

    implicitHeight: Math.max(Fonts.listItemHeight, Fonts.size24)
    leftPadding: Fonts.size12
    rightPadding: Fonts.size12
    spacing: Fonts.size8

    // Qt's default indicator can overlap custom content. Keep an explicit,
    // stable check column so checked and unchecked rows align.
    indicator: Item {
        implicitWidth: 0
        implicitHeight: 0
    }

    contentItem: Item {
        implicitWidth: checkmark.width + (root.reserveCheckColumn ? Fonts.size8 : 0) + label.implicitWidth + (arrow.visible ? Fonts.size16 : 0)
        implicitHeight: Math.max(label.implicitHeight, Fonts.size16)

        Text {
            id: checkmark
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: root.reserveCheckColumn ? Fonts.size20 : 0
            visible: root.reserveCheckColumn
            text: root.checkable && root.checked ? "\u2713" : ""
            font: Fonts.makeBoldFont(Fonts.standardFont)
            color: root.enabled ? Theme.textColor : Theme.disabledTextColor
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        Text {
            id: label
            anchors.left: checkmark.right
            anchors.right: arrow.visible ? arrow.left : parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: root.reserveCheckColumn ? Fonts.size8 : 0
            anchors.rightMargin: arrow.visible ? Fonts.size8 : 0
            text: root.resolvedText
            font: Fonts.standardFont
            color: root.enabled ? Theme.textColor : Theme.disabledTextColor
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        Text {
            id: arrow
            visible: root.subMenu !== null
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: ">"
            font: Fonts.standardFont
            color: root.enabled ? Theme.textColor : Theme.disabledTextColor
        }
    }

    background: Rectangle {
        color: root.highlighted ? Theme.buttonHoveredColor : Theme.popupSurfaceColor
    }

    Accessible.name: root.resolvedText
}
