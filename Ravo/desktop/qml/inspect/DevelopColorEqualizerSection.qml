pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Color Mixer")
    sectionId: "colorEqualizer"
    ColumnLayout {
        id: colorMixer
        property int activeBand: 0
        readonly property var bands: panel.hasPresenter ? panel.presenter.editColorEqBands : []
        readonly property var band: bands.length > activeBand ? bands[activeBand] : null
        readonly property var colors: ["#f87171", "#fb923c", "#facc15", "#4ade80", "#2dd4bf", "#60a5fa", "#a78bfa", "#f472b6"]
        readonly property var hues: [0, 0.0833, 0.1667, 0.3333, 0.5, 0.6667, 0.8333, 0.9444]
        readonly property var bandNames: [qsTr("Red"), qsTr("Orange"), qsTr("Yellow"), qsTr("Green"), qsTr("Aqua"), qsTr("Blue"), qsTr("Purple"), qsTr("Magenta")]

        function hueColor(offset) {
            let hue = colorMixer.hues[colorMixer.activeBand] + offset;
            while (hue < 0)
                hue += 1;
            while (hue > 1)
                hue -= 1;
            return Qt.hsla(hue, 0.72, 0.56, 1);
        }

        Layout.fillWidth: true
        width: parent.width
        spacing: Fonts.size8

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Fonts.size4
            Layout.bottomMargin: Fonts.size8
            spacing: Fonts.size8

            Repeater {
                id: colorEqBands
                model: colorMixer.colors.length
                delegate: Rectangle {
                    required property int index
                    Layout.fillWidth: true
                    Layout.preferredWidth: Fonts.size24
                    Layout.preferredHeight: width
                    Layout.maximumWidth: Fonts.scaledUiSize(28)
                    Layout.maximumHeight: Fonts.scaledUiSize(28)
                    color: colorMixer.colors[index]
                    radius: width / 2
                    border.color: colorMixer.activeBand === index ? "#ffffff" : "transparent"
                    border.width: colorMixer.activeBand === index ? Fonts.size2 : 0
                    scale: colorMixer.activeBand === index ? 1.08 : swatchHover.hovered ? 1.04 : 1
                    Accessible.name: colorMixer.bandNames[index]

                    HoverHandler {
                        id: swatchHover
                    }
                    TapHandler {
                        onTapped: colorMixer.activeBand = index
                    }
                    Behavior on scale {
                        NumberAnimation {
                            duration: 110
                        }
                    }
                }
            }
        }
        DevelopColorSlider {
            objectName: "colorEqBand" + colorMixer.activeBand + "Hue"
            title: qsTr("Hue")
            from: -0.5
            to: 0.5
            stepSize: 0.005
            displayScale: 200
            displayDecimals: 0
            resetValue: 0
            enabled: panel.hasSelection && colorMixer.band !== null
            value: colorMixer.band ? colorMixer.band.hue : 0
            trackGradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: colorMixer.hueColor(-0.18)
                }
                GradientStop {
                    position: 0.5
                    color: colorMixer.hueColor(0)
                }
                GradientStop {
                    position: 1
                    color: colorMixer.hueColor(0.18)
                }
            }
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands && colorMixer.band)
                    panel.commands.previewDevelopNumber(colorMixer.band.hueField, value);
            }
            onValueCommitted: function (value) {
                if (panel.commands && colorMixer.band)
                    panel.commands.setDevelopNumber(colorMixer.band.hueField, value);
            }
            onResetRequested: if (panel.commands && colorMixer.band)
                panel.commands.resetControl(colorMixer.band.hueField)
        }
        DevelopColorSlider {
            objectName: "colorEqBand" + colorMixer.activeBand + "Saturation"
            title: qsTr("Saturation")
            from: -1
            to: 1
            stepSize: 0.05
            displayScale: 100
            displayDecimals: 0
            resetValue: 0
            enabled: panel.hasSelection && colorMixer.band !== null
            value: colorMixer.band ? colorMixer.band.sat : 0
            trackGradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: "#929292"
                }
                GradientStop {
                    position: 1
                    color: colorMixer.hueColor(0)
                }
            }
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands && colorMixer.band)
                    panel.commands.previewDevelopNumber(colorMixer.band.satField, value);
            }
            onValueCommitted: function (value) {
                if (panel.commands && colorMixer.band)
                    panel.commands.setDevelopNumber(colorMixer.band.satField, value);
            }
            onResetRequested: if (panel.commands && colorMixer.band)
                panel.commands.resetControl(colorMixer.band.satField)
        }
        DevelopColorSlider {
            objectName: "colorEqBand" + colorMixer.activeBand + "Luminance"
            title: qsTr("Luminance")
            from: -1
            to: 1
            stepSize: 0.05
            displayScale: 100
            displayDecimals: 0
            resetValue: 0
            enabled: panel.hasSelection && colorMixer.band !== null
            value: colorMixer.band ? colorMixer.band.light : 0
            trackGradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop {
                    position: 0
                    color: "#050505"
                }
                GradientStop {
                    position: 0.5
                    color: colorMixer.hueColor(0)
                }
                GradientStop {
                    position: 1
                    color: "#f4f4f4"
                }
            }
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands && colorMixer.band)
                    panel.commands.previewDevelopNumber(colorMixer.band.lightField, value);
            }
            onValueCommitted: function (value) {
                if (panel.commands && colorMixer.band)
                    panel.commands.setDevelopNumber(colorMixer.band.lightField, value);
            }
            onResetRequested: if (panel.commands && colorMixer.band)
                panel.commands.resetControl(colorMixer.band.lightField)
        }
    }
}
