import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importWorkspace"
    required property var presenter
    signal closeRequested
    readonly property bool compact: width < 1000
    readonly property bool locked: presenter.importWorkActive || presenter.importPreflightActive
    readonly property string candidateKeyboardHelp: qsTr("Arrows navigate · Shift selects a range · Ctrl/⌘ preserves selection · Space checks")
    color: Theme.windowColor
    focus: visible
    Keys.onEscapePressed: if (!presenter.importWorkActive)
        root.closeRequested()

    ImportDialogs {
        id: dialogs
        presenter: root.presenter
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: headerRow.implicitHeight + Fonts.size8 * 2
            color: Theme.toolbarSurfaceColor

            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                anchors.topMargin: Fonts.size8
                anchors.bottomMargin: Fonts.size8
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
                    visible: !root.compact
                }

                CustomLabel {
                    id: routeLabel
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.leftMargin: Fonts.size12
                    color: Theme.placeholderTextColor
                    elide: Text.ElideMiddle
                    text: root.presenter.importSourceRoot.length ? root.presenter.importSourceRoot + (root.presenter.importMode !== "add" && root.presenter.importDestination.length > 0 ? "  →  " + root.presenter.importDestination : "") : ""
                    ToolTip.visible: routeHover.hovered && text.length > 0
                    ToolTip.text: text

                    HoverHandler {
                        id: routeHover
                    }
                }

                CustomButton {
                    visible: root.compact
                    text: qsTr("Source")
                    onClicked: sourceDrawer.open()
                }

                CustomButton {
                    visible: root.compact
                    text: qsTr("Destination")
                    onClicked: destinationDrawer.open()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            ImportSourcePanel {
                visible: !root.compact
                Layout.preferredWidth: 240
                Layout.fillHeight: true
                presenter: root.presenter
                onChooseRequested: dialogs.chooseSource()
            }

            ColumnLayout {
                id: selectionArea
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.fillHeight: true
                spacing: Fonts.size4

                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Fonts.size8

                    CustomLabel {
                        text: qsTr("New Photos")
                        font.bold: true
                        visible: selectionArea.width >= 560
                    }

                    Item {
                        Layout.fillWidth: true
                    }

                    CustomButton {
                        text: qsTr("Check All")
                        enabled: !root.locked
                        onClicked: root.presenter.importCandidates.setAllSelected(true)
                    }

                    CustomButton {
                        text: qsTr("Uncheck All")
                        enabled: !root.locked
                        onClicked: root.presenter.importCandidates.setAllSelected(false)
                    }

                    Slider {
                        id: thumbnailSize
                        Layout.preferredWidth: Math.max(70, Math.min(120, selectionArea.width * 0.16))
                        from: 120
                        to: 320
                        value: 180
                        Accessible.name: qsTr("Thumbnail size")
                    }
                }

                ImportPhotoGrid {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    presenter: root.presenter
                    preferredCell: thumbnailSize.value
                    enabled: !root.locked
                }

                CustomLabel {
                    Layout.fillWidth: true
                    Layout.margins: Fonts.size8
                    elide: Text.ElideRight
                    text: {
                        if (root.presenter.importScanActive)
                            return qsTr("Checking %1 of %2…").arg(root.presenter.importScanCompleted).arg(root.presenter.importScanTotal);
                        const duplicates = qsTr("Duplicate photos: %1").arg(root.presenter.importDuplicateCount);
                        return selectionArea.width >= 720 ? duplicates + " · " + root.candidateKeyboardHelp : duplicates;
                    }
                    color: Theme.placeholderTextColor
                    ToolTip.visible: candidateHelpHover.hovered && !root.presenter.importScanActive
                    ToolTip.text: root.candidateKeyboardHelp

                    HoverHandler {
                        id: candidateHelpHover
                    }
                }
            }

            ImportDestinationPanel {
                visible: !root.compact
                Layout.preferredWidth: 320
                Layout.fillHeight: true
                presenter: root.presenter
                onChooseDestinationRequested: dialogs.chooseDestination()
                onChooseSecondCopyRequested: dialogs.chooseSecondCopy()
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(64, footer.implicitHeight + 24)
            color: Theme.toolbarSurfaceColor

            RowLayout {
                id: footer
                anchors.fill: parent
                anchors.margins: Fonts.standardMargin
                spacing: Fonts.size12

                ColumnLayout {
                    Layout.fillWidth: true

                    CustomLabel {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        elide: Text.ElideRight
                        text: qsTr("Selected: %1 photos · %2 MB").arg(root.presenter.importCandidates.selectedCount).arg((root.presenter.importCandidates.selectedBytes / 1048576).toFixed(1))
                    }

                    CustomLabel {
                        Layout.fillWidth: true
                        visible: root.presenter.errorText.length > 0
                        text: root.presenter.errorText
                        color: Theme.errorColor
                        wrapMode: Text.WordWrap
                    }
                }

                CustomButton {
                    text: qsTr("Cancel")
                    enabled: !root.presenter.importWorkActive
                    onClicked: root.closeRequested()
                }

                CustomButton {
                    objectName: "importConfirmButton"
                    text: root.presenter.importPreflightActive ? qsTr("Checking destination…") : qsTr("Import %1 photos").arg(root.presenter.importCandidates.selectedCount)
                    enabled: root.presenter.importReady
                    onClicked: root.presenter.startPlannedImport()
                }
            }
        }
    }

    Drawer {
        id: sourceDrawer
        edge: Qt.LeftEdge
        width: Math.min(280, root.width - 48)
        height: root.height

        ImportSourcePanel {
            anchors.fill: parent
            presenter: root.presenter
            onChooseRequested: dialogs.chooseSource()
        }
    }

    Drawer {
        id: destinationDrawer
        edge: Qt.RightEdge
        width: Math.min(340, root.width - 48)
        height: root.height

        ImportDestinationPanel {
            anchors.fill: parent
            presenter: root.presenter
            onChooseDestinationRequested: dialogs.chooseDestination()
            onChooseSecondCopyRequested: dialogs.chooseSecondCopy()
        }
    }

    onVisibleChanged: if (!visible) {
        sourceDrawer.close();
        destinationDrawer.close();
    }
}
