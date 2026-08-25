import QtQuick
import QtQuick.Controls
import GeoControls 1.0

Menu {
    id: root
    required property var commands

    modal: true
    dim: false
    overlap: 0
    padding: Fonts.size4
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: Overlay.overlay

    palette.window: Theme.popupSurfaceColor
    palette.windowText: Theme.textColor
    palette.base: Theme.popupSurfaceColor
    palette.text: Theme.textColor
    palette.button: Theme.popupSurfaceColor
    palette.buttonText: Theme.textColor
    palette.highlight: Theme.buttonHoveredColor
    palette.highlightedText: Theme.textColor
    palette.mid: Theme.dividerColor

    background: Rectangle {
        implicitWidth: 240
        color: Theme.popupSurfaceColor
        border.color: Theme.dividerColor
        border.width: 1
        radius: 4
    }

    component StyledItem: MenuItem {
        id: item
        implicitHeight: Math.max(Fonts.listItemHeight, Fonts.size24)
        leftPadding: Fonts.size12
        rightPadding: Fonts.size12
        spacing: Fonts.size8
        contentItem: Item {
            implicitWidth: label.implicitWidth + (arrow.visible ? Fonts.size16 : 0)
            implicitHeight: Math.max(label.implicitHeight, Fonts.size16)
            Text {
                id: label
                anchors.left: parent.left
                anchors.right: arrow.visible ? arrow.left : parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.rightMargin: arrow.visible ? Fonts.size8 : 0
                text: item.text
                font: Fonts.standardFont
                color: item.enabled ? Theme.textColor : Theme.disabledTextColor
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                opacity: 1
            }
            Text {
                id: arrow
                visible: item.subMenu !== null
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: ">"
                font: Fonts.standardFont
                color: item.enabled ? Theme.textColor : Theme.disabledTextColor
            }
        }
        background: Rectangle {
            color: item.highlighted ? Theme.buttonHoveredColor : Theme.popupSurfaceColor
        }
    }

    delegate: StyledItem {}

    component StyledSeparator: MenuSeparator {
        padding: Fonts.size4
        contentItem: Rectangle {
            implicitHeight: 1
            color: Theme.dividerColor
        }
        background: Rectangle {
            color: Theme.popupSurfaceColor
        }
    }

    component StyledSubMenu: Menu {
        modal: false
        dim: false
        padding: Fonts.size4
        palette.window: root.palette.window
        palette.windowText: root.palette.windowText
        palette.base: root.palette.base
        palette.text: root.palette.text
        palette.button: root.palette.button
        palette.buttonText: root.palette.buttonText
        palette.highlight: root.palette.highlight
        palette.highlightedText: root.palette.highlightedText
        background: Rectangle {
            implicitWidth: 200
            color: Theme.popupSurfaceColor
            border.color: Theme.dividerColor
            border.width: 1
            radius: 4
        }
        delegate: StyledItem {}
    }

    StyledItem {
        action: root.commands.loupe
    }
    StyledItem {
        action: root.commands.develop
    }
    StyledSeparator {}
    StyledSubMenu {
        title: qsTr("Rating")
        StyledItem {
            action: root.commands.rating0
        }
        StyledItem {
            action: root.commands.rating1
        }
        StyledItem {
            action: root.commands.rating2
        }
        StyledItem {
            action: root.commands.rating3
        }
        StyledItem {
            action: root.commands.rating4
        }
        StyledItem {
            action: root.commands.rating5
        }
    }
    StyledSubMenu {
        title: qsTr("Color Label")
        StyledItem {
            action: root.commands.colorNone
        }
        StyledItem {
            action: root.commands.colorRed
        }
        StyledItem {
            action: root.commands.colorYellow
        }
        StyledItem {
            action: root.commands.colorGreen
        }
        StyledItem {
            action: root.commands.colorBlue
        }
        StyledItem {
            action: root.commands.colorPurple
        }
    }
    StyledItem {
        action: root.commands.reject
    }
    StyledSeparator {}
    StyledItem {
        action: root.commands.cropTool
    }
    StyledItem {
        action: root.commands.rotateLeft
    }
    StyledItem {
        action: root.commands.rotateRight
    }
    StyledItem {
        action: root.commands.flipHorizontal
    }
    StyledItem {
        action: root.commands.flipVertical
    }
    StyledItem {
        action: root.commands.resetEdits
    }
    StyledSeparator {}
    StyledItem {
        action: root.commands.removePhoto
    }
    StyledItem {
        action: root.commands.removeFromDisk
    }
}
