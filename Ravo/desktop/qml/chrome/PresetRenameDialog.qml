import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root

    objectName: "PresetRenameDialog"
    titleText: qsTr("Rename Preset")
    width: Fonts.messageDialogWidth
    bodyFillHeight: false
    showCloseButton: true

    property string presetPath: ""
    property string presetName: ""
    readonly property bool canRename: nameField.text.trim().length > 0 && nameField.text !== root.presetName

    signal renameAccepted(string path, string name)
    signal renameCanceled

    function openForPreset(path, name) {
        root.presetPath = path;
        root.presetName = name;
        nameField.text = name;
        nameField.originalText = name;
        openDialog();
        Qt.callLater(function () {
            nameField.forceActiveFocus();
            nameField.selectAll();
        });
    }

    function acceptRename() {
        if (!root.canRename)
            return;
        const path = root.presetPath;
        const name = nameField.text;
        close();
        root.renameAccepted(path, name);
    }

    function cancelRename() {
        close();
        root.renameCanceled();
    }

    onCloseRequested: function (reason) {
        root.renameCanceled();
    }

    bodyItem: ColumnLayout {
        width: parent ? parent.width : Fonts.messageDialogWidth
        spacing: Fonts.size8

        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Preset name")
            Accessible.name: qsTr("Preset name")
        }

        CustomTextField {
            id: nameField
            objectName: "presetRenameField"
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.inputFieldHeight
            alignRightWhenFocused: false
            showClipIndicator: false
            showEmptyIndicator: false
            placeholderText: qsTr("Preset name")
            Accessible.name: qsTr("Preset name")
            onAccepted: root.acceptRename()
        }
    }

    footerItem: RowLayout {
        spacing: Fonts.size10

        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            objectName: "presetRenameCancel"
            text: qsTr("Cancel")
            Accessible.name: qsTr("Cancel")
            onClicked: root.cancelRename()
        }
        CustomButton {
            objectName: "presetRenameAccept"
            text: qsTr("Rename")
            enabled: root.canRename
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            Accessible.name: qsTr("Rename")
            onClicked: root.acceptRename()
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
