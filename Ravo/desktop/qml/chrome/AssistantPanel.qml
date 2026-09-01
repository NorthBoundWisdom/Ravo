import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    required property var assistant
    required property var presenter
    required property var windowHost

    signal closeRequested

    width: 380
    height: 480
    radius: 8
    color: Theme.popupSurfaceColor
    border.color: Theme.dividerColor
    border.width: 1
    visible: false
    clip: true

    property bool placed: false

    function hostWidth() {
        return root.windowHost ? root.windowHost.width : root.width;
    }
    function hostHeight() {
        return root.windowHost ? root.windowHost.height : root.height;
    }
    function clampToHost() {
        root.x = Math.max(Fonts.size12, Math.min(root.x, root.hostWidth() - root.width - Fonts.size12));
        root.y = Math.max(Fonts.size12, Math.min(root.y, root.hostHeight() - root.height - Fonts.size12));
    }
    function placeDefault() {
        root.x = Math.max(Fonts.size12, root.hostWidth() - root.width - Fonts.size16);
        root.y = Math.max(Fonts.size12, root.hostHeight() - root.height - Fonts.size48);
        root.placed = true;
    }
    function sendCurrent() {
        if (!root.assistant || root.assistant.busy)
            return;
        const text = promptField.text;
        const photo = root.presenter ? root.presenter.selectedDisplayName : "";
        if (root.assistant.send(text, photo))
            promptField.text = "";
    }

    onVisibleChanged: {
        if (!visible)
            return;
        if (!root.placed)
            root.placeDefault();
        else
            root.clampToHost();
        Qt.callLater(function () {
            if (root.visible)
                promptField.forceActiveFocus();
        });
    }

    Connections {
        target: root.windowHost
        function onWidthChanged() {
            if (root.visible)
                root.clampToHost();
        }
        function onHeightChanged() {
            if (root.visible)
                root.clampToHost();
        }
    }

    Shortcut {
        enabled: root.visible
        sequence: "Escape"
        context: Qt.WindowShortcut
        onActivated: root.closeRequested()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Fonts.toolbarHeight
            color: Theme.toolbarSurfaceColor

            MouseArea {
                id: dragArea
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                property real pressX: 0
                property real pressY: 0
                onPressed: function (event) {
                    pressX = event.x;
                    pressY = event.y;
                }
                onPositionChanged: function (event) {
                    if (!pressed)
                        return;
                    root.x += event.x - pressX;
                    root.y += event.y - pressY;
                    root.clampToHost();
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.size12
                anchors.rightMargin: Fonts.size8
                spacing: Fonts.size8

                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Assistant")
                    font.bold: true
                }
                CustomLabel {
                    text: root.assistant ? root.assistant.model : ""
                    color: Theme.placeholderTextColor
                    elide: Text.ElideRight
                    Layout.maximumWidth: 140
                }
                CustomButton {
                    text: qsTr("Close")
                    onClicked: root.closeRequested()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        ListView {
            id: transcript
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.assistant ? root.assistant.messages : []
            spacing: Fonts.size8
            topMargin: Fonts.size10
            bottomMargin: Fonts.size10
            leftMargin: Fonts.size12
            rightMargin: Fonts.size12

            delegate: ColumnLayout {
                required property var modelData
                width: ListView.view.width - Fonts.size24
                spacing: 2
                CustomLabel {
                    text: modelData.role === "user" ? qsTr("You") : qsTr("Assistant")
                    color: Theme.placeholderTextColor
                    font.pixelSize: Fonts.size10
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: modelData.text
                    wrapMode: Text.WordWrap
                    color: Theme.textColor
                }
            }

            CustomLabel {
                anchors.centerIn: parent
                visible: transcript.count === 0 && !(root.assistant && root.assistant.busy)
                width: parent.width - Fonts.size24
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: Theme.placeholderTextColor
                text: root.assistant && root.assistant.configured ? qsTr("Ask about the selected photo or a Develop edit.") : qsTr("Set the assistant URL, model, and API key in Settings.")
            }

            onCountChanged: Qt.callLater(function () {
                if (transcript.count > 0)
                    transcript.positionViewAtEnd();
            })
        }

        CustomLabel {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size12
            Layout.rightMargin: Fonts.size12
            visible: root.assistant && root.assistant.lastError.length > 0
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: root.assistant ? root.assistant.lastError : ""
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size12
            Layout.rightMargin: Fonts.size12
            Layout.topMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            spacing: Fonts.size8

            CustomTextField {
                id: promptField
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                showEmptyIndicator: false
                showClipIndicator: false
                alignRightWhenFocused: false
                enabled: root.assistant && !root.assistant.busy
                placeholderText: qsTr("Message")
                Keys.onReturnPressed: root.sendCurrent()
                Keys.onEnterPressed: root.sendCurrent()
            }
            CustomButton {
                text: root.assistant && root.assistant.busy ? qsTr("Cancel") : qsTr("Send")
                enabled: root.assistant !== null
                onClicked: {
                    if (root.assistant.busy)
                        root.assistant.cancel();
                    else
                        root.sendCurrent();
                }
            }
        }
    }
}
