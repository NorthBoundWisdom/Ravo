pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Color Calibration")
    sectionId: "calibration"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Output rows × input channels (linear sRGB, D50)")
            wrapMode: Text.WordWrap
            opacity: 0.75
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Red ← Red")
            inputChannel: "red"
            fieldName: "channelMixerRR"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerRR : 1
            identityValue: 1
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Red ← Green")
            inputChannel: "green"
            fieldName: "channelMixerRG"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerRG : 0
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Red ← Blue")
            inputChannel: "blue"
            fieldName: "channelMixerRB"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerRB : 0
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Green ← Red")
            inputChannel: "red"
            fieldName: "channelMixerGR"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerGR : 0
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Green ← Green")
            inputChannel: "green"
            fieldName: "channelMixerGG"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerGG : 1
            identityValue: 1
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Green ← Blue")
            inputChannel: "blue"
            fieldName: "channelMixerGB"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerGB : 0
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Blue ← Red")
            inputChannel: "red"
            fieldName: "channelMixerBR"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerBR : 0
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Blue ← Green")
            inputChannel: "green"
            fieldName: "channelMixerBG"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerBG : 0
        }
        MixerSlider {
            panel: sectionRoot.panel
            title: qsTr("Blue ← Blue")
            inputChannel: "blue"
            fieldName: "channelMixerBB"
            currentValue: panel.hasPresenter ? panel.presenter.editChannelMixerBB : 1
            identityValue: 1
        }
    }
}
