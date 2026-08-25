import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property var presenter
    property var commands
    property bool liveReady: false
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    spacing: Fonts.smallSpacing

    component DevelopSection: CustomEditPanel {
        showAddButton: false
        showDeleteButton: false
        showApplyButton: false
        actionsNeedEditing: false
        actionButtonsEnabled: root.hasSelection
        editing: root.hasSelection
        resetTooltip: qsTr("Reset this section")
        property string sectionId
        onReset: function () {
            if (root.commands && sectionId.length)
                root.commands.resetSection(sectionId);
        }
    }

            CustomLabel {
                Layout.leftMargin: Fonts.standardMargin
                Layout.topMargin: Fonts.size8
                text: qsTr("Develop")
                font.bold: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                Layout.rightMargin: Fonts.standardMargin
                Layout.bottomMargin: Fonts.size12
                spacing: Fonts.smallSpacing

                RowLayout {
                    CustomButton {
                        text: qsTr("Undo")
                        enabled: root.hasPresenter && root.presenter.canUndo
                        onClicked: if (root.commands)
                            root.commands.undo.trigger()
                    }
                    CustomButton {
                        text: qsTr("Redo")
                        enabled: root.hasPresenter && root.presenter.canRedo
                        onClicked: if (root.commands)
                            root.commands.redo.trigger()
                    }
                    CustomButton {
                        text: root.hasPresenter && root.presenter.beforeAfter ? qsTr("After") : qsTr("Before")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.beforeAfter.trigger()
                    }
                    CustomButton {
                        text: qsTr("Reset all")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.resetEdits.trigger()
                    }
                }

                DevelopSection {
                    title: qsTr("Geometry")
                    sectionId: "geometry"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        spacing: Fonts.smallSpacing
                        RowLayout {
                            CustomButton {
                                text: qsTr("Rotate L")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.rotateLeft.trigger()
                            }
                            CustomButton {
                                text: qsTr("Rotate R")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.rotateRight.trigger()
                            }
                            CustomButton {
                                text: qsTr("Flip H")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.flipHorizontal.trigger()
                            }
                            CustomButton {
                                text: qsTr("Flip V")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.flipVertical.trigger()
                            }
                        }
                        CustomButton {
                            text: root.hasPresenter && root.presenter.cropToolActive ? qsTr("Done") : qsTr("Crop & Rotate")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.toggleCropTool()
                        }
                        CustomLabel {
                            text: qsTr("Drag the frame to crop. Drag outside it, or Option/Alt-drag, to straighten.")
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            opacity: 0.75
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: ["free", "1:1", "3:2", "4:3", "5:4", "16:9"]
                            enabled: root.hasSelection
                            currentIndex: {
                                const aspects = ["free", "1:1", "3:2", "4:3", "5:4", "16:9"];
                                const current = root.hasPresenter ? root.presenter.cropAspect : "free";
                                const index = aspects.indexOf(current);
                                return index < 0 ? 0 : index;
                            }
                            onActivated: if (root.commands)
                                root.commands.setCropAspect(currentText)
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Angle")
                            from: -45
                            to: 45
                            stepSize: 0.1
                            validatorDecimals: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editStraighten : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("straighten", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("straighten", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("straighten")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("White Balance")
                    sectionId: "whiteBalance"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Temp")
                            from: 2000
                            to: 12000
                            stepSize: 50
                            validatorDecimals: 0
                            showReset: true
                            resetValue: 6500
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editTemperature : 6500
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("temperature", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("temperature", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("temperature")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Tint")
                            from: -150
                            to: 150
                            stepSize: 1
                            validatorDecimals: 0
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editTint : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("tint", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("tint", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("tint")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Light")
                    sectionId: "light"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Exposure")
                            from: -5
                            to: 5
                            stepSize: 0.05
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editExposure : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("exposure", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("exposure", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("exposure")
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            text: qsTr("Sigmoid Display · Standard SDR")
                            font.bold: true
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Contrast")
                            from: 0.1
                            to: 10
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 1.5
                            delayedCommit: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSigmoidContrast : 1.5
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sigmoidContrast", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sigmoidContrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sigmoidContrast")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Skew")
                            from: -1
                            to: 1
                            stepSize: 0.02
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSigmoidSkew : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sigmoidSkew", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sigmoidSkew", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sigmoidSkew")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Preserve Hue")
                            from: 0
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSigmoidHuePreservation : 1
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sigmoidHuePreservation", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sigmoidHuePreservation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sigmoidHuePreservation")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Contrast")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            visible: root.hasPresenter && !root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editContrast : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("contrast", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("contrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("contrast")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlights")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editHighlights : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("highlights", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("highlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("highlights")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Shadows")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editShadows : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("shadows", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("shadows", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("shadows")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Whites")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editWhites : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("whites", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("whites", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("whites")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Blacks")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editBlacks : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("blacks", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("blacks", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("blacks")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Gamma")
                            from: 0.2
                            to: 3
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGamma : 1
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("gamma", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("gamma", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("gamma")
                        }
                        CustomLabel {
                            text: qsTr("Tone Curve")
                            Layout.fillWidth: true
                        }
                        ToneCurveEditor {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 200
                            editorEnabled: root.hasSelection
                            points: root.hasPresenter ? root.presenter.editToneCurve : [
                                {
                                    "x": 0,
                                    "y": 0
                                },
                                {
                                    "x": 1,
                                    "y": 1
                                }
                            ]
                            samples: root.hasPresenter ? root.presenter.editToneCurveSamples : []
                            onCurveEdited: function (points) {
                                if (root.commands)
                                    root.commands.previewToneCurve(points);
                            }
                            onCurveCommitted: function (points) {
                                if (root.commands)
                                    root.commands.setToneCurve(points);
                            }
                        }
                        CustomLabel {
                            text: qsTr("Drag points to reshape. Click to add. Double-click an interior point to remove it.")
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            opacity: 0.75
                        }
                        CustomButton {
                            text: qsTr("Reset curve")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetControl("toneCurve")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Color")
                    sectionId: "color"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Vibrance")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVibrance : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("vibrance", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("vibrance", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("vibrance")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Saturation")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSaturation : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("saturation", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("saturation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("saturation")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Velvia")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVelvia : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("velvia", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("velvia", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("velvia")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Lift")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editLift : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("lift", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("lift", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("lift")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Color gamma")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorGamma : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("colorGamma", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorGamma", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorGamma")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Gain")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGain : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("gain", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("gain", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("gain")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Color contrast")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorContrast : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("colorContrast", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorContrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorContrast")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Monochrome")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editMonochrome : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("monochrome", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("monochrome", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("monochrome")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Split amount")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitAmount : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitAmount", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitAmount", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitAmount")
                        }
                        HueSlider {
                            Layout.fillWidth: true
                            title: qsTr("Shadow hue")
                            showReset: true
                            resetValue: 0.55
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitShadowsHue : 0.55
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitShadowsHue", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitShadowsHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitShadowsHue")
                        }
                        HueSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlight hue")
                            showReset: true
                            resetValue: 0.08
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitHighlightsHue : 0.08
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitHighlightsHue", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitHighlightsHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitHighlightsHue")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Split balance")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0.5
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitBalance : 0.5
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitBalance", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitBalance", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitBalance")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Detail")
                    sectionId: "detail"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Sharpen")
                            from: 0
                            to: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSharpen : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sharpen", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sharpen", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sharpen")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Radius")
                            from: 0
                            to: 12
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSharpenRadius : 1
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sharpenRadius", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sharpenRadius", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sharpenRadius")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Clarity")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editClarity : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("clarity", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("clarity", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("clarity")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Grain")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGrain : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("grain", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("grain", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("grain")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Effects")
                    sectionId: "effects"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Vignette")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVignette : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("vignette", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("vignette", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("vignette")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Bloom")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editBloom : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("bloom", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("bloom", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("bloom")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Soften")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSoften : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("soften", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("soften", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("soften")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Dehaze")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editDehaze : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("dehaze", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("dehaze", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("dehaze")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("RAW / Denoise / Lens")
                    sectionId: "raw"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlight reconstruction")
                            from: 0
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editRawHighlights : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("rawHighlights", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("rawHighlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("rawHighlights")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Denoise")
                            from: 0
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editDenoise : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("denoise", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("denoise", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("denoise")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Lens distortion")
                            from: -1
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editLensK1 : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("lensK1", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("lensK1", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("lensK1")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Lens vignetting")
                            from: 0
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editLensVignetting : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("lensVignetting", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("lensVignetting", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("lensVignetting")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Tone equalizer")
                    sectionId: "toneEqual"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Blacks")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqBlacks : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqBlacks", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqBlacks", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqBlacks")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Shadows")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqShadows : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqShadows", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqShadows", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqShadows")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Midtones")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqMidtones : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqMidtones", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqMidtones", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqMidtones")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlights")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqHighlights : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqHighlights", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqHighlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqHighlights")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Whites")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqWhites : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqWhites", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqWhites", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqWhites")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Graduated ND / Color EQ")
                    sectionId: "graduated"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Graduated density")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGraduatedDensity : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("graduatedDensity", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("graduatedDensity", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("graduatedDensity")
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
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGraduatedRotation : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("graduatedRotation", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("graduatedRotation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("graduatedRotation")
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: ["0", "1", "2", "3", "4", "5", "6", "7"]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editColorEqBand : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("colorEqBand", Number(currentText))
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Band saturation")
                            from: -1
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorEqSat : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("colorEqSat", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorEqSat", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorEqSat")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Band hue")
                            from: -0.25
                            to: 0.25
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorEqHue : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("colorEqHue", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorEqHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorEqHue")
                        }
                    }
                }
            }
}
