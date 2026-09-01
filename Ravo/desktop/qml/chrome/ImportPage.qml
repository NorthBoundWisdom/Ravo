import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property int selectionAnchor: -1
    signal chooseSourceRequested
    signal chooseDestinationRequested
    signal chooseSecondCopyRequested
    signal closeRequested

    color: Theme.windowColor
    focus: visible
    Keys.onEscapePressed: if (!presenter.importWorkActive)
        root.closeRequested()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.toolbarHeight + Fonts.size8
            color: Theme.toolbarSurfaceColor
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                spacing: Fonts.size12
                CustomButton {
                    text: qsTr("Back")
                    enabled: !root.presenter.importWorkActive
                    onClicked: root.closeRequested()
                }
                CustomLabel {
                    text: qsTr("Import Photos")
                    font.bold: true
                    font.pixelSize: Fonts.size18
                }
                Item {
                    Layout.fillWidth: true
                }
                SegmentedControl {
                    model: [qsTr("Add"), qsTr("Copy"), qsTr("Move")]
                    currentIndex: root.presenter.importMode === "copy" ? 1 : root.presenter.importMode === "move" ? 2 : 0
                    enabled: !root.presenter.importWorkActive
                    onActivated: function (index) {
                        root.presenter.setImportMode(["add", "copy", "move"][index]);
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.preferredWidth: 260
                Layout.fillHeight: true
                color: Theme.railSurfaceColor
                border.color: Theme.dividerColor
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Fonts.standardMargin
                    spacing: Fonts.size8
                    CustomLabel {
                        text: qsTr("Source")
                        font.bold: true
                    }
                    CustomButton {
                        Layout.fillWidth: true
                        text: qsTr("Choose Source…")
                        enabled: !root.presenter.importWorkActive
                        onClicked: root.chooseSourceRequested()
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        text: root.presenter.importSourceRoot.length ? root.presenter.importSourceRoot : qsTr("No source selected")
                        wrapMode: Text.WrapAnywhere
                        color: Theme.placeholderTextColor
                    }
                    CustomCheckBox {
                        text: qsTr("Include subfolders")
                        checked: root.presenter.importRecursive
                        enabled: !root.presenter.importWorkActive
                        onToggled: root.presenter.setImportRecursive(checked)
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.dividerColor
                    }
                    CustomLabel {
                        text: qsTr("Selected: %1 of %2").arg(root.presenter.importCandidates.selectedCount).arg(root.presenter.importCandidates.rowCount())
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        CustomButton {
                            Layout.fillWidth: true
                            text: qsTr("Check All")
                            onClicked: root.presenter.importCandidates.setAllSelected(true)
                        }
                        CustomButton {
                            Layout.fillWidth: true
                            text: qsTr("Uncheck All")
                            onClicked: root.presenter.importCandidates.setAllSelected(false)
                        }
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.windowColor
                BusyIndicator {
                    anchors.centerIn: parent
                    running: root.presenter.importScanActive
                    visible: running
                }
                CustomLabel {
                    anchors.centerIn: parent
                    visible: !root.presenter.importScanActive && root.presenter.importCandidates.rowCount() === 0
                    text: root.presenter.importSourceRoot.length ? qsTr("No supported photos found") : qsTr("Choose a source folder")
                    color: Theme.placeholderTextColor
                }
                GridView {
                    id: candidateGrid
                    anchors.fill: parent
                    anchors.margins: Fonts.size8
                    visible: !root.presenter.importScanActive
                    clip: true
                    cellWidth: 180
                    cellHeight: 150
                    model: root.presenter.importCandidates
                    delegate: Item {
                        required property int index
                        required property string displayName
                        required property string mediaType
                        required property int pixelWidth
                        required property int pixelHeight
                        required property bool selected
                        required property bool eligible
                        required property bool duplicate
                        required property url thumbnailUrl
                        required property string errorText
                        width: candidateGrid.cellWidth
                        height: candidateGrid.cellHeight
                        Component.onCompleted: root.presenter.ensureImportThumbnail(index)
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: Fonts.size3
                            color: Theme.imageSurroundColor
                            border.width: selected ? ControlState.borderFocus : ControlState.borderThin
                            border.color: selected ? Theme.highlightColor : Theme.dividerColor
                            Image {
                                anchors.fill: parent
                                anchors.margins: 3
                                source: thumbnailUrl
                                fillMode: Image.PreserveAspectFit
                                asynchronous: true
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 28
                                color: "#aa000000"
                                CustomLabel {
                                    anchors.centerIn: parent
                                    width: parent.width - 8
                                    text: displayName
                                    horizontalAlignment: Text.AlignHCenter
                                    elide: Text.ElideMiddle
                                }
                            }
                            CustomCheckBox {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 4
                                checked: selected
                                enabled: eligible
                                onClicked: root.presenter.importCandidates.toggleSelected(index)
                            }
                            CustomLabel {
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 6
                                visible: duplicate || !eligible
                                text: duplicate ? qsTr("Already imported") : qsTr("Unavailable")
                                color: Theme.warningColor
                            }
                            MouseArea {
                                anchors.fill: parent
                                z: -1
                                enabled: eligible
                                onClicked: function (mouse) {
                                    const additive = (mouse.modifiers & (Qt.ControlModifier | Qt.MetaModifier)) !== 0;
                                    if ((mouse.modifiers & Qt.ShiftModifier) !== 0 && root.selectionAnchor >= 0)
                                        root.presenter.importCandidates.selectRange(root.selectionAnchor, index, additive);
                                    else
                                        root.presenter.importCandidates.toggleSelected(index);
                                    root.selectionAnchor = index;
                                }
                            }
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                color: Theme.railSurfaceColor
                border.color: Theme.dividerColor
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Fonts.standardMargin
                    spacing: Fonts.size10
                    CustomLabel {
                        text: qsTr("File Handling")
                        font.bold: true
                    }
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
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.dividerColor
                    }
                    CustomLabel {
                        text: qsTr("Destination")
                        font.bold: true
                        visible: root.presenter.importMode !== "add"
                    }
                    CustomButton {
                        Layout.fillWidth: true
                        visible: root.presenter.importMode !== "add"
                        text: qsTr("Choose Destination…")
                        onClicked: root.chooseDestinationRequested()
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        visible: root.presenter.importMode !== "add"
                        text: root.presenter.importDestination.length ? root.presenter.importDestination : qsTr("No destination selected")
                        wrapMode: Text.WrapAnywhere
                        color: Theme.placeholderTextColor
                    }
                    CustomLabel {
                        text: qsTr("Organize")
                        visible: root.presenter.importMode !== "add"
                    }
                    CustomComboBox {
                        Layout.fillWidth: true
                        visible: root.presenter.importMode !== "add"
                        model: [qsTr("Into one folder"), qsTr("Preserve hierarchy"), qsTr("By date (YYYY/MM/DD)")]
                        currentIndex: root.presenter.importOrganization === "hierarchy" ? 1 : root.presenter.importOrganization === "date" ? 2 : 0
                        onActivated: function (index) {
                            root.presenter.setImportOrganization(["single", "hierarchy", "date"][index]);
                        }
                    }
                    CustomLabel {
                        text: qsTr("Rename template")
                        visible: root.presenter.importMode !== "add"
                    }
                    CustomTextField {
                        objectName: "importFilenameTemplate"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        visible: root.presenter.importMode !== "add"
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
                        visible: root.presenter.importMode !== "add"
                        text: qsTr("Tokens: {date}, {stem}, {sequence}, {ext}")
                        wrapMode: Text.WordWrap
                        color: Theme.placeholderTextColor
                    }
                    CustomLabel {
                        text: qsTr("Second copy")
                        font.bold: true
                        visible: root.presenter.importMode !== "add"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.presenter.importMode !== "add"
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
                        visible: root.presenter.importMode !== "add"
                        text: root.presenter.importSecondCopyDestination.length ? root.presenter.importSecondCopyDestination : qsTr("No second copy selected")
                        wrapMode: Text.WrapAnywhere
                        color: Theme.placeholderTextColor
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        visible: root.presenter.importMode === "move"
                        text: qsTr("Move verifies every requested copy before removing its source.")
                        wrapMode: Text.WordWrap
                        color: Theme.warningColor
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.toolbarHeight + Fonts.size8
            color: Theme.toolbarSurfaceColor
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                CustomLabel {
                    Layout.fillWidth: true
                    text: root.presenter.importWorkActive ? qsTr("Importing %1 / %2…").arg(root.presenter.importWorkCompleted).arg(root.presenter.importWorkTotal) : ""
                }
                CustomButton {
                    text: qsTr("Cancel")
                    visible: root.presenter.importWorkActive
                    onClicked: root.presenter.cancelCatalogOperation()
                }
                CustomButton {
                    text: qsTr("Import")
                    enabled: !root.presenter.importWorkActive && !root.presenter.importScanActive && root.presenter.importCandidates.selectedCount > 0 && (root.presenter.importMode === "add" || root.presenter.importDestination.length > 0)
                    onClicked: root.presenter.startPlannedImport()
                }
            }
        }
    }
}
