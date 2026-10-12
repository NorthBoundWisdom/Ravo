pragma Translator: SettingsPage

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "settingsWorkspace"
    required property var presenter
    required property var commands
    required property var languageManager
    required property var assistant
    required property var updates
    required property var panelLayout
    signal chooseBackupFolderRequested
    readonly property string section: commands.controller.settingsSection
    readonly property bool requestedOpen: commands.controller.settingsOpen
    titleText: qsTr("Settings")
    width: Math.min(Fonts.scaledUiSize(640), parent ? parent.width - Fonts.size24 : Fonts.scaledUiSize(640))
    height: Math.min(Fonts.scaledUiSize(720), parent ? parent.height - Fonts.size24 : Fonts.scaledUiSize(720))

    function syncOpen() {
        if (requestedOpen)
            openDialog();
        else
            close();
    }
    function revealSection() {
        const panel = section === "backup" ? backupPanel : section === "assistant" ? assistantPanel : section === "workspace" ? workspacePanel : generalPanel;
        panel.expanded = true;
        Qt.callLater(function () {
            scroll.contentItem.contentY = Math.max(0, Math.min(panel.y, contentColumn.height - scroll.availableHeight));
        });
    }

    onRequestedOpenChanged: syncOpen()
    onSectionChanged: if (opened)
        revealSection()
    onOpened: {
        requestDialogFocus();
        revealSection();
    }
    onClosed: commands.controller.settingsOpen = false
    Component.onCompleted: syncOpen()

    bodyItem: CustomScrollView {
        id: scroll
        objectName: "settingsScroll"
        rightPadding: ScrollBar.vertical.width + Fonts.size4
        contentWidth: availableWidth
        contentHeight: contentColumn.implicitHeight
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            id: contentColumn
            width: scroll.availableWidth
            spacing: Fonts.size8
            SettingsPanel {
                id: generalPanel
                objectName: "settingsSection_general"
                title: qsTr("General")
                onToggled: function (expanded) {
                    if (expanded)
                        root.commands.controller.selectSettingsSection("general");
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Fonts.size8
                    CustomLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.placeholderTextColor
                        text: qsTr("Application preferences apply to all catalogs.")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        CustomLabel {
                            text: qsTr("Language")
                        }
                        CustomComboBox {
                            objectName: "settingsLanguage"
                            Layout.fillWidth: true
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
                    CustomCheckBox {
                        objectName: "settingsAutomaticUpdates"
                        text: qsTr("Check for updates at startup")
                        checked: root.updates.automatic
                        onClicked: {
                            root.updates.setAutomatic(checked);
                            checked = Qt.binding(function () {
                                return root.updates.automatic;
                            });
                        }
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.errorColor
                        text: root.updates.settingsError
                        visible: text.length > 0
                    }
                }
            }
            SettingsPanel {
                id: workspacePanel
                objectName: "settingsSection_workspace"
                title: qsTr("Workspace")
                onToggled: function (expanded) {
                    if (expanded)
                        root.commands.controller.selectSettingsSection("workspace");
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Fonts.size8
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
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
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
            SettingsPanel {
                id: backupPanel
                objectName: "settingsSection_backup"
                title: qsTr("Catalog & Backup")
                onToggled: function (expanded) {
                    if (expanded)
                        root.commands.controller.selectSettingsSection("backup");
                }
                SettingsBackupSection {
                    Layout.fillWidth: true
                    presenter: root.presenter
                    commands: root.commands
                    model: root.commands.controller.backupSettings
                    onChooseFolderRequested: root.chooseBackupFolderRequested()
                }
            }
            SettingsPanel {
                id: assistantPanel
                objectName: "settingsSection_assistant"
                title: qsTr("Assistant")
                onToggled: function (expanded) {
                    if (expanded)
                        root.commands.controller.selectSettingsSection("assistant");
                }
                SettingsAssistantSection {
                    Layout.fillWidth: true
                    assistant: root.assistant
                }
            }
        }
    }
    footerItem: RowLayout {
        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            objectName: "settingsClose"
            text: qsTr("Close")
            onClicked: root.requestClose("button")
        }
    }
}
