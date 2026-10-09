pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0
import "../chrome" as Chrome

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
            text: root.panel.localEditing ? qsTr("Editing mask") : qsTr("Local adjustments")
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
            enabled: root.panel.hasSelection && !root.panel.presenter.develop.localDonePending
            onClicked: root.panel.commands.localAdjustment("done", {})
        }
    }
    Chrome.StudioContextMenu {
        id: createMenu
        objectName: "localCreateMenu"
        fitToContent: true
        Chrome.StudioContextMenuItem {
            text: qsTr("Brush")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 8
            })
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Linear gradient")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 2
            })
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Radial gradient")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 4
            })
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Path")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 7
            })
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Luminance / color range")
            onTriggered: root.panel.commands.localAdjustment("create", {
                kind: 5
            })
        }
    }
    Repeater {
        model: root.panel.hasPresenter && root.panel.presenter.develop.localAdjustments !== undefined ? root.panel.presenter.develop.localAdjustments : []
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
                tooltipText: qsTr("Show / hide mask adjustments")
                onClicked: root.panel.commands.localAdjustment("enable", {
                    id: maskRow.modelData.id,
                    enabled: !maskRow.modelData.enabled
                })
            }
            CustomButton {
                text: "⋯"
                onClicked: maskMenu.popup()
            }
            Chrome.StudioContextMenu {
                id: maskMenu
                fitToContent: true
                Chrome.StudioContextMenuItem {
                    text: qsTr("Rename")
                    onTriggered: {
                        renameDialog.maskId = maskRow.modelData.id;
                        renameDialog.initialText = maskRow.modelData.name;
                        renameDialog.open();
                    }
                }
                Chrome.StudioContextMenuItem {
                    text: qsTr("Duplicate")
                    onTriggered: root.panel.commands.localAdjustment("duplicate", {
                        id: maskRow.modelData.id
                    })
                }
                Chrome.StudioContextMenuItem {
                    text: qsTr("Invert mask")
                    onTriggered: root.panel.commands.localAdjustment("invert", {
                        id: maskRow.modelData.id
                    })
                }
                Chrome.StudioContextMenuSeparator {}
                Chrome.StudioContextMenuItem {
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
    DevelopExpander {
        objectName: "localMaskSettings"
        Layout.fillWidth: true
        visible: root.panel.localEditing
        title: qsTr("Mask settings")
        expanded: false
        MaskEditor {
            panel: root.panel
            mask: root.panel.hasPresenter && root.panel.presenter.develop.editLocalMask !== undefined ? root.panel.presenter.develop.editLocalMask : ({})
        }
    }
    RowLayout {
        visible: root.panel.localEditing
        CustomCheckBox {
            text: qsTr("Draw on photo")
            checked: root.panel.hasPresenter && root.panel.presenter.develop.maskDrawingActive
            onClicked: root.panel.commands.localAdjustment("draw", {
                enabled: checked
            })
        }
        CustomCheckBox {
            text: qsTr("Overlay")
            checked: root.panel.hasPresenter && root.panel.presenter.develop.maskOverlayVisible
            onClicked: if (root.panel.hasPresenter)
                root.panel.presenter.develop.setMaskOverlay("local", checked)
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
    Chrome.StudioContextMenu {
        id: componentMenu
        fitToContent: true
        property int combine: 1
        function add(kind) {
            root.panel.commands.localAdjustment("component", {
                id: root.panel.presenter.develop.activeLocalId,
                kind: kind,
                combine: combine
            });
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Brush")
            onTriggered: componentMenu.add(8)
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Linear gradient")
            onTriggered: componentMenu.add(2)
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Radial gradient")
            onTriggered: componentMenu.add(4)
        }
        Chrome.StudioContextMenuItem {
            text: qsTr("Luminance / color range")
            onTriggered: componentMenu.add(5)
        }
    }
    QmlInputDialogPage {
        id: renameDialog
        objectName: "localRenameDialog"
        property string maskId
        dialogTitle: qsTr("Rename mask")
        onTextSubmitted: function (text) {
            root.panel.commands.localAdjustment("rename", {
                id: maskId,
                name: text
            });
        }
    }
}
