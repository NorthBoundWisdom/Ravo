pragma Translator: ImportPage
import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importSourcePanel"
    required property var presenter
    signal chooseRequested
    color: Theme.railSurfaceColor
    enabled: !presenter.importWorkActive && !presenter.importPreflightActive
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Fonts.standardMargin
        spacing: Fonts.size12
        CustomLabel {
            text: qsTr("Source")
            font.bold: true
        }
        CustomLabel {
            Layout.fillWidth: true
            text: root.presenter.importSourceRoot
            wrapMode: Text.WrapAnywhere
            color: Theme.placeholderTextColor
        }
        CustomButton {
            Layout.fillWidth: true
            text: qsTr("Choose Source…")
            onClicked: root.chooseRequested()
        }
        CustomCheckBox {
            text: qsTr("Include subfolders")
            checked: root.presenter.importRecursive
            onToggled: root.presenter.setImportRecursive(checked)
        }
        CustomButton {
            Layout.fillWidth: true
            visible: root.presenter.importSourceRoot.length > 0
            text: qsTr("Check again")
            onClicked: root.presenter.setImportSourceRoot(root.presenter.importSourceRoot)
        }
        ImportFolderTree {
            objectName: "importSourceFolderTree"
            Layout.fillWidth: true
            Layout.fillHeight: true
            folderModel: root.presenter.importSourceFolders
            onFolderChosen: function (path) {
                root.presenter.setImportSourceRoot(path);
            }
        }
    }
}
