import QtQuick
import GeoControls 1.0

Item {
    id: root
    property var points: []
    property var samples: []
    property var histogramRed: []
    property var histogramGreen: []
    property var histogramBlue: []
    property var histogramLuma: []
    property double histogramMax: 0
    property string histogramMode: "rgb"
    property bool editorEnabled: true
    property int selectedIndex: 0
    property int maxPoints: 20
    property var pendingPoints: []
    signal curveEdited(var points)
    signal curveCommitted(var points)

    implicitHeight: Math.max(220, width)
    clip: true
    focus: true

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
        const list = root.pendingPoints.length ? root.pendingPoints : root.points;
        for (let index = 0; index < list.length; ++index) {
            const dx = toCanvasX(Number(list[index].x)) - px;
            const dy = toCanvasY(Number(list[index].y)) - py;
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
        const minX = Number(list[index - 1].x) + 0.0025;
        const maxX = Number(list[index + 1].x) - 0.0025;
        list[index].x = Math.min(maxX, Math.max(minX, list[index].x));
        list[index].y = Math.min(1, Math.max(0, list[index].y));
    }

    function nudgeSelected(dx, dy) {
        if (!root.editorEnabled)
            return;
        let next = root.clonePoints();
        const index = Math.min(next.length - 1, Math.max(0, root.selectedIndex));
        next[index].x = Number(next[index].x) + dx;
        next[index].y = Number(next[index].y) + dy;
        root.constrainPoint(next, index);
        root.emitEdited(next);
        root.emitCommitted();
    }

    function drawHistogram(ctx, values, color) {
        if (!values || values.length < 2 || root.histogramMax <= 0)
            return;
        ctx.beginPath();
        ctx.moveTo(root.toCanvasX(0), root.toCanvasY(0));
        const last = values.length - 1;
        for (let k = 0; k < values.length; ++k) {
            const x = k / last;
            const y = Math.min(1, Number(values[k] || 0) / root.histogramMax);
            ctx.lineTo(root.toCanvasX(x), root.toCanvasY(y));
        }
        ctx.lineTo(root.toCanvasX(1), root.toCanvasY(0));
        ctx.closePath();
        ctx.fillStyle = color;
        ctx.fill();
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.baseColor
        border.color: Theme.dividerColor
        radius: 2
    }

    Canvas {
        id: canvas
        objectName: "toneCurveCanvas"
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
            ctx.globalCompositeOperation = "lighter";
            if (root.histogramMode === "red")
                root.drawHistogram(ctx, root.histogramRed, Qt.rgba(1, 0.2, 0.15, 0.45));
            else if (root.histogramMode === "green")
                root.drawHistogram(ctx, root.histogramGreen, Qt.rgba(0.15, 1, 0.2, 0.45));
            else if (root.histogramMode === "blue")
                root.drawHistogram(ctx, root.histogramBlue, Qt.rgba(0.2, 0.4, 1, 0.45));
            else if (root.histogramMode === "luma")
                root.drawHistogram(ctx, root.histogramLuma, Qt.rgba(0.85, 0.85, 0.85, 0.4));
            else {
                root.drawHistogram(ctx, root.histogramRed, Qt.rgba(1, 0.2, 0.15, 0.28));
                root.drawHistogram(ctx, root.histogramGreen, Qt.rgba(0.15, 1, 0.2, 0.28));
                root.drawHistogram(ctx, root.histogramBlue, Qt.rgba(0.2, 0.4, 1, 0.28));
            }
            ctx.globalCompositeOperation = "source-over";
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
            const list = root.points;
            for (let index = 0; index < list.length; ++index) {
                const px = root.toCanvasX(Number(list[index].x));
                const py = root.toCanvasY(Number(list[index].y));
                ctx.beginPath();
                ctx.arc(px, py, index === root.selectedIndex ? 6 : 5, 0, Math.PI * 2);
                ctx.fillStyle = index === root.selectedIndex ? Theme.accentColor : Theme.textColor;
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
            root.forceActiveFocus();
            let next = root.clonePoints();
            let index = root.hitIndex(mouse.x, mouse.y);
            if (index < 0 && next.length < root.maxPoints) {
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
            if (index >= 0)
                root.selectedIndex = index;
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
            root.selectedIndex = Math.min(index, next.length - 1);
            root.emitEdited(next);
            root.emitCommitted();
        }
    }

    Keys.onPressed: function (event) {
        if (!root.editorEnabled)
            return;
        if (event.key === Qt.Key_Left) {
            root.nudgeSelected(-0.01, 0);
            event.accepted = true;
        } else if (event.key === Qt.Key_Right) {
            root.nudgeSelected(0.01, 0);
            event.accepted = true;
        } else if (event.key === Qt.Key_Up) {
            root.nudgeSelected(0, 0.01);
            event.accepted = true;
        } else if (event.key === Qt.Key_Down) {
            root.nudgeSelected(0, -0.01);
            event.accepted = true;
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            if (root.selectedIndex > 0 && root.selectedIndex < root.points.length - 1) {
                const next = root.clonePoints();
                next.splice(root.selectedIndex, 1);
                root.selectedIndex = Math.min(root.selectedIndex, next.length - 1);
                root.emitEdited(next);
                root.emitCommitted();
                event.accepted = true;
            }
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
        function onHistogramRedChanged() {
            canvas.requestPaint();
        }
        function onHistogramGreenChanged() {
            canvas.requestPaint();
        }
        function onHistogramBlueChanged() {
            canvas.requestPaint();
        }
        function onHistogramLumaChanged() {
            canvas.requestPaint();
        }
        function onHistogramMaxChanged() {
            canvas.requestPaint();
        }
        function onHistogramModeChanged() {
            canvas.requestPaint();
        }
        function onSelectedIndexChanged() {
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
