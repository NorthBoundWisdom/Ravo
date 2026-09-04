import QtQuick
import QtQuick.Controls

Instantiator {
    id: root
    required property var controller
    required property var hostMenu
    required property string menuPath
    property int insertionIndex: 0

    // Identity must stay stable. Binding this model to controller.revision
    // rebuilds every QQuickMenuItem on each command refresh; Qt 6.8+ native
    // macOS menus then keep untitled NSMenuItem rows (empty File-menu gap).
    model: root.controller ? root.controller.menuEntries(root.menuPath) : []

    delegate: MenuItem {
        required property var modelData

        readonly property var live: {
            if (!root.controller || !modelData || !modelData.actionId)
                return ({
                            title: "",
                            shortcutText: "",
                            enabled: false,
                            checkable: false,
                            checked: false,
                            disabledReason: ""
                        });
            const ignoredRevision = root.controller.revision;
            return root.controller.action(modelData.actionId);
        }

        text: {
            const title = String(live.title || "");
            const shortcut = String(live.shortcutText || "");
            return shortcut.length > 0 ? (title + "\t" + shortcut) : title;
        }
        enabled: live.enabled === true
        checkable: live.checkable === true
        checked: live.checked === true
        onTriggered: root.controller.executeAction(modelData.actionId, "menu")
        Accessible.name: String(live.title || "")
        Accessible.description: live.enabled ? "" : String(live.disabledReason || "")
    }

    onObjectAdded: function (index, object) {
        root.hostMenu.insertItem(root.insertionIndex + index, object);
    }
    onObjectRemoved: function (index, object) {
        root.hostMenu.removeItem(object);
    }
}
