pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

CustomEditPanel {
    id: sectionPanel
    required property var panel
    showAddButton: false
    showDeleteButton: false
    showApplyButton: false
    actionsNeedEditing: false
    actionButtonsEnabled: panel && panel.hasSelection
    editing: false
    effectIndicator: true
    effectActiveTooltip: qsTr("Click to bypass this panel")
    effectBypassedTooltip: qsTr("Click to enable this panel")
    resetTooltip: qsTr("Reset this section")
    property string sectionId
    function syncEffectLamp() {
        if (!panel)
            return;
        modified = panel.hasPresenter && sectionId.length && panel.presenter.sectionModified(sectionId);
        effectEnabled = !panel.hasPresenter || !sectionId.length || panel.presenter.sectionEffectEnabled(sectionId);
    }
    Connections {
        target: panel ? panel.presenter : null
        function onEditChanged() {
            sectionPanel.syncEffectLamp();
        }
        function onSelectionChanged() {
            sectionPanel.syncEffectLamp();
        }
    }
    Component.onCompleted: syncEffectLamp()
    onSectionIdChanged: syncEffectLamp()
    onReset: function () {
        if (panel.commands && sectionId.length)
            panel.commands.resetSection(sectionId);
    }
    onEffectEnabledToggled: function (enabled) {
        if (panel.commands && sectionId.length)
            panel.commands.setSectionEnabled(sectionId, enabled);
    }
}
