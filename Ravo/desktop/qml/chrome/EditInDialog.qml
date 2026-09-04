import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "EditInDialog"
    titleText: qsTr("Edit in…")
    width: Fonts.messageDialogWidth
    bodyFillHeight: false
    showCloseButton: true

    required property var presenter

    property string editorId: "external"
    property string editorVersion: ""
    property string applicationPath: ""
    property string tiffSampleType: "uint16"
    property int maxEdge: 0
    property bool autoStack: true
    property bool openAfterCreate: true

    readonly property var sampleTypeChoices: [
        {
            "label": qsTr("8-bit"),
            "value": "uint8"
        },
        {
            "label": qsTr("16-bit"),
            "value": "uint16"
        }
    ]

    signal prepareAccepted(var options)
    signal checkReturnedAccepted(string workingCopyId)
    signal abandonAccepted(string workingCopyId)
    signal reopenAccepted(string workingCopyId)
    signal refreshStatusAccepted(string workingCopyId)
    signal dialogCanceled

    function sampleTypeIndex(value) {
        for (var index = 0; index < sampleTypeChoices.length; ++index) {
            if (sampleTypeChoices[index].value === value)
                return index;
        }
        return 1;
    }

    function openForSelection() {
        const defaults = root.presenter ? root.presenter.externalEditorDefaultOptions() : ({});
        editorId = defaults.editorId || "external";
        editorVersion = defaults.editorVersion || "";
        applicationPath = defaults.applicationPath || "";
        tiffSampleType = defaults.tiffSampleType || "uint16";
        maxEdge = defaults.maxEdge || 0;
        autoStack = defaults.autoStack !== undefined ? defaults.autoStack : true;
        openAfterCreate = defaults.openAfterCreate !== undefined ? defaults.openAfterCreate : true;
        editorIdField.text = editorId;
        editorVersionField.text = editorVersion;
        applicationPathField.text = applicationPath;
        sampleTypeCombo.currentIndex = sampleTypeIndex(tiffSampleType);
        maxEdgeSpin.realValue = maxEdge;
        autoStackCheck.checked = autoStack;
        openAfterCheck.checked = openAfterCreate;
        openDialog();
        Qt.callLater(function () {
            editorIdField.forceActiveFocus();
        });
    }

    function currentOptions() {
        return {
            "editorId": editorIdField.text.trim(),
            "editorVersion": editorVersionField.text.trim(),
            "applicationPath": applicationPathField.text.trim(),
            "tiffSampleType": tiffSampleType,
            "profile": "srgb",
            "maxEdge": maxEdge,
            "autoStack": autoStack,
            "openAfterCreate": openAfterCreate
        };
    }

    function acceptPrepare() {
        const options = currentOptions();
        if (!options.editorId.length)
            return;
        close();
        prepareAccepted(options);
    }

    function acceptCheckReturned() {
        const session = root.presenter ? root.presenter.externalEditorSession : ({});
        const workingCopyId = session && session.workingCopyId ? String(session.workingCopyId) : "";
        close();
        checkReturnedAccepted(workingCopyId);
    }

    function acceptAbandon() {
        const session = root.presenter ? root.presenter.externalEditorSession : ({});
        const workingCopyId = session && session.workingCopyId ? String(session.workingCopyId) : "";
        close();
        abandonAccepted(workingCopyId);
    }

    function acceptReopen() {
        const session = root.presenter ? root.presenter.externalEditorSession : ({});
        const workingCopyId = session && session.workingCopyId ? String(session.workingCopyId) : "";
        close();
        reopenAccepted(workingCopyId);
    }

    function acceptRefreshStatus() {
        const session = root.presenter ? root.presenter.externalEditorSession : ({});
        const workingCopyId = session && session.workingCopyId ? String(session.workingCopyId) : "";
        refreshStatusAccepted(workingCopyId);
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
            text: qsTr("Create a catalog-owned TIFF working copy, open it in an external editor, then check the returned file. Originals stay byte-identical.")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            CustomLabel {
                text: qsTr("Editor id")
                Accessible.name: qsTr("External editor id")
            }
            CustomTextField {
                id: editorIdField
                objectName: "editInEditorId"
                Layout.fillWidth: true
                text: root.editorId
                Accessible.name: qsTr("External editor id")
                onEditingFinished: root.editorId = text.trim()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            CustomLabel {
                text: qsTr("Editor version")
                Accessible.name: qsTr("External editor version")
            }
            CustomTextField {
                id: editorVersionField
                objectName: "editInEditorVersion"
                Layout.fillWidth: true
                text: root.editorVersion
                Accessible.name: qsTr("External editor version")
                onEditingFinished: root.editorVersion = text.trim()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            CustomLabel {
                text: qsTr("Application")
                Accessible.name: qsTr("External editor application path")
            }
            CustomTextField {
                id: applicationPathField
                objectName: "editInApplicationPath"
                Layout.fillWidth: true
                text: root.applicationPath
                placeholderText: qsTr("Optional path (empty uses OS default)")
                Accessible.name: qsTr("External editor application path")
                onEditingFinished: root.applicationPath = text.trim()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            CustomLabel {
                text: qsTr("TIFF sample")
                Accessible.name: qsTr("Working-copy TIFF sample type")
            }
            CustomComboBox {
                id: sampleTypeCombo
                objectName: "editInTiffSampleType"
                Layout.fillWidth: true
                textRole: "label"
                model: root.sampleTypeChoices
                currentIndex: root.sampleTypeIndex(root.tiffSampleType)
                Accessible.name: qsTr("Working-copy TIFF sample type")
                onActivated: function (index) {
                    root.tiffSampleType = root.sampleTypeChoices[index].value;
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            CustomLabel {
                text: qsTr("Profile")
            }
            CustomLabel {
                objectName: "editInProfileLabel"
                Layout.fillWidth: true
                text: qsTr("sRGB (v1 only)")
                color: Theme.placeholderTextColor
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Fonts.standardMargin
            CustomLabel {
                text: qsTr("Max edge")
                Accessible.name: qsTr("Working-copy max edge")
            }
            CustomSpinBox {
                id: maxEdgeSpin
                objectName: "editInMaxEdge"
                Layout.fillWidth: true
                decimals: 0
                realFrom: 0
                realTo: 65535
                realValue: root.maxEdge
                Accessible.name: qsTr("Working-copy max edge")
                onEditingCommitted: function (value) {
                    root.maxEdge = value;
                }
            }
        }

        CustomCheckBox {
            id: autoStackCheck
            objectName: "editInAutoStack"
            text: qsTr("Auto-stack derived pair on return")
            checked: root.autoStack
            onToggled: root.autoStack = checked
        }

        CustomCheckBox {
            id: openAfterCheck
            objectName: "editInOpenAfterCreate"
            text: qsTr("Open working copy after prepare")
            checked: root.openAfterCreate
            onToggled: root.openAfterCreate = checked
        }

        CustomLabel {
            objectName: "editInSessionStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            visible: Boolean(root.presenter && root.presenter.externalEditorSession && root.presenter.externalEditorSession.workingCopyId)
            text: {
                const session = root.presenter ? root.presenter.externalEditorSession : ({});
                if (!session || !session.workingCopyId)
                    return "";
                if (session.registered)
                    return qsTr("Session %1 (registered)").arg(session.workingCopyId);
                const state = session.machineState ? String(session.machineState) : qsTr("pending");
                const reason = session.reason ? String(session.reason) : state;
                return qsTr("Session %1 — %2").arg(session.workingCopyId).arg(reason);
            }
        }
    }

    footerItem: RowLayout {
        spacing: Fonts.size10

        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            objectName: "editInCancel"
            text: qsTr("Cancel")
            onClicked: root.cancelDialog()
        }
        CustomButton {
            objectName: "editInAbandon"
            text: qsTr("Abandon")
            enabled: Boolean(root.presenter && root.presenter.externalEditorSession && root.presenter.externalEditorSession.workingCopyId && !root.presenter.externalEditorSession.registered)
            onClicked: root.acceptAbandon()
        }
        CustomButton {
            objectName: "editInReopen"
            text: qsTr("Reopen")
            enabled: Boolean(root.presenter && ((root.presenter.externalEditorSession && root.presenter.externalEditorSession.workingCopyId && !root.presenter.externalEditorSession.registered) || (root.presenter.selectedAssetId && root.presenter.selectedAssetId.length)))
            onClicked: root.acceptReopen()
        }
        CustomButton {
            objectName: "editInRefreshStatus"
            text: qsTr("Refresh Status")
            enabled: Boolean(root.presenter && root.presenter.externalEditorSession && root.presenter.externalEditorSession.workingCopyId && !root.presenter.externalEditorSession.registered)
            onClicked: root.acceptRefreshStatus()
        }
        CustomButton {
            objectName: "editInCheckReturned"
            text: qsTr("Check Returned")
            enabled: Boolean(root.presenter && root.presenter.externalEditorSession && root.presenter.externalEditorSession.workingCopyId && !root.presenter.externalEditorSession.registered)
            onClicked: root.acceptCheckReturned()
        }
        CustomButton {
            objectName: "editInPrepare"
            text: qsTr("Prepare Working Copy")
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            onClicked: root.acceptPrepare()
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
