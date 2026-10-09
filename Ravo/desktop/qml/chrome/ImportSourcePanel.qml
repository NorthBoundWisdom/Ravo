pragma Translator: ImportPage
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importSourcePanel"
    required property var presenter
    property var commands
    signal chooseRequested
    onVisibleChanged: if (!visible)
        folderMenu.close()
    color: Theme.railSurfaceColor
    enabled: !presenter.imports.importWorkActive && !presenter.imports.importPreflightActive && !presenter.imports.importInteractionBlocked
    StudioContextMenu {
        id: folderMenu
        objectName: "importSourceFolderContextMenu"
        property string folderPath: ""
        StudioContextMenuItem {
            objectName: "importSourceCopyPath"
            displayText: qsTranslate("StudioCommands", "Copy Path")
            enabled: root.commands && folderMenu.folderPath.length > 0
            onTriggered: root.commands.run(root.commands.ids.libraryCopyFolderPath, folderMenu.folderPath)
        }
        StudioContextMenuItem {
            objectName: "importSourceRevealFolder"
            displayText: root.commands && root.commands.controller ? root.commands.controller.action(root.commands.ids.libraryRevealFolder).title : ""
            enabled: root.commands && folderMenu.folderPath.length > 0
            onTriggered: root.commands.run(root.commands.ids.libraryRevealFolder, folderMenu.folderPath)
        }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Fonts.standardMargin
        spacing: Fonts.size12
        CustomLabel {
            text: qsTr("Source")
            font.bold: true
        }
        CustomLabel {
            Layout.fillWidth: true
            text: root.presenter.imports.importSourceRoot
            wrapMode: Text.WrapAnywhere
            color: Theme.placeholderTextColor
        }
        CustomButton {
            Layout.fillWidth: true
            text: qsTr("Choose Source…")
            onClicked: root.chooseRequested()
        }
        CustomCheckBox {
            objectName: "importIncludeSubfolders"
            text: qsTr("Include subfolders")
            checked: root.presenter.imports.importRecursive
            onClicked: root.presenter.imports.setImportRecursive(checked)
        }
        CustomButton {
            Layout.fillWidth: true
            text: qsTr("Check again")
            onClicked: {
                if (root.presenter.imports.importSourceRoot.length > 0)
                    root.presenter.imports.setImportSourceRoot(root.presenter.imports.importSourceRoot);
                else
                    root.presenter.imports.refreshImportSources();
            }
        }
        Rectangle {
            objectName: "importSourceTreeSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.baseColor
            border.color: Theme.dividerColor
            border.width: ControlState.borderThin
            radius: ControlState.radiusSmall
            ImportFolderTree {
                id: sourceTree
                objectName: "importSourceFolderTree"
                anchors.fill: parent
                anchors.margins: Fonts.size4
                folderModel: root.presenter.imports.importSourceFolders
                ScrollBar.vertical: CustomScrollBar {
                    policy: ScrollBar.AsNeeded
                }
                onFolderChosen: function (path) {
                    root.presenter.imports.setImportSourceRoot(path);
                }
                onFolderContextRequested: function (path, position) {
                    folderMenu.folderPath = path;
                    const point = sourceTree.mapToItem(folderMenu.parent, position.x, position.y);
                    folderMenu.popup(point.x, point.y);
                }
            }
        }
    }
}
