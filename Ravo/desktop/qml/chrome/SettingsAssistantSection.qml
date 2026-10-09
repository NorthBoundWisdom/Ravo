import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    objectName: "settingsAssistantSection"
    required property var assistant
    spacing: Fonts.size16
    CustomLabel {
        text: qsTr("Assistant")
        font.bold: true
        font.pixelSize: Fonts.size18
    }
    CustomLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.placeholderTextColor
        text: qsTr("OpenAI-compatible endpoint used by the floating Assistant panel. The default is the xAI API.")
    }
    CustomLabel {
        text: qsTr("URL")
    }
    CustomTextField {
        Layout.fillWidth: true
        showEmptyIndicator: false
        showClipIndicator: false
        alignRightWhenFocused: false
        text: root.assistant.endpoint
        placeholderText: "https://api.x.ai/v1"
        onEditingCommitted: function (value) {
            if (!root.assistant.setEndpoint(value))
                text = root.assistant.endpoint;
        }
    }
    CustomLabel {
        text: qsTr("Model")
    }
    CustomTextField {
        Layout.fillWidth: true
        showEmptyIndicator: false
        showClipIndicator: false
        alignRightWhenFocused: false
        text: root.assistant.model
        placeholderText: "grok-4.5"
        onEditingCommitted: function (value) {
            if (!root.assistant.setModel(value))
                text = root.assistant.model;
        }
    }
    CustomLabel {
        text: qsTr("API key")
    }
    RowLayout {
        Layout.fillWidth: true
        CustomTextField {
            Layout.fillWidth: true
            showEmptyIndicator: false
            showClipIndicator: false
            alignRightWhenFocused: false
            echoMode: showKey.checked ? TextInput.Normal : TextInput.Password
            text: root.assistant.apiKey
            onEditingCommitted: function (value) {
                root.assistant.setApiKey(value);
            }
        }
        CustomCheckBox {
            id: showKey
            text: qsTr("Show")
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.assistant.lastError
        color: Theme.errorColor
        wrapMode: Text.WordWrap
    }
}
