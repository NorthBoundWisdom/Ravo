import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "importWorkspace"
    required property var presenter
    property var commands
    signal closeRequested
    readonly property bool compact: width < 1000
    readonly property bool locked: presenter.imports.importWorkActive || presenter.imports.importPreflightActive || presenter.imports.importInteractionBlocked
    readonly property string candidateKeyboardHelp: qsTr("Arrows navigate · Shift selects a range · Ctrl/⌘ preserves selection · Space checks")
    color: Theme.windowColor
    focus: visible
    Keys.onEscapePressed: if (!presenter.imports.importWorkActive)
        root.commands.run(root.commands.ids.windowDismiss)

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
                    enabled: !root.presenter.imports.importWorkActive
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
                    text: root.presenter.imports.importSourceRoot.length ? root.presenter.imports.importSourceRoot + (root.presenter.imports.importMode !== "add" && root.presenter.imports.importDestination.length > 0 ? "  →  " + root.presenter.imports.importDestination : "") : ""
                    CustomToolTip {
                        visible: routeHover.hovered && routeLabel.text.length > 0
                        text: routeLabel.text
                    }

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
                commands: root.commands
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
                        onClicked: root.presenter.imports.importCandidates.setAllSelected(true)
                    }

                    CustomButton {
                        text: qsTr("Uncheck All")
                        enabled: !root.locked
                        onClicked: root.presenter.imports.importCandidates.setAllSelected(false)
                    }

                    CustomSlider {
                        id: thumbnailSize
                        objectName: "importThumbnailSize"
                        Layout.fillWidth: false
                        Layout.minimumWidth: Fonts.size80
                        Layout.preferredWidth: Math.max(Fonts.size80, Math.min(Fonts.size120, selectionArea.width * 0.16))
                        from: 120
                        to: 320
                        value: 180
                        stepSize: 1
                        validatorDecimals: 0
                        showTitle: false
                        showValueLabel: false
                        showStepButton: false
                        onValueEdited: function (value) {
                            thumbnailSize.value = value;
                        }
                        onValueCommitted: function (value) {
                            thumbnailSize.value = value;
                        }
                        Accessible.name: qsTr("Thumbnail size")
                    }
                }

                ImportPhotoGrid {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    presenter: root.presenter
                    commands: root.commands
                    preferredCell: thumbnailSize.visualValue
                    enabled: !root.locked
                }

                CustomLabel {
                    Layout.fillWidth: true
                    Layout.margins: Fonts.size8
                    elide: Text.ElideRight
                    text: {
                        if (root.presenter.imports.importScanActive)
                            return qsTr("Checking %1 of %2…").arg(root.presenter.imports.importScanCompleted).arg(root.presenter.imports.importScanTotal);
                        const duplicates = qsTr("Duplicate photos: %1").arg(root.presenter.imports.importDuplicateCount);
                        return selectionArea.width >= 720 ? duplicates + " · " + root.candidateKeyboardHelp : duplicates;
                    }
                    color: Theme.placeholderTextColor
                    CustomToolTip {
                        visible: candidateHelpHover.hovered && !root.presenter.imports.importScanActive
                        text: root.candidateKeyboardHelp
                    }

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
                        text: qsTr("Selected: %1 photos · %2 MB").arg(root.presenter.imports.importCandidates.selectedCount).arg((root.presenter.imports.importCandidates.selectedBytes / 1048576).toFixed(1))
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
                    objectName: "importSourceCancel"
                    text: qsTr("Cancel")
                    enabled: !root.presenter.imports.importWorkActive && !root.presenter.imports.importPreflightActive && root.presenter.imports.importSourceRoot.length > 0
                    onClicked: root.commands.run(root.commands.ids.libraryCancelOperation)
                }

                CustomButton {
                    objectName: "importConfirmButton"
                    text: root.presenter.imports.importPreflightActive ? qsTr("Checking destination…") : qsTr("Import %1 photos").arg(root.presenter.imports.importCandidates.selectedCount)
                    enabled: root.presenter.imports.importReady
                    onClicked: root.presenter.imports.startPlannedImport()
                }
            }
        }
    }

    CustomDrawer {
        id: sourceDrawer
        edge: Qt.LeftEdge
        width: Math.min(280, root.width - 48)
        height: root.height

        ImportSourcePanel {
            anchors.fill: parent
            presenter: root.presenter
            commands: root.commands
            onChooseRequested: dialogs.chooseSource()
        }
    }

    CustomDrawer {
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

    CustomPopup {
        objectName: "importPlanningDialog"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        visible: root.visible && root.presenter.imports.importInteractionBlocked
        closePolicy: Popup.NoAutoClose
        padding: Fonts.standardMargin
        background: Rectangle {
            color: Theme.railSurfaceColor
            border.color: Theme.dividerColor
            border.width: ControlState.borderThin
            radius: ControlState.radiusSmall
        }
        contentItem: ColumnLayout {
            spacing: Fonts.size12
            Keys.onEscapePressed: root.commands.run(root.commands.ids.libraryCancelOperation)
            CustomBusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: true
            }
            CustomLabel {
                text: qsTr("Planning destination…")
            }
            CustomButton {
                objectName: "importPlanningCancel"
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Cancel")
                onClicked: root.commands.run(root.commands.ids.libraryCancelOperation)
            }
        }
    }
}
