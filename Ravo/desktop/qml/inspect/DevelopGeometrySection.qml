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
        DevelopCropControls {
            panel: sectionRoot.panel
            visible: !panel.cropPinned
            Layout.fillWidth: true
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
                value: panel.hasPresenter ? (modelData.field === "straighten" ? panel.presenter.develop.editStraighten : panel.presenter.develop.editPerspective[modelData.key]) : 0
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
            checked: panel.hasPresenter && panel.presenter.develop.editPerspective.constrainCrop
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("perspectiveConstrainCrop", checked ? 1 : 0)
        }
        Connections {
            target: panel.presenter ? panel.presenter.develop : null
            function onEditChanged() {
                const constrained = panel.hasPresenter && panel.presenter.develop.editPerspective.constrainCrop;
                if (perspectiveConstrainCropBox.checked !== constrained)
                    perspectiveConstrainCropBox.checked = constrained;
            }
        }
        Connections {
            target: panel.presenter
            function onSelectionChanged() {
                perspectiveConstrainCropBox.checked = panel.hasPresenter && panel.presenter.develop.editPerspective.constrainCrop;
            }
        }
        CustomComboBox {
            objectName: "perspectiveInterpolation"
            Layout.fillWidth: true
            enabled: panel.hasSelection
            model: [qsTr("Bilinear — fast"), qsTr("Lanczos 2"), qsTr("Lanczos 3 — best quality")]
            currentIndex: panel.hasPresenter ? panel.presenter.develop.editPerspective.interpolationIndex : 2
            Accessible.name: qsTr("Perspective interpolation")
            onActivated: function (index) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("perspectiveInterpolationIndex", index);
            }
        }
        CustomCheckBox {
            id: canvasEnabledBox
            objectName: "canvasEnabled"
            text: qsTr("Enlarge Canvas")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.develop.editCanvasEnabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("canvasEnabled", checked ? 1 : 0)
        }
        Connections {
            target: panel.presenter ? panel.presenter.develop : null
            function onEditChanged() {
                const enabled = panel.hasPresenter && panel.presenter.develop.editCanvasEnabled;
                if (canvasEnabledBox.checked !== enabled)
                    canvasEnabledBox.checked = enabled;
            }
        }
        Connections {
            target: panel.presenter
            function onSelectionChanged() {
                canvasEnabledBox.checked = panel.hasPresenter && panel.presenter.develop.editCanvasEnabled;
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
                value: panel.hasPresenter ? panel.presenter.develop.editCanvas[modelData.key] : 0
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
            model: panel.hasPresenter ? panel.presenter.develop.editCanvas.colorChoices : []
            currentIndex: panel.hasPresenter ? panel.presenter.develop.editCanvas.colorIndex : 0
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
