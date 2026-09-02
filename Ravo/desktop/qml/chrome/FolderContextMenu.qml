import QtQuick
import QtQuick.Controls
import GeoControls 1.0

StudioContextMenu {
    id: root
    required property var commands
    required property var presenter
    property string folderUri: ""
    property string folderId: ""
    property string displayName: ""
    property bool missing: false
    property bool hasChildren: false
    property bool collapsed: false

    readonly property bool isAllPhotographs: folderUri.length === 0
    readonly property string localPath: root.presenter && folderUri.length > 0 ? root.presenter.folderLocalPath(folderUri) : ""
    readonly property string revealTitle: {
        const spec = root.commands && root.commands.controller ? root.commands.controller.action(root.commands.ids.libraryRevealFolder) : ({});
        return spec.title || qsTr("Show in File Manager");
    }

    StudioContextMenuItem {
        displayText: root.isAllPhotographs ? qsTr("Import Photos...") : qsTr("Import Photos from This Folder...")
        enabled: root.commands && root.presenter && root.presenter.catalogOpen && !root.presenter.busy && !root.presenter.importWorkActive && (root.isAllPhotographs || (!root.missing && root.localPath.length > 0))
        onTriggered: {
            if (root.isAllPhotographs)
                root.commands.importPhotos.trigger();
            else
                root.commands.run(root.commands.ids.libraryImportFolderPath, root.localPath);
        }
    }
    StudioContextMenuItem {
        displayText: root.revealTitle
        enabled: !root.isAllPhotographs && !root.missing && root.localPath.length > 0
        onTriggered: root.commands.run(root.commands.ids.libraryRevealFolder, root.folderUri)
    }
    StudioContextMenuItem {
        displayText: qsTr("Update Folder Location...")
        enabled: root.missing && root.folderId.length > 0 && root.commands && root.presenter && !root.presenter.busy
        onTriggered: root.commands.run(root.commands.ids.libraryFolderRelink, root.folderId)
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        displayText: root.collapsed ? qsTr("Expand") : qsTr("Collapse")
        enabled: root.hasChildren && !root.isAllPhotographs && root.presenter
        onTriggered: root.presenter.folders.toggleCollapsed(root.folderUri)
    }
    StudioContextMenuSeparator {}
    StudioContextMenuItem {
        displayText: qsTr("Remove from Catalog...")
        enabled: !root.isAllPhotographs && root.commands && root.presenter && root.presenter.catalogOpen && !root.presenter.busy && !root.presenter.importWorkActive && root.folderUri.length > 0
        onTriggered: root.commands.run(root.commands.ids.libraryRemoveFolder, root.folderUri)
    }
}
