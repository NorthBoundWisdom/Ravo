import QtQuick

Item {
    id: root
    required property var controller
    width: 0
    height: 0

    Instantiator {
        model: root.controller ? root.controller.shortcutEntries : []
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.sequence
            enabled: modelData.enabled
            context: Qt.WindowShortcut
            onActivated: root.controller.executeAction(modelData.actionId, "keyboard")
        }
    }
}
