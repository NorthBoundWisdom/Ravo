pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: groupRoot
    required property var panel
    Layout.fillWidth: true
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Velvia")
        font.bold: true
        wrapMode: Text.WordWrap
    }
    CustomCheckBox {
        objectName: "velviaEnabled"
        text: qsTr("Enable Velvia")
        enabled: panel.hasSelection
        checked: panel.hasPresenter && panel.presenter.editVelviaParams.enabled
        onToggled: if (panel.liveReady && panel.commands)
            panel.commands.setDevelopNumber("velviaEnabled", checked ? 1 : 0)
    }
    CustomSlider {
        objectName: "velviaStrength"
        Layout.fillWidth: true
        title: qsTr("Strength")
        from: 0
        to: 100
        stepSize: 1
        validatorDecimals: 1
        showReset: true
        resetValue: 25
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editVelviaParams.strength : 25
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("velviaStrength", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("velviaStrength", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("velviaStrength")
    }
    CustomSlider {
        objectName: "velviaBias"
        Layout.fillWidth: true
        title: qsTr("Mid-tones bias")
        from: 0
        to: 1
        stepSize: 0.01
        validatorDecimals: 2
        showReset: true
        resetValue: 1
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editVelviaParams.bias : 1
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("velviaBias", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("velviaBias", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("velviaBias")
    }
    CustomLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.72
        visible: panel.hasPresenter && panel.presenter.editVelviaParams.masked
        text: qsTr("Loaded Velvia mask is preserved but edited outside this panel.")
    }
    CustomButton {
        text: qsTr("Disable and reset Velvia")
        enabled: panel.hasSelection
        onClicked: if (panel.commands)
            panel.commands.resetControl("velvia")
    }
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("3D LUT")
        font.bold: true
        wrapMode: Text.WordWrap
    }
    RowLayout {
        Layout.fillWidth: true
        CustomTextField {
            objectName: "lut3dFile"
            Layout.fillWidth: true
            maximumLength: 4096
            showEmptyIndicator: true
            showClipIndicator: true
            placeholderText: qsTr("Choose a .cube file")
            enabled: panel.hasSelection
            text: panel.hasPresenter ? panel.presenter.editLut3d.filePath : ""
            onEditingCommitted: function (committedText) {
                if (panel.commands)
                    panel.commands.setDevelopText("lut3dFile", committedText);
            }
        }
        CustomButton {
            text: qsTr("Choose…")
            enabled: panel.hasSelection
            onClicked: panel.openLut3dDialog()
        }
    }
    CustomCheckBox {
        objectName: "lut3dEnabled"
        text: qsTr("Enable 3D LUT")
        enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editLut3d.hasFile
        checked: panel.hasPresenter && panel.presenter.editLut3d.enabled
        onToggled: if (panel.liveReady && panel.commands)
            panel.commands.setDevelopNumber("lut3dEnabled", checked ? 1 : 0)
    }
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Input color space")
    }
    CustomComboBox {
        objectName: "lut3dInputSpace"
        Layout.fillWidth: true
        model: panel.hasPresenter ? panel.presenter.editLut3d.spaceChoices : []
        currentIndex: panel.hasPresenter ? panel.presenter.editLut3d.inputSpaceIndex : 0
        enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editLut3d.hasFile
        Accessible.name: qsTr("3D LUT input color space")
        onActivated: function (index) {
            if (panel.commands)
                panel.commands.setDevelopNumber("lut3dInputSpaceIndex", index);
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Output color space")
    }
    CustomComboBox {
        objectName: "lut3dOutputSpace"
        Layout.fillWidth: true
        model: panel.hasPresenter ? panel.presenter.editLut3d.spaceChoices : []
        currentIndex: panel.hasPresenter ? panel.presenter.editLut3d.outputSpaceIndex : 0
        enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editLut3d.hasFile
        Accessible.name: qsTr("3D LUT output color space")
        onActivated: function (index) {
            if (panel.commands)
                panel.commands.setDevelopNumber("lut3dOutputSpaceIndex", index);
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Interpolation")
    }
    CustomComboBox {
        objectName: "lut3dInterpolation"
        Layout.fillWidth: true
        model: panel.hasPresenter ? panel.presenter.editLut3d.interpolationChoices : []
        currentIndex: panel.hasPresenter ? panel.presenter.editLut3d.interpolationIndex : 0
        enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editLut3d.hasFile
        Accessible.name: qsTr("3D LUT interpolation")
        onActivated: function (index) {
            if (panel.commands)
                panel.commands.setDevelopNumber("lut3dInterpolationIndex", index);
        }
    }
    CustomSlider {
        objectName: "lut3dStrength"
        Layout.fillWidth: true
        title: qsTr("Strength")
        from: 0
        to: 1
        stepSize: 0.01
        validatorDecimals: 2
        showReset: true
        resetValue: 1
        delayedCommit: true
        enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editLut3d.hasFile
        value: panel.hasPresenter ? panel.presenter.editLut3d.strength : 1
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("lut3dStrength", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("lut3dStrength", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("lut3dStrength")
    }
    CustomButton {
        text: qsTr("Disable and reset 3D LUT")
        enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editLut3d.present
        onClicked: if (panel.commands)
            panel.commands.resetControl("lut3d")
    }
    Expander {
        Layout.fillWidth: true
        title: qsTr("Color Balance RGB · more")
        expanded: false
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("darktable UCS (2022)"), qsTr("JzAzBz (2021)")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.formulaIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("colorBalanceFormula", currentIndex)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Global · Luminance"),
                    "key": "globalY",
                    "field": "colorBalanceGlobalY",
                    "minimum": -0.05,
                    "maximum": 0.05,
                    "reset": 0,
                    "step": 0.001,
                    "decimals": 3
                },
                {
                    "title": qsTr("Global · Chroma"),
                    "key": "globalChroma",
                    "field": "colorBalanceGlobalChroma",
                    "minimum": 0,
                    "maximum": 0.01,
                    "reset": 0,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Global · Hue"),
                    "key": "globalHue",
                    "field": "colorBalanceGlobalHue",
                    "minimum": 0,
                    "maximum": 360,
                    "reset": 0,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Shadows · Luminance"),
                    "key": "shadowsY",
                    "field": "colorBalanceShadowsY",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Shadows · Chroma"),
                    "key": "shadowsChroma",
                    "field": "colorBalanceShadowsChroma",
                    "minimum": 0,
                    "maximum": 0.5,
                    "reset": 0,
                    "step": 0.005,
                    "decimals": 3
                },
                {
                    "title": qsTr("Shadows · Hue"),
                    "key": "shadowsHue",
                    "field": "colorBalanceShadowsHue",
                    "minimum": 0,
                    "maximum": 360,
                    "reset": 0,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Midtones · Luminance"),
                    "key": "midtonesY",
                    "field": "colorBalanceMidtonesY",
                    "minimum": -0.25,
                    "maximum": 0.25,
                    "reset": 0,
                    "step": 0.005,
                    "decimals": 3
                },
                {
                    "title": qsTr("Midtones · Chroma"),
                    "key": "midtonesChroma",
                    "field": "colorBalanceMidtonesChroma",
                    "minimum": 0,
                    "maximum": 0.1,
                    "reset": 0,
                    "step": 0.001,
                    "decimals": 3
                },
                {
                    "title": qsTr("Midtones · Hue"),
                    "key": "midtonesHue",
                    "field": "colorBalanceMidtonesHue",
                    "minimum": 0,
                    "maximum": 360,
                    "reset": 0,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Highlights · Luminance"),
                    "key": "highlightsY",
                    "field": "colorBalanceHighlightsY",
                    "minimum": -0.5,
                    "maximum": 0.5,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Highlights · Chroma"),
                    "key": "highlightsChroma",
                    "field": "colorBalanceHighlightsChroma",
                    "minimum": 0,
                    "maximum": 0.2,
                    "reset": 0,
                    "step": 0.002,
                    "decimals": 3
                },
                {
                    "title": qsTr("Highlights · Hue"),
                    "key": "highlightsHue",
                    "field": "colorBalanceHighlightsHue",
                    "minimum": 0,
                    "maximum": 360,
                    "reset": 0,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Shadows fall-off"),
                    "key": "shadowsFalloff",
                    "field": "colorBalanceShadowsFalloff",
                    "minimum": 0,
                    "maximum": 3,
                    "reset": 1,
                    "step": 0.05,
                    "decimals": 2
                },
                {
                    "title": qsTr("Highlights fall-off"),
                    "key": "highlightsFalloff",
                    "field": "colorBalanceHighlightsFalloff",
                    "minimum": 0,
                    "maximum": 3,
                    "reset": 1,
                    "step": 0.05,
                    "decimals": 2
                },
                {
                    "title": qsTr("Mask grey fulcrum"),
                    "key": "maskGreyFulcrum",
                    "field": "colorBalanceMaskGreyFulcrum",
                    "minimum": 0.000001,
                    "maximum": 1,
                    "reset": 0.1845,
                    "step": 0.001,
                    "decimals": 4
                },
                {
                    "title": qsTr("White fulcrum · EV"),
                    "key": "whiteFulcrumEv",
                    "field": "colorBalanceWhiteFulcrumEv",
                    "minimum": -2,
                    "maximum": 2,
                    "reset": 0,
                    "step": 0.05,
                    "decimals": 2
                },
                {
                    "title": qsTr("Grey fulcrum"),
                    "key": "greyFulcrum",
                    "field": "colorBalanceGreyFulcrum",
                    "minimum": 0.1,
                    "maximum": 0.5,
                    "reset": 0.1845,
                    "step": 0.001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Chroma · Global"),
                    "key": "chromaGlobal",
                    "field": "colorBalanceChromaGlobal",
                    "minimum": -0.5,
                    "maximum": 0.5,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Chroma · Shadows"),
                    "key": "chromaShadows",
                    "field": "colorBalanceChromaShadows",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Chroma · Midtones"),
                    "key": "chromaMidtones",
                    "field": "colorBalanceChromaMidtones",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Chroma · Highlights"),
                    "key": "chromaHighlights",
                    "field": "colorBalanceChromaHighlights",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Saturation · Global"),
                    "key": "saturationGlobal",
                    "field": "colorBalanceSaturationGlobal",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Saturation · Shadows"),
                    "key": "saturationShadows",
                    "field": "colorBalanceSaturationShadows",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Saturation · Midtones"),
                    "key": "saturationMidtones",
                    "field": "colorBalanceSaturationMidtones",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Saturation · Highlights"),
                    "key": "saturationHighlights",
                    "field": "colorBalanceSaturationHighlights",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Brilliance · Global"),
                    "key": "brillianceGlobal",
                    "field": "colorBalanceBrillianceGlobal",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Brilliance · Shadows"),
                    "key": "brillianceShadows",
                    "field": "colorBalanceBrillianceShadows",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Brilliance · Midtones"),
                    "key": "brillianceMidtones",
                    "field": "colorBalanceBrillianceMidtones",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Brilliance · Highlights"),
                    "key": "brillianceHighlights",
                    "field": "colorBalanceBrillianceHighlights",
                    "minimum": -1,
                    "maximum": 1,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Vibrance"),
                    "key": "vibrance",
                    "field": "colorBalanceVibrance",
                    "minimum": -0.5,
                    "maximum": 0.5,
                    "reset": 0,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Hue rotation"),
                    "key": "hueRotation",
                    "field": "colorBalanceHueRotation",
                    "minimum": -180,
                    "maximum": 180,
                    "reset": 0,
                    "step": 1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Contrast"),
                    "key": "contrast",
                    "field": "colorBalanceContrast",
                    "minimum": -0.5,
                    "maximum": 0.5,
                    "reset": 0,
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
                value: panel.hasPresenter ? panel.presenter.editColorBalanceRgb[modelData.key] : modelData.reset
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
    }
}
