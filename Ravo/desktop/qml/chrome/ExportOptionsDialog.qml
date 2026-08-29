import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "ExportOptionsDialog"
    titleText: batchMode ? qsTr("Export Selected Photos") : qsTr("Export Photo")
    width: Fonts.messageDialogWidth
    bodyFillHeight: false
    showCloseButton: true

    required property var presenter
    readonly property bool batchMode: presenter && presenter.selectedCount > 1
    property string filenameTemplate: "{stem}-{sequence}{ext}"
    property string formatId: ""
    property string jpegSubsamplingId: ""
    property string pngBitDepthId: ""
    property string tiffSampleTypeId: ""
    property string tiffCompressionId: ""
    property string metadataModeId: ""
    property double jpegQuality: 0
    property double pngCompression: 0
    property double tiffCompressionLevel: 0
    property bool tiffGrayscaleIfNeutral: false
    property double tiffResolutionDpi: 0

    property var formatChoices: presenter.exportFormatChoices()
    property var jpegSubsamplingChoices: presenter.jpegSubsamplingChoices()
    property var pngBitDepthChoices: presenter.pngBitDepthChoices()
    property var tiffSampleTypeChoices: presenter.tiffSampleTypeChoices()
    property var tiffCompressionChoices: presenter.tiffCompressionChoices()
    property var metadataModeChoices: presenter.exportMetadataModeChoices()
    readonly property var optionBounds: presenter.exportOptionBounds()
    readonly property bool tiffLevelEnabled: formatId === "tiff" && tiffCompressionId !== "none"
    readonly property bool canContinue: formatId.length > 0 && (!batchMode || filenameTemplate.trim().length > 0) && (formatId === "original" || metadataModeId.length > 0) && (formatId !== "jpeg" || jpegSubsamplingId.length > 0) && (formatId !== "png" || pngBitDepthId.length > 0) && (formatId !== "tiff" || (tiffSampleTypeId.length > 0 && tiffCompressionId.length > 0))

    signal exportAccepted(string format, var options, string filenameTemplate)
    signal exportCanceled

    onCloseRequested: function (reason) {
        root.exportCanceled();
    }

    function choiceIndex(choices, id) {
        for (var i = 0; i < choices.length; ++i) {
            if (choices[i].id === id)
                return i;
        }
        return -1;
    }

    function resetFromPresenter() {
        const defaults = presenter.exportDefaultOptions();
        formatChoices = presenter.exportFormatChoices();
        jpegSubsamplingChoices = presenter.jpegSubsamplingChoices();
        pngBitDepthChoices = presenter.pngBitDepthChoices();
        tiffSampleTypeChoices = presenter.tiffSampleTypeChoices();
        tiffCompressionChoices = presenter.tiffCompressionChoices();
        metadataModeChoices = presenter.exportMetadataModeChoices();
        formatId = defaults.format;
        jpegQuality = defaults.quality;
        jpegSubsamplingId = defaults.jpegSubsampling;
        pngBitDepthId = defaults.pngBitDepth;
        pngCompression = defaults.pngCompression;
        tiffSampleTypeId = defaults.tiffSampleType;
        tiffCompressionId = defaults.tiffCompression;
        tiffCompressionLevel = defaults.tiffCompressionLevel;
        tiffGrayscaleIfNeutral = defaults.tiffGrayscaleIfNeutral;
        tiffResolutionDpi = defaults.tiffResolutionDpi;
        metadataModeId = defaults.metadataMode;
        filenameTemplate = "{stem}-{sequence}{ext}";
    }

    function selectedOptions() {
        if (formatId === "jpeg")
            return {
                "quality": jpegQualitySpin.realValue,
                "jpegSubsampling": jpegSubsamplingId,
                "metadataMode": metadataModeId
            };
        if (formatId === "png")
            return {
                "pngBitDepth": pngBitDepthId,
                "pngCompression": pngCompressionSpin.realValue,
                "metadataMode": metadataModeId
            };
        if (formatId === "tiff")
            return {
                "tiffSampleType": tiffSampleTypeId,
                "tiffCompression": tiffCompressionId,
                "tiffCompressionLevel": tiffCompressionLevelSpin.realValue,
                "tiffGrayscaleIfNeutral": tiffGrayscaleIfNeutral,
                "tiffResolutionDpi": tiffResolutionSpin.realValue,
                "metadataMode": metadataModeId
            };
        return {};
    }

    function openForExport() {
        resetFromPresenter();
        openDialog();
        Qt.callLater(function () {
            formatCombo.forceActiveFocus();
        });
    }

    function acceptExport() {
        if (!root.canContinue)
            return;
        const format = formatId;
        const options = selectedOptions();
        close();
        root.exportAccepted(format, options, filenameTemplate);
    }

    function cancelExport() {
        close();
        root.exportCanceled();
    }

    bodyItem: ColumnLayout {
        id: body
        spacing: Fonts.size10
        width: parent ? parent.width : Fonts.messageDialogWidth

        Keys.onEscapePressed: root.cancelExport()
        Keys.onReturnPressed: root.acceptExport()
        Keys.onEnterPressed: root.acceptExport()

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            visible: root.batchMode

            CustomLabel {
                text: qsTr("Filename template")
                Accessible.name: qsTr("Batch export filename template")
            }
            CustomTextField {
                id: filenameTemplateField
                objectName: "exportFilenameTemplate"
                Layout.fillWidth: true
                Layout.preferredHeight: Fonts.inputFieldHeight
                showEmptyIndicator: true
                showClipIndicator: false
                alignRightWhenFocused: false
                text: root.filenameTemplate
                placeholderText: qsTr("{stem}-{sequence}{ext}")
                Accessible.name: qsTr("Batch export filename template")
                onTextChanged: if (root.filenameTemplate !== text)
                    root.filenameTemplate = text
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin

            CustomLabel {
                text: qsTr("Format")
                Accessible.name: qsTr("Format")
            }
            CustomComboBox {
                id: formatCombo
                objectName: "exportFormat"
                Layout.fillWidth: true
                textRole: "label"
                model: root.formatChoices
                currentIndex: root.choiceIndex(root.formatChoices, root.formatId)
                Accessible.name: qsTr("Format")
                onActivated: function (index) {
                    root.formatId = model[index].id;
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            visible: root.formatId !== "original"

            CustomLabel {
                text: qsTr("Metadata")
                Accessible.name: qsTr("Metadata privacy")
            }
            CustomComboBox {
                id: metadataModeCombo
                objectName: "metadataMode"
                Layout.fillWidth: true
                textRole: "label"
                model: root.metadataModeChoices
                currentIndex: root.choiceIndex(root.metadataModeChoices, root.metadataModeId)
                Accessible.name: qsTr("Metadata privacy")
                onActivated: function (index) {
                    root.metadataModeId = model[index].id;
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Fonts.size8
            visible: root.formatId === "jpeg"

            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Quality")
                    Accessible.name: qsTr("JPEG quality")
                }
                CustomSpinBox {
                    id: jpegQualitySpin
                    objectName: "jpegQuality"
                    Layout.fillWidth: true
                    decimals: 0
                    realFrom: root.optionBounds.jpegQualityMin
                    realTo: root.optionBounds.jpegQualityMax
                    realValue: root.jpegQuality
                    Accessible.name: qsTr("JPEG quality")
                    onEditingCommitted: function (value) {
                        root.jpegQuality = value;
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Subsampling")
                    Accessible.name: qsTr("JPEG subsampling")
                }
                CustomComboBox {
                    id: jpegSubsamplingCombo
                    objectName: "jpegSubsampling"
                    Layout.fillWidth: true
                    textRole: "label"
                    model: root.jpegSubsamplingChoices
                    currentIndex: root.choiceIndex(root.jpegSubsamplingChoices, root.jpegSubsamplingId)
                    Accessible.name: qsTr("JPEG subsampling")
                    onActivated: function (index) {
                        root.jpegSubsamplingId = model[index].id;
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Fonts.size8
            visible: root.formatId === "png"

            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Bit depth")
                    Accessible.name: qsTr("PNG bit depth")
                }
                CustomComboBox {
                    id: pngBitDepthCombo
                    objectName: "pngBitDepth"
                    Layout.fillWidth: true
                    textRole: "label"
                    model: root.pngBitDepthChoices
                    currentIndex: root.choiceIndex(root.pngBitDepthChoices, root.pngBitDepthId)
                    Accessible.name: qsTr("PNG bit depth")
                    onActivated: function (index) {
                        root.pngBitDepthId = model[index].id;
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Compression")
                    Accessible.name: qsTr("PNG compression")
                }
                CustomSpinBox {
                    id: pngCompressionSpin
                    objectName: "pngCompression"
                    Layout.fillWidth: true
                    decimals: 0
                    realFrom: root.optionBounds.pngCompressionMin
                    realTo: root.optionBounds.pngCompressionMax
                    realValue: root.pngCompression
                    Accessible.name: qsTr("PNG compression")
                    onEditingCommitted: function (value) {
                        root.pngCompression = value;
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Fonts.size8
            visible: root.formatId === "tiff"

            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Sample type")
                    Accessible.name: qsTr("TIFF sample type")
                }
                CustomComboBox {
                    id: tiffSampleTypeCombo
                    objectName: "tiffSampleType"
                    Layout.fillWidth: true
                    textRole: "label"
                    model: root.tiffSampleTypeChoices
                    currentIndex: root.choiceIndex(root.tiffSampleTypeChoices, root.tiffSampleTypeId)
                    Accessible.name: qsTr("TIFF sample type")
                    onActivated: function (index) {
                        root.tiffSampleTypeId = model[index].id;
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Compression")
                    Accessible.name: qsTr("TIFF compression")
                }
                CustomComboBox {
                    id: tiffCompressionCombo
                    objectName: "tiffCompression"
                    Layout.fillWidth: true
                    textRole: "label"
                    model: root.tiffCompressionChoices
                    currentIndex: root.choiceIndex(root.tiffCompressionChoices, root.tiffCompressionId)
                    Accessible.name: qsTr("TIFF compression")
                    onActivated: function (index) {
                        root.tiffCompressionId = model[index].id;
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Compression level")
                    enabled: root.tiffLevelEnabled
                    Accessible.name: qsTr("TIFF compression level")
                }
                CustomSpinBox {
                    id: tiffCompressionLevelSpin
                    objectName: "tiffCompressionLevel"
                    Layout.fillWidth: true
                    enabled: root.tiffLevelEnabled
                    decimals: 0
                    realFrom: root.optionBounds.tiffCompressionLevelMin
                    realTo: root.optionBounds.tiffCompressionLevelMax
                    realValue: root.tiffCompressionLevel
                    Accessible.name: qsTr("TIFF compression level")
                    onEditingCommitted: function (value) {
                        root.tiffCompressionLevel = value;
                    }
                }
            }
            CustomCheckBox {
                id: tiffGrayscaleCheck
                objectName: "tiffGrayscaleIfNeutral"
                text: qsTr("Write grayscale when the image is neutral")
                checked: root.tiffGrayscaleIfNeutral
                Accessible.name: qsTr("Write grayscale when the image is neutral")
                onCheckedChanged: root.tiffGrayscaleIfNeutral = checked
            }
            RowLayout {
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Resolution (dpi)")
                    Accessible.name: qsTr("TIFF resolution")
                }
                CustomSpinBox {
                    id: tiffResolutionSpin
                    objectName: "tiffResolutionDpi"
                    Layout.fillWidth: true
                    decimals: 0
                    realFrom: root.optionBounds.tiffResolutionDpiMin
                    realTo: root.optionBounds.tiffResolutionDpiMax
                    realValue: root.tiffResolutionDpi
                    Accessible.name: qsTr("TIFF resolution")
                    onEditingCommitted: function (value) {
                        root.tiffResolutionDpi = value;
                    }
                }
            }
        }

        CustomLabel {
            Layout.fillWidth: true
            visible: root.formatId === "original"
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("Original copy writes the exact source bytes. Rendered format options are not used.")
            Accessible.name: qsTr("Original copy writes the exact source bytes. Rendered format options are not used.")
        }
    }

    footerItem: RowLayout {
        spacing: Fonts.size10

        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            id: cancelButton
            objectName: "exportCancel"
            text: qsTr("Cancel")
            Accessible.name: qsTr("Cancel")
            onClicked: root.cancelExport()
        }
        CustomButton {
            id: continueButton
            objectName: "exportContinue"
            text: qsTr("Continue")
            enabled: root.canContinue
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            Accessible.name: qsTr("Continue")
            onClicked: root.acceptExport()
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
