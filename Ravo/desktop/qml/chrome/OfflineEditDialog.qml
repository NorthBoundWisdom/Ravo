import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

DialogShell {
    id: root
    objectName: "OfflineEditDialog"
    titleText: qsTr("Offline-edit proxies")
    width: Fonts.messageDialogWidth
    bodyFillHeight: false
    showCloseButton: true

    required property var presenter
    required property var commands

    readonly property var status: presenter && presenter.offlineEditMediaStatus ? presenter.offlineEditMediaStatus : ({})
    readonly property bool hasSelection: Boolean(presenter && presenter.selectedAssetId && presenter.selectedAssetId.length)
    readonly property bool proxyPresent: Boolean(status.proxyPresent)
    readonly property bool pinned: Boolean(status.pinned)

    function openForSelection() {
        if (root.commands) {
            root.commands.run(root.commands.ids.photoOfflineEditRefreshStatus);
            root.commands.run(root.commands.ids.photoOfflineEditList);
        }
        openDialog();
    }

    function statusLine() {
        if (!root.hasSelection)
            return qsTr("Select a photo.");
        const media = status.mediaState || "missing";
        const reason = status.reason || "";
        const provenance = status.pixelProvenance || "";
        const pin = root.pinned ? qsTr("pinned") : qsTr("unpinned");
        if (provenance.length)
            return qsTr("media_state=%1 · %2 · %3 · %4").arg(media).arg(pin).arg(provenance).arg(reason);
        return qsTr("media_state=%1 · %2 · %3").arg(media).arg(pin).arg(reason);
    }

    ColumnLayout {
        spacing: Fonts.size8
        width: parent.width

        CustomLabel {
            objectName: "offlineEditStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.statusLine()
        }
        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Baked sRGB proxy: Develop applies identity while media_state=proxy (no double-grade). Export stays fail-closed until reconnect. Before/After and scopes consume the same verified proxy.")
        }
        CustomLabel {
            objectName: "offlineEditProxyListStatus"
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: {
                const rows = presenter && presenter.offlineEditProxyList ? presenter.offlineEditProxyList : [];
                return qsTr("%1 prox%2 listed").arg(rows.length).arg(rows.length === 1 ? qsTr("y") : qsTr("ies"));
            }
        }
    }

    footerItem: RowLayout {
        spacing: Fonts.size8

        Item {
            Layout.fillWidth: true
        }
        CustomButton {
            objectName: "offlineEditCancel"
            text: qsTr("Close")
            onClicked: root.cancelDialog()
        }
        CustomButton {
            objectName: "offlineEditRefreshStatus"
            text: qsTr("Refresh")
            enabled: root.hasSelection
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditRefreshStatus)
        }
        CustomButton {
            objectName: "offlineEditCreate"
            text: qsTr("Create")
            enabled: root.hasSelection
            buttonColor: Theme.highlightColor
            buttonTextColor: Theme.highlightedTextColor
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditCreate, {
                "maxEdge": 2048
            })
        }
        CustomButton {
            objectName: "offlineEditPin"
            text: root.pinned ? qsTr("Unpin") : qsTr("Pin")
            enabled: root.hasSelection && root.proxyPresent
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditPin, {
                "pinned": !root.pinned
            })
        }
        CustomButton {
            objectName: "offlineEditDelete"
            text: qsTr("Delete")
            enabled: root.hasSelection && root.proxyPresent
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditDelete, {
                "force": false
            })
        }
        CustomButton {
            objectName: "offlineEditReconnect"
            text: qsTr("Reconnect")
            enabled: root.hasSelection
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditReconnect, {
                "clearProxy": false
            })
        }
        CustomButton {
            objectName: "offlineEditReconnectClear"
            text: qsTr("Reconnect + clear")
            enabled: root.hasSelection
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditReconnect, {
                "clearProxy": true
            })
        }
        CustomButton {
            objectName: "offlineEditEvict"
            text: qsTr("Evict unpinned")
            onClicked: root.commands.run(root.commands.ids.photoOfflineEditEvict, {
                "maxTotalBytes": 1
            })
        }
        Item {
            Layout.fillWidth: true
        }
    }
}
