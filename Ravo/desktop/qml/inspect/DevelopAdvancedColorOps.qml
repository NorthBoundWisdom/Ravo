pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: groupRoot
    required property var panel
    Layout.fillWidth: true
    Expander {
        Layout.fillWidth: true
        title: qsTr("Color · Advanced")
        expanded: false
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color look-up table · D50 Lab")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomCheckBox {
            text: qsTr("Enable color look-up table")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editColorChecker.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("colorCheckerEnabled", checked ? 1 : 0)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("IT8 skin tones"), qsTr("Expanded color checker"), qsTr("Helmholtz/Kohlrausch monochrome"), qsTr("Fuji Astia emulation"), qsTr("Fuji Classic Chrome emulation"), qsTr("Fuji Monochrome emulation"), qsTr("Fuji Provia emulation"), qsTr("Fuji Velvia emulation")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editColorChecker.presetIndex : -1
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("colorCheckerPreset", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: {
                const labels = [];
                const count = panel.hasPresenter ? panel.presenter.editColorChecker.patchCount : 0;
                for (let index = 0; index < count; ++index)
                    labels.push(qsTr("Patch %1").arg(index + 1));
                return labels;
            }
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editColorChecker.patchCount > 0
            currentIndex: panel.hasPresenter ? panel.presenter.editColorChecker.patchIndex : -1
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("colorCheckerPatch", currentIndex)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Source · L*"),
                    "key": "sourceL",
                    "field": "colorCheckerSourceL"
                },
                {
                    "title": qsTr("Source · a*"),
                    "key": "sourceA",
                    "field": "colorCheckerSourceA"
                },
                {
                    "title": qsTr("Source · b*"),
                    "key": "sourceB",
                    "field": "colorCheckerSourceB"
                },
                {
                    "title": qsTr("Target · L*"),
                    "key": "targetL",
                    "field": "colorCheckerTargetL"
                },
                {
                    "title": qsTr("Target · a*"),
                    "key": "targetA",
                    "field": "colorCheckerTargetA"
                },
                {
                    "title": qsTr("Target · b*"),
                    "key": "targetB",
                    "field": "colorCheckerTargetB"
                }
            ]
            delegate: ColorCheckerNumberField {
                panel: groupRoot.panel
            }
        }
        CustomButton {
            text: qsTr("Disable and reset color look-up table")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("colorChecker")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color Balance · legacy Lab / ProPhoto RGB")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomLabel {
            Layout.fillWidth: true
            text: panel.hasPresenter && panel.presenter.editLegacyColorBalance.enabled ? qsTr("Enabled") : qsTr("Inactive until edited")
            opacity: 0.75
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Lift / Gamma / Gain"), qsTr("Slope / Offset / Power")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editLegacyColorBalance.modeIndex : 1
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("legacyColorBalanceMode", currentIndex)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Lift · Factor"),
                    "key": "liftFactor",
                    "field": "legacyColorBalanceLiftFactor",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Lift · Red"),
                    "key": "liftRed",
                    "field": "legacyColorBalanceLiftRed",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Lift · Green"),
                    "key": "liftGreen",
                    "field": "legacyColorBalanceLiftGreen",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Lift · Blue"),
                    "key": "liftBlue",
                    "field": "legacyColorBalanceLiftBlue",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Gamma · Factor"),
                    "key": "gammaFactor",
                    "field": "legacyColorBalanceGammaFactor",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Gamma · Red"),
                    "key": "gammaRed",
                    "field": "legacyColorBalanceGammaRed",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Gamma · Green"),
                    "key": "gammaGreen",
                    "field": "legacyColorBalanceGammaGreen",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Gamma · Blue"),
                    "key": "gammaBlue",
                    "field": "legacyColorBalanceGammaBlue",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Gain · Factor"),
                    "key": "gainFactor",
                    "field": "legacyColorBalanceGainFactor",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Gain · Red"),
                    "key": "gainRed",
                    "field": "legacyColorBalanceGainRed",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Gain · Green"),
                    "key": "gainGreen",
                    "field": "legacyColorBalanceGainGreen",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Gain · Blue"),
                    "key": "gainBlue",
                    "field": "legacyColorBalanceGainBlue",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.00001,
                    "decimals": 5
                },
                {
                    "title": qsTr("Input saturation"),
                    "key": "inputSaturation",
                    "field": "legacyColorBalanceInputSaturation",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Contrast"),
                    "key": "contrast",
                    "field": "legacyColorBalanceContrast",
                    "minimum": 0.01,
                    "maximum": 1.99,
                    "reset": 1,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Contrast fulcrum (%)"),
                    "key": "greyFulcrum",
                    "field": "legacyColorBalanceGreyFulcrum",
                    "minimum": 0.1,
                    "maximum": 100,
                    "reset": 18,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Output saturation"),
                    "key": "outputSaturation",
                    "field": "legacyColorBalanceOutputSaturation",
                    "minimum": 0,
                    "maximum": 2,
                    "reset": 1,
                    "step": 0.0001,
                    "decimals": 4
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
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editLegacyColorBalance[modelData.key] : modelData.reset
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
        CustomButton {
            text: qsTr("Disable and reset legacy Color Balance")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("legacyColorBalance")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color Correction · D50 Lab")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomCheckBox {
            objectName: "colorCorrectionEnabled"
            text: qsTr("Enable Color Correction")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editColorCorrection.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("colorCorrectionEnabled", checked ? 1 : 0)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Highlights · a*"),
                    "key": "highlightA",
                    "field": "colorCorrectionHighlightA",
                    "minimum": -40,
                    "maximum": 40,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Highlights · b*"),
                    "key": "highlightB",
                    "field": "colorCorrectionHighlightB",
                    "minimum": -40,
                    "maximum": 40,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Shadows · a*"),
                    "key": "shadowA",
                    "field": "colorCorrectionShadowA",
                    "minimum": -40,
                    "maximum": 40,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Shadows · b*"),
                    "key": "shadowB",
                    "field": "colorCorrectionShadowB",
                    "minimum": -40,
                    "maximum": 40,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Saturation"),
                    "key": "saturation",
                    "field": "colorCorrectionSaturation",
                    "minimum": -3,
                    "maximum": 3,
                    "reset": 1,
                    "step": 0.01,
                    "decimals": 2
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
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editColorCorrection[modelData.key] : modelData.reset
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
        CustomButton {
            text: qsTr("Disable and reset Color Correction")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("colorCorrection")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color contrast")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomCheckBox {
            objectName: "colorContrastEnabled"
            text: qsTr("Enable Color contrast")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editColorContrast.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("colorContrastEnabled", checked ? 1 : 0)
        }
        Repeater {
            model: [
                {
                    "title": "a* ×",
                    "key": "aSteepness",
                    "field": "colorContrastASteepness",
                    "minimum": 0,
                    "maximum": 5,
                    "reset": 1
                },
                {
                    "title": "b* ×",
                    "key": "bSteepness",
                    "field": "colorContrastBSteepness",
                    "minimum": 0,
                    "maximum": 5,
                    "reset": 1
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: 0.01
                validatorDecimals: 3
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editColorContrast[modelData.key] : modelData.reset
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
        Repeater {
            model: [
                {
                    "title": "a* +",
                    "key": "aOffset",
                    "field": "colorContrastAOffset"
                },
                {
                    "title": "b* +",
                    "key": "bOffset",
                    "field": "colorContrastBOffset"
                }
            ]
            delegate: ColorContrastOffsetField {
                panel: groupRoot.panel
            }
        }
        RowLayout {
            Layout.fillWidth: true
            CustomCheckBox {
                Layout.fillWidth: true
                text: qsTr("Allow extended chroma")
                enabled: panel.hasSelection
                checked: panel.hasPresenter && panel.presenter.editColorContrast.unbound
                onToggled: if (panel.liveReady && panel.commands)
                    panel.commands.setDevelopNumber("colorContrastUnbound", checked ? 1 : 0)
            }
            CustomButton {
                text: qsTr("Reset")
                enabled: panel.hasSelection
                onClicked: if (panel.commands)
                    panel.commands.resetControl("colorContrastUnbound")
            }
        }
        CustomButton {
            text: qsTr("Disable and reset Color contrast")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("colorContrast")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color Harmonizer")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomCheckBox {
            objectName: "colorHarmonizerEnabled"
            text: qsTr("Enable Color Harmonizer")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editColorHarmonizer.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("colorHarmonizerEnabled", checked ? 1 : 0)
        }
        CustomComboBox {
            objectName: "colorHarmonizerRuleIndex"
            Layout.fillWidth: true
            model: panel.hasPresenter ? panel.presenter.editColorHarmonizer.ruleChoices : []
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editColorHarmonizer.ruleIndex : 3
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("colorHarmonizerRuleIndex", currentIndex)
        }
        Repeater {
            model: panel.hasPresenter ? panel.presenter.editColorHarmonizer.sharedControls : []
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                visible: modelData.visible
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editColorHarmonizer[modelData.key] : modelData.reset
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
        CustomSlider {
            Layout.fillWidth: true
            readonly property var nodeControl: panel.hasPresenter ? panel.presenter.editColorHarmonizer.customNodeControl : ({})
            title: nodeControl.title !== undefined ? nodeControl.title : qsTr("Custom nodes")
            from: nodeControl.minimum !== undefined ? nodeControl.minimum : 2
            to: nodeControl.maximum !== undefined ? nodeControl.maximum : 4
            stepSize: nodeControl.step !== undefined ? nodeControl.step : 1
            validatorDecimals: nodeControl.decimals !== undefined ? nodeControl.decimals : 0
            showReset: true
            resetValue: nodeControl.reset !== undefined ? nodeControl.reset : 4
            delayedCommit: true
            visible: panel.hasPresenter ? nodeControl.visible : false
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editColorHarmonizer.customRule
            value: panel.hasPresenter ? panel.presenter.editColorHarmonizer.customNodeCount : 4
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber(nodeControl.field, value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber(nodeControl.field, value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl(nodeControl.field)
        }
        Repeater {
            model: panel.hasPresenter ? panel.presenter.editColorHarmonizer.customHueControls : []
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                visible: modelData.visible
                enabled: panel.hasSelection && modelData.visible
                value: panel.hasPresenter ? panel.presenter.editColorHarmonizer[modelData.key] : modelData.reset
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
        Repeater {
            model: panel.hasPresenter ? panel.presenter.editColorHarmonizer.nodeSaturationControls : []
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                visible: modelData.visible
                enabled: panel.hasSelection && modelData.visible
                value: panel.hasPresenter ? panel.presenter.editColorHarmonizer[modelData.key] : modelData.reset
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
        CustomButton {
            text: qsTr("Disable and reset Color Harmonizer")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("colorHarmonizer")
        }
        MaskEditor {
            panel: groupRoot.panel
            objectName: "colorHarmonizerMaskEditor"
            mask: panel.hasPresenter ? panel.presenter.editColorHarmonizerMask : ({})
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color Reconstruction")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomCheckBox {
            objectName: "colorReconstructionEnabled"
            text: qsTr("Enable Color Reconstruction")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editColorReconstruction.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("colorReconstructionEnabled", checked ? 1 : 0)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Precedence")
            wrapMode: Text.WordWrap
        }
        CustomComboBox {
            objectName: "colorReconstructionPrecedence"
            Layout.fillWidth: true
            model: panel.hasPresenter ? panel.presenter.editColorReconstruction.precedenceChoices : []
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editColorReconstruction.precedenceIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("colorReconstructionPrecedenceIndex", currentIndex)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Threshold"),
                    "key": "threshold",
                    "field": "colorReconstructionThreshold",
                    "minimum": 50,
                    "maximum": 150,
                    "reset": 100,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Spatial extent"),
                    "key": "spatial",
                    "field": "colorReconstructionSpatial",
                    "minimum": 0,
                    "maximum": 1000,
                    "reset": 400,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Range extent"),
                    "key": "range",
                    "field": "colorReconstructionRange",
                    "minimum": 0,
                    "maximum": 50,
                    "reset": 10,
                    "step": 0.1,
                    "decimals": 1
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
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editColorReconstruction[modelData.key] : modelData.reset
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
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Hue")
            from: 0
            to: 360
            stepSize: 0.1
            validatorDecimals: 1
            showReset: true
            resetValue: 237.6
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editColorReconstruction.precedenceIndex === 2
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editColorReconstruction.hueDegrees : 237.6
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("colorReconstructionHueDegrees", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("colorReconstructionHueDegrees", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("colorReconstructionHueDegrees")
        }
        CustomButton {
            text: qsTr("Disable and reset Color Reconstruction")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("colorReconstruction")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color Zones")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomCheckBox {
            objectName: "colorZonesEnabled"
            text: qsTr("Enable Color Zones")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editColorZones.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("colorZonesEnabled", checked ? 1 : 0)
        }
        RowLayout {
            Layout.fillWidth: true
            CustomComboBox {
                objectName: "colorZonesSelectBy"
                Layout.fillWidth: true
                textRole: "label"
                model: panel.hasPresenter ? panel.presenter.editColorZones.selectByChoices : []
                currentIndex: panel.hasPresenter ? panel.presenter.editColorZones.selectByIndex : 2
                Accessible.name: qsTr("Color Zones select by")
                onActivated: function (index) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber("colorZonesSelectByIndex", model[index].index);
                }
            }
            CustomComboBox {
                objectName: "colorZonesBand"
                Layout.fillWidth: true
                textRole: "label"
                model: panel.hasPresenter ? panel.presenter.editColorZones.bandChoices : []
                currentIndex: panel.hasPresenter ? panel.presenter.editColorZones.bandIndex : 0
                Accessible.name: qsTr("Color Zones band")
                onActivated: function (index) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber("colorZonesBandIndex", model[index].index);
                }
            }
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Color Zones mix")
            from: -200
            to: 200
            stepSize: 1
            validatorDecimals: 0
            showReset: false
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editColorZones.strength : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("colorZonesStrength", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("colorZonesStrength", value);
            }
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Lightness curve"),
                    "key": "lightness",
                    "field": "colorZonesLightness"
                },
                {
                    "title": qsTr("Chroma curve"),
                    "key": "chroma",
                    "field": "colorZonesChroma"
                },
                {
                    "title": qsTr("Hue curve"),
                    "key": "hue",
                    "field": "colorZonesHue"
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
                enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editColorZones.editable
                value: panel.hasPresenter ? panel.presenter.editColorZones[modelData.key] : 0.5
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
                    "title": qsTr("Lightness interpolation"),
                    "key": "lightnessInterpolationIndex",
                    "field": "colorZonesLightnessInterpolationIndex"
                },
                {
                    "title": qsTr("Chroma interpolation"),
                    "key": "chromaInterpolationIndex",
                    "field": "colorZonesChromaInterpolationIndex"
                },
                {
                    "title": qsTr("Hue interpolation"),
                    "key": "hueInterpolationIndex",
                    "field": "colorZonesHueInterpolationIndex"
                }
            ]
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                CustomLabel {
                    Layout.fillWidth: true
                    text: modelData.title
                }
                CustomComboBox {
                    Layout.preferredWidth: Fonts.standardFontMetrics.averageCharacterWidth * 20
                    textRole: "label"
                    model: panel.hasPresenter ? panel.presenter.editColorZones.interpolationChoices : []
                    currentIndex: panel.hasPresenter ? panel.presenter.editColorZones[modelData.key] : 1
                    enabled: panel.hasSelection
                    onActivated: function (index) {
                        if (panel.commands)
                            panel.commands.setDevelopNumber(modelData.field, model[index].index);
                    }
                }
            }
        }
        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.72
            visible: panel.hasPresenter && (!panel.presenter.editColorZones.editable || panel.presenter.editColorZones.masked)
            text: panel.hasPresenter && panel.presenter.editColorZones.masked ? qsTr("Loaded Color Zones mask is preserved but edited outside this panel.") : qsTr("Loaded custom-node curves are preserved; reset Color Zones to use the eight-band editor.")
        }
        CustomButton {
            text: qsTr("Disable and reset Color Zones")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("colorZones")
        }
    }
}
