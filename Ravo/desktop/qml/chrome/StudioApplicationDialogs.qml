import QtQuick
import GeoControls 1.0

Item {
    id: root
    required property var updates
    required property var windowHost
    property bool otherModalOpen: false
    property bool windowReady: true
    readonly property bool modalOpen: aboutDialog.visible || updateDialog.visible
    property bool updatePending: false

    function openAbout() {
        aboutDialog.openWithButtons();
    }

    function showPendingUpdate() {
        if (updatePending && windowReady && !otherModalOpen && !aboutDialog.visible) {
            updatePending = false;
            if (!updateDialog.visible)
                updateDialog.openWithButtons();
        }
    }
    onOtherModalOpenChanged: Qt.callLater(showPendingUpdate)
    onWindowReadyChanged: Qt.callLater(showPendingUpdate)

    Connections {
        target: root.updates
        function onPresentationRequested() {
            if (!updateDialog.visible)
                root.updatePending = true;
            Qt.callLater(root.showPendingUpdate);
        }
    }

    MessageDialog {
        id: aboutDialog
        objectName: "aboutDialog"
        parentItem: root.windowHost.contentItem
        titleText: qsTranslate("Main", "About Ravo Studio")
        messageText: qsTranslate("Main", "Ravo Studio is a local photo library and editor.") + "\n" + qsTr("Version %1").arg(root.updates.currentVersion)
        buttons: [qsTr("OK")]
        defaultButtonText: qsTr("OK")
        onVisibleChanged: Qt.callLater(root.showPendingUpdate)
    }

    MessageDialog {
        id: updateDialog
        objectName: "updateDialog"
        parentItem: root.windowHost.contentItem
        titleText: root.updates.updateAvailable ? qsTr("Update Available") : qsTr("Check for Updates")
        messageText: root.updates.busy ? qsTr("Checking for updates…") : root.updates.lastError.length > 0 ? root.updates.lastError : root.updates.updateAvailable ? qsTr("Ravo Studio %1 is available. You are using %2.").arg(root.updates.latestVersion).arg(root.updates.currentVersion) : qsTr("You are using the latest version of Ravo Studio (%1).").arg(root.updates.currentVersion)
        buttons: root.updates.updateAvailable ? [qsTr("Later"), qsTr("Open GitHub Release")] : !root.updates.busy && root.updates.lastError.length > 0 ? [qsTr("Close"), qsTr("Retry")] : [qsTr("Close")]
        defaultButtonText: root.updates.updateAvailable ? qsTr("Later") : qsTr("Close")
        onFinished: function (buttonText) {
            // MessageDialog closes after finished(). Defer a retry/open so its
            // result (including a browser error) cannot be closed by that tail.
            if (buttonText === qsTr("Open GitHub Release"))
                Qt.callLater(root.updates.openReleasePage);
            else if (buttonText === qsTr("Retry"))
                Qt.callLater(root.updates.checkManually);
        }
        // The shared dialog's global Return fallback has no button identity.
        // It dismisses safely; only the explicit download button opens a URL.
    }
}
