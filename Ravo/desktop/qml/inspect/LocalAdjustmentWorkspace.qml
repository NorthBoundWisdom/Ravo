pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    required property var panel
    Layout.leftMargin: Fonts.standardMargin
    Layout.rightMargin: Fonts.standardMargin
    spacing: Fonts.smallSpacing

    RowLayout {
        Layout.fillWidth: true
        CustomLabel {
            Layout.fillWidth: true
            text: root.panel.localEditing ? qsTr("Editing mask") : qsTr("Global")
            font.bold: true
        }
        CustomButton {
            objectName: "createLocalMask"
            text: qsTr("New mask")
            enabled: root.panel.hasSelection
            onClicked: createMenu.popup()
        }
        CustomButton {
            objectName: "finishLocalMask"
            text: qsTr("Done")
            visible: root.panel.localEditing
            enabled: root.panel.hasSelection && !root.panel.presenter.localDonePending
            onClicked: root.panel.commands.localAdjustment("done", {})
        }
        CustomButton {
            text: "⋯"
            visible: !root.panel.localEditing
            onClicked: globalMenu.popup()
        }
    }
    Menu {
        id: createMenu
        MenuItem {
            text: qsTr("Brush")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 8
            })
        }
        MenuItem {
            text: qsTr("Linear gradient")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 2
            })
        }
        MenuItem {
            text: qsTr("Radial gradient")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 4
            })
        }
        MenuItem {
            text: qsTr("Path")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 7
            })
        }
        MenuItem {
            text: qsTr("Luminance / color range")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 5
            })
        }
    }
    Menu {
        id: globalMenu
        MenuItem {
            text: qsTr("Advanced operation instances")
            checkable: true
            checked: root.panel.showAdvancedInstances
            onTriggered: root.panel.showAdvancedInstances = checked
        }
    }
    Repeater {
        model: root.panel.hasPresenter && root.panel.presenter.localAdjustments !== undefined ? root.panel.presenter.localAdjustments : []
        delegate: RowLayout {
            id: maskRow
            required property var modelData
            Layout.fillWidth: true
            CustomButton {
                objectName: "selectLocalMask_" + maskRow.modelData.id
                Layout.fillWidth: true
                text: maskRow.modelData.name
                checkable: true
                checked: maskRow.modelData.selected
                onClicked: root.panel.commands.localAdjustment("select", {
                    id: maskRow.modelData.id
                })
            }
            CustomButton {
                text: maskRow.modelData.enabled ? "◉" : "○"
                ToolTip.text: qsTr("Show / hide mask adjustments")
                ToolTip.visible: hovered
                onClicked: root.panel.commands.localAdjustment("enable", {
                    id: maskRow.modelData.id,
                    enabled: !maskRow.modelData.enabled
                })
            }
            CustomButton {
                text: "⋯"
                onClicked: maskMenu.popup()
            }
            Menu {
                id: maskMenu
                MenuItem {
                    text: qsTr("Rename")
                    onTriggered: {
                        renameDialog.maskId = maskRow.modelData.id;
                        renameText.text = maskRow.modelData.name;
                        renameDialog.open();
                    }
                }
                MenuItem {
                    text: qsTr("Duplicate")
                    onTriggered: root.panel.commands.localAdjustment("duplicate", {
                        id: maskRow.modelData.id
                    })
                }
                MenuItem {
                    text: qsTr("Invert mask")
                    onTriggered: root.panel.commands.localAdjustment("invert", {
                        id: maskRow.modelData.id
                    })
                }
                MenuSeparator {}
                MenuItem {
                    text: qsTr("Delete")
                    onTriggered: root.panel.commands.localAdjustment("delete", {
                        id: maskRow.modelData.id
                    })
                }
            }
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: root.panel.localEditing
        text: qsTr("Adjustments below affect only the selected mask.")
        wrapMode: Text.WordWrap
        opacity: 0.75
    }
    Expander {
        Layout.fillWidth: true
        visible: root.panel.localEditing
        title: qsTr("Mask settings")
        expanded: false
        MaskEditor {
            panel: root.panel
            mask: root.panel.hasPresenter && root.panel.presenter.editLocalMask !== undefined ? root.panel.presenter.editLocalMask : ({})
        }
    }
    RowLayout {
        visible: root.panel.localEditing
        CustomCheckBox {
            text: qsTr("Draw on photo")
            checked: root.panel.hasPresenter && root.panel.presenter.maskDrawingActive
            onClicked: root.panel.commands.localAdjustment("draw", {
                enabled: checked
            })
        }
        CustomCheckBox {
            text: qsTr("Overlay")
            checked: root.panel.hasPresenter && root.panel.presenter.maskOverlayVisible
            onClicked: if (root.panel.hasPresenter)
                root.panel.presenter.setMaskOverlay("local", checked)
        }
    }
    RowLayout {
        visible: root.panel.localEditing
        CustomButton {
            text: qsTr("Add")
            onClicked: {
                componentMenu.combine = 1;
                componentMenu.popup();
            }
        }
        CustomButton {
            text: qsTr("Subtract")
            onClicked: {
                componentMenu.combine = 3;
                componentMenu.popup();
            }
        }
        CustomButton {
            text: qsTr("Intersect")
            onClicked: {
                componentMenu.combine = 2;
                componentMenu.popup();
            }
        }
    }
    Menu {
        id: componentMenu
        property int combine: 1
        function add(kind) {
            root.panel.commands.localAdjustment("component", {
                id: root.panel.presenter.activeLocalId,
                kind: kind,
                combine: combine
            });
        }
        MenuItem {
            text: qsTr("Brush")
            onTriggered: componentMenu.add(8)
        }
        MenuItem {
            text: qsTr("Linear gradient")
            onTriggered: componentMenu.add(2)
        }
        MenuItem {
            text: qsTr("Radial gradient")
            onTriggered: componentMenu.add(4)
        }
        MenuItem {
            text: qsTr("Luminance / color range")
            onTriggered: componentMenu.add(5)
        }
    }
    Dialog {
        id: renameDialog
        property string maskId
        title: qsTr("Rename mask")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.panel.commands.localAdjustment("rename", {
            id: maskId,
            name: renameText.text
        })
        TextField {
            id: renameText
            selectByMouse: true
        }
    }
}
