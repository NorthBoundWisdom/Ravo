pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Item {
    id: root
    property var panel
    property string operation: "exposure" // "exposure" | "colorBalanceRgb"
    property string objectNamePrefix: operation === "exposure" ? "exposureInstance" : "colorBalanceRgbInstance"

    readonly property bool hasPresenter: panel && panel.hasPresenter
    readonly property var instances: {
        if (!root.hasPresenter)
            return [];
        return root.operation === "exposure" ? root.panel.presenter.exposureInstances : root.panel.presenter.colorBalanceRgbInstances;
    }

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Fonts.size4

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size6
            CustomLabel {
                text: qsTr("Instances")
                font.bold: true
                Layout.fillWidth: true
            }
            CustomButton {
                objectName: root.objectNamePrefix + "Add"
                text: qsTr("Add")
                enabled: root.hasPresenter && root.panel.hasSelection
                onClicked: {
                    if (root.operation === "exposure")
                        root.panel.presenter.addExposureInstance();
                    else
                        root.panel.presenter.addColorBalanceRgbInstance();
                }
            }
            CustomButton {
                objectName: root.objectNamePrefix + "Duplicate"
                text: qsTr("Duplicate")
                enabled: root.hasPresenter && root.panel.hasSelection
                onClicked: {
                    if (root.operation === "exposure")
                        root.panel.presenter.duplicateExposureInstance();
                    else
                        root.panel.presenter.duplicateColorBalanceRgbInstance();
                }
            }
        }

        Repeater {
            model: root.instances
            delegate: RowLayout {
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: Fonts.size4

                CustomButton {
                    objectName: root.objectNamePrefix + "Select" + index
                    Layout.fillWidth: true
                    text: (modelData.name && modelData.name.length) ? modelData.name : modelData.id
                    checked: !!modelData.selected
                    checkable: true
                    enabled: root.hasPresenter && root.panel.hasSelection
                    onClicked: {
                        if (root.operation === "exposure")
                            root.panel.presenter.selectExposureInstance(modelData.id);
                        else
                            root.panel.presenter.selectColorBalanceRgbInstance(modelData.id);
                    }
                }

                CustomCheckBox {
                    objectName: root.objectNamePrefix + "Bypass" + index
                    text: qsTr("Bypass")
                    checked: !!modelData.bypass
                    enabled: root.hasPresenter && root.panel.hasSelection && !modelData.synthetic
                    onToggled: {
                        if (root.operation === "exposure")
                            root.panel.presenter.setExposureInstanceBypass(modelData.id, checked);
                        else
                            root.panel.presenter.setColorBalanceRgbInstanceBypass(modelData.id, checked);
                    }
                }

                CustomButton {
                    objectName: root.objectNamePrefix + "Up" + index
                    text: "↑"
                    enabled: root.hasPresenter && root.panel.hasSelection && index > 0 && !modelData.synthetic
                    onClicked: {
                        if (root.operation === "exposure")
                            root.panel.presenter.reorderExposureInstance(index, index - 1);
                        else
                            root.panel.presenter.reorderColorBalanceRgbInstance(index, index - 1);
                    }
                }
                CustomButton {
                    objectName: root.objectNamePrefix + "Down" + index
                    text: "↓"
                    enabled: root.hasPresenter && root.panel.hasSelection && index + 1 < root.instances.length && !modelData.synthetic
                    onClicked: {
                        if (root.operation === "exposure")
                            root.panel.presenter.reorderExposureInstance(index, index + 1);
                        else
                            root.panel.presenter.reorderColorBalanceRgbInstance(index, index + 1);
                    }
                }
                CustomButton {
                    objectName: root.objectNamePrefix + "Delete" + index
                    text: qsTr("Delete")
                    enabled: root.hasPresenter && root.panel.hasSelection && !modelData.synthetic
                    onClicked: {
                        if (root.operation === "exposure")
                            root.panel.presenter.deleteExposureInstance(modelData.id);
                        else
                            root.panel.presenter.deleteColorBalanceRgbInstance(modelData.id);
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.hasPresenter && root.panel.hasSelection
            spacing: Fonts.size4
            CustomTextField {
                id: renameField
                objectName: root.objectNamePrefix + "Rename"
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                showEmptyIndicator: false
                showClipIndicator: false
                alignRightWhenFocused: false
                placeholderText: qsTr("Rename selected instance")
                text: {
                    if (!root.hasPresenter)
                        return "";
                    const rows = root.instances;
                    for (let i = 0; i < rows.length; ++i) {
                        if (rows[i].selected)
                            return rows[i].name || "";
                    }
                    return "";
                }
            }
            CustomButton {
                objectName: root.objectNamePrefix + "RenameApply"
                text: qsTr("Rename")
                onClicked: {
                    const id = root.operation === "exposure" ? root.panel.presenter.selectedExposureInstanceId : root.panel.presenter.selectedColorBalanceRgbInstanceId;
                    if (root.operation === "exposure")
                        root.panel.presenter.renameExposureInstance(id, renameField.text);
                    else
                        root.panel.presenter.renameColorBalanceRgbInstance(id, renameField.text);
                }
            }
        }
    }
}
