pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("RAW Repair / Denoise / Lens")
    sectionId: "raw"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Demosaicing")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Auto — RCD / Markesteijn 3"), qsTr("PPG — Bayer compatibility"), qsTr("Markesteijn 1 — X-Trans fast"), qsTr("Markesteijn 3 — X-Trans quality")]
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.selectedMediaType === "image/x-raw"
            currentIndex: panel.hasPresenter ? panel.presenter.editDemosaicModeIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("demosaicModeIndex", currentIndex)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Auto selects RCD for Bayer and Markesteijn 3-pass for X-Trans. Explicit sensor-mismatched modes fail instead of changing algorithms.")
            wrapMode: Text.WordWrap
            opacity: 0.75
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("RAW wavelet denoise")
            from: 0
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.selectedMediaType === "image/x-raw"
            value: panel.hasPresenter ? panel.presenter.editRawDenoiseThreshold : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("rawDenoiseThreshold", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("rawDenoiseThreshold", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("rawDenoiseThreshold")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Hot pixels")
            from: 0
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editHotPixelsStrength : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("hotPixelsStrength", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("hotPixelsStrength", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("hotPixelsStrength")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Hot pixel threshold")
            from: 0
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0.05
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editHotPixelsThreshold : 0.05
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("hotPixelsThreshold", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("hotPixelsThreshold", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("hotPixelsThreshold")
        }
        CustomCheckBox {
            text: qsTr("Permissive (3 neighbours)")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editHotPixelsPermissive
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("hotPixelsPermissive", checked ? 1 : 0)
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("RAW chromatic aberration")
            from: 0
            to: 5
            stepSize: 1
            validatorDecimals: 0
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editRawCaIterations : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("rawCaIterations", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("rawCaIterations", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("rawCaIterations")
        }
        CustomCheckBox {
            text: qsTr("Avoid CA color shift")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editRawCaAvoidShift
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("rawCaAvoidShift", checked ? 1 : 0)
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Highlight reconstruction")
            from: 0
            to: 1
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editRawHighlights : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("rawHighlights", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("rawHighlights", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("rawHighlights")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Lens distortion")
            from: -1
            to: 1
            stepSize: 0.01
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editLensK1 : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("lensK1", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("lensK1", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("lensK1")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Lens vignetting")
            from: 0
            to: 1
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editLensVignetting : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("lensVignetting", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("lensVignetting", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("lensVignetting")
        }
    }
}
