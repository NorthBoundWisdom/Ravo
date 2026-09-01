pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Geometry")
    sectionId: "geometry"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        spacing: Fonts.smallSpacing
        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size6
            CustomButton {
                display: AbstractButton.IconOnly
                icon.source: "qrc:/GeoControls/icons/RotateCcw.svg"
                tooltipText: qsTr("Rotate Left")
                enabled: panel.hasSelection
                implicitWidth: Fonts.iconButtonSize
                implicitHeight: Fonts.iconButtonSize
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                Layout.fillWidth: true
                defaultPadding: 0
                onClicked: if (panel.commands)
                    panel.commands.rotateLeft.trigger()
            }
            CustomButton {
                display: AbstractButton.IconOnly
                icon.source: "qrc:/GeoControls/icons/RotateCw.svg"
                tooltipText: qsTr("Rotate Right")
                enabled: panel.hasSelection
                implicitWidth: Fonts.iconButtonSize
                implicitHeight: Fonts.iconButtonSize
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                Layout.fillWidth: true
                defaultPadding: 0
                onClicked: if (panel.commands)
                    panel.commands.rotateRight.trigger()
            }
            CustomButton {
                display: AbstractButton.IconOnly
                icon.source: "qrc:/GeoControls/icons/FlipHorizontal.svg"
                tooltipText: qsTr("Flip Horizontal")
                enabled: panel.hasSelection
                implicitWidth: Fonts.iconButtonSize
                implicitHeight: Fonts.iconButtonSize
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                Layout.fillWidth: true
                defaultPadding: 0
                onClicked: if (panel.commands)
                    panel.commands.flipHorizontal.trigger()
            }
            CustomButton {
                display: AbstractButton.IconOnly
                icon.source: "qrc:/GeoControls/icons/FlipVertical.svg"
                tooltipText: qsTr("Flip Vertical")
                enabled: panel.hasSelection
                implicitWidth: Fonts.iconButtonSize
                implicitHeight: Fonts.iconButtonSize
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                Layout.fillWidth: true
                defaultPadding: 0
                onClicked: if (panel.commands)
                    panel.commands.flipVertical.trigger()
            }
        }
        CustomButton {
            Layout.fillWidth: true
            text: panel.hasPresenter && panel.presenter.cropToolActive ? qsTr("Done") : qsTr("Crop & Rotate")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.toggleCropTool()
        }
        CustomLabel {
            text: qsTr("Drag the frame to crop. Drag outside it, or Option/Alt-drag, to straighten.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            opacity: 0.75
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size6
            Repeater {
                model: [
                    {
                        "label": qsTr("Auto"),
                        "mode": "full"
                    },
                    {
                        "label": qsTr("Vertical"),
                        "mode": "vertical"
                    },
                    {
                        "label": qsTr("Horizontal"),
                        "mode": "horizontal"
                    }
                ]
                delegate: CustomButton {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.label
                    enabled: panel.hasSelection
                    tooltipText: qsTr("Analyze visible lines and apply a bounded perspective correction")
                    onClicked: if (panel.commands)
                        panel.commands.autoPerspective(modelData.mode)
                }
            }
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Angle"),
                    "key": "angle",
                    "field": "straighten",
                    "minimum": -45,
                    "maximum": 45,
                    "step": 0.1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Vertical"),
                    "key": "vertical",
                    "field": "perspectiveVertical",
                    "minimum": -2,
                    "maximum": 2,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Horizontal"),
                    "key": "horizontal",
                    "field": "perspectiveHorizontal",
                    "minimum": -2,
                    "maximum": 2,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Shear"),
                    "key": "shear",
                    "field": "perspectiveShear",
                    "minimum": -0.5,
                    "maximum": 0.5,
                    "step": 0.005,
                    "decimals": 3
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: 0
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? (modelData.field === "straighten" ? panel.presenter.editStraighten : panel.presenter.editPerspective[modelData.key]) : 0
                onValueEdited: function (value) {
                    if (panel.liveReady && panel.commands)
                        panel.commands.previewDevelopNumber(modelData.field, value);
                }
                onValueCommitted: function (value) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber(modelData.field, value);
                }
                onResetRequested: if (panel.commands)
                    panel.commands.resetControl(modelData.field)
            }
        }
        CustomCheckBox {
            id: perspectiveConstrainCropBox
            objectName: "perspectiveConstrainCrop"
            text: qsTr("Constrain crop")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editPerspective.constrainCrop
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("perspectiveConstrainCrop", checked ? 1 : 0)
        }
        Connections {
            target: panel.presenter
            function onEditChanged() {
                const constrained = panel.hasPresenter && panel.presenter.editPerspective.constrainCrop;
                if (perspectiveConstrainCropBox.checked !== constrained)
                    perspectiveConstrainCropBox.checked = constrained;
            }
            function onSelectionChanged() {
                perspectiveConstrainCropBox.checked = panel.hasPresenter && panel.presenter.editPerspective.constrainCrop;
            }
        }
        CustomComboBox {
            objectName: "perspectiveInterpolation"
            Layout.fillWidth: true
            enabled: panel.hasSelection
            model: [qsTr("Bilinear — fast"), qsTr("Lanczos 2"), qsTr("Lanczos 3 — best quality")]
            currentIndex: panel.hasPresenter ? panel.presenter.editPerspective.interpolationIndex : 2
            Accessible.name: qsTr("Perspective interpolation")
            onActivated: function (index) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("perspectiveInterpolationIndex", index);
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size6
            CustomComboBox {
                Layout.fillWidth: true
                model: ["free", "1:1", "3:2", "4:3", "5:4", "16:9"]
                enabled: panel.hasSelection
                displayText: panel.hasPresenter && panel.presenter.cropAspect === "locked" ? qsTr("Custom") : currentText
                currentIndex: {
                    const aspects = ["free", "1:1", "3:2", "4:3", "5:4", "16:9"];
                    const current = panel.hasPresenter ? panel.presenter.cropAspect : "free";
                    return aspects.indexOf(current);
                }
                onActivated: if (panel.commands)
                    panel.commands.setCropAspect(currentText)
            }
            CustomButton {
                display: AbstractButton.IconOnly
                checkable: true
                checked: panel.hasPresenter && panel.presenter.cropAspect !== "free"
                icon.source: checked ? "qrc:/GeoControls/icons/Lock.svg" : "qrc:/GeoControls/icons/Unlock.svg"
                tooltipText: checked ? qsTr("Unlock aspect ratio") : qsTr("Lock aspect ratio")
                enabled: panel.hasSelection
                implicitWidth: Fonts.iconButtonSize
                implicitHeight: Fonts.iconButtonSize
                Layout.preferredWidth: implicitWidth
                Layout.preferredHeight: implicitHeight
                defaultPadding: 0
                onToggled: if (panel.commands)
                    panel.commands.setCropAspect(checked ? "locked" : "free")
            }
        }
        CustomCheckBox {
            id: canvasEnabledBox
            objectName: "canvasEnabled"
            text: qsTr("Enlarge Canvas")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editCanvasEnabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("canvasEnabled", checked ? 1 : 0)
        }
        Connections {
            target: panel.presenter
            function onEditChanged() {
                const enabled = panel.hasPresenter && panel.presenter.editCanvasEnabled;
                if (canvasEnabledBox.checked !== enabled)
                    canvasEnabledBox.checked = enabled;
            }
            function onSelectionChanged() {
                canvasEnabledBox.checked = panel.hasPresenter && panel.presenter.editCanvasEnabled;
            }
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Canvas left (%)"),
                    "key": "left",
                    "field": "canvasLeft"
                },
                {
                    "title": qsTr("Canvas right (%)"),
                    "key": "right",
                    "field": "canvasRight"
                },
                {
                    "title": qsTr("Canvas top (%)"),
                    "key": "top",
                    "field": "canvasTop"
                },
                {
                    "title": qsTr("Canvas bottom (%)"),
                    "key": "bottom",
                    "field": "canvasBottom"
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: visible ? implicitHeight : 0
                Layout.maximumHeight: visible ? 65535 : 0
                visible: canvasEnabledBox.checked
                title: modelData.title
                from: 0
                to: 100
                stepSize: 0.1
                validatorDecimals: 1
                showReset: false
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editCanvas[modelData.key] : 0
                onValueEdited: function (value) {
                    if (panel.liveReady && panel.commands)
                        panel.commands.previewDevelopNumber(modelData.field, value);
                }
                onValueCommitted: function (value) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber(modelData.field, value);
                }
            }
        }
        CustomComboBox {
            objectName: "canvasColor"
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            Layout.maximumHeight: visible ? 65535 : 0
            visible: canvasEnabledBox.checked
            enabled: panel.hasSelection
            textRole: "label"
            model: panel.hasPresenter ? panel.presenter.editCanvas.colorChoices : []
            currentIndex: panel.hasPresenter ? panel.presenter.editCanvas.colorIndex : 0
            Accessible.name: qsTr("Canvas color")
            onActivated: function (index) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("canvasColorIndex", model[index].index);
            }
        }
        CustomButton {
            Layout.preferredHeight: visible ? implicitHeight : 0
            Layout.maximumHeight: visible ? 65535 : 0
            visible: canvasEnabledBox.checked
            text: qsTr("Reset canvas")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("canvas")
        }
    }
}
