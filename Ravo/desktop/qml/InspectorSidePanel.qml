import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    property var colorChoices: []
    property var swatchColor: function (name) {
        return Theme.midColor;
    }
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    readonly property bool developOpen: hasPresenter && presenter.browseMode === "develop"

    color: Theme.railSurfaceColor

    function infoRow(label, value) {
        return label + ": " + (value && value.length ? value : "—");
    }

    Flickable {
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        contentWidth: width
        contentHeight: column.implicitHeight

        ColumnLayout {
            id: column
            width: parent.width
            spacing: Fonts.smallSpacing

            CustomLabel {
                Layout.leftMargin: Fonts.standardMargin
                Layout.topMargin: Fonts.size12
                text: qsTr("Photo")
                font.bold: true
            }

            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                Layout.rightMargin: Fonts.standardMargin
                wrapMode: Text.WrapAnywhere
                elide: Text.ElideMiddle
                text: root.presenter && root.presenter.selectedDisplayName.length ? root.presenter.selectedDisplayName : qsTr("No photo selected")
            }

            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                Layout.rightMargin: Fonts.standardMargin
                wrapMode: Text.WrapAnywhere
                color: Theme.placeholderTextColor
                text: root.infoRow(qsTr("Folder"), root.presenter ? root.presenter.selectedFolderPath : "")
            }
            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                color: Theme.placeholderTextColor
                text: root.infoRow(qsTr("Type"), root.presenter ? root.presenter.selectedMediaType : "")
            }
            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                color: Theme.placeholderTextColor
                text: root.infoRow(qsTr("Size"), root.presenter ? root.presenter.selectedDimensions : "")
            }
            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                color: Theme.placeholderTextColor
                text: root.infoRow(qsTr("File"), root.presenter ? root.presenter.selectedFileSize : "")
            }
            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                color: Theme.placeholderTextColor
                text: root.presenter && root.presenter.selectedHasEdits ? qsTr("Edited") : qsTr("No edits")
            }
            CustomLabel {
                Layout.fillWidth: true
                Layout.leftMargin: Fonts.standardMargin
                Layout.rightMargin: Fonts.standardMargin
                wrapMode: Text.WrapAnywhere
                color: Theme.placeholderTextColor
                font.pixelSize: Fonts.size10
                text: root.presenter ? root.presenter.selectedUri : ""
            }

            CustomLabel {
                Layout.leftMargin: Fonts.standardMargin
                Layout.topMargin: Fonts.size8
                text: qsTr("Review")
                font.bold: true
            }

            RatingControl {
                Layout.leftMargin: Fonts.standardMargin
                enabled: root.hasSelection
                rating: root.hasPresenter ? root.presenter.selectedRating : 0
                onRatingChangedByUser: function (value) {
                    if (root.commands)
                        root.commands.setRating(value);
                }
            }

            RowLayout {
                Layout.leftMargin: Fonts.standardMargin
                spacing: Fonts.size8
                Repeater {
                    model: ["none"].concat(root.colorChoices)
                    delegate: Rectangle {
                        required property string modelData
                        width: 18
                        height: 18
                        radius: 9
                        color: root.swatchColor(modelData)
                        border.width: root.hasPresenter && root.presenter.selectedColorLabel === modelData ? 2 : 1
                        border.color: Theme.textColor
                        MouseArea {
                            anchors.fill: parent
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.setColorLabel(modelData)
                        }
                    }
                }
            }

            CustomButton {
                Layout.leftMargin: Fonts.standardMargin
                text: root.hasPresenter && root.presenter.selectedRejected ? qsTr("Unreject") : qsTr("Reject")
                enabled: root.hasSelection
                onClicked: if (root.commands)
                    root.commands.reject.trigger()
            }

            CustomLabel {
                visible: root.developOpen
                Layout.leftMargin: Fonts.standardMargin
                Layout.topMargin: Fonts.size8
                text: qsTr("Develop")
                font.bold: true
            }

            ColumnLayout {
                visible: root.developOpen
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

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Geometry")
                    expanded: true
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
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("straighten", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("straighten", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("straighten")
                        }
                        CustomButton {
                            text: qsTr("Reset geometry")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetSection("geometry")
                        }
                    }
                }

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("White Balance")
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
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
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("temperature", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("temperature", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("temperature")
                        }
                        CustomSlider {
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
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("tint", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("tint", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("tint")
                        }
                        CustomButton {
                            text: qsTr("Reset WB")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetSection("whiteBalance")
                        }
                    }
                }

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Light")
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            title: qsTr("Exposure")
                            from: -5
                            to: 5
                            stepSize: 0.05
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editExposure : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("exposure", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("exposure", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("exposure")
                        }
                        CustomSlider {
                            title: qsTr("Contrast")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editContrast : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("contrast", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("contrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("contrast")
                        }
                        CustomSlider {
                            title: qsTr("Highlights")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editHighlights : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("highlights", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("highlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("highlights")
                        }
                        CustomSlider {
                            title: qsTr("Shadows")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editShadows : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("shadows", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("shadows", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("shadows")
                        }
                        CustomSlider {
                            title: qsTr("Whites")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editWhites : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("whites", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("whites", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("whites")
                        }
                        CustomSlider {
                            title: qsTr("Blacks")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editBlacks : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("blacks", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("blacks", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("blacks")
                        }
                        CustomSlider {
                            title: qsTr("Gamma")
                            from: 0.2
                            to: 3
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGamma : 1
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("gamma", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("gamma", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("gamma")
                        }
                        CustomButton {
                            text: qsTr("Reset light")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetSection("light")
                        }
                    }
                }

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Color")
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            title: qsTr("Vibrance")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVibrance : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("vibrance", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("vibrance", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("vibrance")
                        }
                        CustomSlider {
                            title: qsTr("Saturation")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSaturation : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("saturation", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("saturation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("saturation")
                        }
                        CustomSlider {
                            title: qsTr("Velvia")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVelvia : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("velvia", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("velvia", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("velvia")
                        }
                        CustomSlider {
                            title: qsTr("Lift")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editLift : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("lift", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("lift", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("lift")
                        }
                        CustomSlider {
                            title: qsTr("Color gamma")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorGamma : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("colorGamma", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorGamma", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorGamma")
                        }
                        CustomSlider {
                            title: qsTr("Gain")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGain : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("gain", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("gain", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("gain")
                        }
                        CustomSlider {
                            title: qsTr("Color contrast")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorContrast : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("colorContrast", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorContrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorContrast")
                        }
                        CustomSlider {
                            title: qsTr("Monochrome")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editMonochrome : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("monochrome", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("monochrome", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("monochrome")
                        }
                        CustomSlider {
                            title: qsTr("Split amount")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitAmount : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("splitAmount", value);
                            }
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
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("splitShadowsHue", value);
                            }
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
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("splitHighlightsHue", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitHighlightsHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitHighlightsHue")
                        }
                        CustomSlider {
                            title: qsTr("Split balance")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0.5
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitBalance : 0.5
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("splitBalance", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitBalance", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitBalance")
                        }
                        CustomButton {
                            text: qsTr("Reset color")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetSection("color")
                        }
                    }
                }

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Detail")
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            title: qsTr("Sharpen")
                            from: 0
                            to: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSharpen : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("sharpen", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sharpen", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sharpen")
                        }
                        CustomSlider {
                            title: qsTr("Radius")
                            from: 0
                            to: 12
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSharpenRadius : 1
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("sharpenRadius", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sharpenRadius", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sharpenRadius")
                        }
                        CustomSlider {
                            title: qsTr("Clarity")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editClarity : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("clarity", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("clarity", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("clarity")
                        }
                        CustomSlider {
                            title: qsTr("Grain")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGrain : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("grain", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("grain", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("grain")
                        }
                        CustomButton {
                            text: qsTr("Reset detail")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetSection("detail")
                        }
                    }
                }

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Effects")
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            title: qsTr("Vignette")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVignette : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("vignette", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("vignette", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("vignette")
                        }
                        CustomSlider {
                            title: qsTr("Bloom")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editBloom : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("bloom", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("bloom", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("bloom")
                        }
                        CustomSlider {
                            title: qsTr("Soften")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSoften : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("soften", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("soften", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("soften")
                        }
                        CustomSlider {
                            title: qsTr("Dehaze")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editDehaze : 0
                            onValueEdited: function (value) {
                                if (root.commands)
                                    root.commands.previewDevelopNumber("dehaze", value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("dehaze", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("dehaze")
                        }
                        CustomButton {
                            text: qsTr("Reset effects")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetSection("effects")
                        }
                    }
                }
            }
        }
    }
}
