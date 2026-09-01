import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Popup {
    id: root
    required property var controller
    required property var windowHost
    property var previousFocusItem: null
    property string inlineMessage: ""

    parent: Overlay.overlay
    x: Math.round((parent.width - width) / 2)
    y: Fonts.size12
    width: Math.min(680, Math.max(320, parent.width - Fonts.size24))
    height: Math.min(520, Math.max(260, parent.height - Fonts.size24))
    modal: true
    dim: true
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    visible: controller && controller.paletteOpen
    padding: 0

    background: Rectangle {
        color: Theme.popupSurfaceColor
        border.color: Theme.dividerColor
        border.width: 1
        radius: 8
    }

    function moveSelection(delta) {
        if (commandList.count === 0) {
            commandList.currentIndex = -1;
            return;
        }
        commandList.currentIndex = Math.max(0, Math.min(commandList.count - 1, commandList.currentIndex + delta));
        commandList.positionViewAtIndex(commandList.currentIndex, ListView.Contain);
    }

    function executeCurrent() {
        if (commandList.currentIndex < 0 || commandList.currentIndex >= commandList.count)
            return;
        const entry = root.controller.paletteEntries[commandList.currentIndex];
        if (!entry.enabled) {
            root.inlineMessage = entry.disabledReason;
            return;
        }
        const actionId = entry.actionId;
        root.controller.paletteOpen = false;
        Qt.callLater(function () {
            root.controller.executeAction(actionId, "palette");
        });
    }

    onAboutToShow: {
        previousFocusItem = windowHost.activeFocusItem;
        inlineMessage = "";
    }
    onOpened: {
        search.text = "";
        commandList.currentIndex = commandList.count > 0 ? 0 : -1;
        Qt.callLater(function () {
            search.forceActiveFocus();
        });
    }
    onClosed: {
        if (controller && controller.paletteOpen)
            controller.paletteOpen = false;
        const focusItem = previousFocusItem;
        previousFocusItem = null;
        Qt.callLater(function () {
            if (focusItem && focusItem.visible)
                focusItem.forceActiveFocus();
        });
    }

    Shortcut {
        enabled: root.visible
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: root.close()
    }

    Connections {
        target: root.controller
        function onCommandsChanged() {
            if (commandList.currentIndex >= commandList.count)
                commandList.currentIndex = commandList.count - 1;
            if (commandList.currentIndex < 0 && commandList.count > 0)
                commandList.currentIndex = 0;
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size12
            Layout.rightMargin: Fonts.size12
            Layout.topMargin: Fonts.size10
            Layout.bottomMargin: Fonts.size10
            spacing: Fonts.size8

            CustomLabel {
                text: ">"
                color: Theme.accentColor
                font.bold: true
            }
            CustomTextField {
                id: search
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                placeholderText: qsTr("Type a command")
                text: root.controller ? root.controller.paletteQuery : ""
                onTextChanged: {
                    root.inlineMessage = "";
                    if (root.controller && root.controller.paletteQuery !== text)
                        root.controller.paletteQuery = text;
                    commandList.currentIndex = commandList.count > 0 ? 0 : -1;
                }
                Keys.onPressed: function (event) {
                    if (event.key === Qt.Key_Down) {
                        root.moveSelection(1);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Up) {
                        root.moveSelection(-1);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        root.executeCurrent();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Escape) {
                        root.controller.paletteOpen = false;
                        event.accepted = true;
                    }
                }
                Accessible.name: qsTr("Command search")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        ListView {
            id: commandList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.controller ? root.controller.paletteEntries : []
            currentIndex: count > 0 ? 0 : -1
            highlightMoveDuration: 60
            highlight: Rectangle {
                color: Theme.buttonHoveredColor
            }

            delegate: Item {
                id: row
                required property var modelData
                required property int index
                width: ListView.view.width
                height: Math.max(Fonts.listItemHeight + Fonts.size10, details.implicitHeight + Fonts.size12)

                RowLayout {
                    id: details
                    anchors.fill: parent
                    anchors.leftMargin: Fonts.size12
                    anchors.rightMargin: Fonts.size12
                    anchors.topMargin: Fonts.size6
                    anchors.bottomMargin: Fonts.size6
                    spacing: Fonts.size10

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        CustomLabel {
                            Layout.fillWidth: true
                            text: row.modelData.category + ": " + row.modelData.title
                            color: row.modelData.enabled ? Theme.textColor : Theme.disabledTextColor
                            elide: Text.ElideRight
                            font.bold: commandList.currentIndex === row.index
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            visible: !row.modelData.enabled
                            text: row.modelData.disabledReason
                            color: Theme.placeholderTextColor
                            font.pixelSize: Fonts.size10
                            elide: Text.ElideRight
                        }
                    }
                    CustomLabel {
                        visible: row.modelData.checked
                        text: "\u2713"
                        color: Theme.accentColor
                    }
                    CustomLabel {
                        visible: row.modelData.shortcutText.length > 0
                        text: row.modelData.shortcutText
                        color: Theme.placeholderTextColor
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    onEntered: commandList.currentIndex = row.index
                    onClicked: {
                        commandList.currentIndex = row.index;
                        root.executeCurrent();
                    }
                }
                Accessible.name: row.modelData.category + ": " + row.modelData.title
                Accessible.description: row.modelData.enabled ? "" : row.modelData.disabledReason
            }

            CustomLabel {
                anchors.centerIn: parent
                visible: commandList.count === 0
                text: qsTr("No matching commands")
                color: Theme.placeholderTextColor
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.inlineMessage.length > 0 ? Fonts.inputFieldHeight : 0
            visible: root.inlineMessage.length > 0
            color: Theme.toolbarSurfaceColor
            CustomLabel {
                anchors.fill: parent
                anchors.leftMargin: Fonts.size12
                anchors.rightMargin: Fonts.size12
                verticalAlignment: Text.AlignVCenter
                text: root.inlineMessage
                color: Theme.placeholderTextColor
                elide: Text.ElideRight
            }
        }
    }
}
