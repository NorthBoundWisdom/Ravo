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
    property color curveColor: Theme.textColor
    property string channelLabel: "RGB"
    property var regionSplits: [0.25, 0.5, 0.75]
    property bool showRegionSplits: false
    property int selectedIndex: 0
    property int maxPoints: 20
    property var pendingPoints: []
    property bool pointerInside: false
    property int hoverIndex: -1
    property var hoverPoint: ({
            "x": 0,
            "y": 0
        })
    signal curveEdited(var points)
    signal curveCommitted(var points)

    implicitHeight: Math.max(Fonts.size200, Math.min(Fonts.size300, width * 0.72))
    clip: true
    focus: true
    activeFocusOnTab: true

    readonly property int padding: Fonts.size14
    readonly property real plotWidth: Math.max(1, width - padding * 2)
    readonly property real plotHeight: Math.max(1, height - padding * 2)
    readonly property var displayPoints: pendingPoints.length ? pendingPoints : points
    readonly property var readoutPoint: {
        if (pointerInside)
            return hoverPoint;
        if (!displayPoints || displayPoints.length === 0)
            return {
                "x": 0,
                "y": 0
            };
        return displayPoints[Math.min(displayPoints.length - 1, Math.max(0, selectedIndex))];
    }

    function percent(value) {
        return Math.round(Math.min(1, Math.max(0, Number(value))) * 100) + "%";
    }

    function clonePoints() {
        const copied = [];
        for (let index = 0; index < root.points.length; ++index) {
            copied.push({
                "x": Number(root.points[index].x),
                "y": Number(root.points[index].y)
            });
        }
        if (copied.length < 2)
            return [
                {
                    "x": 0,
                    "y": 0
                },
                {
                    "x": 1,
                    "y": 1
                }
            ];
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

    function drawRegionGuides(ctx) {
        if (!root.showRegionSplits || !root.regionSplits || root.regionSplits.length !== 3)
            return;
        const bounds = [0, Math.min(1, Math.max(0, Number(root.regionSplits[0]))), Math.min(1, Math.max(0, Number(root.regionSplits[1]))), Math.min(1, Math.max(0, Number(root.regionSplits[2]))), 1];
        for (let region = 0; region < 4; ++region) {
            if (region % 2 === 0) {
                ctx.fillStyle = Qt.rgba(1, 1, 1, 0.025);
                ctx.fillRect(root.toCanvasX(bounds[region]), root.padding, root.plotWidth * (bounds[region + 1] - bounds[region]), root.plotHeight);
            }
        }
        for (let index = 1; index < 4; ++index) {
            const x = root.toCanvasX(bounds[index]);
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.22);
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(x, root.padding);
            ctx.lineTo(x, root.padding + root.plotHeight);
            ctx.stroke();
            ctx.fillStyle = Qt.rgba(1, 1, 1, 0.72);
            ctx.beginPath();
            ctx.moveTo(x - 4, root.padding + root.plotHeight);
            ctx.lineTo(x + 4, root.padding + root.plotHeight);
            ctx.lineTo(x, root.padding + root.plotHeight - 6);
            ctx.closePath();
            ctx.fill();
        }
    }

    function drawAxisRamps(ctx) {
        const horizontal = ctx.createLinearGradient(root.padding, 0, root.padding + root.plotWidth, 0);
        horizontal.addColorStop(0, "#111111");
        horizontal.addColorStop(1, "#eeeeee");
        ctx.fillStyle = horizontal;
        ctx.fillRect(root.padding, root.padding + root.plotHeight - 3, root.plotWidth, 3);

        const vertical = ctx.createLinearGradient(0, root.padding + root.plotHeight, 0, root.padding);
        vertical.addColorStop(0, "#111111");
        vertical.addColorStop(1, "#eeeeee");
        ctx.fillStyle = vertical;
        ctx.fillRect(root.padding, root.padding, 3, root.plotHeight);
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.baseColor
        border.color: root.activeFocus ? root.curveColor : Theme.midColor
        border.width: root.activeFocus ? Fonts.size2 : Fonts.size1
        radius: Fonts.size2
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
            root.drawRegionGuides(ctx);
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.1);
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
            root.drawAxisRamps(ctx);
            ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.28);
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(root.toCanvasX(0), root.toCanvasY(0));
            ctx.lineTo(root.toCanvasX(1), root.toCanvasY(1));
            ctx.stroke();
            if (root.samples && root.samples.length > 1) {
                ctx.strokeStyle = root.curveColor;
                ctx.lineWidth = 2.25;
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
            if (root.pointerInside) {
                const hoverX = root.toCanvasX(Number(root.hoverPoint.x));
                const hoverY = root.toCanvasY(Number(root.hoverPoint.y));
                ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.16);
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.moveTo(hoverX, root.padding);
                ctx.lineTo(hoverX, root.padding + root.plotHeight);
                ctx.moveTo(root.padding, hoverY);
                ctx.lineTo(root.padding + root.plotWidth, hoverY);
                ctx.stroke();
            }
            const list = root.displayPoints;
            for (let index = 0; index < list.length; ++index) {
                const px = root.toCanvasX(Number(list[index].x));
                const py = root.toCanvasY(Number(list[index].y));
                const selected = index === root.selectedIndex;
                const hovered = index === root.hoverIndex;
                ctx.beginPath();
                ctx.arc(px, py, selected ? 5.5 : hovered ? 4.5 : 3.25, 0, Math.PI * 2);
                ctx.fillStyle = selected ? Theme.baseColor : root.curveColor;
                ctx.fill();
                ctx.strokeStyle = root.curveColor;
                ctx.lineWidth = selected ? 2 : 1.25;
                ctx.stroke();
            }
        }
    }

    Rectangle {
        z: 2
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: root.padding + Fonts.size6
        anchors.topMargin: root.padding + Fonts.size6
        implicitWidth: readoutRow.implicitWidth + Fonts.size12
        implicitHeight: readoutRow.implicitHeight + Fonts.size8
        color: Qt.alpha(Theme.windowColor, 0.82)
        radius: Fonts.size2

        Row {
            id: readoutRow
            anchors.centerIn: parent
            spacing: Fonts.size10

            Text {
                text: qsTr("Input") + "  " + root.percent(root.readoutPoint.x)
                color: Theme.textColor
                font: Fonts.annotationFont
            }
            Text {
                text: qsTr("Output") + "  " + root.percent(root.readoutPoint.y)
                color: Theme.textColor
                font: Fonts.annotationFont
            }
        }
    }

    Rectangle {
        z: 2
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: root.padding + Fonts.size6
        anchors.topMargin: root.padding + Fonts.size6
        implicitWidth: channelRow.implicitWidth + Fonts.size12
        implicitHeight: channelRow.implicitHeight + Fonts.size8
        color: Qt.alpha(Theme.windowColor, 0.82)
        radius: Fonts.size2

        Row {
            id: channelRow
            anchors.centerIn: parent
            spacing: Fonts.size6

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: Fonts.size6
                height: width
                radius: width / 2
                color: root.curveColor
            }
            Text {
                text: root.channelLabel
                color: root.curveColor
                font: Fonts.makeBoldFont(Fonts.annotationFont)
            }
        }
    }

    MouseArea {
        z: 3
        anchors.fill: parent
        enabled: root.editorEnabled
        acceptedButtons: Qt.LeftButton
        hoverEnabled: true
        property int activeIndex: -1
        property bool dragging: false
        cursorShape: dragging ? Qt.ClosedHandCursor : root.hoverIndex >= 0 ? Qt.OpenHandCursor : Qt.CrossCursor

        onEntered: {
            root.pointerInside = true;
            canvas.requestPaint();
        }
        onExited: {
            if (!dragging)
                root.pointerInside = false;
            root.hoverIndex = -1;
            canvas.requestPaint();
        }

        onPressed: function (mouse) {
            root.forceActiveFocus();
            root.pointerInside = true;
            root.hoverPoint = root.fromCanvas(mouse.x, mouse.y);
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
            root.hoverIndex = index;
            if (index >= 0) {
                root.selectedIndex = index;
                root.hoverPoint = {
                    "x": Number(next[index].x),
                    "y": Number(next[index].y)
                };
            }
            canvas.requestPaint();
        }
        onPositionChanged: function (mouse) {
            root.pointerInside = containsMouse || dragging;
            root.hoverPoint = root.fromCanvas(mouse.x, mouse.y);
            root.hoverIndex = root.hitIndex(mouse.x, mouse.y);
            if (!dragging || activeIndex < 0) {
                canvas.requestPaint();
                return;
            }
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
            root.hoverIndex = activeIndex;
            root.hoverPoint = {
                "x": Number(next[activeIndex].x),
                "y": Number(next[activeIndex].y)
            };
            canvas.requestPaint();
        }
        onReleased: function () {
            if (dragging)
                root.emitCommitted();
            dragging = false;
            activeIndex = -1;
            root.pointerInside = containsMouse;
            root.hoverIndex = containsMouse ? root.hitIndex(mouseX, mouseY) : -1;
            canvas.requestPaint();
        }
        onCanceled: {
            dragging = false;
            activeIndex = -1;
            root.pointerInside = false;
            root.hoverIndex = -1;
            root.pendingPoints = [];
            canvas.requestPaint();
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
            root.selectedIndex = Math.min(Math.max(0, root.selectedIndex), Math.max(0, root.points.length - 1));
            canvas.requestPaint();
        }
        function onPendingPointsChanged() {
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
        function onCurveColorChanged() {
            canvas.requestPaint();
        }
        function onRegionSplitsChanged() {
            canvas.requestPaint();
        }
        function onShowRegionSplitsChanged() {
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
