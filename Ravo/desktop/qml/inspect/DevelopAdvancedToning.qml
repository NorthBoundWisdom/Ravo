pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: groupRoot
    required property var panel
    Layout.fillWidth: true
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Split Toning")
        font.bold: true
        wrapMode: Text.WordWrap
    }
    CustomCheckBox {
        objectName: "splitToningEnabled"
        text: qsTr("Enable Split Toning")
        enabled: panel.hasSelection
        checked: panel.hasPresenter && panel.presenter.editSplitToning.enabled
        onToggled: if (panel.liveReady && panel.commands)
            panel.commands.setDevelopNumber("splitToningEnabled", checked ? 1 : 0)
    }
    CustomSlider {
        Layout.fillWidth: true
        title: qsTr("Split Toning mix")
        from: 0
        to: 1
        stepSize: 0.01
        validatorDecimals: 2
        showReset: false
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitToning.mix : 1
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitMix", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitMix", value);
        }
    }
    HueSlider {
        Layout.fillWidth: true
        title: qsTr("Shadow hue")
        showReset: true
        resetValue: 0
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitShadowsHue : 0
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitShadowsHue", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitShadowsHue", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("splitShadowsHue")
    }
    CustomSlider {
        Layout.fillWidth: true
        title: qsTr("Shadow saturation")
        from: 0
        to: 1
        stepSize: 0.01
        validatorDecimals: 2
        showReset: false
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitToning.shadowSaturation : 0.5
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitShadowSaturation", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitShadowSaturation", value);
        }
    }
    HueSlider {
        Layout.fillWidth: true
        title: qsTr("Highlight hue")
        showReset: true
        resetValue: 0.2
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitHighlightsHue : 0.2
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitHighlightsHue", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitHighlightsHue", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("splitHighlightsHue")
    }
    CustomSlider {
        Layout.fillWidth: true
        title: qsTr("Highlight saturation")
        from: 0
        to: 1
        stepSize: 0.01
        validatorDecimals: 2
        showReset: false
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitToning.highlightSaturation : 0.5
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitHighlightSaturation", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitHighlightSaturation", value);
        }
    }
    CustomSlider {
        Layout.fillWidth: true
        title: qsTr("Split balance")
        from: 0
        to: 1
        showReset: true
        resetValue: 0.5
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitBalance : 0.5
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitBalance", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitBalance", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("splitBalance")
    }
    CustomSlider {
        Layout.fillWidth: true
        title: qsTr("Midtone compression")
        from: 0
        to: 100
        stepSize: 1
        validatorDecimals: 1
        showReset: false
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editSplitToning.compress : 33
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("splitCompress", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("splitCompress", value);
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.72
        visible: panel.hasPresenter && panel.presenter.editSplitToning.masked
        text: qsTr("Loaded Split Toning mask is preserved but edited outside this panel.")
    }
    CustomButton {
        text: qsTr("Disable and reset Split Toning")
        enabled: panel.hasSelection
        onClicked: if (panel.commands)
            panel.commands.resetControl("splitToning")
    }
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Monochrome")
        font.bold: true
        wrapMode: Text.WordWrap
    }
    CustomCheckBox {
        objectName: "monochromeEnabled"
        text: qsTr("Enable Monochrome")
        enabled: panel.hasSelection
        checked: panel.hasPresenter && panel.presenter.editMonochromeFilter.enabled
        onToggled: if (panel.liveReady && panel.commands)
            panel.commands.setDevelopNumber("monochromeEnabled", checked ? 1 : 0)
    }
    Repeater {
        model: [
            {
                "title": qsTr("Filter a*"),
                "key": "filterA",
                "field": "monochromeFilterA",
                "from": -128,
                "to": 128,
                "reset": 0,
                "step": 1,
                "decimals": 1
            },
            {
                "title": qsTr("Filter b*"),
                "key": "filterB",
                "field": "monochromeFilterB",
                "from": -128,
                "to": 128,
                "reset": 0,
                "step": 1,
                "decimals": 1
            },
            {
                "title": qsTr("Filter size"),
                "key": "size",
                "field": "monochromeSize",
                "from": 0.5,
                "to": 3,
                "reset": 2,
                "step": 0.1,
                "decimals": 1
            },
            {
                "title": qsTr("Keep highlights"),
                "key": "highlights",
                "field": "monochromeHighlights",
                "from": 0,
                "to": 1,
                "reset": 0,
                "step": 0.01,
                "decimals": 2
            },
            {
                "title": qsTr("Monochrome mix"),
                "key": "mix",
                "field": "monochromeMix",
                "from": 0,
                "to": 1,
                "reset": 1,
                "step": 0.01,
                "decimals": 2
            }
        ]
        delegate: CustomSlider {
            required property var modelData
            Layout.fillWidth: true
            title: modelData.title
            from: modelData.from
            to: modelData.to
            stepSize: modelData.step
            validatorDecimals: modelData.decimals
            showReset: false
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editMonochromeFilter[modelData.key] : modelData.reset
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber(modelData.field, value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber(modelData.field, value);
            }
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.72
        visible: panel.hasPresenter && panel.presenter.editMonochromeFilter.masked
        text: qsTr("Loaded Monochrome mask is preserved but edited outside this panel.")
    }
    CustomButton {
        text: qsTr("Disable and reset Monochrome")
        enabled: panel.hasSelection
        onClicked: if (panel.commands)
            panel.commands.resetControl("monochrome")
    }
}
