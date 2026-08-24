import QtQuick
import QtQuick.Dialogs

Item {
    id: root

    property string dialogTitle: qsTr("Select Folder")
    property url currentFolder: ""

    signal folderAccepted(string folderPath)
    signal folderRejected
    signal dialogClosed

    FolderDialog {
        id: folderDialog
        title: root.dialogTitle
        currentFolder: root.currentFolder
        onAccepted: {
            root.folderAccepted(root.toLocalFile(selectedFolder));
            folderDialog.close();
        }
        onRejected: {
            root.folderRejected();
            folderDialog.close();
        }
        onVisibleChanged: {
            if (!visible)
                root.dialogClosed();
        }
    }

    function toLocalFile(urlValue) {
        if (urlValue === undefined || urlValue === null)
            return "";
        if (typeof urlValue === "string")
            return urlValue.startsWith("file://") ? Qt.urlToLocalFile(urlValue) : urlValue;
        if (urlValue.toLocalFile)
            return urlValue.toLocalFile();
        return String(urlValue);
    }

    function openDialog() {
        folderDialog.open();
    }

    function closeDialog() {
        folderDialog.close();
    }
}
