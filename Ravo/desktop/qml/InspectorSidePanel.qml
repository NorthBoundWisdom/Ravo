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

    color: Theme.contentSurfaceColor

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: 1
        color: Theme.dividerColor
        z: 1
    }

    function infoRow(label, value) {
        return label + ": " + (value && value.length ? value : "—");
    }

    Flickable {
        anchors.fill: parent
        clip: true
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
                            text: root.hasPresenter && root.presenter.cropToolActive ? qsTr("Done cropping") : qsTr("Crop overlay")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.toggleCropTool()
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: ["free", "1:1", "3:2", "4:3", "5:4", "16:9"]
                            enabled: root.hasSelection
                            onActivated: if (root.commands)
                                root.commands.setCropAspect(currentText)
                        }
                        CustomSlider {
                            title: qsTr("Crop X")
                            from: 0
                            to: 0.9
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editCropX : 0
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("cropX", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("cropX")
                        }
                        CustomSlider {
                            title: qsTr("Crop Y")
                            from: 0
                            to: 0.9
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editCropY : 0
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("cropY", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("cropY")
                        }
                        CustomSlider {
                            title: qsTr("Crop W")
                            from: 0.1
                            to: 1
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editCropWidth : 1
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("cropWidth", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("cropWidth")
                        }
                        CustomSlider {
                            title: qsTr("Crop H")
                            from: 0.1
                            to: 1
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editCropHeight : 1
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("cropHeight", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("cropHeight")
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
