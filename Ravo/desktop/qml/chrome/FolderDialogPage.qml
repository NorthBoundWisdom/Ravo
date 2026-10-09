import QtQuick
import QtQuick.Dialogs

Item {
    id: root

    required property var presenter

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
            const path = root.toLocalFile(selectedFolder);
            if (path.length > 0)
                root.folderAccepted(path);
            else
                root.folderRejected();
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
        return root.presenter.folderLocalPath(String(urlValue));
    }

    function openDialog() {
        folderDialog.open();
    }

    function closeDialog() {
        folderDialog.close();
    }
}
