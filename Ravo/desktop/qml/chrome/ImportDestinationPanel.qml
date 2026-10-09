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
    enabled: !presenter.imports.importWorkActive && !presenter.imports.importPreflightActive && !presenter.imports.importInteractionBlocked
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
                    selected: root.presenter.imports.importMode === mode
                    enabled: index < 2
                    onClicked: root.presenter.imports.setImportMode(mode)
                    tooltipText: index === 2 ? qsTr("Ingest transports are Copy-only; Move and camera delete stay rejected.") : ""
                }
            }
        }
    }
    CustomScrollView {
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
            height: destinationSection.visible ? Math.max(implicitHeight, destinationScroll.availableHeight) : implicitHeight
            spacing: Fonts.size12
            ColumnLayout {
                objectName: "importPreviewSettings"
                Layout.fillWidth: true
                CustomLabel {
                    text: qsTr("Build Previews")
                }
                CustomComboBox {
                    objectName: "importPreviewPolicy"
                    Layout.fillWidth: true
                    Accessible.name: qsTr("Build Previews")
                    model: [qsTr("Minimal (320)"), qsTr("Standard (1600)"), qsTr("1:1")]
                    currentIndex: root.presenter.imports.importPreviewPolicy === "minimal" ? 0 : root.presenter.imports.importPreviewPolicy === "one-to-one" ? 2 : 1
                    onActivated: function (index) {
                        root.presenter.imports.setImportPreviewPolicy(["minimal", "standard", "one-to-one"][index]);
                    }
                }
            }
            ColumnLayout {
                objectName: "importRenameSettings"
                Layout.fillWidth: true
                visible: root.presenter.imports.importMode !== "add"
                CustomCheckBox {
                    objectName: "importRenameEnabled"
                    text: qsTr("Rename files")
                    checked: root.presenter.imports.importRenameEnabled
                    onClicked: root.presenter.imports.setImportRenameEnabled(checked)
                }
                CustomLabel {
                    visible: !root.presenter.imports.importRenameEnabled
                    text: qsTr("Keep original names")
                    color: Theme.placeholderTextColor
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.presenter.imports.importRenameEnabled
                    Repeater {
                        model: 3
                        RowLayout {
                            required property int index
                            Layout.fillWidth: true
                            CustomLabel {
                                text: qsTr("Part %1").arg(index + 1)
                            }
                            CustomComboBox {
                                objectName: "importRenamePart" + index
                                Layout.fillWidth: true
                                Accessible.name: qsTr("Part %1").arg(index + 1)
                                model: index === 0 ? [qsTr("Original filename"), qsTr("Capture date (YYYYMMDD)"), qsTr("Sequence (0001)")] : [qsTr("None"), qsTr("Original filename"), qsTr("Capture date (YYYYMMDD)"), qsTr("Sequence (0001)")]
                                currentIndex: root.presenter.imports.importRenameParts[index] - (index === 0 ? 1 : 0)
                                onActivated: function (choice) {
                                    root.presenter.imports.setImportRenamePart(index, choice + (index === 0 ? 1 : 0));
                                }
                            }
                        }
                    }
                    CustomLabel {
                        text: qsTr("Separator")
                    }
                    CustomComboBox {
                        objectName: "importRenameSeparator"
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Separator")
                        model: [qsTr("Underscore (_)"), qsTr("Hyphen (-)"), qsTr("None")]
                        currentIndex: root.presenter.imports.importRenameSeparator
                        onActivated: function (index) {
                            root.presenter.imports.setImportRenameSeparator(index);
                        }
                    }
                    CustomLabel {
                        text: qsTr("Filename example")
                    }
                    CustomTextField {
                        objectName: "importFilenameTemplate"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        readOnly: true
                        alignRightWhenFocused: false
                        showClipIndicator: false
                        showEmptyIndicator: false
                        text: root.presenter.imports.importRenameExample
                        Accessible.name: qsTr("Filename example")
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        text: qsTr("The original extension is kept.")
                        wrapMode: Text.WordWrap
                        color: Theme.placeholderTextColor
                    }
                }
            }
            ColumnLayout {
                objectName: "importSecondCopySettings"
                Layout.fillWidth: true
                visible: root.presenter.imports.importMode !== "add"
                CustomCheckBox {
                    objectName: "importSecondCopyEnabled"
                    text: qsTr("Second copy")
                    checked: root.presenter.imports.importSecondCopyEnabled
                    onClicked: root.presenter.imports.setImportSecondCopyEnabled(checked)
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.presenter.imports.importSecondCopyEnabled
                    CustomButton {
                        objectName: "importChooseSecondCopy"
                        Layout.alignment: Qt.AlignTop
                        text: qsTr("change")
                        Accessible.name: qsTr("Choose Second Copy…")
                        onClicked: root.chooseSecondCopyRequested()
                    }
                    CustomLabel {
                        objectName: "importSecondCopyPath"
                        Layout.fillWidth: true
                        text: root.presenter.imports.importSecondCopyDestination.length ? root.presenter.imports.importSecondCopyDestination : qsTr("No second copy selected")
                        wrapMode: Text.WrapAnywhere
                        color: Theme.placeholderTextColor
                    }
                }
            }
            ColumnLayout {
                id: destinationSection
                objectName: "importDestinationSection"
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.presenter.imports.importMode !== "add"
                spacing: Fonts.size8
                CustomLabel {
                    text: qsTr("Destination")
                    font.bold: true
                }
                RowLayout {
                    Layout.fillWidth: true
                    CustomButton {
                        objectName: "importChangeDestination"
                        Layout.alignment: Qt.AlignTop
                        text: qsTr("change")
                        Accessible.name: qsTr("Choose Destination…")
                        onClicked: root.chooseDestinationRequested()
                    }
                    CustomLabel {
                        objectName: "importDestinationPath"
                        Layout.fillWidth: true
                        text: root.presenter.imports.importDestination
                        wrapMode: Text.WrapAnywhere
                        font.bold: true
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: root.presenter.imports.importDestination.length > 0 && root.presenter.imports.importDestinationError.length > 0
                    text: root.presenter.imports.importDestinationError
                    color: Theme.warningColor
                    wrapMode: Text.WordWrap
                }
                CustomButton {
                    visible: root.presenter.imports.importDestinationError.length > 0 && root.presenter.imports.importDestination.length > 0
                    text: qsTr("Check again")
                    onClicked: root.presenter.imports.setImportDestination(root.presenter.imports.importDestination)
                }
                RowLayout {
                    Layout.fillWidth: true
                    CustomLabel {
                        text: qsTr("Organize")
                    }
                    CustomComboBox {
                        Layout.fillWidth: true
                        Accessible.name: qsTr("Organize")
                        model: [qsTr("Into one folder"), qsTr("Preserve hierarchy"), qsTr("By date (YYYY/MM/DD)"), qsTr("By month (YYYY/MM)")]
                        currentIndex: root.presenter.imports.importOrganization === "hierarchy" ? 1 : root.presenter.imports.importOrganization === "date" ? 2 : root.presenter.imports.importOrganization === "month" ? 3 : 0
                        onActivated: function (index) {
                            root.presenter.imports.setImportOrganization(["single", "hierarchy", "date", "month"][index]);
                        }
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Folders are created only when you import.")
                    wrapMode: Text.WordWrap
                    color: Theme.placeholderTextColor
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: root.presenter.imports.importDestinationPreviewActive
                    text: qsTr("Planning destination…")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    visible: root.presenter.imports.importDestinationPreviewError.length > 0
                    text: root.presenter.imports.importDestinationPreviewError
                    wrapMode: Text.WordWrap
                    color: Theme.warningColor
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
                        folderModel: root.presenter.imports.importDestinationFolders
                        ScrollBar.vertical: CustomScrollBar {
                            policy: ScrollBar.AsNeeded
                        }
                        onFolderChosen: function (path) {
                            root.presenter.imports.setImportDestination(path);
                        }
                    }
                }
            }
        }
    }
}
