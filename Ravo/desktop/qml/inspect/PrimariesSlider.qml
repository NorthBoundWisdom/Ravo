pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

CustomSlider {
    required property var panel
    required property var modelData
    Layout.fillWidth: true
    title: modelData.title
    from: modelData.minimum
    to: modelData.maximum
    stepSize: modelData.step
    validatorDecimals: modelData.decimals
    showReset: true
    resetValue: modelData.reset
    delayedCommit: true
    enabled: panel.hasSelection
    value: panel.hasPresenter ? panel.presenter.editPrimaries[modelData.key] : modelData.reset
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
