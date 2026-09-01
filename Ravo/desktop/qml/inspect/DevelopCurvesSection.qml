pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Curves")
    sectionId: "curves"
    ColumnLayout {
        id: curveControls
        Layout.fillWidth: true
        width: parent.width
        spacing: Fonts.smallSpacing
        property bool editRegions: false
        readonly property bool rgbFamily: !panel.hasPresenter || panel.presenter.editCurve.familyIndex === 0
        readonly property bool masterChannel: !panel.hasPresenter || panel.presenter.editCurve.channel === 0
        readonly property bool regionsAvailable: rgbFamily && masterChannel && (!panel.hasPresenter || panel.presenter.editCurve.linked)
        readonly property color activeCurveColor: {
            if (!panel.hasPresenter || panel.presenter.editCurve.channel === 0)
                return Theme.textColor;
            if (panel.presenter.editCurve.familyIndex === 1)
                return panel.presenter.editCurve.channel === 1 ? "#d97bdc" : "#64c7c9";
            if (panel.presenter.editCurve.channel === 1)
                return "#ed6a70";
            if (panel.presenter.editCurve.channel === 2)
                return "#65c982";
            return "#6f9df4";
        }
        readonly property string activeChannelLabel: {
            if (!panel.hasPresenter)
                return qsTr("RGB");
            if (panel.presenter.editCurve.familyIndex === 1)
                return [qsTr("Master"), qsTr("a"), qsTr("b")][panel.presenter.editCurve.channel];
            return [qsTr("RGB"), qsTr("Red"), qsTr("Green"), qsTr("Blue")][panel.presenter.editCurve.channel];
        }

        onRegionsAvailableChanged: if (!regionsAvailable)
            editRegions = false

        RowLayout {
            objectName: "curveFamily"
            Layout.fillWidth: true
            spacing: Fonts.size6

            CustomLabel {
                text: qsTr("Curve")
                opacity: 0.72
                Layout.preferredWidth: Fonts.size60
            }
            CurveOptionButton {
                objectName: "curveFamilyRgb"
                Layout.fillWidth: true
                text: qsTr("RGB")
                selected: curveControls.rgbFamily
                enabled: panel.hasSelection
                onClicked: {
                    curveControls.editRegions = false;
                    if (panel.hasPresenter)
                        panel.presenter.setCurveFamily(0);
                }
            }
            CurveOptionButton {
                objectName: "curveFamilyTone"
                Layout.fillWidth: true
                text: qsTr("Tone")
                selected: !curveControls.rgbFamily
                enabled: panel.hasSelection
                onClicked: {
                    curveControls.editRegions = false;
                    if (panel.hasPresenter)
                        panel.presenter.setCurveFamily(1);
                }
            }
        }

        RowLayout {
            objectName: "curveChannel"
            Layout.fillWidth: true
            spacing: Fonts.size6

            CustomLabel {
                text: qsTr("Channel")
                opacity: 0.72
                Layout.preferredWidth: Fonts.size60
            }
            Repeater {
                model: curveControls.rgbFamily ? [
                    {
                        "title": qsTr("RGB"),
                        "color": Theme.textColor
                    },
                    {
                        "title": qsTr("R"),
                        "color": "#ed6a70"
                    },
                    {
                        "title": qsTr("G"),
                        "color": "#65c982"
                    },
                    {
                        "title": qsTr("B"),
                        "color": "#6f9df4"
                    }
                ] : [
                    {
                        "title": qsTr("Master"),
                        "color": Theme.textColor
                    },
                    {
                        "title": qsTr("a"),
                        "color": "#d97bdc"
                    },
                    {
                        "title": qsTr("b"),
                        "color": "#64c7c9"
                    }
                ]
                delegate: CurveOptionButton {
                    required property int index
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.title
                    selectionColor: modelData.color
                    selected: (panel.hasPresenter ? panel.presenter.editCurve.channel : 0) === index
                    enabled: panel.hasSelection
                    onClicked: {
                        curveControls.editRegions = false;
                        if (panel.hasPresenter)
                            panel.presenter.setCurveChannel(index);
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size6

            CustomLabel {
                text: qsTr("Edit")
                opacity: 0.72
                Layout.preferredWidth: Fonts.size60
            }
            CurveOptionButton {
                objectName: "curvePointMode"
                Layout.fillWidth: true
                text: qsTr("Point")
                selected: !curveControls.editRegions
                enabled: panel.hasSelection
                onClicked: curveControls.editRegions = false
            }
            CurveOptionButton {
                objectName: "curveParametricMode"
                Layout.fillWidth: true
                visible: curveControls.regionsAvailable
                text: qsTr("Parametric")
                selected: curveControls.editRegions
                enabled: panel.hasSelection
                onClicked: curveControls.editRegions = true
            }
            CustomButton {
                objectName: "resetActiveCurve"
                text: qsTr("Reset curve")
                defaultHeight: Fonts.inputFieldHeight
                enabled: panel.hasSelection
                onClicked: if (panel.commands)
                    panel.commands.resetControl(curveControls.rgbFamily ? "rgbCurve" : "toneCurve")
            }
        }

        ToneCurveEditor {
            id: curveEditor
            objectName: "curveEditor"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Fonts.size200, Math.min(Fonts.size300, width * 0.72))
            editorEnabled: panel.hasSelection
            curveColor: curveControls.activeCurveColor
            channelLabel: curveControls.activeChannelLabel
            showRegionSplits: curveControls.editRegions && curveControls.regionsAvailable
            regionSplits: panel.hasPresenter ? [panel.presenter.editCurve.split0, panel.presenter.editCurve.split1, panel.presenter.editCurve.split2] : [0.25, 0.5, 0.75]
            histogramMode: panel.hasPresenter ? panel.presenter.editCurve.histogramMode : "rgb"
            histogramRed: panel.hasPresenter ? panel.presenter.scopeHistogramRed : []
            histogramGreen: panel.hasPresenter ? panel.presenter.scopeHistogramGreen : []
            histogramBlue: panel.hasPresenter ? panel.presenter.scopeHistogramBlue : []
            histogramLuma: panel.hasPresenter ? panel.presenter.scopeHistogramLuma : []
            histogramMax: panel.hasPresenter ? panel.presenter.scopeHistogramMax : 0
            points: panel.hasPresenter ? panel.presenter.editCurvePoints : [
                {
                    "x": 0,
                    "y": 0
                },
                {
                    "x": 1,
                    "y": 1
                }
            ]
            samples: panel.hasPresenter ? panel.presenter.editCurveSamples : []
            onCurveEdited: function (points) {
                if (panel.commands)
                    panel.commands.previewCurve(panel.hasPresenter && panel.presenter.editCurve.familyIndex === 1 ? "tone" : "rgb", panel.hasPresenter ? panel.presenter.editCurve.channel : 0, points);
            }
            onCurveCommitted: function (points) {
                if (panel.commands)
                    panel.commands.setCurve(panel.hasPresenter && panel.presenter.editCurve.familyIndex === 1 ? "tone" : "rgb", panel.hasPresenter ? panel.presenter.editCurve.channel : 0, points);
            }
        }
        CustomLabel {
            text: qsTr("Drag points to reshape. Click to add. Double-click or Delete removes an interior point. Arrow keys nudge the selected point.")
            wrapMode: Text.WordWrap
            Layout.fillWidth: true
            visible: !curveControls.editRegions
            opacity: 0.75
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: curveControls.editRegions && curveControls.regionsAvailable
            spacing: Fonts.smallSpacing

            CustomLabel {
                text: qsTr("Parametric regions")
                font: Fonts.makeBoldFont(Fonts.standardFont)
                Layout.fillWidth: true
            }
            Repeater {
                model: [
                    {
                        "title": qsTr("Shadows"),
                        "key": "parametricShadows",
                        "field": "rgbCurveShadows"
                    },
                    {
                        "title": qsTr("Darks"),
                        "key": "parametricDarks",
                        "field": "rgbCurveDarks"
                    },
                    {
                        "title": qsTr("Lights"),
                        "key": "parametricLights",
                        "field": "rgbCurveLights"
                    },
                    {
                        "title": qsTr("Highlights"),
                        "key": "parametricHighlights",
                        "field": "rgbCurveHighlights"
                    }
                ]
                delegate: CustomSlider {
                    required property var modelData
                    Layout.fillWidth: true
                    title: modelData.title
                    from: -1
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: panel.hasSelection
                    value: panel.hasPresenter ? panel.presenter.editCurve[modelData.key] : 0
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

        CustomEditPanel {
            Layout.fillWidth: true
            title: qsTr("Curve settings")
            initialExpanded: false
            showAddButton: false
            showDeleteButton: false
            showResetButton: false
            showApplyButton: false
            panelColor: Theme.alternateBaseColor
            titleBarColor: Theme.contentSurfaceColor
            borderColor: Theme.lightColor
            borderWidth: ControlState.borderFocus
            padding: Fonts.size10

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Fonts.size8
                rowSpacing: Fonts.size8

                CustomLabel {
                    text: qsTr("Interpolation")
                    opacity: 0.72
                }
                CustomComboBox {
                    objectName: "curveInterpolation"
                    Layout.fillWidth: true
                    model: [qsTr("Monotonic"), qsTr("Centripetal"), qsTr("Cubic")]
                    enabled: panel.hasSelection
                    currentIndex: panel.hasPresenter ? panel.presenter.editCurve.interpolationIndex : 0
                    onActivated: if (panel.commands)
                        panel.commands.setDevelopNumber(curveControls.rgbFamily ? "rgbCurveInterpolation" : "toneCurveInterpolation", currentIndex)
                }

                CustomLabel {
                    visible: !curveControls.rgbFamily
                    text: qsTr("Working space")
                    opacity: 0.72
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    visible: !curveControls.rgbFamily
                    model: [qsTr("RGB, linked"), qsTr("Lab"), qsTr("XYZ"), qsTr("Lab independent"), qsTr("sRGB"), qsTr("Linear RGB")]
                    enabled: panel.hasSelection
                    currentIndex: panel.hasPresenter ? panel.presenter.editCurve.workingSpaceIndex : 0
                    onActivated: if (panel.commands)
                        panel.commands.setDevelopNumber("toneCurveWorkingSpace", currentIndex)
                }

                CustomLabel {
                    visible: curveControls.masterChannel && (!panel.hasPresenter || panel.presenter.editCurve.linked)
                    text: qsTr("Preserve colors")
                    opacity: 0.72
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    visible: curveControls.masterChannel && (!panel.hasPresenter || panel.presenter.editCurve.linked)
                    model: [qsTr("None"), qsTr("Luminance"), qsTr("Max RGB"), qsTr("Average RGB"), qsTr("Sum RGB"), qsTr("Norm RGB"), qsTr("Basic power")]
                    enabled: panel.hasSelection
                    currentIndex: panel.hasPresenter ? panel.presenter.editCurve.preserveIndex : 1
                    onActivated: if (panel.commands)
                        panel.commands.setDevelopNumber(curveControls.rgbFamily ? "rgbCurvePreserve" : "toneCurvePreserve", currentIndex)
                }

                CustomCheckBox {
                    Layout.columnSpan: 2
                    text: qsTr("Compensate middle grey")
                    visible: curveControls.rgbFamily
                    enabled: panel.hasSelection
                    checked: panel.hasPresenter && panel.presenter.editCurve.compensate
                    onToggled: if (panel.commands)
                        panel.commands.setDevelopNumber("rgbCurveCompensate", checked ? 1 : 0)
                }

                ColumnLayout {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    visible: curveControls.regionsAvailable
                    spacing: Fonts.smallSpacing

                    CustomLabel {
                        text: qsTr("Region boundaries")
                        font: Fonts.makeBoldFont(Fonts.standardFont)
                        Layout.fillWidth: true
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Shadows / Darks"),
                                "key": "split0",
                                "field": "rgbCurveSplit0",
                                "reset": 0.25
                            },
                            {
                                "title": qsTr("Darks / Lights"),
                                "key": "split1",
                                "field": "rgbCurveSplit1",
                                "reset": 0.5
                            },
                            {
                                "title": qsTr("Lights / Highlights"),
                                "key": "split2",
                                "field": "rgbCurveSplit2",
                                "reset": 0.75
                            }
                        ]
                        delegate: CustomSlider {
                            required property var modelData
                            Layout.fillWidth: true
                            title: modelData.title
                            from: 0.05
                            to: 0.95
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: modelData.reset
                            delayedCommit: true
                            enabled: panel.hasSelection
                            value: panel.hasPresenter ? panel.presenter.editCurve[modelData.key] : modelData.reset
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
        }
    }
}
