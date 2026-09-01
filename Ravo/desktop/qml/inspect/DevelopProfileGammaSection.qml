pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Unbreak input profile")
    sectionId: "profileGamma"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomCheckBox {
            text: qsTr("Enable correction")
            enabled: panel.hasSelection
            checked: panel.hasPresenter && panel.presenter.editProfileGamma.enabled
            onToggled: if (panel.liveReady && panel.commands)
                panel.commands.setDevelopNumber("profileGammaEnabled", checked ? 1 : 0)
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: [qsTr("Logarithmic"), qsTr("Gamma")]
            enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editProfileGamma.enabled
            currentIndex: panel.hasPresenter ? panel.presenter.editProfileGamma.modeIndex : 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber("profileGammaModeIndex", currentIndex)
        }
        Repeater {
            model: [
                {
                    "title": qsTr("Dynamic range"),
                    "key": "dynamicRange",
                    "field": "profileGammaDynamicRange",
                    "minimum": 0.01,
                    "maximum": 32,
                    "reset": 10,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Middle gray luma"),
                    "key": "greyPoint",
                    "field": "profileGammaGreyPoint",
                    "minimum": 0.1,
                    "maximum": 100,
                    "reset": 18,
                    "step": 0.1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Black relative exposure"),
                    "key": "shadowsRange",
                    "field": "profileGammaShadowsRange",
                    "minimum": -16,
                    "maximum": 16,
                    "reset": -5,
                    "step": 0.05,
                    "decimals": 2
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                visible: panel.hasPresenter && panel.presenter.editProfileGamma.modeIndex === 0
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editProfileGamma.enabled
                value: panel.hasPresenter ? panel.presenter.editProfileGamma[modelData.key] : modelData.reset
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
        Repeater {
            model: [
                {
                    "title": qsTr("Linear part"),
                    "key": "linear",
                    "field": "profileGammaLinear",
                    "minimum": 0,
                    "maximum": 1,
                    "reset": 0.1,
                    "step": 0.0001,
                    "decimals": 4
                },
                {
                    "title": qsTr("Gamma exponent"),
                    "key": "gamma",
                    "field": "profileGammaGamma",
                    "minimum": 0,
                    "maximum": 1,
                    "reset": 0.45,
                    "step": 0.0001,
                    "decimals": 4
                }
            ]
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                visible: panel.hasPresenter && panel.presenter.editProfileGamma.modeIndex === 1
                title: modelData.title
                from: modelData.minimum
                to: modelData.maximum
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                enabled: panel.hasSelection && panel.hasPresenter && panel.presenter.editProfileGamma.enabled
                value: panel.hasPresenter ? panel.presenter.editProfileGamma[modelData.key] : modelData.reset
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
