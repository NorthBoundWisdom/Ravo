pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Input Profile")
    sectionId: "inputProfile"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Source metadata"), qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"), qsTr("Linear ProPhoto RGB"), qsTr("Display P3"), qsTr("HLG P3")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editInputColor.inputProfileIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("inputProfile", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Linear ProPhoto RGB"), qsTr("Display P3"), qsTr("Adobe RGB")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editInputColor.workingProfileIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("workingProfile", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Perceptual"), qsTr("Relative colorimetric"), qsTr("Saturation"), qsTr("Absolute colorimetric")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editInputColor.intentIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("renderingIntent", currentIndex)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("No gamut clipping"), qsTr("Clip to sRGB"), qsTr("Clip to Adobe RGB"), qsTr("Clip to linear Rec709"), qsTr("Clip to linear Rec2020")]
            enabled: panel.hasSelection
            currentIndex: panel.hasPresenter ? panel.presenter.editInputColor.normalizeIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("gamutNormalize", currentIndex)
        }
        CustomCheckBox {
            text: qsTr("RAW blue mapping")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editInputColor.blueMapping
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("blueMapping", checked ? 1 : 0)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: panel.hasPresenter ? qsTr("%1 → %2").arg(panel.presenter.editInputColor.inputProfile).arg(panel.presenter.editInputColor.workingProfile) : ""
            wrapMode: Text.WordWrap
            opacity: 0.75
        }
    }
}
