import QtQuick
import QtQuick.Controls
import GeoControls 1.0

MenuBar {
    id: menuBar
    required property var actions

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
        title: qsTr("File")
        MenuItem {
            action: actions.createLibrary
        }
        MenuItem {
            action: actions.openLibrary
        }
        MenuSeparator {}
        MenuItem {
            action: actions.importPhotos
        }
        MenuItem {
            action: actions.importFolder
        }
        MenuSeparator {}
        MenuItem {
            action: actions.closeWindow
        }
        MenuItem {
            action: actions.preferences
        }
        MenuItem {
            action: actions.quitApp
        }
    }

    Menu {
        title: qsTr("Edit")
        MenuItem {
            action: actions.undo
        }
        MenuItem {
            action: actions.redo
        }
        MenuSeparator {}
        MenuItem {
            action: actions.resetEdits
        }
    }

    Menu {
        title: qsTr("View")
        MenuItem {
            action: actions.grid
        }
        MenuItem {
            action: actions.loupe
        }
        MenuItem {
            action: actions.develop
        }
        MenuSeparator {}
        MenuItem {
            action: actions.fit
        }
        MenuItem {
            action: actions.fill
        }
        MenuItem {
            action: actions.actualSize
        }
        MenuSeparator {}
        MenuItem {
            action: actions.beforeAfter
        }
    }

    Menu {
        title: qsTr("Photo")
        MenuItem {
            action: actions.previousPhoto
        }
        MenuItem {
            action: actions.nextPhoto
        }
        MenuSeparator {}
        MenuItem {
            action: actions.rotateLeft
        }
        MenuItem {
            action: actions.rotateRight
        }
        MenuSeparator {}
        Menu {
            title: qsTr("Rating")
            MenuItem {
                action: actions.rating0
            }
            MenuItem {
                action: actions.rating1
            }
            MenuItem {
                action: actions.rating2
            }
            MenuItem {
                action: actions.rating3
            }
            MenuItem {
                action: actions.rating4
            }
            MenuItem {
                action: actions.rating5
            }
        }
        Menu {
            title: qsTr("Color Label")
            MenuItem {
                action: actions.colorNone
            }
            MenuItem {
                action: actions.colorRed
            }
            MenuItem {
                action: actions.colorYellow
            }
            MenuItem {
                action: actions.colorGreen
            }
            MenuItem {
                action: actions.colorBlue
            }
            MenuItem {
                action: actions.colorPurple
            }
        }
        MenuItem {
            action: actions.reject
        }
        MenuSeparator {}
        MenuItem {
            action: actions.removePhoto
        }
    }

    Menu {
        title: qsTr("Help")
        MenuItem {
            action: actions.about
        }
    }
}
