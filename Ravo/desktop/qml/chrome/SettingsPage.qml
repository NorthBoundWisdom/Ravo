import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "settingsWorkspace"
    required property var presenter
    required property var commands
    required property var languageManager
    required property var assistant
    required property var panelLayout
    signal closeRequested
    signal chooseBackupFolderRequested
    readonly property var sections: [
        {
            "id": "general",
            "title": qsTr("General")
        },
        {
            "id": "workspace",
            "title": qsTr("Workspace")
        },
        {
            "id": "backup",
            "title": qsTr("Catalog & Backup")
        },
        {
            "id": "assistant",
            "title": qsTr("Assistant")
        }
    ]
    readonly property string section: commands.controller.settingsSection
    readonly property bool compact: width < 760
    color: Theme.windowColor
    focus: visible
    Keys.onEscapePressed: root.closeRequested()
    MouseArea {
        anchors.fill: parent
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
            CustomComboBox {
                objectName: "settingsCompactSection"
                visible: root.compact
                Layout.preferredWidth: Fonts.scaledUiSize(180)
                model: root.sections
                textRole: "title"
                currentIndex: {
                    for (let i = 0; i < model.length; ++i)
                        if (model[i].id === root.section)
                            return i;
                    return -1;
                }
                onActivated: function (index) {
                    root.commands.controller.selectSettingsSection(root.sections[index].id);
                }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Fonts.size20
            ColumnLayout {
                visible: !root.compact
                Layout.preferredWidth: Fonts.scaledUiSize(180)
                Layout.alignment: Qt.AlignTop
                spacing: Fonts.size4
                Repeater {
                    model: root.sections
                    CustomButton {
                        required property var modelData
                        Layout.fillWidth: true
                        objectName: "settingsSection_" + modelData.id
                        text: modelData.title
                        buttonColor: root.section === modelData.id ? Theme.highlightColor : Theme.toolbarSurfaceColor
                        buttonTextColor: root.section === modelData.id ? Theme.highlightedTextColor : Theme.textColor
                        onClicked: root.commands.controller.selectSettingsSection(modelData.id)
                    }
                }
            }
            CustomScrollView {
                id: scroll
                objectName: "settingsScroll"
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                Loader {
                    width: Math.min(scroll.availableWidth, Fonts.scaledUiSize(880))
                    active: root.visible
                    sourceComponent: root.section === "backup" ? backupSection : root.section === "assistant" ? assistantSection : root.section === "workspace" ? workspaceSection : generalSection
                }
            }
        }
    }
    Component {
        id: generalSection
        ColumnLayout {
            spacing: Fonts.size16
            CustomLabel {
                text: qsTr("General")
                font.bold: true
                font.pixelSize: Fonts.size18
            }
            CustomLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.placeholderTextColor
                text: qsTr("Application preferences apply to all catalogs.")
            }
            RowLayout {
                CustomLabel {
                    text: qsTr("Language")
                }
                CustomComboBox {
                    objectName: "settingsLanguage"
                    Layout.preferredWidth: Fonts.scaledUiSize(240)
                    textRole: "label"
                    model: root.languageManager.languageOptions
                    currentIndex: {
                        for (let i = 0; i < model.length; ++i)
                            if (model[i].code === root.languageManager.language)
                                return i;
                        return -1;
                    }
                    onActivated: function (index) {
                        root.languageManager.setLanguage(model[index].code);
                    }
                }
            }
            CustomLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.errorColor
                text: root.languageManager.lastError
                visible: text.length > 0
            }
        }
    }
    Component {
        id: workspaceSection
        ColumnLayout {
            spacing: Fonts.size16
            CustomLabel {
                text: qsTr("Workspace")
                font.bold: true
                font.pixelSize: Fonts.size18
            }
            CustomLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.placeholderTextColor
                text: qsTr("Panel sizes are saved automatically. You can also resize them by dragging their edges.")
            }
            Repeater {
                model: [
                    {
                        "label": qsTr("Left panel width"),
                        "key": "leftWidth",
                        "min": "leftMin",
                        "max": "leftMax"
                    },
                    {
                        "label": qsTr("Right panel width"),
                        "key": "rightWidth",
                        "min": "rightMin",
                        "max": "rightMax"
                    },
                    {
                        "label": qsTr("Filmstrip height"),
                        "key": "filmstripHeight",
                        "min": "filmstripMin",
                        "max": "filmstripMax"
                    }
                ]
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    CustomLabel {
                        text: modelData.label
                        Layout.preferredWidth: Fonts.scaledUiSize(160)
                    }
                    CustomSpinBox {
                        objectName: "settingsLayout_" + modelData.key
                        Layout.preferredWidth: Fonts.scaledUiSize(120)
                        decimals: 0
                        realFrom: root.panelLayout.constraints[modelData.min]
                        realTo: root.panelLayout.constraints[modelData.max]
                        realValue: root.panelLayout[modelData.key]
                        onEditingCommitted: function (value) {
                            if (modelData.key === "leftWidth")
                                root.panelLayout.setSideWidths(value, root.panelLayout.rightWidth);
                            else if (modelData.key === "rightWidth")
                                root.panelLayout.setSideWidths(root.panelLayout.leftWidth, value);
                            else
                                root.panelLayout.setFilmstripHeight(value);
                        }
                    }
                    CustomLabel {
                        text: qsTr("px")
                        color: Theme.placeholderTextColor
                    }
                }
            }
            CustomButton {
                objectName: "settingsResetLayout"
                text: qsTr("Reset panel sizes")
                onClicked: root.panelLayout.resetToDefaults()
            }
            CustomLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                visible: text.length > 0
                text: root.panelLayout.lastError
                color: Theme.errorColor
            }
        }
    }
    Component {
        id: backupSection
        SettingsBackupSection {
            presenter: root.presenter
            commands: root.commands
            model: root.commands.controller.backupSettings
            onChooseFolderRequested: root.chooseBackupFolderRequested()
        }
    }
    Component {
        id: assistantSection
        SettingsAssistantSection {
            assistant: root.assistant
        }
    }
}
