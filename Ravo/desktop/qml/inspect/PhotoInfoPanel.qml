import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property var presenter
    property var commands
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    readonly property bool metadataEditing: metadataDialog.visible
    spacing: Fonts.smallSpacing

    function infoRow(label, value) {
        return label + ": " + (value && value.length ? value : "—");
    }

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
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        wrapMode: Text.Wrap
        color: Theme.placeholderTextColor
        text: root.infoRow(qsTr("Capture"), root.presenter ? root.presenter.selectedCaptureSummary : "")
    }

    CustomLabel {
        Layout.leftMargin: Fonts.standardMargin
        text: qsTr("Exposure")
        font.bold: true
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        enabled: root.hasSelection && root.presenter.selectedCount === 1 && !root.presenter.busy && root.presenter.develop.canAdjustExposure
        Repeater {
            model: [
                {
                    text: qsTr("−1 EV"),
                    delta: -1
                },
                {
                    text: qsTr("−⅓ EV"),
                    delta: -1 / 3
                },
                {
                    text: qsTr("+⅓ EV"),
                    delta: 1 / 3
                },
                {
                    text: qsTr("+1 EV"),
                    delta: 1
                }
            ]
            CustomButton {
                required property var modelData
                Layout.fillWidth: true
                text: modelData.text
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.photoAdjustExposure, modelData.delta)
            }
        }
    }
    CustomButton {
        objectName: "editMetadataButton"
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        text: qsTr("Edit Metadata...")
        enabled: root.hasSelection && root.commands && !root.presenter.busy
        onClicked: {
            metadataDialog.context = root.presenter.metadataEditContext();
            metadataDialog.changes = ({});
            metadataDialog.rows = metadataDialog.fields.map(function (field) {
                return {
                    key: field.key,
                    label: field.label,
                    value: field.value
                };
            });
            metadataDialog.openDialog();
        }
    }

    DialogShell {
        id: metadataDialog
        objectName: "metadataEditDialog"
        titleText: qsTr("Edit Metadata...") + (context.assets && context.assets.length > 1 ? " (" + context.assets.length + ")" : "")
        width: parent ? Math.min(Fonts.scaledUiSize(720), parent.width - 2 * Fonts.standardMargin) : Fonts.messageDialogWidth
        bodyFillHeight: false
        property var context: ({})
        property var rows: []
        property var changes: ({})
        readonly property var fields: [
            {
                key: "title",
                label: qsTr("Title"),
                value: root.hasPresenter ? root.presenter.selectedTitle : ""
            },
            {
                key: "headline",
                label: qsTr("Headline"),
                value: root.hasPresenter ? root.presenter.selectedHeadline : ""
            },
            {
                key: "description",
                label: qsTr("Description"),
                value: root.hasPresenter ? root.presenter.selectedDescription : ""
            },
            {
                key: "creator",
                label: qsTr("Creator"),
                value: root.hasPresenter ? root.presenter.selectedCreator : ""
            },
            {
                key: "copyright",
                label: qsTr("Copyright"),
                value: root.hasPresenter ? root.presenter.selectedCopyright : ""
            },
            {
                key: "credit",
                label: qsTr("Credit"),
                value: root.hasPresenter ? root.presenter.selectedCredit : ""
            },
            {
                key: "source",
                label: qsTr("Source"),
                value: root.hasPresenter ? root.presenter.selectedSource : ""
            },
            {
                key: "instructions",
                label: qsTr("Instructions"),
                value: root.hasPresenter ? root.presenter.selectedInstructions : ""
            },
            {
                key: "usage_terms",
                label: qsTr("Usage Terms"),
                value: root.hasPresenter ? root.presenter.selectedUsageTerms : ""
            },
            {
                key: "job_id",
                label: qsTr("Job ID"),
                value: root.hasPresenter ? root.presenter.selectedJobId : ""
            },
            {
                key: "country",
                label: qsTr("Country"),
                value: root.hasPresenter ? root.presenter.selectedCountry : ""
            },
            {
                key: "province_state",
                label: qsTr("Province / State"),
                value: root.hasPresenter ? root.presenter.selectedProvinceState : ""
            },
            {
                key: "city",
                label: qsTr("City"),
                value: root.hasPresenter ? root.presenter.selectedCity : ""
            },
            {
                key: "sublocation",
                label: qsTr("Sublocation"),
                value: root.hasPresenter ? root.presenter.selectedSublocation : ""
            }
        ]
        bodyItem: CustomScrollView {
            id: metadataScroll
            clip: true
            contentWidth: availableWidth
            implicitHeight: Math.min(metadataRows.implicitHeight, metadataDialog.parent ? metadataDialog.parent.height * 0.65 : Fonts.scaledUiSize(480))
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ColumnLayout {
                id: metadataRows
                objectName: "metadataEditRows"
                width: metadataScroll.availableWidth
                spacing: Fonts.smallSpacing
                Repeater {
                    model: metadataDialog.rows
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Fonts.standardMargin
                        CustomLabel {
                            Layout.preferredWidth: Math.min(Fonts.scaledUiSize(150), metadataRows.width * 0.3)
                            wrapMode: Text.WordWrap
                            text: modelData.label
                        }
                        CustomTextField {
                            objectName: "metadataEditValue_" + modelData.key
                            Layout.fillWidth: true
                            showEmptyIndicator: false
                            showClipIndicator: false
                            alignRightWhenFocused: false
                            text: modelData.value
                            Accessible.name: modelData.label
                            onTextChanged: {
                                if (!metadataDialog.visible)
                                    return;
                                const changes = Object.assign({}, metadataDialog.changes);
                                if (text === modelData.value)
                                    delete changes[modelData.key];
                                else
                                    changes[modelData.key] = text;
                                metadataDialog.changes = changes;
                            }
                        }
                    }
                }
            }
        }
        footerItem: RowLayout {
            Item {
                Layout.fillWidth: true
            }
            CustomButton {
                objectName: "metadataEditCancel"
                text: qsTr("Cancel")
                onClicked: metadataDialog.close()
            }
            CustomButton {
                objectName: "metadataEditSave"
                text: qsTr("Save")
                enabled: root.hasSelection && root.commands && Object.keys(metadataDialog.changes).length > 0
                onClicked: {
                    const result = root.commands.setMetadataFields(metadataDialog.changes, metadataDialog.context);
                    if (result && result.accepted)
                        metadataDialog.close();
                }
            }
        }
    }
}
