pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Tone equalizer")
    sectionId: "toneEqual"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Blacks")
            from: -2
            to: 2
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editToneEqBlacks : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("toneEqBlacks", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("toneEqBlacks", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("toneEqBlacks")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Shadows")
            from: -2
            to: 2
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editToneEqShadows : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("toneEqShadows", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("toneEqShadows", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("toneEqShadows")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Midtones")
            from: -2
            to: 2
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editToneEqMidtones : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("toneEqMidtones", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("toneEqMidtones", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("toneEqMidtones")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Highlights")
            from: -2
            to: 2
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editToneEqHighlights : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("toneEqHighlights", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("toneEqHighlights", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("toneEqHighlights")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Whites")
            from: -2
            to: 2
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editToneEqWhites : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("toneEqWhites", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("toneEqWhites", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("toneEqWhites")
        }
    }
}
