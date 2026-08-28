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
    readonly property var history: hasPresenter ? presenter.recipeHistory : []
    readonly property var activeId: hasPresenter ? presenter.activeHistoryId : 0
    readonly property var activeSeq: hasPresenter ? presenter.activeHistorySeq : 0
    spacing: 0

    function entryText(entry) {
        if (!entry)
            return "";
        if (entry.summary && entry.summary.length)
            return entry.summary;
        if (entry.label && entry.label.length)
            return entry.label;
        if (entry.kind === "snapshot")
            return qsTr("Snapshot #%1").arg(entry.seq);
        return qsTr("Edit #%1").arg(entry.seq);
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size12
        Layout.bottomMargin: Fonts.size8
        text: qsTr("History")
        font.bold: true
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        Layout.bottomMargin: Fonts.size8
        visible: !root.hasSelection || root.history.length === 0
        wrapMode: Text.WordWrap
        color: Theme.placeholderTextColor
        text: !root.hasSelection ? qsTr("Select a photo to see its edit history.") :
                                   qsTr("No saved edits yet. Changes appear here after they are stored.")
    }

    ListView {
        id: historyList
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        clip: true
        spacing: Fonts.size2
        boundsBehavior: Flickable.StopAtBounds
        visible: root.hasSelection && root.history.length > 0
        model: root.history
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }

        delegate: Item {
            id: row
            required property var modelData
            readonly property bool current: modelData.id === root.activeId
            readonly property bool inactive: modelData.seq > root.activeSeq
            width: ListView.view.width
            height: Fonts.listItemHeight
            opacity: row.inactive ? 0.55 : 1

            Rectangle {
                anchors.fill: parent
                radius: Fonts.buttonBorderRadius
                color: row.current || rowMouse.containsMouse ? Theme.buttonHoveredColor : "transparent"
            }

            CustomLabel {
                anchors.fill: parent
                anchors.leftMargin: Fonts.size8
                anchors.rightMargin: Fonts.size8
                elide: Text.ElideRight
                wrapMode: Text.NoWrap
                maximumLineCount: 1
                verticalAlignment: Text.AlignVCenter
                font: row.current ? Fonts.makeBoldFont(Fonts.standardFont) : Fonts.standardFont
                textColor: row.inactive ? Theme.disabledTextColor : Theme.textColor
                text: root.entryText(modelData)
            }

            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                enabled: root.hasSelection && root.commands
                cursorShape: Qt.PointingHandCursor
                onClicked: if (root.commands)
                    root.commands.restoreHistory(row.modelData.id)
            }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Theme.dividerColor
    }

    CustomButton {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        Layout.topMargin: Fonts.size8
        Layout.bottomMargin: Fonts.size8
        text: qsTr("Snapshot")
        enabled: root.hasSelection && root.commands
        onClicked: if (root.commands)
            root.commands.createSnapshot(qsTr("Snapshot"))
    }
}
