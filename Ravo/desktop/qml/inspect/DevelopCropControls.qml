pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: cropControls
    required property var panel
    property bool pinned: false
    spacing: Fonts.smallSpacing
    CustomLabel {
        visible: cropControls.pinned
        text: qsTr("Crop & Rotate")
        font.bold: true
        Layout.fillWidth: true
    }
    CustomLabel {
        text: qsTr("Aspect ratio")
        Layout.fillWidth: true
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Fonts.size6
        CustomComboBox {
            objectName: "cropAspectRatio"
            Accessible.name: qsTr("Aspect ratio")
            Layout.fillWidth: true
            model: ["free", "1:1", "3:2", "4:3", "5:4", "16:9"]
            enabled: panel.hasSelection
            displayText: panel.hasPresenter && panel.presenter.cropAspect === "locked" ? qsTr("Custom") : currentText
            currentIndex: {
                const aspects = ["free", "1:1", "3:2", "4:3", "5:4", "16:9"];
                const current = panel.hasPresenter ? panel.presenter.cropAspect : "free";
                return aspects.indexOf(current);
            }
            onActivated: if (panel.commands)
                panel.commands.setCropAspect(currentText)
        }
        CustomButton {
            display: AbstractButton.IconOnly
            checkable: true
            checked: panel.hasPresenter && panel.presenter.cropAspect !== "free"
            icon.source: checked ? "qrc:/GeoControls/icons/Lock.svg" : "qrc:/GeoControls/icons/Unlock.svg"
            tooltipText: checked ? qsTr("Unlock aspect ratio") : qsTr("Lock aspect ratio")
            enabled: panel.hasSelection
            implicitWidth: Fonts.iconButtonSize
            implicitHeight: Fonts.iconButtonSize
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            defaultPadding: 0
            onToggled: if (panel.commands)
                panel.commands.setCropAspect(checked ? "locked" : "free")
        }
    }
    CustomButton {
        objectName: "cropAutoLevel"
        Layout.fillWidth: true
        text: qsTr("Auto Level")
        enabled: panel.hasSelection
        onClicked: if (panel.commands)
            panel.commands.autoPerspective("level")
    }
    CustomSlider {
        objectName: "cropFineRotation"
        Layout.fillWidth: true
        title: qsTr("Angle")
        from: -45
        to: 45
        stepSize: 0.01
        validatorDecimals: 2
        showReset: true
        resetValue: 0
        delayedCommit: true
        enabled: panel.hasSelection
        value: panel.hasPresenter ? panel.presenter.editStraighten : 0
        onValueEdited: function (value) {
            if (panel.liveReady && panel.commands)
                panel.commands.previewDevelopNumber("straighten", value);
        }
        onValueCommitted: function (value) {
            if (panel.commands)
                panel.commands.setDevelopNumber("straighten", value);
        }
        onResetRequested: if (panel.commands)
            panel.commands.resetControl("straighten")
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Fonts.size6
        CustomButton {
            display: AbstractButton.IconOnly
            icon.source: "qrc:/GeoControls/icons/RotateCcw.svg"
            tooltipText: qsTr("Rotate Left")
            enabled: panel.hasSelection
            implicitWidth: Fonts.iconButtonSize
            implicitHeight: Fonts.iconButtonSize
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            Layout.fillWidth: true
            defaultPadding: 0
            onClicked: if (panel.commands)
                panel.commands.rotateLeft.trigger()
        }
        CustomButton {
            display: AbstractButton.IconOnly
            icon.source: "qrc:/GeoControls/icons/RotateCw.svg"
            tooltipText: qsTr("Rotate Right")
            enabled: panel.hasSelection
            implicitWidth: Fonts.iconButtonSize
            implicitHeight: Fonts.iconButtonSize
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            Layout.fillWidth: true
            defaultPadding: 0
            onClicked: if (panel.commands)
                panel.commands.rotateRight.trigger()
        }
        CustomButton {
            display: AbstractButton.IconOnly
            icon.source: "qrc:/GeoControls/icons/FlipHorizontal.svg"
            tooltipText: qsTr("Flip Horizontal")
            enabled: panel.hasSelection
            implicitWidth: Fonts.iconButtonSize
            implicitHeight: Fonts.iconButtonSize
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            Layout.fillWidth: true
            defaultPadding: 0
            onClicked: if (panel.commands)
                panel.commands.flipHorizontal.trigger()
        }
        CustomButton {
            display: AbstractButton.IconOnly
            icon.source: "qrc:/GeoControls/icons/FlipVertical.svg"
            tooltipText: qsTr("Flip Vertical")
            enabled: panel.hasSelection
            implicitWidth: Fonts.iconButtonSize
            implicitHeight: Fonts.iconButtonSize
            Layout.preferredWidth: implicitWidth
            Layout.preferredHeight: implicitHeight
            Layout.fillWidth: true
            defaultPadding: 0
            onClicked: if (panel.commands)
                panel.commands.flipVertical.trigger()
        }
    }
    CustomButton {
        Layout.fillWidth: true
        text: panel.hasPresenter && panel.presenter.cropToolActive ? qsTr("Done") : qsTr("Crop & Rotate")
        enabled: panel.hasSelection
        onClicked: if (panel.commands)
            panel.commands.toggleCropTool()
    }
    CustomLabel {
        text: qsTr("Drag the frame to crop. Drag outside it, or Option/Alt-drag, to straighten.")
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
        opacity: 0.75
    }
}
