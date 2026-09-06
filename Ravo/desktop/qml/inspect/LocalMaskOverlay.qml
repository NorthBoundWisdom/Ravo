import QtQuick

Item {
    id: root
    required property var presenter
    required property var commands
    property string gestureToken: ""
    property string gestureScope: ""
    property string gestureAsset: ""
    clip: true

    function sendGesture(action, x, y, handle) {
        let argument = {
            id: gestureScope,
            asset: gestureAsset
        };
        if (action === "gesture_begin")
            argument.handle = handle;
        else
            argument.token = gestureToken;
        if (action !== "gesture_cancel") {
            argument.x = x / root.width;
            argument.y = y / root.height;
        }
        return root.commands.localAdjustment(action, argument);
    }
    function cancelGesture() {
        if (gestureToken.length)
            sendGesture("gesture_cancel", 0, 0, "");
        gestureToken = "";
    }

    Keys.onEscapePressed: function (event) {
        if (gestureToken.length)
            cancelGesture();
        else
            root.commands.localAdjustment("done", {});
        event.accepted = true;
    }
    Connections {
        target: root.presenter
        function onEditingScopeChanged() {
            root.gestureToken = "";
        }
    }

    Repeater {
        model: root.presenter.localMaskHandles
        delegate: Rectangle {
            required property var modelData
            width: modelData.id === "rotation" ? 10 : 13
            height: width
            radius: width / 2
            x: modelData.x * root.width - width / 2
            y: modelData.y * root.height - height / 2
            color: "#f5f5f5"
            border.width: 2
            border.color: "#202020"
        }
    }
    MouseArea {
        objectName: "localMaskDrawingSurface"
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        preventStealing: true
        cursorShape: Qt.CrossCursor
        onPressed: function (mouse) {
            let handle = "draw";
            let nearest = 18 * 18;
            const handles = root.presenter.localMaskHandles;
            for (let i = 0; i < handles.length; ++i) {
                const dx = mouse.x - handles[i].x * root.width;
                const dy = mouse.y - handles[i].y * root.height;
                const distance = dx * dx + dy * dy;
                if (distance < nearest) {
                    handle = handles[i].id;
                    nearest = distance;
                }
            }
            if (handle === "draw" && !root.presenter.maskDrawingActive) {
                mouse.accepted = false;
                return;
            }
            root.gestureScope = root.presenter.activeLocalId;
            root.gestureAsset = root.presenter.selectedAssetId;
            const result = root.sendGesture("gesture_begin", mouse.x, mouse.y, handle);
            if (result && result.ok) {
                root.gestureToken = result.token;
                root.forceActiveFocus();
            } else {
                mouse.accepted = false;
            }
        }
        onPositionChanged: function (mouse) {
            if (pressed && root.gestureToken.length)
                root.sendGesture("gesture_update", mouse.x, mouse.y, "");
        }
        onReleased: function (mouse) {
            if (!root.gestureToken.length)
                return;
            const result = root.sendGesture("gesture_end", mouse.x, mouse.y, "");
            if (!result || !result.ok)
                root.cancelGesture();
            root.gestureToken = "";
        }
        onCanceled: root.cancelGesture()
    }
}
