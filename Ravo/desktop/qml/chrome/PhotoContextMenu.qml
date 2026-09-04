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
        action: root.commands.copyParameters
    }
    StudioContextMenuItem {
        action: root.commands.pasteParameters
    }
    StudioContextMenuItem {
        action: root.commands.pasteParametersToSelection
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        action: root.commands.copyPhotoInfo
    }
    StudioContextMenuItem {
        action: root.commands.copyPhotoParameters
    }
    StudioContextMenuItem {
        objectName: "revealInFileManagerMenuItem"
        action: root.commands.revealInFileManager
    }
    StudioContextMenuItem {
        objectName: "editInMenuItem"
        action: root.commands.editIn
        displayText: qsTr("Edit in…")
    }
    StudioContextMenuItem {
        objectName: "offlineEditMenuItem"
        action: root.commands.offlineEdit
        displayText: qsTr("Offline-edit proxies…")
    }
    StudioContextMenuItem {
        objectName: "aiProposalMenuItem"
        action: root.commands.aiProposal
        displayText: qsTr("Inspect AI Proposal…")
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
