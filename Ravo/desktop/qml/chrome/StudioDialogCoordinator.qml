import QtQuick
import GeoControls 1.0

// Window-level pending dialog / confirmation orchestration for Studio chrome.
QtObject {
    id: root

    required property var windowHost
    required property var presenter
    required property var optionsDialog
    required property var fileDialog
    required property var batchDialog
    required property var actions
    readonly property bool companionConfirmationVisible: companionDialog.visible

    function openExportDialog() {
        if (!presenter.selectedAssetId.length)
            return;
        windowHost.clearPendingExport();
        optionsDialog.openForExport();
    }

    function openCompanionExportDialog() {
        windowHost.clearPendingExport();
        startExport("companion-jpeg", {}, "{stem}");
    }

    function startExport(format, options, filenameTemplate) {
        windowHost.pendingExportFormat = format;
        windowHost.pendingExportOptions = options;
        windowHost.pendingExportFilenameTemplate = filenameTemplate;
        if (presenter.selectedCount > 1) {
            batchDialog.currentFolder = presenter.defaultCatalogFolder;
            batchDialog.openDialog();
        } else {
            fileDialog.nameFilters = [windowHost.exportNameFilter(format)];
            fileDialog.currentFolder = presenter.defaultCatalogFolder;
            fileDialog.initialSelectedFile = presenter.selectedDisplayName + (format === "companion-jpeg" ? ".jpg" : "");
            fileDialog.openDialog();
        }
    }

    function finishExportFile(path) {
        const format = windowHost.pendingExportFormat;
        const options = windowHost.pendingExportOptions;
        windowHost.clearPendingExport();
        actions.run(actions.ids.libraryExportWrite, { "path": path, "format": format, "options": options });
    }

    function finishExportBatch(directory) {
        const format = windowHost.pendingExportFormat;
        const options = windowHost.pendingExportOptions;
        const filenameTemplate = windowHost.pendingExportFilenameTemplate;
        windowHost.clearPendingExport();
        actions.run(actions.ids.libraryExportBatchWrite, {
            "directory": directory, "filenameTemplate": filenameTemplate, "format": format, "options": options
        });
    }

    property MessageDialog companionDialog: MessageDialog {
        objectName: "CompanionMissingDialog"
        parentItem: root.windowHost.contentItem
        titleText: qsTranslate("Main", "Companion JPEG unavailable")
        messageText: qsTranslate("Main", "A companion JPEG is missing. Export the selected photos from RAW instead?")
        buttons: [qsTranslate("Main", "Cancel"), qsTranslate("Main", "Export from RAW")]
        defaultButtonText: qsTranslate("Main", "Cancel")
        onFinished: function (buttonText) {
            root.windowHost.clearPendingExport();
            if (buttonText === qsTranslate("Main", "Export from RAW")) {
                root.openExportDialog();
                root.optionsDialog.formatId = "jpeg";
            }
        }
    }

    property Connections presenterSignals: Connections {
        target: root.presenter
        function onCompanionExportReady() { root.openCompanionExportDialog(); }
        function onCompanionExportMissing() {
            root.windowHost.clearPendingExport();
            root.companionDialog.openWithButtons();
        }
        function onSelectionChanged() { root.companionDialog.close(); }
        function onCatalogChanged() { root.companionDialog.close(); }
    }

    property string pendingRelinkFolderId: ""
    property string removeFolderConfirmationToken: ""
    property string removeFolderPath: ""

    function resetFolderDialogs() {
        pendingRelinkFolderId = "";
        removeFolderConfirmationToken = "";
        removeFolderPath = "";
    }

    function openSelectedAssetDialog(dialog) {
        if (!dialog)
            return;
        dialog.openForSelection();
    }
}
