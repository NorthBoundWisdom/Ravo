import QtQuick
import GeoControls 1.0

Item {
    id: root
    property var points: []
    property var samples: []
    property bool editorEnabled: true
    property var pendingPoints: []
    signal curveEdited(var points)
    signal curveCommitted(var points)

    implicitHeight: Math.max(180, width)
    clip: true

    readonly property int padding: 10
    readonly property real plotWidth: Math.max(1, width - padding * 2)
    readonly property real plotHeight: Math.max(1, height - padding * 2)

    function clonePoints() {
        const copied = [];
        for (let index = 0; index < root.points.length; ++index) {
            copied.push({
                "x": Number(root.points[index].x),
                "y": Number(root.points[index].y)
            });
        }
        if (copied.length < 2)
            return [{
                    "x": 0,
                    "y": 0
                }, {
                    "x": 1,
                    "y": 1
                }];
        return copied;
    }

    function emitEdited(next) {
        root.pendingPoints = next;
        root.curveEdited(next);
    }

    function emitCommitted() {
        const next = root.pendingPoints.length ? root.pendingPoints : root.clonePoints();
        root.curveCommitted(next);
        root.pendingPoints = [];
    }

    function toCanvasX(x) {
        return root.padding + x * root.plotWidth;
    }

    function toCanvasY(y) {
        return root.padding + (1 - y) * root.plotHeight;
    }

    function fromCanvas(px, py) {
        return {
            "x": Math.min(1, Math.max(0, (px - root.padding) / root.plotWidth)),
            "y": Math.min(1, Math.max(0, 1 - (py - root.padding) / root.plotHeight))
        };
    }

    function hitIndex(px, py) {
        let best = -1;
        let bestDistance = 14;
        for (let index = 0; index < root.points.length; ++index) {
            const dx = toCanvasX(Number(root.points[index].x)) - px;
            const dy = toCanvasY(Number(root.points[index].y)) - py;
            const distance = Math.hypot(dx, dy);
            if (distance <= bestDistance) {
                bestDistance = distance;
                best = index;
            }
        }
        return best;
    }

    function constrainPoint(list, index) {
        const last = list.length - 1;
        if (index <= 0) {
            list[0].x = 0;
            list[0].y = Math.min(1, Math.max(0, list[0].y));
            return;
        }
        if (index >= last) {
            list[last].x = 1;
            list[last].y = Math.min(1, Math.max(0, list[last].y));
            return;
        }
        const minX = Number(list[index - 1].x) + 0.01;
        const maxX = Number(list[index + 1].x) - 0.01;
        list[index].x = Math.min(maxX, Math.max(minX, list[index].x));
        list[index].y = Math.min(1, Math.max(0, list[index].y));
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.baseColor
        border.color: Theme.dividerColor
        radius: 2
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d");
            ctx.reset();
            ctx.fillStyle = Theme.baseColor;
            ctx.fillRect(0, 0, width, height);
            ctx.strokeStyle = Theme.dividerColor;
            ctx.lineWidth = 1;
            for (let step = 0; step <= 4; ++step) {
                const t = step / 4;
                ctx.beginPath();
                ctx.moveTo(root.toCanvasX(t), root.padding);
                ctx.lineTo(root.toCanvasX(t), root.padding + root.plotHeight);
                ctx.stroke();
                ctx.beginPath();
                ctx.moveTo(root.padding, root.toCanvasY(t));
                ctx.lineTo(root.padding + root.plotWidth, root.toCanvasY(t));
                ctx.stroke();
            }
            ctx.strokeStyle = Theme.midColor;
            ctx.beginPath();
            ctx.moveTo(root.toCanvasX(0), root.toCanvasY(0));
            ctx.lineTo(root.toCanvasX(1), root.toCanvasY(1));
            ctx.stroke();
            if (root.samples && root.samples.length > 1) {
                ctx.strokeStyle = Theme.accentColor;
                ctx.lineWidth = 2;
                ctx.beginPath();
                for (let sample = 0; sample < root.samples.length; ++sample) {
                    const x = sample / (root.samples.length - 1);
                    const y = Number(root.samples[sample]);
                    const px = root.toCanvasX(x);
                    const py = root.toCanvasY(y);
                    if (sample === 0)
                        ctx.moveTo(px, py);
                    else
                        ctx.lineTo(px, py);
                }
                ctx.stroke();
            }
            ctx.fillStyle = Theme.textColor;
            for (let index = 0; index < root.points.length; ++index) {
                const px = root.toCanvasX(Number(root.points[index].x));
                const py = root.toCanvasY(Number(root.points[index].y));
                ctx.beginPath();
                ctx.arc(px, py, 5, 0, Math.PI * 2);
                ctx.fill();
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.editorEnabled
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        property int activeIndex: -1
        property bool dragging: false

        onPressed: function (mouse) {
            let next = root.clonePoints();
            let index = root.hitIndex(mouse.x, mouse.y);
            if (index < 0 && next.length < 16) {
                const added = root.fromCanvas(mouse.x, mouse.y);
                let insertAt = next.length - 1;
                for (let cursor = 1; cursor < next.length; ++cursor) {
                    if (added.x < Number(next[cursor].x)) {
                        insertAt = cursor;
                        break;
                    }
                }
                next.splice(insertAt, 0, added);
                root.constrainPoint(next, insertAt);
                index = insertAt;
                root.emitEdited(next);
            }
            activeIndex = index;
            dragging = index >= 0;
        }
        onPositionChanged: function (mouse) {
            if (!dragging || activeIndex < 0)
                return;
            const next = (root.pendingPoints.length ? root.pendingPoints : root.clonePoints()).map(function (point) {
                return {
                    "x": point.x,
                    "y": point.y
                };
            });
            if (activeIndex >= next.length)
                return;
            const mapped = root.fromCanvas(mouse.x, mouse.y);
            next[activeIndex].x = mapped.x;
            next[activeIndex].y = mapped.y;
            root.constrainPoint(next, activeIndex);
            root.emitEdited(next);
        }
        onReleased: function () {
            if (dragging)
                root.emitCommitted();
            dragging = false;
            activeIndex = -1;
        }
        onDoubleClicked: function (mouse) {
            const index = root.hitIndex(mouse.x, mouse.y);
            if (index <= 0 || index >= root.points.length - 1)
                return;
            const next = root.clonePoints();
            next.splice(index, 1);
            root.emitEdited(next);
            root.emitCommitted();
        }
    }

    Connections {
        target: root
        function onPointsChanged() {
            canvas.requestPaint();
        }
        function onSamplesChanged() {
            canvas.requestPaint();
        }
        function onWidthChanged() {
            canvas.requestPaint();
        }
        function onHeightChanged() {
            canvas.requestPaint();
        }
    }
}
