pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

CustomSlider {
    required property var panel
    id: mixer
    required property string inputChannel
    property string fieldName
    property double currentValue: 0
    property double identityValue: 0
    readonly property color lowTrackColor: {
        if (inputChannel === "red")
            return "#3f9297";
        if (inputChannel === "green")
            return "#a65b9a";
        if (inputChannel === "blue")
            return "#c09a45";
        return Theme.midColor;
    }
    readonly property color highTrackColor: {
        if (inputChannel === "red")
            return "#d45c64";
        if (inputChannel === "green")
            return "#58b574";
        if (inputChannel === "blue")
            return "#5b83d1";
        return Theme.midColor;
    }
    Layout.fillWidth: true
    from: -2
    to: 2
    stepSize: 0.01
    validatorDecimals: 2
    showReset: true
    resetValue: identityValue
    delayedCommit: true
    enabled: panel.hasSelection
    value: currentValue
    trackGradient: Gradient {
        orientation: Gradient.Horizontal
        GradientStop {
            position: 0
            color: mixer.lowTrackColor
        }
        GradientStop {
            position: 0.5
            color: Theme.midColor
        }
        GradientStop {
            position: 1
            color: mixer.highTrackColor
        }
    }
    onValueEdited: function (value) {
        if (panel.liveReady && panel.commands)
            panel.commands.previewDevelopNumber(fieldName, value);
    }
    onValueCommitted: function (value) {
        if (panel.commands)
            panel.commands.setDevelopNumber(fieldName, value);
    }
    onResetRequested: if (panel.commands)
        panel.commands.resetControl(fieldName)
}
