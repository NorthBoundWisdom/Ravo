pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Color")
    sectionId: "color"
    ColumnLayout {
        id: colorCore
        property int gradingMode: 0
        property bool gradingDetails: false

        Layout.fillWidth: true
        width: parent.width
        spacing: Fonts.size12

        DevelopSection {
            panel: sectionRoot.panel
            title: qsTr("White Balance")
            sectionId: "whiteBalance"
            collapsible: false
            animateHeight: false
            borderWidth: 0
            borderRadius: 0
            padding: 0
            panelColor: "transparent"
            titleBarColor: "transparent"

            ColumnLayout {
                Layout.fillWidth: true
                width: parent.width
                spacing: Fonts.size6

                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("As shot"), qsTr("Camera reference"), qsTr("As shot → reference"), qsTr("Manual coefficients")]
                    enabled: panel.hasSelection
                    currentIndex: panel.hasPresenter ? panel.presenter.editWhiteBalance.modeIndex : 0
                    onActivated: if (panel.commands)
                        panel.commands.setDevelopNumber("whiteBalanceMode", currentIndex)
                }
                CustomCheckBox {
                    objectName: "whiteBalancePickActive"
                    text: qsTr("Pick white on photo")
                    enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editWhiteBalance.canPick
                    checked: panel.hasPresenter && panel.presenter.whiteBalancePickActive
                    onToggled: if (panel.commands)
                        panel.commands.setWhiteBalancePickActive(checked)
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: panel.hasPresenter && panel.presenter.whiteBalancePickActive
                    text: qsTr("Click a neutral patch in the photo. RAW only; Perspective and Canvas must be off.")
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                }
                Repeater {
                    model: panel.hasPresenter && (panel.presenter.editWhiteBalance.modeIndex === 3 || panel.presenter.editWhiteBalance.hasCoefficients) ? [
                        {
                            "title": qsTr("Red coefficient"),
                            "key": "red",
                            "field": "whiteBalanceRed",
                            "low": "#4091bd",
                            "high": "#e7a044",
                            "value": panel.presenter.editWhiteBalance.red,
                            "commands": panel.commands,
                            "liveReady": panel.liveReady,
                            "enabled": panel.hasSelection
                        },
                        {
                            "title": qsTr("Green coefficient"),
                            "key": "green",
                            "field": "whiteBalanceGreen",
                            "low": "#43af56",
                            "high": "#d24bab",
                            "value": panel.presenter.editWhiteBalance.green,
                            "commands": panel.commands,
                            "liveReady": panel.liveReady,
                            "enabled": panel.hasSelection
                        },
                        {
                            "title": qsTr("Blue coefficient"),
                            "key": "blue",
                            "field": "whiteBalanceBlue",
                            "low": "#e7a044",
                            "high": "#4091bd",
                            "value": panel.presenter.editWhiteBalance.blue,
                            "commands": panel.commands,
                            "liveReady": panel.liveReady,
                            "enabled": panel.hasSelection
                        },
                        {
                            "title": qsTr("Fourth coefficient"),
                            "key": "fourth",
                            "field": "whiteBalanceFourth",
                            "low": "#43af56",
                            "high": "#d24bab",
                            "value": panel.presenter.editWhiteBalance.fourth,
                            "commands": panel.commands,
                            "liveReady": panel.liveReady,
                            "enabled": panel.hasSelection
                        }
                    ] : []
                    delegate: DevelopColorSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        visible: true
                        title: modelData.title
                        from: 0.000001
                        to: 8
                        stepSize: 0.01
                        displayDecimals: 2
                        resetValue: 1
                        enabled: modelData.enabled
                        value: modelData.value
                        trackGradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop {
                                position: 0
                                color: modelData.low
                            }
                            GradientStop {
                                position: 0.5
                                color: "#bebebe"
                            }
                            GradientStop {
                                position: 1
                                color: modelData.high
                            }
                        }
                        onValueEdited: function (value) {
                            if (modelData.liveReady && modelData.commands)
                                modelData.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (modelData.commands)
                                modelData.commands.setDevelopNumber(modelData.field, value);
                        }
                        onResetRequested: if (modelData.commands)
                            modelData.commands.resetControl(modelData.field)
                    }
                }
            }
        }

        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Presence")
            font.bold: true
        }
        DevelopColorSlider {
            title: qsTr("Vibrance")
            from: -1
            to: 1
            stepSize: 0.01
            displayScale: 100
            displayDecimals: 0
            resetValue: 0
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editVibrance : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("vibrance", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("vibrance", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("vibrance")
        }
        DevelopColorSlider {
            title: qsTr("Saturation")
            from: -1
            to: 1
            stepSize: 0.01
            displayScale: 100
            displayDecimals: 0
            resetValue: 0
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSaturation : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("saturation", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("saturation", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("saturation")
        }

        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Hue")
            font.bold: true
        }
        DevelopColorSlider {
            objectName: "colorHueRotation"
            title: qsTr("Hue")
            from: -180
            to: 180
            stepSize: 1
            displayDecimals: 0
            resetValue: 0
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.hueRotation : 0
            trackGradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: "#e45c65"
                }
                GradientStop {
                    position: 0.17
                    color: "#e4d642"
                }
                GradientStop {
                    position: 0.34
                    color: "#63c85b"
                }
                GradientStop {
                    position: 0.5
                    color: "#4ad7d2"
                }
                GradientStop {
                    position: 0.67
                    color: "#597ce5"
                }
                GradientStop {
                    position: 0.84
                    color: "#c850dc"
                }
                GradientStop {
                    position: 1
                    color: "#e45c65"
                }
            }
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("colorBalanceHueRotation", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("colorBalanceHueRotation", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("colorBalanceHueRotation")
        }

        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Color Grading")
            font.bold: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size8

            CustomIconActionButton {
                objectName: "colorGradingThreeWay"
                iconText: "◉"
                active: colorCore.gradingMode === 0
                activeColor: "transparent"
                inactiveColor: Theme.buttonPressedColor
                borderColor: active ? Theme.textColor : "transparent"
                borderWidth: active ? Fonts.size2 : 0
                onClicked: colorCore.gradingMode = 0
            }
            CustomIconActionButton {
                objectName: "colorGradingGlobal"
                iconText: "●"
                active: colorCore.gradingMode === 1
                activeColor: "transparent"
                inactiveColor: Theme.buttonPressedColor
                borderColor: active ? Theme.textColor : "transparent"
                borderWidth: active ? Fonts.size2 : 0
                onClicked: colorCore.gradingMode = 1
            }
            Rectangle {
                Layout.preferredWidth: Fonts.size1
                Layout.preferredHeight: Fonts.size20
                color: Theme.midColor
                opacity: 0.45
            }
            CustomIconActionButton {
                objectName: "colorGradingDetails"
                iconText: "☷"
                active: colorCore.gradingDetails
                activeColor: Theme.buttonHoveredColor
                inactiveColor: "transparent"
                onClicked: colorCore.gradingDetails = !colorCore.gradingDetails
            }
            Item {
                Layout.fillWidth: true
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: colorCore.gradingMode === 0
            spacing: Fonts.size12

            ColorGradeWheel {
                objectName: "colorBalanceMidtonesWheel"
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(Fonts.scaledUiSize(156), colorCore.width * 0.58)
                title: qsTr("Midtones")
                wheelDiameter: Layout.preferredWidth
                hueField: "colorBalanceMidtonesHue"
                chromaField: "colorBalanceMidtonesChroma"
                luminanceField: "colorBalanceMidtonesY"
                hue: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.midtonesHue : 0
                chroma: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.midtonesChroma : 0
                luminance: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.midtonesY : 0
                maxChroma: 0.1
                luminanceFrom: -0.25
                luminanceTo: 0.25
                luminanceStep: 0.005
                luminanceDecimals: 0
                editorEnabled: panel.hasSelection
                commands: panel.commands
                liveReady: panel.liveReady
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Fonts.size12

                ColorGradeWheel {
                    objectName: "colorBalanceShadowsWheel"
                    title: qsTr("Shadows")
                    wheelDiameter: Fonts.scaledUiSize(132)
                    hueField: "colorBalanceShadowsHue"
                    chromaField: "colorBalanceShadowsChroma"
                    luminanceField: "colorBalanceShadowsY"
                    hue: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.shadowsHue : 0
                    chroma: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.shadowsChroma : 0
                    luminance: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.shadowsY : 0
                    maxChroma: 0.5
                    luminanceFrom: -1
                    luminanceTo: 1
                    luminanceStep: 0.01
                    luminanceDecimals: 0
                    editorEnabled: panel.hasSelection
                    commands: panel.commands
                    liveReady: panel.liveReady
                }
                ColorGradeWheel {
                    objectName: "colorBalanceHighlightsWheel"
                    title: qsTr("Highlights")
                    wheelDiameter: Fonts.scaledUiSize(132)
                    hueField: "colorBalanceHighlightsHue"
                    chromaField: "colorBalanceHighlightsChroma"
                    luminanceField: "colorBalanceHighlightsY"
                    hue: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.highlightsHue : 0
                    chroma: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.highlightsChroma : 0
                    luminance: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.highlightsY : 0
                    maxChroma: 0.2
                    luminanceFrom: -0.5
                    luminanceTo: 0.5
                    luminanceStep: 0.01
                    luminanceDecimals: 0
                    editorEnabled: panel.hasSelection
                    commands: panel.commands
                    liveReady: panel.liveReady
                }
            }
        }
        ColorGradeWheel {
            objectName: "colorBalanceGlobalWheel"
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(Fonts.scaledUiSize(180), colorCore.width * 0.72)
            visible: colorCore.gradingMode === 1
            title: qsTr("Global")
            wheelDiameter: Layout.preferredWidth
            hueField: "colorBalanceGlobalHue"
            chromaField: "colorBalanceGlobalChroma"
            luminanceField: "colorBalanceGlobalY"
            hue: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.globalHue : 0
            chroma: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.globalChroma : 0
            luminance: panel.hasPresenter ? panel.presenter.editColorBalanceRgb.globalY : 0
            maxChroma: 0.5
            luminanceFrom: -0.05
            luminanceTo: 0.05
            luminanceStep: 0.001
            luminanceDecimals: 0
            editorEnabled: panel.hasSelection
            commands: panel.commands
            liveReady: panel.liveReady
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: colorCore.gradingDetails
            spacing: Fonts.size6

            Repeater {
                model: [
                    {
                        "title": qsTr("Shadows falloff"),
                        "key": "shadowsFalloff",
                        "field": "colorBalanceShadowsFalloff",
                        "from": 0,
                        "to": 3,
                        "reset": 1,
                        "value": panel.hasPresenter ? panel.presenter.editColorBalanceRgb.shadowsFalloff : 1,
                        "commands": panel.commands,
                        "liveReady": panel.liveReady,
                        "enabled": panel.hasSelection
                    },
                    {
                        "title": qsTr("Highlights falloff"),
                        "key": "highlightsFalloff",
                        "field": "colorBalanceHighlightsFalloff",
                        "from": 0,
                        "to": 3,
                        "reset": 1,
                        "value": panel.hasPresenter ? panel.presenter.editColorBalanceRgb.highlightsFalloff : 1,
                        "commands": panel.commands,
                        "liveReady": panel.liveReady,
                        "enabled": panel.hasSelection
                    },
                    {
                        "title": qsTr("Mask grey fulcrum"),
                        "key": "maskGreyFulcrum",
                        "field": "colorBalanceMaskGreyFulcrum",
                        "from": 0,
                        "to": 1,
                        "reset": 0.1845,
                        "value": panel.hasPresenter ? panel.presenter.editColorBalanceRgb.maskGreyFulcrum : 0.1845,
                        "commands": panel.commands,
                        "liveReady": panel.liveReady,
                        "enabled": panel.hasSelection
                    }
                ]
                delegate: DevelopColorSlider {
                    required property var modelData
                    title: modelData.title
                    from: modelData.from
                    to: modelData.to
                    stepSize: 0.01
                    displayDecimals: 2
                    resetValue: modelData.reset
                    enabled: modelData.enabled
                    value: modelData.value
                    onValueEdited: function (value) {
                        if (modelData.liveReady && modelData.commands)
                            modelData.commands.previewDevelopNumber(modelData.field, value);
                    }
                    onValueCommitted: function (value) {
                        if (modelData.commands)
                            modelData.commands.setDevelopNumber(modelData.field, value);
                    }
                    onResetRequested: if (modelData.commands)
                        modelData.commands.resetControl(modelData.field)
                }
            }
        }
    }
}
