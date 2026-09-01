pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    required property var panel
    id: maskEditor
    required property var mask
    Layout.fillWidth: true
    spacing: Fonts.smallSpacing

    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Mask")
        font.bold: true
        wrapMode: Text.WordWrap
    }
    CustomLabel {
        Layout.fillWidth: true
        text: maskEditor.mask.status !== undefined ? maskEditor.mask.status : ""
        wrapMode: Text.WordWrap
    }
    CustomLabel {
        Layout.fillWidth: true
        text: qsTr("Mask kind")
    }
    CustomComboBox {
        objectName: "maskKind"
        Layout.fillWidth: true
        model: maskEditor.mask.kindChoices !== undefined ? maskEditor.mask.kindChoices : []
        currentIndex: maskEditor.mask.kindIndex !== undefined ? maskEditor.mask.kindIndex : 0
        visible: currentIndex >= 0
        enabled: panel.hasSelection && (maskEditor.mask.editable || !maskEditor.mask.attached)
        onActivated: if (panel.commands)
            panel.commands.setDevelopNumber(maskEditor.mask.kindField, currentIndex)
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: maskEditor.mask.attached === true && maskEditor.mask.kindIndex < 0
        text: maskEditor.mask.kindLabel !== undefined ? maskEditor.mask.kindLabel : ""
        wrapMode: Text.WordWrap
    }
    RowLayout {
        Layout.fillWidth: true
        visible: maskEditor.mask.attached === true
        CustomCheckBox {
            Layout.fillWidth: true
            text: qsTr("Invert mask")
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            checked: maskEditor.mask.inverted === true
            onToggled: if (panel.liveReady && panel.commands && maskEditor.mask.editable === true)
                panel.commands.setDevelopNumber(maskEditor.mask.invertedField, checked ? 1 : 0)
        }
        CustomButton {
            text: qsTr("Reset")
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onClicked: if (panel.commands)
                panel.commands.resetControl(maskEditor.mask.invertedField)
        }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: maskEditor.mask.selectorsVisible === true
        spacing: Fonts.smallSpacing

        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Source")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: maskEditor.mask.sourceChoices !== undefined ? maskEditor.mask.sourceChoices : []
            currentIndex: maskEditor.mask.sourceIndex !== undefined ? maskEditor.mask.sourceIndex : 0
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber(maskEditor.mask.sourceField, currentIndex)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Channel")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: maskEditor.mask.channelChoices !== undefined ? maskEditor.mask.channelChoices : []
            currentIndex: maskEditor.mask.channelIndex !== undefined ? maskEditor.mask.channelIndex : 0
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber(maskEditor.mask.channelField, currentIndex)
        }
    }
    Repeater {
        model: maskEditor.mask.numericControls !== undefined ? maskEditor.mask.numericControls : []
        delegate: CustomSlider {
            required property var modelData
            Layout.fillWidth: true
            title: modelData.title
            from: modelData.min
            to: modelData.max
            stepSize: modelData.step
            validatorDecimals: modelData.decimals
            showReset: true
            resetValue: modelData.reset
            delayedCommit: true
            visible: modelData.visible
            enabled: panel.hasSelection && maskEditor.mask.editable === true && modelData.visible
            value: maskEditor.mask[modelData.key] !== undefined ? maskEditor.mask[modelData.key] : modelData.reset
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands && maskEditor.mask.editable === true && modelData.visible)
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
    CustomCheckBox {
        Layout.fillWidth: true
        visible: maskEditor.mask.attached === true
        text: qsTr("Show mask overlay")
        enabled: panel.hasSelection
        checked: panel.hasPresenter && panel.presenter.maskOverlayVisible && panel.presenter.maskOverlayTarget === maskEditor.mask.target
        onToggled: if (panel.hasPresenter)
            panel.presenter.setMaskOverlay(maskEditor.mask.target, checked)
    }
    CustomCheckBox {
        Layout.fillWidth: true
        objectName: "maskPlaceActive"
        visible: maskEditor.mask.attached === true && (maskEditor.mask.kindName === "circle" || maskEditor.mask.kindName === "ellipse" || maskEditor.mask.kindName === "linear_gradient")
        text: qsTr("Place on photo")
        enabled: panel.hasSelection && maskEditor.mask.editable === true && panel.hasPresenter && panel.presenter.maskPlaceGeometryAllowed
        checked: panel.hasPresenter && panel.presenter.maskPlaceActive && panel.presenter.maskOverlayVisible && panel.presenter.maskOverlayTarget === maskEditor.mask.target
        onToggled: if (panel.hasPresenter && panel.commands) {
            if (checked)
                panel.presenter.setMaskOverlay(maskEditor.mask.target, true)
            panel.commands.setMaskPlaceActive(checked)
        }
    }
    CustomLabel {
        Layout.fillWidth: true
        visible: panel.hasPresenter && panel.presenter.maskPlaceActive && maskEditor.mask.attached === true
        text: qsTr("Click the photo to place the circle, ellipse, or gradient. Canvas, Perspective, straighten, rotate, and flip must be off.")
        wrapMode: Text.WordWrap
        opacity: 0.75
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: maskEditor.mask.groupVisible === true
        spacing: Fonts.smallSpacing
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Group child")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: {
                const count = maskEditor.mask.childCount !== undefined ? maskEditor.mask.childCount : 0;
                const items = [];
                for (let i = 0; i < count; ++i)
                    items.push(qsTr("Child %1").arg(i + 1));
                return items;
            }
            currentIndex: maskEditor.mask.childIndex !== undefined ? maskEditor.mask.childIndex : 0
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber(maskEditor.mask.childIndexField, currentIndex)
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Child kind")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: maskEditor.mask.childKindChoices !== undefined ? maskEditor.mask.childKindChoices : []
            currentIndex: maskEditor.mask.childKindIndex !== undefined ? maskEditor.mask.childKindIndex : 0
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onActivated: if (panel.commands && maskEditor.mask.childKindValues)
                panel.commands.setDevelopNumber(maskEditor.mask.childKindField, maskEditor.mask.childKindValues[currentIndex])
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Combine")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: maskEditor.mask.operatorChoices !== undefined ? maskEditor.mask.operatorChoices : []
            currentIndex: maskEditor.mask.childOperatorIndex !== undefined ? maskEditor.mask.childOperatorIndex : 0
            enabled: panel.hasSelection && maskEditor.mask.editable === true && maskEditor.mask.childIndex > 0
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber(maskEditor.mask.childOperatorField, currentIndex)
        }
        CustomCheckBox {
            Layout.fillWidth: true
            text: qsTr("Invert child")
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            checked: maskEditor.mask.childInverted === true
            onToggled: if (panel.commands)
                panel.commands.setDevelopNumber(maskEditor.mask.childInvertedField, checked ? 1 : 0)
        }
        RowLayout {
            Layout.fillWidth: true
            CustomButton {
                text: qsTr("Add circle")
                enabled: panel.hasSelection && maskEditor.mask.editable === true
                onClicked: if (panel.commands)
                    panel.commands.setDevelopNumber(maskEditor.mask.addChildField, 3)
            }
            CustomButton {
                text: qsTr("Remove child")
                enabled: panel.hasSelection && maskEditor.mask.editable === true && maskEditor.mask.childCount > 1
                onClicked: if (panel.commands)
                    panel.commands.setDevelopNumber(maskEditor.mask.removeChildField, 1)
            }
        }
    }
    ColumnLayout {
        Layout.fillWidth: true
        visible: maskEditor.mask.pointsVisible === true
        spacing: Fonts.smallSpacing
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Point")
        }
        CustomComboBox {
            Layout.fillWidth: true
            model: {
                const count = maskEditor.mask.pointCount !== undefined ? maskEditor.mask.pointCount : 0;
                const items = [];
                for (let i = 0; i < count; ++i)
                    items.push(qsTr("Point %1").arg(i + 1));
                return items;
            }
            currentIndex: maskEditor.mask.pointIndex !== undefined ? maskEditor.mask.pointIndex : 0
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onActivated: if (panel.commands)
                panel.commands.setDevelopNumber(maskEditor.mask.pointIndexField, currentIndex)
        }
        RowLayout {
            Layout.fillWidth: true
            CustomButton {
                text: qsTr("Add point")
                enabled: panel.hasSelection && maskEditor.mask.editable === true
                onClicked: if (panel.commands)
                    panel.commands.setDevelopNumber(maskEditor.mask.addPointField, 1)
            }
            CustomButton {
                text: qsTr("Remove point")
                enabled: panel.hasSelection && maskEditor.mask.editable === true && maskEditor.mask.pointCount > 2
                onClicked: if (panel.commands)
                    panel.commands.setDevelopNumber(maskEditor.mask.removePointField, 1)
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        visible: maskEditor.mask.attached === true
        CustomButton {
            text: qsTr("Reset to all")
            enabled: panel.hasSelection && maskEditor.mask.editable === true
            onClicked: if (panel.commands)
                panel.commands.resetControl(maskEditor.mask.kindField)
        }
        CustomButton {
            text: qsTr("Detach mask")
            enabled: panel.hasSelection && maskEditor.mask.canDetach === true
            onClicked: if (panel.commands)
                panel.commands.resetControl(maskEditor.mask.detachField)
        }
    }
}
