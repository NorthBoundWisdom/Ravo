pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Output & Soft Proof")
    sectionId: "outputProfile"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"), qsTr("Linear ProPhoto RGB"), qsTr("PQ Rec2020"), qsTr("HLG Rec2020"), qsTr("PQ P3"), qsTr("HLG P3"), qsTr("Display P3")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editOutputColor.outputProfileIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("outputProfile", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Perceptual"), qsTr("Relative colorimetric"), qsTr("Saturation"), qsTr("Absolute colorimetric")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editOutputColor.intentIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("outputRenderingIntent", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Proof off"), qsTr("Soft proof"), qsTr("Gamut warning")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editOutputColor.proofModeIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("proofMode", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"), qsTr("Linear ProPhoto RGB"), qsTr("PQ Rec2020"), qsTr("HLG Rec2020"), qsTr("PQ P3"), qsTr("HLG P3"), qsTr("Display P3")]
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editOutputColor.proofModeIndex !== 0
            currentIndex: panel.hasPresenter ? panel.presenter.editOutputColor.proofProfileIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("proofProfile", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Perceptual"), qsTr("Relative colorimetric"), qsTr("Saturation"), qsTr("Absolute colorimetric")]
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editOutputColor.proofModeIndex !== 0
            currentIndex: panel.hasPresenter ? panel.presenter.editOutputColor.proofIntentIndex : 1
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("proofIntent", currentIndex)
        }
        CustomCheckBox {
            text: qsTr("Black-point compensation")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editOutputColor.blackPointCompensation
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("outputBlackPointCompensation", checked ? 1 : 0)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: panel.hasPresenter ? qsTr("%1 · %2 · proof %3").arg(panel.presenter.editOutputColor.outputProfile).arg(panel.presenter.editOutputColor.proofMode).arg(panel.presenter.editOutputColor.proofProfile) : ""
            wrapMode: Text.WordWrap
            opacity: 0.75
        }
    }
}
