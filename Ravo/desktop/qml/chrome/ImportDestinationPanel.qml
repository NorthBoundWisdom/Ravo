pragma Translator: ImportPage
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importDestinationPanel"
    required property var presenter
    signal chooseDestinationRequested
    signal chooseSecondCopyRequested
    color: Theme.railSurfaceColor
    enabled: !presenter.importWorkActive && !presenter.importPreflightActive
    ScrollView {
        anchors.fill: parent
        anchors.margins: Fonts.standardMargin
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: parent.width
            spacing: Fonts.size12
            ImportSection {
                objectName: "importDestinationSection"
                Layout.fillWidth: true
                visible: root.presenter.importMode !== "add"
                title: qsTr("Destination")
                CustomLabel {
                    objectName: "importDestinationPath"
                    Layout.fillWidth: true
                    text: root.presenter.importDestination.length ? root.presenter.importDestination : qsTr("Choose Destination…")
                    wrapMode: Text.WrapAnywhere
                    font.bold: true
                }
                CustomButton {
                    Layout.fillWidth: true
                    text: qsTr("Choose Destination…")
                    onClicked: root.chooseDestinationRequested()
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: root.presenter.importDestinationError.length > 0
                    text: root.presenter.importDestinationError
                    color: Theme.warningColor
                    wrapMode: Text.WordWrap
                }
                CustomButton {
                    visible: root.presenter.importDestinationError.length > 0 && root.presenter.importDestination.length > 0
                    text: qsTr("Check again")
                    onClicked: root.presenter.setImportDestination(root.presenter.importDestination)
                }
                CustomLabel {
                    text: qsTr("Organize")
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Into one folder"), qsTr("Preserve hierarchy"), qsTr("By date (YYYY/MM/DD)"), qsTr("By month (YYYY/MM)")]
                    currentIndex: root.presenter.importOrganization === "hierarchy" ? 1 : root.presenter.importOrganization === "date" ? 2 : root.presenter.importOrganization === "month" ? 3 : 0
                    onActivated: function (index) {
                        root.presenter.setImportOrganization(["single", "hierarchy", "date", "month"][index]);
                    }
                }
                ImportFolderTree {
                    objectName: "importDestinationFolderTree"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 220
                    folderModel: root.presenter.importDestinationFolders
                    onFolderChosen: function (path) {
                        root.presenter.setImportDestination(path);
                    }
                }
            }
            ImportSection {
                objectName: "importDestinationPreviewSection"
                Layout.fillWidth: true
                visible: root.presenter.importMode !== "add" && root.presenter.importDestination.length > 0
                title: qsTr("Destination preview")
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Folders are created only when you import.")
                    wrapMode: Text.WordWrap
                    color: Theme.placeholderTextColor
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: root.presenter.importDestinationPreviewActive
                    text: qsTr("Planning destination…")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: root.presenter.importDestinationPreviewError.length > 0
                    text: root.presenter.importDestinationPreviewError
                    wrapMode: Text.WordWrap
                    color: Theme.warningColor
                }
                ListView {
                    objectName: "importDestinationPreviewTree"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(contentHeight, 260)
                    clip: true
                    model: root.presenter.importDestinationPreview
                    delegate: RowLayout {
                        required property var modelData
                        width: ListView.view.width
                        height: Fonts.listItemHeight
                        spacing: Fonts.size8
                        Item {
                            Layout.preferredWidth: Math.min(modelData.depth * Fonts.size16, parent.width * 0.35)
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: modelData.name
                            elide: Text.ElideMiddle
                            font.italic: modelData.willCreate
                            ToolTip.visible: previewHover.hovered
                            ToolTip.text: modelData.path
                            HoverHandler {
                                id: previewHover
                            }
                        }
                        CustomLabel {
                            visible: modelData.willCreate
                            text: qsTr("Will create")
                            color: Theme.placeholderTextColor
                        }
                        CustomLabel {
                            visible: modelData.secondCopy && modelData.depth === 0
                            text: qsTr("Second copy")
                        }
                        CustomLabel {
                            text: qsTr("%1 photos").arg(modelData.photoCount)
                        }
                    }
                }
            }
            ImportSection {
                Layout.fillWidth: true
                title: qsTr("File Handling")
                CustomLabel {
                    text: qsTr("Build Previews")
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Minimal (320)"), qsTr("Standard (1600)"), qsTr("1:1")]
                    currentIndex: root.presenter.importPreviewPolicy === "minimal" ? 0 : root.presenter.importPreviewPolicy === "one-to-one" ? 2 : 1
                    onActivated: function (index) {
                        root.presenter.setImportPreviewPolicy(["minimal", "standard", "one-to-one"][index]);
                    }
                }
            }
            ImportSection {
                Layout.fillWidth: true
                visible: root.presenter.importMode !== "add"
                title: qsTr("Rename template")
                expanded: false
                CustomTextField {
                    objectName: "importFilenameTemplate"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Fonts.inputFieldHeight
                    alignRightWhenFocused: false
                    showClipIndicator: false
                    showEmptyIndicator: false
                    text: root.presenter.importFilenameTemplate
                    placeholderText: qsTr("Keep original names")
                    Accessible.name: qsTr("Import filename template")
                    onEditingFinished: root.presenter.setImportFilenameTemplate(text)
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Tokens: {date}, {stem}, {sequence}, {ext}")
                    wrapMode: Text.WordWrap
                    color: Theme.placeholderTextColor
                }
            }
            ImportSection {
                Layout.fillWidth: true
                visible: root.presenter.importMode !== "add"
                title: qsTr("Second copy")
                expanded: false
                RowLayout {
                    Layout.fillWidth: true
                    CustomButton {
                        objectName: "importChooseSecondCopy"
                        Layout.fillWidth: true
                        text: qsTr("Choose Second Copy…")
                        onClicked: root.chooseSecondCopyRequested()
                    }
                    CustomButton {
                        objectName: "importClearSecondCopy"
                        visible: root.presenter.importSecondCopyDestination.length > 0
                        text: qsTr("Clear")
                        onClicked: root.presenter.setImportSecondCopyDestination("")
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: root.presenter.importSecondCopyDestination.length ? root.presenter.importSecondCopyDestination : qsTr("No second copy selected")
                    wrapMode: Text.WrapAnywhere
                    color: Theme.placeholderTextColor
                }
            }
        }
    }
}
