pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Graduated ND")
    sectionId: "graduated"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Graduated density")
            from: -4
            to: 4
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editGraduatedDensity : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("graduatedDensity", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("graduatedDensity", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("graduatedDensity")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Graduated rotation")
            from: -180
            to: 180
            stepSize: 1
            validatorDecimals: 0
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editGraduatedRotation : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("graduatedRotation", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("graduatedRotation", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("graduatedRotation")
        }
        MaskEditor {
            panel: sectionRoot.panel
            objectName: "graduatedMaskEditor"
            mask: panel.hasPresenter ? panel.presenter.editGraduatedMask : ({})
        }
    }
}
