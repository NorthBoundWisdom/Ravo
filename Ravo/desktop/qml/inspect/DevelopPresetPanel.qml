import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property var presenter
    property var commands
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    readonly property var presets: hasPresenter ? presenter.editPresets : []
    spacing: 0

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size12
        Layout.bottomMargin: Fonts.size8
        text: qsTr("Presets")
        font.bold: true
    }

    CustomButton {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        Layout.bottomMargin: Fonts.size8
        objectName: "presetImportButton"
        text: qsTr("Import…")
        enabled: root.hasSelection && root.commands
        onClicked: if (root.commands)
            root.commands.run(root.commands.ids.presetImport)
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        Layout.bottomMargin: Fonts.size8
        visible: root.presets.length === 0
        wrapMode: Text.WordWrap
        color: Theme.placeholderTextColor
        text: qsTr("Import a Lightroom .xmp preset or a Ravo style to apply it to the selected photo.")
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
                enabled: root.hasSelection && root.commands
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.commands)
                    root.commands.run(root.commands.ids.presetApplyPath, presetRow.modelData.path)
            }
        }
    }
}
