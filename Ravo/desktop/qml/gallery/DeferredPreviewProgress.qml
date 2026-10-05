import QtQuick

// Presentation only: transient thumbnail demand never changes rail geometry.
Item {
    id: root
    property bool workActive: false
    property int completed: 0
    property int total: 0
    property color trackColor: "transparent"
    property color progressColor: "transparent"
    readonly property int minimumPending: 8
    readonly property int revealDelay: 500
    readonly property int remaining: Math.max(0, total - completed)
    readonly property bool pending: workActive || remaining > 0
    property bool revealed: false
    visible: revealed && pending

    onPendingChanged: {
        if (!pending)
            revealed = false;
    }

    Timer {
        interval: root.revealDelay
        running: root.pending && root.remaining >= root.minimumPending && !root.revealed
        onTriggered: {
            if (root.pending && root.remaining >= root.minimumPending)
                root.revealed = true;
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: root.trackColor
        Rectangle {
            width: parent.width * (root.total > 0 ? Math.min(1, root.completed / root.total) : 0)
            height: parent.height
            radius: parent.radius
            color: root.progressColor
        }
    }
}
