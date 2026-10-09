import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    objectName: "settingsBackupSection"
    required property var presenter
    required property var commands
    required property var model
    signal chooseFolderRequested
    readonly property var status: presenter.backupScheduleStatus
    spacing: Fonts.size16

    function timeText(value) {
        return value > 0 ? new Date(value).toLocaleString(Qt.locale(), Locale.ShortFormat) : qsTr("Never");
    }
    function sizeText(value) {
        if (!value || value <= 0)
            return qsTr("0 B");
        if (value < 1024)
            return qsTr("%1 B").arg(value);
        if (value < 1024 * 1024)
            return qsTr("%1 KiB").arg((value / 1024).toFixed(1));
        return qsTr("%1 MiB").arg((value / (1024 * 1024)).toFixed(1));
    }
    CustomLabel {
        text: qsTr("Automatic backups")
        font.bold: true
        font.pixelSize: Fonts.size18
    }
    CustomLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.placeholderTextColor
        text: qsTr("These settings apply to the current catalog. Backups include the catalog and edit history, but exclude original media and preview caches. Scheduled backups run while Studio is open.")
    }
    CustomLabel {
        Layout.fillWidth: true
        text: root.presenter.catalogPath
        textFormat: Text.PlainText
        elide: Text.ElideMiddle
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: !root.model.loaded
        wrapMode: Text.WordWrap
        text: root.model.disabledReason
    }
    ColumnLayout {
        Layout.fillWidth: true
        enabled: root.model.canEdit
        spacing: Fonts.size12
        CustomCheckBox {
            objectName: "settingsBackupEnabled"
            text: qsTr("Enable automatic backups")
            checked: root.model.enabled
            onClicked: root.model.enabled = checked
        }
        CustomLabel {
            text: qsTr("Backup folder")
        }
        RowLayout {
            Layout.fillWidth: true
            CustomTextField {
                objectName: "settingsBackupDirectory"
                Layout.fillWidth: true
                showEmptyIndicator: false
                showClipIndicator: false
                alignRightWhenFocused: false
                text: root.model.directory
                onTextEdited: root.model.directory = text
            }
            CustomButton {
                text: qsTr("Choose folder…")
                onClicked: if (root.model.beginDirectorySelection())
                    root.chooseFolderRequested()
            }
        }
        RowLayout {
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Interval (minutes)")
            }
            CustomSpinBox {
                objectName: "settingsBackupInterval"
                Layout.preferredWidth: Fonts.scaledUiSize(140)
                decimals: 0
                realFrom: root.model.limits.intervalMin
                realTo: root.model.limits.intervalMax
                realValue: root.model.intervalMinutes
                onEditingCommitted: function (value) {
                    root.model.intervalMinutes = value;
                }
            }
        }
        RowLayout {
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Backups to keep")
            }
            CustomSpinBox {
                objectName: "settingsBackupRetention"
                Layout.preferredWidth: Fonts.scaledUiSize(140)
                decimals: 0
                realFrom: root.model.limits.retentionMin
                realTo: root.model.limits.retentionMax
                realValue: root.model.retentionCount
                onEditingCommitted: function (value) {
                    root.model.retentionCount = value;
                }
            }
        }
    }
    RowLayout {
        Item {
            implicitWidth: saveButton.implicitWidth
            implicitHeight: saveButton.implicitHeight
            CustomButton {
                id: saveButton
                objectName: "settingsBackupSave"
                text: root.model.saving ? qsTr("Saving…") : qsTr("Save backup settings")
                enabled: root.model.canApply
                onClicked: root.model.apply()
            }
            MouseArea {
                id: saveHover
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
            }
            CustomToolTip {
                visible: saveHover.containsMouse && !saveButton.enabled
                text: root.model.disabledReason
            }
        }
        CustomButton {
            text: qsTr("Reload saved settings")
            enabled: !root.model.saving && (root.model.dirty || root.model.lastError.length > 0)
            onClicked: root.model.reload()
        }
    }
    CustomLabel {
        visible: root.model.dirty
        text: qsTr("Unsaved changes")
        color: Theme.placeholderTextColor
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: text.length > 0
        text: root.model.lastError
        color: Theme.errorColor
        wrapMode: Text.WordWrap
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Theme.dividerColor
    }
    CustomLabel {
        text: root.status.enabled ? qsTr("On") : qsTr("Off")
        font.bold: true
    }
    CustomLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Last verified: %1 · %2").arg(root.timeText(root.status.lastSuccessUnixMs)).arg(root.sizeText(root.status.lastBackupBytes))
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: root.status.enabled === true
        wrapMode: Text.WordWrap
        text: qsTr("Next: %1 · Keep %2").arg(root.timeText(root.status.nextRunUnixMs)).arg(root.status.retentionCount)
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: String(root.status.lastError || "").length > 0
        wrapMode: Text.WordWrap
        color: Theme.errorColor
        text: qsTr("Last failure: %1").arg(String(root.status.lastError || ""))
    }
    CustomButton {
        text: qsTr("Run backup now")
        enabled: root.model.canRun
        onClicked: root.model.runNow()
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: root.presenter.catalogOperationActive
        wrapMode: Text.WordWrap
        text: root.presenter.catalogOperationStage
    }
    CustomButton {
        text: qsTr("Cancel")
        visible: root.presenter.catalogOperationActive && !root.model.saving
        enabled: root.commands.controller.action(root.commands.ids.libraryCancelOperation).enabled
        onClicked: root.commands.run(root.commands.ids.libraryCancelOperation)
    }
}
