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
    readonly property var historyEntries: {
        const all = root.history;
        const out = [];
        for (let i = 0; i < all.length; ++i) {
            if ((all[i].kind || "history") !== "snapshot")
                out.push(all[i]);
        }
        if (root.hasSelection) {
            out.push({
                         "id": 0,
                         "seq": 0,
                         "kind": "history",
                         "label": "",
                         "summary": qsTr("Original")
                     });
        }
        return out;
    }
    readonly property var snapshotEntries: {
        const all = root.history;
        const out = [];
        for (let i = 0; i < all.length; ++i) {
            if (all[i].kind === "snapshot")
                out.push(all[i]);
        }
        return out;
    }
    readonly property var activeId: hasPresenter ? presenter.activeHistoryId : 0
    readonly property var activeSeq: hasPresenter ? presenter.activeHistorySeq : 0
    spacing: 0

    function entryText(entry, snapshotList) {
        if (!entry)
            return "";
        if (snapshotList) {
            if (entry.label && entry.label.length)
                return entry.label;
            if (entry.summary && entry.summary.length)
                return entry.summary;
            return qsTr("Snapshot #%1").arg(entry.seq);
        }
        if (entry.summary && entry.summary.length)
            return entry.summary;
        if (entry.label && entry.label.length)
            return entry.label;
        return qsTr("Edit #%1").arg(entry.seq);
    }

    component HistoryEntryRow: Item {
        id: row
        required property var modelData
        required property bool dimFuture
        required property bool snapshotList
        property bool renaming: false
        readonly property bool current: modelData.id === root.activeId
        readonly property bool inactive: row.dimFuture && modelData.seq > root.activeSeq
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
            visible: !row.renaming
            font: row.current ? Fonts.makeBoldFont(Fonts.standardFont) : Fonts.standardFont
            textColor: row.inactive ? Theme.disabledTextColor : Theme.textColor
            text: root.entryText(modelData, row.snapshotList)
        }

        TextInput {
            id: renameField
            anchors.fill: parent
            anchors.leftMargin: Fonts.size8
            anchors.rightMargin: Fonts.size8
            visible: row.renaming
            clip: true
            color: Theme.textColor
            font: Fonts.standardFont
            selectByMouse: true
            verticalAlignment: TextInput.AlignVCenter
            onEditingFinished: row.commitRename()
            Keys.onEscapePressed: function (event) {
                row.renaming = false;
                event.accepted = true;
            }
        }

        MouseArea {
            id: rowMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: root.hasSelection && root.commands && !row.renaming
            cursorShape: Qt.PointingHandCursor
            onClicked: if (root.commands)
                root.commands.restoreHistory(row.modelData.id)
            onDoubleClicked: {
                if (!row.snapshotList || !root.commands)
                    return;
                renameField.text = root.entryText(modelData, true);
                row.renaming = true;
                renameField.forceActiveFocus();
                renameField.selectAll();
            }
        }

        function commitRename() {
            if (!row.renaming)
                return;
            row.renaming = false;
            const next = renameField.text.trim();
            const current = root.entryText(modelData, true);
            if (!next.length || next === current || !root.commands)
                return;
            root.commands.renameSnapshot(modelData.id, next);
        }
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size12
        Layout.bottomMargin: Fonts.size8
        text: qsTr("History")
        font.bold: true
    }

    GridLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        Layout.bottomMargin: Fonts.size8
        columns: 2
        columnSpacing: Fonts.size4
        rowSpacing: Fonts.size4

        CustomButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            text: qsTr("Undo")
            enabled: root.hasPresenter && root.presenter.canUndo
            onClicked: if (root.commands)
                root.commands.undo.trigger()
        }
        CustomButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            text: qsTr("Redo")
            enabled: root.hasPresenter && root.presenter.canRedo
            onClicked: if (root.commands)
                root.commands.redo.trigger()
        }
        CustomButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            text: root.hasPresenter && root.presenter.beforeAfter ? qsTr("After") : qsTr("Before")
            enabled: root.hasSelection
            onClicked: if (root.commands)
                root.commands.beforeAfter.trigger()
        }
        CustomButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            text: qsTr("Reset all")
            enabled: root.hasSelection
            onClicked: if (root.commands)
                root.commands.resetEdits.trigger()
        }
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        Layout.bottomMargin: Fonts.size8
        visible: !root.hasSelection
        wrapMode: Text.WordWrap
        color: Theme.placeholderTextColor
        text: qsTr("Select a photo to see its edit history.")
    }

    ListView {
        id: historyList
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: Fonts.listItemHeight
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        clip: true
        spacing: Fonts.size2
        boundsBehavior: Flickable.StopAtBounds
        visible: root.hasSelection
        model: root.historyEntries
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }
        delegate: HistoryEntryRow {
            dimFuture: true
            snapshotList: false
        }
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        visible: root.hasSelection
        color: Theme.dividerColor
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size8
        Layout.bottomMargin: Fonts.size8
        visible: root.hasSelection
        text: qsTr("Snapshots")
        font.bold: true
    }

    CustomLabel {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        Layout.bottomMargin: Fonts.size8
        visible: root.hasSelection && root.snapshotEntries.length === 0
        wrapMode: Text.WordWrap
        color: Theme.placeholderTextColor
        text: qsTr("Named snapshots appear here.")
    }

    ListView {
        id: snapshotList
        Layout.fillWidth: true
        Layout.preferredHeight: Math.min(root.snapshotEntries.length * Fonts.listItemHeight,
                                         Fonts.scaledUiSize(180))
        Layout.maximumHeight: Fonts.scaledUiSize(180)
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        clip: true
        spacing: Fonts.size2
        boundsBehavior: Flickable.StopAtBounds
        visible: root.hasSelection && root.snapshotEntries.length > 0
        model: root.snapshotEntries
        ScrollBar.vertical: ScrollBar {
            policy: ScrollBar.AsNeeded
        }
        delegate: HistoryEntryRow {
            dimFuture: false
            snapshotList: true
        }
    }

    CustomButton {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        Layout.topMargin: Fonts.size8
        text: qsTr("Snapshot")
        enabled: root.hasSelection && root.commands
        onClicked: if (root.commands)
            root.commands.createSnapshot("")
    }

    GridLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.size8
        Layout.rightMargin: Fonts.size8
        Layout.topMargin: Fonts.size4
        Layout.bottomMargin: Fonts.size8
        columns: 2
        columnSpacing: Fonts.size4
        rowSpacing: Fonts.size4

        CustomButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            text: qsTr("Copy")
            enabled: root.hasSelection && root.commands
            onClicked: if (root.commands)
                root.commands.copyEdits.trigger()
        }
        CustomButton {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.minimumWidth: 0
            text: qsTr("Paste")
            enabled: root.hasPresenter && root.presenter.hasCopiedEdits && root.hasSelection &&
                     root.commands
            onClicked: if (root.commands)
                root.commands.pasteEdits.trigger()
        }
    }
}
