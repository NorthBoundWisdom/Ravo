import QtQuick
import QtQuick.Controls
import GeoControls 1.0

StudioContextMenu {
    id: root
    required property var commands

    StudioContextMenuItem {
        objectName: "viewPhotoMenuItem"
        action: root.commands.loupe
        displayText: qsTr("View Photo")
    }
    StudioContextMenuItem {
        objectName: "editPhotoMenuItem"
        action: root.commands.develop
        displayText: qsTr("Edit Photo")
    }
    StudioContextMenuSeparator {}
    StudioContextSubMenu {
        title: qsTr("Rating")
        StudioContextMenuItem {
            action: root.commands.rating0
        }
        StudioContextMenuItem {
            action: root.commands.rating1
        }
        StudioContextMenuItem {
            action: root.commands.rating2
        }
        StudioContextMenuItem {
            action: root.commands.rating3
        }
        StudioContextMenuItem {
            action: root.commands.rating4
        }
        StudioContextMenuItem {
            action: root.commands.rating5
        }
    }
    StudioContextSubMenu {
        title: qsTr("Color Label")
        StudioContextMenuItem {
            action: root.commands.colorNone
        }
        StudioContextMenuItem {
            action: root.commands.colorRed
        }
        StudioContextMenuItem {
            action: root.commands.colorYellow
        }
        StudioContextMenuItem {
            action: root.commands.colorGreen
        }
        StudioContextMenuItem {
            action: root.commands.colorBlue
        }
        StudioContextMenuItem {
            action: root.commands.colorPurple
        }
    }
    StudioContextMenuItem {
        action: root.commands.reject
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        action: root.commands.cropTool
    }
    StudioContextMenuItem {
        action: root.commands.rotateLeft
    }
    StudioContextMenuItem {
        action: root.commands.rotateRight
    }
    StudioContextMenuItem {
        action: root.commands.flipHorizontal
    }
    StudioContextMenuItem {
        action: root.commands.flipVertical
    }
    StudioContextMenuItem {
        action: root.commands.resetEdits
    }
    StudioContextMenuItem {
        action: root.commands.copyEdits
    }
    StudioContextMenuItem {
        action: root.commands.pasteEdits
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        action: root.commands.copyPhotoInfo
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        action: root.commands.exportPhoto
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        action: root.commands.removePhoto
    }
    StudioContextMenuItem {
        action: root.commands.removeFromDisk
    }
}
