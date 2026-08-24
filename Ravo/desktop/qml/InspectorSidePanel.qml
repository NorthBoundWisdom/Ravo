import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var colorChoices: []
    property var swatchColor: function (name) { return Theme.midColor }
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
        return label + ": " + (value && value.length ? value : "—")
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
                text: root.presenter && root.presenter.selectedDisplayName.length
                      ? root.presenter.selectedDisplayName : qsTr("No photo selected")
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
                onRatingChangedByUser: function (value) { if (root.hasPresenter) root.presenter.setRating(value) }
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
                            onClicked: if (root.hasPresenter) root.presenter.setColorLabel(modelData)
                        }
                    }
                }
            }

            CustomButton {
                Layout.leftMargin: Fonts.standardMargin
                text: root.hasPresenter && root.presenter.selectedRejected ? qsTr("Unreject") : qsTr("Reject")
                enabled: root.hasSelection
                onClicked: if (root.hasPresenter) root.presenter.toggleRejected()
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
                spacing: Fonts.smallSpacing

                RowLayout {
                    CustomButton { text: qsTr("Undo"); enabled: root.hasPresenter && root.presenter.canUndo; onClicked: if (root.hasPresenter) root.presenter.undoEdit() }
                    CustomButton { text: qsTr("Redo"); enabled: root.hasPresenter && root.presenter.canRedo; onClicked: if (root.hasPresenter) root.presenter.redoEdit() }
                    CustomButton {
                        text: root.hasPresenter && root.presenter.beforeAfter ? qsTr("After") : qsTr("Before")
                        enabled: root.hasSelection
                        onClicked: if (root.hasPresenter) root.presenter.toggleBeforeAfter()
                    }
                    CustomButton {
                        text: qsTr("Reset all")
                        enabled: root.hasSelection
                        onClicked: if (root.hasPresenter) root.presenter.resetAllEdits()
                    }
                }

                CustomLabel { text: qsTr("Geometry"); font.bold: true }
                RowLayout {
                    CustomButton { text: qsTr("Rotate L"); enabled: root.hasSelection; onClicked: if (root.hasPresenter) root.presenter.rotateLeft() }
                    CustomButton { text: qsTr("Rotate R"); enabled: root.hasSelection; onClicked: if (root.hasPresenter) root.presenter.rotateRight() }
                    CustomButton { text: qsTr("Reset"); enabled: root.hasSelection; onClicked: if (root.hasPresenter) root.presenter.resetSection("geometry") }
                }
                CustomSlider {
                    title: qsTr("Crop X"); from: 0; to: 0.9; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editCropX : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("cropX", value) }
                }
                CustomSlider {
                    title: qsTr("Crop Y"); from: 0; to: 0.9; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editCropY : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("cropY", value) }
                }
                CustomSlider {
                    title: qsTr("Crop W"); from: 0.1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editCropWidth : 1; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("cropWidth", value) }
                }
                CustomSlider {
                    title: qsTr("Crop H"); from: 0.1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editCropHeight : 1; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("cropHeight", value) }
                }

                CustomLabel { text: qsTr("White Balance"); font.bold: true }
                CustomSlider {
                    title: qsTr("Temp"); from: 2000; to: 12000; stepSize: 50
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editTemperature : 6500; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("temperature", value) }
                }
                CustomSlider {
                    title: qsTr("Tint"); from: -150; to: 150; stepSize: 1
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editTint : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("tint", value) }
                }
                CustomButton { text: qsTr("Reset WB"); enabled: root.hasSelection; onClicked: if (root.hasPresenter) root.presenter.resetSection("whiteBalance") }

                CustomLabel { text: qsTr("Light"); font.bold: true }
                CustomSlider {
                    title: qsTr("Exposure"); from: -5; to: 5; stepSize: 0.05; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editExposure : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("exposure", value) }
                }
                CustomSlider {
                    title: qsTr("Contrast"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editContrast : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("contrast", value) }
                }
                CustomSlider {
                    title: qsTr("Highlights"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editHighlights : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("highlights", value) }
                }
                CustomSlider {
                    title: qsTr("Shadows"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editShadows : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("shadows", value) }
                }
                CustomSlider {
                    title: qsTr("Whites"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editWhites : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("whites", value) }
                }
                CustomSlider {
                    title: qsTr("Blacks"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editBlacks : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("blacks", value) }
                }
                CustomButton { text: qsTr("Reset light"); enabled: root.hasSelection; onClicked: if (root.hasPresenter) root.presenter.resetSection("light") }

                CustomLabel { text: qsTr("Color"); font.bold: true }
                CustomSlider {
                    title: qsTr("Vibrance"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVibrance : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("vibrance", value) }
                }
                CustomSlider {
                    title: qsTr("Saturation"); from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSaturation : 0; delayedCommit: true
                    onValueCommitted: function (value) { if (root.hasPresenter) root.presenter.setDevelopNumber("saturation", value) }
                }
                CustomButton { text: qsTr("Reset color"); enabled: root.hasSelection; onClicked: if (root.hasPresenter) root.presenter.resetSection("color") }
            }
        }
    }
}
