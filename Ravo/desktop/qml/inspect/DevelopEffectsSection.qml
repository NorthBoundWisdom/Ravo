pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Effects")
    sectionId: "effects"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Vignette")
            from: -1
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVignette : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vignette", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vignette", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vignette")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Vignette midpoint")
            from: 0
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0.8
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVignetteParams.midpoint : 0.8
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vignetteMidpoint", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vignetteMidpoint", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vignetteMidpoint")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Vignette feather")
            from: 0.05
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0.5
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVignetteParams.falloff : 0.5
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vignetteFalloff", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vignetteFalloff", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vignetteFalloff")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Vignette roundness")
            from: 0.5
            to: 5
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 1
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVignetteParams.shape : 1
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vignetteShape", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vignetteShape", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vignetteShape")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Vignette center X")
            from: -1
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVignetteParams.centerX : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vignetteCenterX", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vignetteCenterX", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vignetteCenterX")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Vignette center Y")
            from: -1
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVignetteParams.centerY : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vignetteCenterY", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vignetteCenterY", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vignetteCenterY")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Bloom")
            from: 0
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editBloom : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("bloom", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("bloom", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("bloom")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Soften")
            from: 0
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSoften : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("soften", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("soften", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("soften")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Dehaze")
            from: -1
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editDehaze : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("dehaze", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("dehaze", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("dehaze")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Distance")
            from: 0
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0.2
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editDehazeDistance : 0.2
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("dehazeDistance", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("dehazeDistance", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("dehazeDistance")
        }
        CustomCheckBox {
            text: qsTr("Adaptive window scale")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editDehazeAdaptive
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("dehazeAdaptive", checked ? 1 : 0)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Output Dither / Posterize")
            font.bold: true
        }
        CustomCheckBox {
            objectName: "outputDitherEnabled"
            text: qsTr("Enable output dither")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editOutputDither.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("outputDitherEnabled", checked ? 1 : 0)
        }
        CustomComboBox {
            id: outputDitherMethodCombo
            objectName: "outputDitherMethod"
            Layout.fillWidth: true
            enabled: panel.hasSelection
            textRole: "label"
            model: panel.hasPresenter ? panel.presenter.editOutputDither.methodChoices : []
            currentIndex: panel.hasPresenter ? panel.presenter.editOutputDither.methodIndex : 10
            Accessible.name: qsTr("Output dither method")
            onActivated: function (index) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("outputDitherMethodIndex", model[index].index);
            }
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Random damping (dB)")
            from: panel.hasPresenter ? panel.presenter.editOutputDither.dampingMinimum : -200
            to: panel.hasPresenter ? panel.presenter.editOutputDither.dampingMaximum : 0
            stepSize: 0.1
            validatorDecimals: 1
            showReset: true
            resetValue: -100
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editOutputDither.dampingVisible
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editOutputDither.dampingDb : -100
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("outputDitherDamping", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("outputDitherDamping", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("outputDitherDamping")
        }
        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.72
            text: qsTr("Auto dithers integer exports; previews and float output are only clipped.")
        }
        CustomButton {
            text: qsTr("Reset output dither")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("outputDither")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Frame / Border")
            font.bold: true
        }
        CustomCheckBox {
            objectName: "outputFrameEnabled"
            text: qsTr("Enable frame")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editOutputFrame.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("outputFrameEnabled", checked ? 1 : 0)
        }
        RowLayout {
            Layout.fillWidth: true
            CustomComboBox {
                objectName: "outputFrameOrientation"
                Layout.fillWidth: true
                textRole: "label"
                model: panel.hasPresenter ? panel.presenter.editOutputFrame.orientationChoices : []
                currentIndex: panel.hasPresenter ? panel.presenter.editOutputFrame.orientationIndex : 0
                Accessible.name: qsTr("Frame orientation")
                onActivated: function (index) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber("outputFrameOrientationIndex", model[index].index);
                }
            }
            CustomComboBox {
                objectName: "outputFrameBasis"
                Layout.fillWidth: true
                textRole: "label"
                model: panel.hasPresenter ? panel.presenter.editOutputFrame.basisChoices : []
                currentIndex: panel.hasPresenter ? panel.presenter.editOutputFrame.basisIndex : 0
                Accessible.name: qsTr("Frame size basis")
                onActivated: function (index) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber("outputFrameBasisIndex", model[index].index);
                }
            }
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Outer aspect (-1 constant, 0 image)"),
                    "key": "aspect",
                    "field": "outputFrameAspect",
                    "from": -1,
                    "to": 3,
                    "reset": -1
                },
                {
                    "title": qsTr("Border size"),
                    "key": "size",
                    "field": "outputFrameSize",
                    "from": 0,
                    "to": 0.5,
                    "reset": 0.1
                },
                {
                    "title": qsTr("Horizontal position"),
                    "key": "positionH",
                    "field": "outputFramePositionH",
                    "from": 0,
                    "to": 1,
                    "reset": 0.5
                },
                {
                    "title": qsTr("Vertical position"),
                    "key": "positionV",
                    "field": "outputFramePositionV",
                    "from": 0,
                    "to": 1,
                    "reset": 0.5
                },
                {
                    "title": qsTr("Frame line size"),
                    "key": "lineSize",
                    "field": "outputFrameLineSize",
                    "from": 0,
                    "to": 1,
                    "reset": 0
                },
                {
                    "title": qsTr("Frame line offset"),
                    "key": "lineOffset",
                    "field": "outputFrameLineOffset",
                    "from": 0,
                    "to": 1,
                    "reset": 0.5
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.from
                to: modelData.to
                stepSize: 0.01
                validatorDecimals: 2
                showReset: false
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editOutputFrame[modelData.key] : modelData.reset
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
        Repeater {
            model: [
                {
                    "title": qsTr("Border red"),
                    "key": "borderRed",
                    "field": "outputFrameBorderRed",
                    "reset": 1
                },
                {
                    "title": qsTr("Border green"),
                    "key": "borderGreen",
                    "field": "outputFrameBorderGreen",
                    "reset": 1
                },
                {
                    "title": qsTr("Border blue"),
                    "key": "borderBlue",
                    "field": "outputFrameBorderBlue",
                    "reset": 1
                },
                {
                    "title": qsTr("Frame red"),
                    "key": "lineRed",
                    "field": "outputFrameLineRed",
                    "reset": 0
                },
                {
                    "title": qsTr("Frame green"),
                    "key": "lineGreen",
                    "field": "outputFrameLineGreen",
                    "reset": 0
                },
                {
                    "title": qsTr("Frame blue"),
                    "key": "lineBlue",
                    "field": "outputFrameLineBlue",
                    "reset": 0
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                showReset: false
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editOutputFrame[modelData.key] : modelData.reset
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
        CustomButton {
            text: qsTr("Reset frame")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("outputFrame")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Text Watermark")
            font.bold: true
        }
        CustomCheckBox {
            objectName: "watermarkEnabled"
            text: qsTr("Enable watermark")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editWatermark.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("watermarkEnabled", checked ? 1 : 0)
        }
        RowLayout {
            Layout.fillWidth: true
            CustomLabel {
                text: qsTr("Text")
            }
            CustomTextField {
                objectName: "watermarkText"
                Layout.fillWidth: true
                maximumLength: 256
                showEmptyIndicator: false
                showClipIndicator: false
                enabled: panel.hasSelection
                text: panel.hasPresenter ? panel.presenter.editWatermark.text : "RAVO"
                onEditingCommitted: function (committedText) {
                    if (panel.commands)
                        panel.commands.setDevelopText("watermarkText", committedText);
                }
            }
        }
        CustomComboBox {
            objectName: "watermarkAlignment"
            Layout.fillWidth: true
            textRole: "label"
            model: panel.hasPresenter ? panel.presenter.editWatermark.alignmentChoices : []
            currentIndex: panel.hasPresenter ? panel.presenter.editWatermark.alignmentIndex : 8
            Accessible.name: qsTr("Watermark alignment")
            onActivated: function (index) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("watermarkAlignmentIndex", model[index].index);
            }
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Watermark opacity"),
                    "key": "opacity",
                    "field": "watermarkOpacity",
                    "from": 0,
                    "to": 1,
                    "reset": 0.5,
                    "step": 0.01
                },
                {
                    "title": qsTr("Text height (% short side)"),
                    "key": "scale",
                    "field": "watermarkScale",
                    "from": 0.5,
                    "to": 50,
                    "reset": 8,
                    "step": 0.5
                },
                {
                    "title": qsTr("Horizontal offset"),
                    "key": "offsetX",
                    "field": "watermarkOffsetX",
                    "from": -1,
                    "to": 1,
                    "reset": 0,
                    "step": 0.01
                },
                {
                    "title": qsTr("Vertical offset"),
                    "key": "offsetY",
                    "field": "watermarkOffsetY",
                    "from": -1,
                    "to": 1,
                    "reset": 0,
                    "step": 0.01
                },
                {
                    "title": qsTr("Watermark rotation"),
                    "key": "rotation",
                    "field": "watermarkRotation",
                    "from": -180,
                    "to": 180,
                    "reset": 0,
                    "step": 1
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.from
                to: modelData.to
                stepSize: modelData.step
                validatorDecimals: modelData.step < 1 ? 2 : 0
                showReset: false
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editWatermark[modelData.key] : modelData.reset
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
        Repeater {
            model: [
                {
                    "title": qsTr("Watermark red"),
                    "key": "red",
                    "field": "watermarkRed",
                    "reset": 1
                },
                {
                    "title": qsTr("Watermark green"),
                    "key": "green",
                    "field": "watermarkGreen",
                    "reset": 1
                },
                {
                    "title": qsTr("Watermark blue"),
                    "key": "blue",
                    "field": "watermarkBlue",
                    "reset": 1
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                showReset: false
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editWatermark[modelData.key] : modelData.reset
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
        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.72
            text: qsTr("Portable fixed 5×7 text. Supported tokens: {stem}, {asset_id}.")
        }
        CustomButton {
            text: qsTr("Reset watermark")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("watermark")
        }
    }
}
