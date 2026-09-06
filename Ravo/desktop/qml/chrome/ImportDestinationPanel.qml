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
    Rectangle {
        id: transferModes
        objectName: "importTransferModes"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Fonts.standardMargin
        height: ControlState.minInputHeight
        radius: ControlState.radiusSmall
        color: Theme.baseColor
        border.color: Theme.midColor
        border.width: ControlState.borderThin
        Row {
            objectName: "importTransferMode"
            anchors.fill: parent
            anchors.margins: transferModes.border.width
            spacing: 0
            Repeater {
                model: [qsTr("Copy"), qsTr("Add"), qsTr("Move")]
                SegmentedButton {
                    required property int index
                    required property string modelData
                    readonly property string mode: ["copy", "add", "move"][index]
                    objectName: "importTransferModeSegment" + index
                    width: parent.width / 3
                    height: parent.height
                    text: modelData
                    selected: root.presenter.importMode === mode
                    enabled: index < 2
                    onClicked: root.presenter.setImportMode(mode)
                    ToolTip.visible: hovered && index === 2
                    ToolTip.text: qsTr("Ingest transports are Copy-only; Move and camera delete stay rejected.")
                }
            }
        }
    }
    ScrollView {
        id: destinationScroll
        anchors.top: transferModes.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Fonts.standardMargin
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            id: settingsColumn
            width: parent.width
            height: destinationSection.visible && destinationSection.expanded ? Math.max(implicitHeight, destinationScroll.availableHeight) : implicitHeight
            spacing: Fonts.size12
            ImportSection {
                id: destinationSection
                objectName: "importDestinationSection"
                Layout.fillWidth: true
                stretchContent: true
                visible: root.presenter.importMode !== "add"
                title: qsTr("Destination")
                CustomLabel {
                    objectName: "importDestinationPath"
                    Layout.fillWidth: true
                    visible: root.presenter.importDestination.length > 0
                    text: root.presenter.importDestination
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
                    visible: root.presenter.importDestination.length > 0 && root.presenter.importDestinationError.length > 0
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
                Rectangle {
                    objectName: "importDestinationTreeSurface"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    implicitHeight: Fonts.scaledUiSize(160)
                    Layout.minimumHeight: implicitHeight
                    color: Theme.baseColor
                    border.color: Theme.dividerColor
                    border.width: ControlState.borderThin
                    radius: ControlState.radiusSmall
                    ImportFolderTree {
                        objectName: "importDestinationFolderTree"
                        anchors.fill: parent
                        anchors.margins: Fonts.size4
                        folderModel: root.presenter.importDestinationFolders
                        ScrollBar.vertical: ScrollBar {
                            policy: ScrollBar.AsNeeded
                        }
                        onFolderChosen: function (path) {
                            root.presenter.setImportDestination(path);
                        }
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
