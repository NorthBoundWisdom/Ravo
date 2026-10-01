import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "PhotoMergeDialog"
    required property var presenter
    required property var actions
    property var context: ({})
    readonly property bool hdr: context.kind === "hdr"
    titleText: hdr ? qsTr("Merge Exposure Bracket") : qsTr("Stitch Panorama")
    width: Fonts.messageDialogWidth
    bodyFillHeight: false
    showCloseButton: true

    function openForContext(value) {
        context = value;
        exposure.text = "";
        maxEdge.text = "0";
        deghost.text = "0.2";
        align.checked = true;
        crop.checked = true;
        openDialog();
    }
    bodyItem: ScrollView {
        id: scroll
        clip: true
        implicitHeight: Math.min(contentColumn.implicitHeight, root.parentItem ? Math.max(Fonts.scaledUiSize(120), root.parentItem.height * 0.6) : Fonts.scaledUiSize(420))
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ColumnLayout {
            id: contentColumn
            width: scroll.availableWidth
            spacing: Fonts.standardMargin
            CustomLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Merge %1 original photos into a new 16-bit TIFF in this library. Existing edits are not baked in; originals are preserved.").arg(root.context.count || 0)
            }
            CustomLabel {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.hdr ? qsTr("HDR uses exposure metadata and maps the result for normal display. The output is not a RAW DNG.") : qsTr("Planar panorama: use overlapping photos taken from the same viewpoint. 360° spherical panoramas are not supported.")
            }
            CustomCheckBox {
                id: align
                objectName: "mergeAutoAlign"
                text: qsTr("Automatically align photos")
                checked: true
                enabled: root.hdr
            }
            Repeater {
                model: root.context.sources || []
                CustomLabel {
                    required property string modelData
                    required property int index
                    Layout.fillWidth: true
                    text: (index + 1) + ". " + modelData
                    elide: Text.ElideMiddle
                }
            }
            CustomCheckBox {
                id: crop
                objectName: "mergeAutoCrop"
                text: qsTr("Crop to remove uncovered edges")
                checked: true
            }
            CustomLabel {
                visible: root.hdr
                text: qsTr("Exposure stops in selection order (optional)")
            }
            CustomTextField {
                id: exposure
                objectName: "mergeExposureStops"
                visible: root.hdr
                Layout.fillWidth: true
                placeholderText: qsTr("For example: -2, 0, 2. Leave empty to use metadata.")
            }
            CustomLabel {
                visible: root.hdr
                text: qsTr("Deghost threshold (0 disables; 0.2 default)")
            }
            CustomTextField {
                id: deghost
                objectName: "mergeDeghost"
                visible: root.hdr
                text: "0.2"
                Layout.fillWidth: true
            }
            CustomLabel {
                text: qsTr("Maximum input edge in pixels (0 uses full resolution)")
            }
            CustomTextField {
                id: maxEdge
                objectName: "mergeMaxEdge"
                text: "0"
                Layout.fillWidth: true
            }
        }
    }
    footerItem: RowLayout {
        spacing: Fonts.size8
        CustomButton {
            text: qsTr("Cancel")
            onClicked: root.close()
        }
        CustomButton {
            objectName: "mergeStart"
            text: root.hdr ? qsTr("Merge HDR") : qsTr("Stitch Panorama")
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            onClicked: {
                const options = {
                    "token": root.context.token,
                    "autoAlign": align.checked,
                    "autoCrop": crop.checked,
                    "exposureStops": root.hdr ? exposure.text : "",
                    "deghost": root.hdr ? deghost.text : "0",
                    "maxEdge": maxEdge.text
                };
                root.close();
                root.actions.run(root.actions.ids.photoMergeApply, options);
            }
        }
        Item {
            Layout.fillWidth: true
        }
    }
    Connections {
        target: root.presenter
        function onSelectionChanged() {
            root.close();
        }
        function onCatalogChanged() {
            root.close();
        }
    }
}
