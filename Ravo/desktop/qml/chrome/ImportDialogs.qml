pragma Translator: Main
import QtQuick

Item {
    id: root
    required property var presenter
    function chooseSource() {
        source.currentFolder = presenter.imports.importSourceFolderUrl;
        source.openDialog();
    }
    function chooseDestination() {
        destination.currentFolder = presenter.imports.importDestinationFolderUrl;
        destination.openDialog();
    }
    function chooseSecondCopy() {
        secondCopy.currentFolder = presenter.imports.importSecondCopyFolderUrl;
        secondCopy.openDialog();
    }
    FolderDialogPage {
        id: source
        dialogTitle: qsTr("Choose Import Source")
        onFolderAccepted: function (path) {
            root.presenter.imports.setImportSourceRoot(path);
        }
    }
    FolderDialogPage {
        id: destination
        dialogTitle: qsTr("Choose Import Destination")
        onFolderAccepted: function (path) {
            root.presenter.imports.setImportDestination(path);
        }
    }
    FolderDialogPage {
        id: secondCopy
        dialogTitle: qsTr("Choose Import Second Copy")
        onFolderAccepted: function (path) {
            root.presenter.imports.setImportSecondCopyDestination(path);
        }
    }
}
