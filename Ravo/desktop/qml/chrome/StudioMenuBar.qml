import QtQuick
import QtQuick.Controls
import GeoControls 1.0

MenuBar {
    id: menuBar
    required property var controller

    function commandCount(path) {
        return controller ? controller.menuEntries(path).length : 0;
    }

    background: Rectangle {
        color: Theme.windowColor
        implicitHeight: Fonts.menuBarHeight
    }
    delegate: MenuBarItem {
        id: menuBarItem
        implicitHeight: Fonts.menuBarHeight
        contentItem: Text {
            text: menuBarItem.text
            font: Fonts.standardFont
            color: menuBarItem.enabled ? Theme.textColor : Theme.disabledTextColor
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: menuBarItem.highlighted || menuBarItem.down ? Theme.buttonHoveredColor : "transparent"
        }
    }

    Menu {
        id: fileMenu
        title: qsTr("File")
        StudioCommandMenuItems { id: fileLibrary; controller: menuBar.controller; hostMenu: fileMenu; menuPath: "file.library"; insertionIndex: 0 }
        MenuSeparator {}
        StudioCommandMenuItems { id: fileTransfer; controller: menuBar.controller; hostMenu: fileMenu; menuPath: "file.transfer"; insertionIndex: menuBar.commandCount("file.library") + 1 }
        MenuSeparator {}
        StudioCommandMenuItems { controller: menuBar.controller; hostMenu: fileMenu; menuPath: "file.window"; insertionIndex: menuBar.commandCount("file.library") + menuBar.commandCount("file.transfer") + 2 }
    }
    Menu {
        id: editMenu
        title: qsTr("Edit")
        StudioCommandMenuItems { id: editHistory; controller: menuBar.controller; hostMenu: editMenu; menuPath: "edit.history"; insertionIndex: 0 }
        MenuSeparator {}
        StudioCommandMenuItems { controller: menuBar.controller; hostMenu: editMenu; menuPath: "edit.reset"; insertionIndex: menuBar.commandCount("edit.history") + 1 }
    }
    Menu {
        id: viewMenu
        title: qsTr("View")
        StudioCommandMenuItems { id: viewMode; controller: menuBar.controller; hostMenu: viewMenu; menuPath: "view.mode"; insertionIndex: 0 }
        MenuSeparator {}
        StudioCommandMenuItems { id: viewZoom; controller: menuBar.controller; hostMenu: viewMenu; menuPath: "view.zoom"; insertionIndex: menuBar.commandCount("view.mode") + 1 }
        MenuSeparator {}
        StudioCommandMenuItems { id: viewCompare; controller: menuBar.controller; hostMenu: viewMenu; menuPath: "view.compare"; insertionIndex: menuBar.commandCount("view.mode") + menuBar.commandCount("view.zoom") + 2 }
        MenuSeparator {}
        StudioCommandMenuItems { controller: menuBar.controller; hostMenu: viewMenu; menuPath: "view.commands"; insertionIndex: menuBar.commandCount("view.mode") + menuBar.commandCount("view.zoom") + menuBar.commandCount("view.compare") + 3 }
    }
    Menu {
        id: photoMenu
        title: qsTr("Photo")
        StudioCommandMenuItems { id: photoNavigate; controller: menuBar.controller; hostMenu: photoMenu; menuPath: "photo.navigate"; insertionIndex: 0 }
        MenuSeparator {}
        StudioCommandMenuItems { id: photoTransform; controller: menuBar.controller; hostMenu: photoMenu; menuPath: "photo.transform"; insertionIndex: menuBar.commandCount("photo.navigate") + 1 }
        MenuSeparator {}
        Menu {
            id: ratingMenu
            title: qsTr("Rating")
            StudioCommandMenuItems { controller: menuBar.controller; hostMenu: ratingMenu; menuPath: "photo.rating"; insertionIndex: 0 }
        }
        Menu {
            id: colorMenu
            title: qsTr("Color Label")
            StudioCommandMenuItems { controller: menuBar.controller; hostMenu: colorMenu; menuPath: "photo.color"; insertionIndex: 0 }
        }
        StudioCommandMenuItems { id: photoReview; controller: menuBar.controller; hostMenu: photoMenu; menuPath: "photo.review"; insertionIndex: menuBar.commandCount("photo.navigate") + menuBar.commandCount("photo.transform") + 4 }
        MenuSeparator {}
        StudioCommandMenuItems { controller: menuBar.controller; hostMenu: photoMenu; menuPath: "photo.delete"; insertionIndex: menuBar.commandCount("photo.navigate") + menuBar.commandCount("photo.transform") + menuBar.commandCount("photo.review") + 5 }
    }
    Menu {
        id: helpMenu
        title: qsTr("Help")
        StudioCommandMenuItems { controller: menuBar.controller; hostMenu: helpMenu; menuPath: "help.about"; insertionIndex: 0 }
    }
}
