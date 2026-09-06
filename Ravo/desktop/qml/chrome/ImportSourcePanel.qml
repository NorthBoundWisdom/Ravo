pragma Translator: ImportPage
import QtQuick
import QtQuick.Controls
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
            objectName: "importIncludeSubfolders"
            text: qsTr("Include subfolders")
            checked: root.presenter.importRecursive
            onClicked: root.presenter.setImportRecursive(checked)
        }
        CustomButton {
            Layout.fillWidth: true
            visible: root.presenter.importSourceRoot.length > 0
            text: qsTr("Check again")
            onClicked: root.presenter.setImportSourceRoot(root.presenter.importSourceRoot)
        }
        Rectangle {
            objectName: "importSourceTreeSurface"
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.baseColor
            border.color: Theme.dividerColor
            border.width: ControlState.borderThin
            radius: ControlState.radiusSmall
            ImportFolderTree {
                objectName: "importSourceFolderTree"
                anchors.fill: parent
                anchors.margins: Fonts.size4
                folderModel: root.presenter.importSourceFolders
                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AsNeeded
                }
                onFolderChosen: function (path) {
                    root.presenter.setImportSourceRoot(path);
                }
            }
        }
    }
}
