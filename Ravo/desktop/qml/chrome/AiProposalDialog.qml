import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "AiProposalDialog"
    titleText: qsTr("AI Proposal")
    width: Fonts.messageDialogWidth + Fonts.size48
    bodyFillHeight: false
    showCloseButton: true

    required property var presenter

    property string proposeKind: "global"
    property string semanticLabel: "subject"

    signal proposeAccepted(string kind, string semanticLabel)
    signal selectAccepted(string proposalId)
    signal refreshAccepted
    signal applyAccepted
    signal rejectAccepted
    signal cancelAccepted
    signal dialogCanceled

    readonly property var kindChoices: [
        {
            "label": qsTr("Global edit (stub)"),
            "value": "global"
        },
        {
            "label": qsTr("Semantic mask (stub)"),
            "value": "semantic-mask"
        }
    ]

    function kindIndex(value) {
        for (var i = 0; i < kindChoices.length; ++i) {
            if (kindChoices[i].value === value)
                return i;
        }
        return 0;
    }

    function openForSelection() {
        proposeKind = "global";
        semanticLabel = "subject";
        kindCombo.currentIndex = kindIndex(proposeKind);
        semanticField.text = semanticLabel;
        openDialog();
        if (root.presenter)
            root.refreshAccepted();
        Qt.callLater(function () {
            kindCombo.forceActiveFocus();
        });
    }

    function cancelDialog() {
        close();
        dialogCanceled();
    }

    onCloseRequested: function (reason) {
        root.dialogCanceled();
    }

    bodyItem: ColumnLayout {
        width: parent ? parent.width : Fonts.messageDialogWidth
        spacing: Fonts.size10

        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("Stub proposals only (ADR-0121). No network and no model weights. Inspect the field diff, then apply, reject, or cancel.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.size8
            CustomLabel {
                text: qsTr("Create")
            }
            CustomComboBox {
                id: kindCombo
                objectName: "aiProposalKind"
                Layout.fillWidth: true
                model: root.kindChoices.map(function (item) {
                    return item.label;
                })
                currentIndex: root.kindIndex(root.proposeKind)
                onActivated: function (index) {
                    root.proposeKind = root.kindChoices[index].value;
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.proposeKind === "semantic-mask"
            spacing: Fonts.size8
            CustomLabel {
                text: qsTr("Label")
            }
            CustomTextField {
                id: semanticField
                objectName: "aiProposalSemanticLabel"
                Layout.fillWidth: true
                text: root.semanticLabel
                onEditingFinished: root.semanticLabel = text.trim()
            }
        }

        CustomLabel {
            objectName: "aiProposalListHeader"
            Layout.fillWidth: true
            text: qsTr("Proposals for selection")
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Fonts.size48 * 3
            radius: Fonts.size4
            color: Theme.baseColor
            border.color: Theme.dividerColor
            border.width: 1
            clip: true

            ListView {
                id: proposalList
                objectName: "aiProposalList"
                anchors.fill: parent
                anchors.margins: Fonts.size4
                model: root.presenter ? root.presenter.aiProposals : []
                delegate: Item {
                    width: proposalList.width
                    height: Fonts.toolbarHeight
                    required property var modelData
                    readonly property string proposalId: modelData && modelData.id ? String(modelData.id) : ""
                    readonly property bool selected: root.presenter && root.presenter.selectedAiProposal && root.presenter.selectedAiProposal.id === proposalId
                    Rectangle {
                        anchors.fill: parent
                        color: parent.selected ? Theme.highlightColor : "transparent"
                        opacity: parent.selected ? 0.25 : 1.0
                    }
                    CustomLabel {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: Fonts.size8
                        elide: Text.ElideRight
                        text: {
                            const row = parent.modelData || ({});
                            return qsTr("%1 · %2 · %3").arg(row.kind || "").arg(row.status || "").arg(row.id || "");
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.selectAccepted(parent.proposalId)
                    }
                }
            }
        }

        CustomLabel {
            objectName: "aiProposalInspect"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: {
                const proposal = root.presenter ? root.presenter.selectedAiProposal : ({});
                if (!proposal || !proposal.id)
                    return qsTr("No proposal selected.");
                const provider = proposal.provider || ({});
                const fields = proposal.fields || [];
                const diffs = proposal.fieldDiff || [];
                let fieldText = fields.map(function (item) {
                    return item.field + "=" + item.value;
                }).join(", ");
                let diffText = diffs.map(function (item) {
                    return item.field + " → " + item.value;
                }).join("; ");
                return qsTr("Selected %1 (%2)\nProvider %3 / %4\nFields: %5\nDiff: %6").arg(proposal.id).arg(proposal.status || "").arg(provider.providerId || "").arg(provider.modelId || "").arg(fieldText || "—").arg(diffText || "—");
            }
        }
    }

    footerItem: RowLayout {
        spacing: Fonts.size8

        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            objectName: "aiProposalRefresh"
            text: qsTr("Refresh")
            onClicked: root.refreshAccepted()
        }
        CustomButton {
            objectName: "aiProposalCreate"
            text: qsTr("Create Stub")
            onClicked: {
                root.semanticLabel = semanticField.text.trim();
                root.proposeAccepted(root.proposeKind, root.semanticLabel);
            }
        }
        CustomButton {
            objectName: "aiProposalCancelAction"
            text: qsTr("Cancel Proposal")
            enabled: Boolean(root.presenter && root.presenter.selectedAiProposal && root.presenter.selectedAiProposal.pending)
            onClicked: root.cancelAccepted()
        }
        CustomButton {
            objectName: "aiProposalReject"
            text: qsTr("Reject")
            enabled: Boolean(root.presenter && root.presenter.selectedAiProposal && root.presenter.selectedAiProposal.pending)
            onClicked: root.rejectAccepted()
        }
        CustomButton {
            objectName: "aiProposalApply"
            text: qsTr("Apply")
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            enabled: Boolean(root.presenter && root.presenter.selectedAiProposal && root.presenter.selectedAiProposal.pending)
            onClicked: root.applyAccepted()
        }
        CustomButton {
            objectName: "aiProposalClose"
            text: qsTr("Close")
            onClicked: root.cancelDialog()
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
