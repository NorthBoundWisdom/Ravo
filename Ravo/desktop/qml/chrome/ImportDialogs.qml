pragma Translator: Main
import QtQuick

Item {
    id: root
    required property var presenter
    function chooseSource() {
        source.currentFolder = presenter.importSourceFolderUrl;
        source.openDialog();
    }
    function chooseDestination() {
        destination.currentFolder = presenter.importDestinationFolderUrl;
        destination.openDialog();
    }
    function chooseSecondCopy() {
        secondCopy.currentFolder = presenter.importSecondCopyFolderUrl;
        secondCopy.openDialog();
    }
    FolderDialogPage {
        id: source
        dialogTitle: qsTr("Choose Import Source")
        onFolderAccepted: function (path) {
            root.presenter.setImportSourceRoot(path);
        }
    }
    FolderDialogPage {
        id: destination
        dialogTitle: qsTr("Choose Import Destination")
        onFolderAccepted: function (path) {
            root.presenter.setImportDestination(path);
        }
    }
    FolderDialogPage {
        id: secondCopy
        dialogTitle: qsTr("Choose Import Second Copy")
        onFolderAccepted: function (path) {
            root.presenter.setImportSecondCopyDestination(path);
        }
    }
}
