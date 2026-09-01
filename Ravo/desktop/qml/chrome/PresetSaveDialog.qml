pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root

    objectName: "ParameterSelectionDialog"
    titleText: presetMode ? qsTr("Save Preset") : qsTr("Copy Parameters")
    width: Math.max(Fonts.messageDialogWidth, 520)
    bodyFillHeight: false
    showCloseButton: true

    property string mode: "preset"
    property int selectedCount: 0
    readonly property bool presetMode: mode === "preset"
    readonly property bool canAccept: selectedCount > 0 && (!presetMode || nameField.text.trim().length > 0)

    signal saveAccepted(string name, var fields)
    signal copyAccepted(var fields)
    signal selectionCanceled

    function refreshSelectedCount() {
        var count = 0;
        for (var index = 0; index < parameterModel.count; ++index) {
            if (parameterModel.get(index).included)
                ++count;
        }
        selectedCount = count;
    }

    function setAllIncluded(included) {
        for (var index = 0; index < parameterModel.count; ++index)
            parameterModel.setProperty(index, "included", included);
        refreshSelectedCount();
    }

    function selectedFields() {
        var fields = [];
        for (var index = 0; index < parameterModel.count; ++index) {
            const item = parameterModel.get(index);
            if (item.included)
                fields.push(item.field);
        }
        return fields;
    }

    function loadParameters(parameters) {
        parameterModel.clear();
        for (var index = 0; index < parameters.length; ++index) {
            parameterModel.append({
                "field": String(parameters[index].field || ""),
                "label": String(parameters[index].label || parameters[index].field || ""),
                "group": String(parameters[index].group || ""),
                "included": false
            });
        }
        selectedCount = 0;
    }

    function openForPreset(suggestedName, parameters) {
        mode = "preset";
        loadParameters(parameters);
        nameField.text = suggestedName;
        nameField.originalText = suggestedName;
        openDialog();
        Qt.callLater(function () {
            nameField.forceActiveFocus();
            nameField.selectAll();
        });
    }

    function openForCopy(parameters) {
        mode = "copy";
        loadParameters(parameters);
        nameField.text = "";
        nameField.originalText = "";
        openDialog();
        Qt.callLater(function () {
            parameterList.forceActiveFocus();
        });
    }

    function acceptSelection() {
        if (!canAccept)
            return;
        const fields = selectedFields();
        close();
        if (presetMode)
            root.saveAccepted(nameField.text, fields);
        else
            root.copyAccepted(fields);
    }

    function cancelSelection() {
        close();
        root.selectionCanceled();
    }

    onCloseRequested: function (reason) {
        root.selectionCanceled();
    }

    ListModel {
        id: parameterModel
    }

    bodyItem: ColumnLayout {
        width: parent ? parent.width : Fonts.messageDialogWidth
        spacing: Fonts.size8

        CustomLabel {
            Layout.fillWidth: true
            visible: root.presetMode
            text: qsTr("Preset name")
            Accessible.name: qsTr("Preset name")
        }

        CustomTextField {
            id: nameField
            objectName: "presetSaveName"
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.inputFieldHeight
            visible: root.presetMode
            alignRightWhenFocused: false
            showClipIndicator: false
            showEmptyIndicator: true
            placeholderText: qsTr("Preset name")
            Accessible.name: qsTr("Preset name")
        }

        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: root.presetMode ? qsTr("Choose which modified parameters this preset will apply. Nothing is selected by default.") : qsTr("Choose which modified parameters to copy. Nothing is selected by default.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size8

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("%1 selected").arg(root.selectedCount)
                Accessible.name: text
            }
            CustomButton {
                objectName: "presetSelectAll"
                text: qsTr("Select all")
                enabled: parameterModel.count > 0 && root.selectedCount < parameterModel.count
                onClicked: root.setAllIncluded(true)
            }
            CustomButton {
                objectName: "presetClearSelection"
                text: qsTr("Clear")
                enabled: root.selectedCount > 0
                onClicked: root.setAllIncluded(false)
            }
        }

        CustomLabel {
            Layout.fillWidth: true
            visible: parameterModel.count === 0
            text: qsTr("No modified parameters are available.")
            color: Theme.placeholderTextColor
        }

        ListView {
            id: parameterList
            objectName: "presetParameterList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(320, Math.max(Fonts.listItemHeight * 3, parameterModel.count * Fonts.listItemHeight))
            visible: parameterModel.count > 0
            clip: true
            spacing: Fonts.size2
            boundsBehavior: Flickable.StopAtBounds
            model: parameterModel
            Accessible.name: qsTr("Modified parameters")

            delegate: CustomCheckBox {
                id: presetParameter
                required property int index
                required property string field
                required property string label
                required property string group
                required property bool included
                width: ListView.view.width
                height: Fonts.listItemHeight
                text: group.length > 0 ? group + " · " + label : label
                checked: included
                Accessible.name: presetParameter.text
                onToggled: {
                    parameterModel.setProperty(index, "included", presetParameter.checked);
                    root.refreshSelectedCount();
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
            objectName: "parameterSelectionCancel"
            text: qsTr("Cancel")
            Accessible.name: qsTr("Cancel")
            onClicked: root.cancelSelection()
        }
        CustomButton {
            objectName: "parameterSelectionAccept"
            text: root.presetMode ? qsTr("Save") : qsTr("Copy")
            enabled: root.canAccept
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            Accessible.name: text
            onClicked: root.acceptSelection()
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
