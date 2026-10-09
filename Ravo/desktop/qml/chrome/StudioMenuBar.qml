import QtQuick
import QtQuick.Controls
import GeoControls 1.0

CustomMenuBar {
    id: menuBar
    required property var controller

    function commandCount(path) {
        return controller ? controller.menuEntries(path).length : 0;
    }

    CustomMenu {
        id: fileMenu
        title: qsTr("File")
        StudioCommandMenuItems {
            id: fileLibrary
            controller: menuBar.controller
            hostMenu: fileMenu
            menuPath: "file.library"
            insertionIndex: 0
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            id: fileTransfer
            controller: menuBar.controller
            hostMenu: fileMenu
            menuPath: "file.transfer"
            insertionIndex: menuBar.commandCount("file.library") + 1
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            id: fileRecovery
            controller: menuBar.controller
            hostMenu: fileMenu
            menuPath: "file.recovery"
            insertionIndex: menuBar.commandCount("file.library") + menuBar.commandCount("file.transfer") + 2
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            controller: menuBar.controller
            hostMenu: fileMenu
            menuPath: "file.window"
            insertionIndex: menuBar.commandCount("file.library") + menuBar.commandCount("file.transfer") + menuBar.commandCount("file.recovery") + 3
        }
    }
    CustomMenu {
        id: editMenu
        title: qsTr("Edit")
        StudioCommandMenuItems {
            id: editHistory
            controller: menuBar.controller
            hostMenu: editMenu
            menuPath: "edit.history"
            insertionIndex: 0
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            controller: menuBar.controller
            hostMenu: editMenu
            menuPath: "edit.reset"
            insertionIndex: menuBar.commandCount("edit.history") + 1
        }
    }
    CustomMenu {
        id: viewMenu
        title: qsTr("View")
        StudioCommandMenuItems {
            id: viewMode
            controller: menuBar.controller
            hostMenu: viewMenu
            menuPath: "view.mode"
            insertionIndex: 0
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            id: viewZoom
            controller: menuBar.controller
            hostMenu: viewMenu
            menuPath: "view.zoom"
            insertionIndex: menuBar.commandCount("view.mode") + 1
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            id: viewCompare
            controller: menuBar.controller
            hostMenu: viewMenu
            menuPath: "view.compare"
            insertionIndex: menuBar.commandCount("view.mode") + menuBar.commandCount("view.zoom") + 2
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            controller: menuBar.controller
            hostMenu: viewMenu
            menuPath: "view.commands"
            insertionIndex: menuBar.commandCount("view.mode") + menuBar.commandCount("view.zoom") + menuBar.commandCount("view.compare") + 3
        }
    }
    CustomMenu {
        id: photoMenu
        title: qsTr("Photo")
        StudioCommandMenuItems {
            id: photoNavigate
            controller: menuBar.controller
            hostMenu: photoMenu
            menuPath: "photo.navigate"
            insertionIndex: 0
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            id: photoTransform
            controller: menuBar.controller
            hostMenu: photoMenu
            menuPath: "photo.transform"
            insertionIndex: menuBar.commandCount("photo.navigate") + 1
        }
        CustomMenuSeparator {}
        CustomMenu {
            id: ratingMenu
            title: qsTr("Rating")
            StudioCommandMenuItems {
                controller: menuBar.controller
                hostMenu: ratingMenu
                menuPath: "photo.rating"
                insertionIndex: 0
            }
        }
        CustomMenu {
            id: colorMenu
            title: qsTr("Color Label")
            StudioCommandMenuItems {
                controller: menuBar.controller
                hostMenu: colorMenu
                menuPath: "photo.color"
                insertionIndex: 0
            }
        }
        StudioCommandMenuItems {
            id: photoReview
            controller: menuBar.controller
            hostMenu: photoMenu
            menuPath: "photo.review"
            insertionIndex: menuBar.commandCount("photo.navigate") + menuBar.commandCount("photo.transform") + 4
        }
        CustomMenuSeparator {}
        StudioCommandMenuItems {
            controller: menuBar.controller
            hostMenu: photoMenu
            menuPath: "photo.delete"
            insertionIndex: menuBar.commandCount("photo.navigate") + menuBar.commandCount("photo.transform") + menuBar.commandCount("photo.review") + 5
        }
    }
    CustomMenu {
        id: helpMenu
        title: qsTr("Help")
        StudioCommandMenuItems {
            controller: menuBar.controller
            hostMenu: helpMenu
            menuPath: "help.about"
            insertionIndex: 0
        }
    }
}
