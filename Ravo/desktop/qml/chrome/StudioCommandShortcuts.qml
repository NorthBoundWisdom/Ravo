import QtQuick

Item {
    id: root
    required property var controller
    width: 0
    height: 0

    Instantiator {
        model: root.controller ? root.controller.shortcutBindings : []
        delegate: Shortcut {
            required property var modelData
            sequence: modelData.sequence
            enabled: {
                if (!root.controller)
                    return false;
                // The controller owns policy; revision re-evaluates availability
                // without rebuilding the immutable shortcut delegates.
                const commandRevision = root.controller.revision;
                return root.controller.shortcutEnabled(modelData.actionId, modelData.sequence);
            }
            context: Qt.WindowShortcut
            onActivated: root.controller.executeAction(modelData.actionId, "keyboard")
        }
    }
}
