import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "BackupScheduleDialog"
    titleText: qsTr("Scheduled Backups")
    width: Fonts.messageDialogWidth
    bodyFillHeight: false
    showCloseButton: true

    property int intervalMinutes: 1440
    property int retentionCount: 7
    readonly property var intervalChoices: [
        {
            "label": qsTr("Every 15 minutes"),
            "minutes": 15
        },
        {
            "label": qsTr("Hourly"),
            "minutes": 60
        },
        {
            "label": qsTr("Daily"),
            "minutes": 1440
        },
        {
            "label": qsTr("Weekly"),
            "minutes": 10080
        }
    ]

    signal scheduleAccepted(int intervalMinutes, int retentionCount)
    signal scheduleCanceled

    function intervalIndex(minutes) {
        for (var index = 0; index < intervalChoices.length; ++index) {
            if (intervalChoices[index].minutes === minutes)
                return index;
        }
        return 2;
    }

    function openForPolicy(policy) {
        intervalMinutes = policy && policy.intervalMinutes ? policy.intervalMinutes : 1440;
        retentionCount = policy && policy.retentionCount ? policy.retentionCount : 7;
        const index = intervalIndex(intervalMinutes);
        intervalMinutes = intervalChoices[index].minutes;
        intervalCombo.currentIndex = index;
        retentionSpin.realValue = retentionCount;
        openDialog();
        Qt.callLater(function () {
            intervalCombo.forceActiveFocus();
        });
    }

    function acceptSchedule() {
        close();
        scheduleAccepted(intervalMinutes, retentionCount);
    }

    function cancelSchedule() {
        close();
        scheduleCanceled();
    }

    onCloseRequested: function (reason) {
        root.scheduleCanceled();
    }

    bodyItem: ColumnLayout {
        width: parent ? parent.width : Fonts.messageDialogWidth
        spacing: Fonts.size10

        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("Choose how often Ravo creates a verified catalog backup and how many verified backups it keeps. You will choose the destination folder next.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin

            CustomLabel {
                text: qsTr("Frequency")
                Accessible.name: qsTr("Backup frequency")
            }
            CustomComboBox {
                id: intervalCombo
                objectName: "backupScheduleInterval"
                Layout.fillWidth: true
                textRole: "label"
                model: root.intervalChoices
                currentIndex: root.intervalIndex(root.intervalMinutes)
                Accessible.name: qsTr("Backup frequency")
                onActivated: function (index) {
                    root.intervalMinutes = root.intervalChoices[index].minutes;
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin

            CustomLabel {
                text: qsTr("Keep backups")
                Accessible.name: qsTr("Backup retention count")
            }
            CustomSpinBox {
                id: retentionSpin
                objectName: "backupScheduleRetention"
                Layout.fillWidth: true
                decimals: 0
                realFrom: 1
                realTo: 100
                realValue: root.retentionCount
                Accessible.name: qsTr("Backup retention count")
                onEditingCommitted: function (value) {
                    root.retentionCount = value;
                }
            }
        }
    }

    footerItem: RowLayout {
        spacing: Fonts.size10

        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            objectName: "backupScheduleCancel"
            text: qsTr("Cancel")
            onClicked: root.cancelSchedule()
        }
        CustomButton {
            objectName: "backupScheduleChooseFolder"
            text: qsTr("Choose Folder…")
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            onClicked: root.acceptSchedule()
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
