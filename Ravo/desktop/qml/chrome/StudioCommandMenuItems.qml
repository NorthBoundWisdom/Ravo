import QtQuick
import QtQuick.Controls

Instantiator {
    id: root
    required property var controller
    required property var hostMenu
    required property string menuPath
    property int insertionIndex: 0

    model: {
        if (!root.controller)
            return [];
        const ignoredRevision = root.controller.revision;
        return root.controller.menuEntries(root.menuPath);
    }

    delegate: MenuItem {
        required property var modelData
        text: modelData.shortcutText.length > 0 ? modelData.title + "\t" + modelData.shortcutText : modelData.title
        enabled: modelData.enabled
        checkable: modelData.checkable
        checked: modelData.checked
        onTriggered: root.controller.executeAction(modelData.actionId, "menu")
        Accessible.name: modelData.title
        Accessible.description: modelData.enabled ? "" : modelData.disabledReason
    }

    onObjectAdded: function (index, object) {
        root.hostMenu.insertItem(root.insertionIndex + index, object);
    }
    onObjectRemoved: function (index, object) {
        root.hostMenu.removeItem(object);
    }
}
