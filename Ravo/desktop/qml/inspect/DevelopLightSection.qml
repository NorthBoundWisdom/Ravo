pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Light")
    sectionId: "light"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Exposure")
            from: -3
            to: 4
            stepSize: 0.001
            validatorDecimals: 3
            showReset: true
            resetValue: 0
            delayedCommit: true
            visible: !panel.hasPresenter || panel.presenter.editExposureParams.modeIndex === 0
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editExposureParams.exposureEv : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("exposure", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("exposure", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("exposure")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Contrast")
            from: 0.7
            to: 3
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 1.5
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editSigmoidEnabled
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSigmoidContrast : 1.5
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("sigmoidContrast", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("sigmoidContrast", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("sigmoidContrast")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Contrast")
            from: -1
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            visible: panel.hasPresenter && !panel.presenter.editSigmoidEnabled
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editContrast : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("contrast", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("contrast", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("contrast")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Highlights")
            from: -1
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editHighlights : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("highlights", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("highlights", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("highlights")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Shadows")
            from: -1
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editShadows : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("shadows", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("shadows", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("shadows")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Whites")
            from: -1
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editWhites : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("whites", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("whites", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("whites")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Blacks")
            from: -0.1
            to: 0.1
            stepSize: 0.001
            validatorDecimals: 3
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editBlacks : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("blacks", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("blacks", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("blacks")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Exposure mode")
            font.bold: true
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Manual"), qsTr("Deflicker")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editExposureParams.modeIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("exposureMode", currentIndex)
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Exposure black")
            from: -0.1
            to: 0.1
            stepSize: 0.0001
            validatorDecimals: 4
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editExposureParams.black : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("exposureBlack", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("exposureBlack", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("exposureBlack")
        }
        CustomCheckBox {
            text: qsTr("Compensate exposure bias")
            visible: !panel.hasPresenter || panel.presenter.editExposureParams.modeIndex === 0
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editExposureParams.compensateExposureBias
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("exposureCompensateBias", checked ? 1 : 0)
        }
        CustomCheckBox {
            text: qsTr("Compensate highlight preservation")
            visible: !panel.hasPresenter || panel.presenter.editExposureParams.modeIndex === 0
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editExposureParams.compensateHighlightPreservation
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("exposureCompensateHighlight", checked ? 1 : 0)
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Deflicker percentile")
            from: 0
            to: 100
            stepSize: 0.1
            validatorDecimals: 1
            showReset: true
            resetValue: 50
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editExposureParams.modeIndex === 1
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editExposureParams.deflickerPercentile : 50
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("exposureDeflickerPercentile", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("exposureDeflickerPercentile", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("exposureDeflickerPercentile")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Deflicker target EV")
            from: -18
            to: 18
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: -4
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editExposureParams.modeIndex === 1
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editExposureParams.deflickerTargetEv : -4
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("exposureDeflickerTarget", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("exposureDeflickerTarget", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("exposureDeflickerTarget")
        }
        CustomLabel {
            Layout.fillWidth: true
            visible: panel.hasPresenter && panel.presenter.editSigmoidEnabled
            text: qsTr("Sigmoid Display · Standard SDR")
            font.bold: true
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Skew")
            from: -1
            to: 1
            stepSize: 0.02
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editSigmoidEnabled
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSigmoidSkew : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("sigmoidSkew", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("sigmoidSkew", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("sigmoidSkew")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Preserve Hue")
            from: 0
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 1
            delayedCommit: true
            visible: panel.hasPresenter && panel.presenter.editSigmoidEnabled
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSigmoidHuePreservation : 1
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("sigmoidHuePreservation", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("sigmoidHuePreservation", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("sigmoidHuePreservation")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Gamma")
            from: 0.2
            to: 3
            showReset: true
            resetValue: 1
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editGamma : 1
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("gamma", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("gamma", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("gamma")
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("RGB levels")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("RGB, linked"), qsTr("RGB, independent")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editRgbLevels.modeIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("rgbLevelsMode", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            visible: !panel.hasPresenter || panel.presenter.editRgbLevels.modeIndex === 0
            model: [qsTr("None"), qsTr("Luminance"), qsTr("Max RGB"), qsTr("Average RGB"), qsTr("Sum RGB"), qsTr("Norm RGB"), qsTr("Basic power")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editRgbLevels.preserveIndex : 1
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("rgbLevelsPreserve", currentIndex)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Black"),
                    "key": "black",
                    "field": "rgbLevelsBlack",
                    "reset": 0
                },
                {
                    "title": qsTr("Grey"),
                    "key": "grey",
                    "field": "rgbLevelsGrey",
                    "reset": 0.5
                },
                {
                    "title": qsTr("White"),
                    "key": "white",
                    "field": "rgbLevelsWhite",
                    "reset": 1
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: 0
                to: 1
                stepSize: 0.001
                validatorDecimals: 3
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editRgbLevels[modelData.key] : modelData.reset
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
                    "title": qsTr("Green black"),
                    "key": "blackG",
                    "field": "rgbLevelsBlackG",
                    "reset": 0
                },
                {
                    "title": qsTr("Green grey"),
                    "key": "greyG",
                    "field": "rgbLevelsGreyG",
                    "reset": 0.5
                },
                {
                    "title": qsTr("Green white"),
                    "key": "whiteG",
                    "field": "rgbLevelsWhiteG",
                    "reset": 1
                },
                {
                    "title": qsTr("Blue black"),
                    "key": "blackB",
                    "field": "rgbLevelsBlackB",
                    "reset": 0
                },
                {
                    "title": qsTr("Blue grey"),
                    "key": "greyB",
                    "field": "rgbLevelsGreyB",
                    "reset": 0.5
                },
                {
                    "title": qsTr("Blue white"),
                    "key": "whiteB",
                    "field": "rgbLevelsWhiteB",
                    "reset": 1
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                visible: panel.hasPresenter && panel.presenter.editRgbLevels.modeIndex === 1
                title: modelData.title
                from: 0
                to: 1
                stepSize: 0.001
                validatorDecimals: 3
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editRgbLevels[modelData.key] : modelData.reset
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
            text: qsTr("Reset RGB levels")
            enabled: panel.hasSelection
            onClicked: if (panel.commands)
                panel.commands.resetControl("rgbLevels")
        }
    }
}
