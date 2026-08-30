import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0
import "../chrome" as Chrome

Item {
    id: root
    property var presenter
    property var commands
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    readonly property var presets: hasPresenter ? presenter.editPresets : []
    readonly property var saveParameters: hasPresenter ? presenter.modifiedParameterChoices : []

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        CustomLabel {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.topMargin: Fonts.size12
            Layout.bottomMargin: Fonts.size8
            text: qsTr("Presets")
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            spacing: Fonts.size8

            CustomButton {
                Layout.fillWidth: true
                objectName: "presetImportButton"
                text: qsTr("Import…")
                enabled: root.hasSelection && root.commands
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.presetImport)
            }

            CustomButton {
                Layout.fillWidth: true
                objectName: "presetSaveButton"
                text: qsTr("Save…")
                enabled: root.hasSelection && root.saveParameters.length > 0 && root.commands
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.presetSave)
            }
        }

        CustomLabel {
            Layout.fillWidth: true
            Layout.leftMargin: Fonts.standardMargin
            Layout.rightMargin: Fonts.standardMargin
            Layout.bottomMargin: Fonts.size8
            visible: root.presets.length === 0
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("Import a preset, or save selected changes from the current photo.")
        }

        ListView {
            id: presetList
            objectName: "presetList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Fonts.listItemHeight
            Layout.leftMargin: Fonts.size8
            Layout.rightMargin: Fonts.size8
            Layout.bottomMargin: Fonts.size8
            clip: true
            spacing: Fonts.size2
            boundsBehavior: Flickable.StopAtBounds
            visible: root.presets.length > 0
            model: root.presets
            delegate: Item {
                id: presetRow
                required property var modelData
                width: ListView.view.width
                height: Fonts.listItemHeight

                Rectangle {
                    anchors.fill: parent
                    radius: Fonts.buttonBorderRadius
                    color: presetMouse.containsMouse ? Theme.buttonHoveredColor : "transparent"
                }

                CustomLabel {
                    anchors.fill: parent
                    anchors.leftMargin: Fonts.size8
                    anchors.rightMargin: Fonts.size8 + Fonts.iconButtonSize
                    elide: Text.ElideRight
                    wrapMode: Text.NoWrap
                    maximumLineCount: 1
                    verticalAlignment: Text.AlignVCenter
                    text: presetRow.modelData.name
                }

                MouseArea {
                    id: presetMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    enabled: root.commands
                    cursorShape: Qt.PointingHandCursor
                    onClicked: function (mouse) {
                        if (!root.commands)
                            return;
                        if (mouse.button === Qt.RightButton) {
                            presetMenu.presetPath = presetRow.modelData.path;
                            presetMenu.presetName = presetRow.modelData.name;
                            presetMenu.popup();
                            return;
                        }
                        if (root.hasSelection)
                            root.commands.run(root.commands.ids.presetApplyPath, presetRow.modelData.path);
                    }
                }
            }
        }
    }

    Chrome.StudioContextMenu {
        id: presetMenu
        objectName: "presetContextMenu"
        property string presetPath: ""
        property string presetName: ""

        Chrome.StudioContextMenuItem {
            objectName: "presetRenameItem"
            text: qsTr("Rename…")
            enabled: presetMenu.presetPath.length > 0 && presetMenu.presetName.length > 0 && root.commands
            onTriggered: if (root.commands)
                root.commands.run(root.commands.ids.presetRename, {
                    "path": presetMenu.presetPath,
                    "name": presetMenu.presetName
                })
        }
        Chrome.StudioContextMenuItem {
            objectName: "presetDeleteItem"
            text: qsTr("Delete…")
            enabled: presetMenu.presetPath.length > 0 && presetMenu.presetName.length > 0 && root.commands
            onTriggered: if (root.commands)
                root.commands.run(root.commands.ids.presetDelete, {
                    "path": presetMenu.presetPath,
                    "name": presetMenu.presetName
                })
        }
        Chrome.StudioContextMenuSeparator {}
        Chrome.StudioContextMenuItem {
            objectName: "presetCopyInfoItem"
            text: qsTr("Copy Preset Info")
            enabled: presetMenu.presetPath.length > 0 && root.commands
            onTriggered: if (root.commands)
                root.commands.run(root.commands.ids.presetCopyInfo, presetMenu.presetPath)
        }
    }
}
