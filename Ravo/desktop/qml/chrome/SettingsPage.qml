import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var languageManager
    property var assistant
    color: Theme.windowColor

    signal closeRequested

    focus: visible
    Keys.onEscapePressed: root.closeRequested()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Fonts.standardMargin
        spacing: Fonts.size12

        RowLayout {
            Layout.fillWidth: true
            CustomButton {
                text: qsTr("Back")
                icon.source: "qrc:/GeoControls/icons/Undo.svg"
                onClicked: root.closeRequested()
            }
            CustomLabel {
                text: qsTr("Settings")
                font.bold: true
                font.pixelSize: Fonts.size18
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("Ravo Studio uses a single dark workspace modeled on a photography library.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin

            CustomLabel {
                text: qsTr("Language")
            }

            CustomComboBox {
                id: languageCombo
                Layout.preferredWidth: 220
                textRole: "label"
                model: [
                    { "code": "en_US", "label": qsTr("English") },
                    { "code": "zh_CN", "label": qsTr("Simplified Chinese") }
                ]
                currentIndex: root.languageManager &&
                              root.languageManager.language === "zh_CN" ? 1 : 0
                onActivated: function (index) {
                    if (root.languageManager)
                        root.languageManager.setLanguage(model[index].code)
                }
            }
        }

        CustomLabel {
            Layout.fillWidth: true
            visible: root.languageManager && root.languageManager.lastError.length > 0
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: root.languageManager ? root.languageManager.lastError : ""
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        CustomLabel {
            text: qsTr("Assistant")
            font.bold: true
        }

        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("OpenAI-compatible endpoint used by the floating Assistant panel. The default is the xAI API.")
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Fonts.standardMargin
            rowSpacing: Fonts.size8

            CustomLabel {
                text: qsTr("URL")
            }
            CustomTextField {
                id: endpointField
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                showEmptyIndicator: false
                showClipIndicator: false
                alignRightWhenFocused: false
                text: root.assistant ? root.assistant.endpoint : ""
                placeholderText: "https://api.x.ai/v1"
                onEditingCommitted: function (committed) {
                    if (root.assistant && !root.assistant.setEndpoint(committed))
                        text = root.assistant.endpoint;
                }
            }

            CustomLabel {
                text: qsTr("Model")
            }
            CustomTextField {
                id: modelField
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                showEmptyIndicator: false
                showClipIndicator: false
                alignRightWhenFocused: false
                text: root.assistant ? root.assistant.model : ""
                placeholderText: "grok-4.5"
                onEditingCommitted: function (committed) {
                    if (root.assistant && !root.assistant.setModel(committed))
                        text = root.assistant.model;
                }
            }

            CustomLabel {
                text: qsTr("API key")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Fonts.size8
                CustomTextField {
                    id: apiKeyField
                    Layout.fillWidth: true
                    Layout.preferredHeight: Fonts.inputFieldHeight
                    showEmptyIndicator: false
                    showClipIndicator: false
                    alignRightWhenFocused: false
                    echoMode: apiKeyVisible.checked ? TextInput.Normal : TextInput.Password
                    text: root.assistant ? root.assistant.apiKey : ""
                    onEditingCommitted: function (committed) {
                        if (root.assistant)
                            root.assistant.setApiKey(committed);
                    }
                }
                CustomCheckBox {
                    id: apiKeyVisible
                    text: qsTr("Show")
                }
            }
        }

        CustomLabel {
            Layout.fillWidth: true
            visible: root.assistant && root.assistant.lastError.length > 0
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: root.assistant ? root.assistant.lastError : ""
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
