import QtQuick

// Window-level pending dialog / confirmation orchestration for Studio chrome.
QtObject {
    id: root

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
