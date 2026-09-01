import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root

    property string title: ""
    property string hueField
    property string chromaField
    property string luminanceField
    property double hue: 0
    property double chroma: 0
    property double luminance: 0
    property double maxChroma: 0.5
    property double luminanceFrom: -1
    property double luminanceTo: 1
    property double luminanceStep: 0.01
    property int luminanceDecimals: 0
    property double luminanceReset: 0
    property double luminanceDisplayScale: 100
    property double wheelDiameter: Fonts.scaledUiSize(112)
    property bool editorEnabled: true
    property var commands
    property bool liveReady: false

    spacing: Fonts.size4
    Layout.fillWidth: true
    Layout.preferredWidth: wheelDiameter

    function polarFromPoint(px, py) {
        const size = wheel.width;
        const cx = size / 2;
        const cy = size / 2;
        const radius = Math.max(1, size / 2 - 2);
        const dx = px - cx;
        const dy = cy - py;
        const distance = Math.sqrt(dx * dx + dy * dy);
        let nextHue = Math.atan2(dy, dx) * 180 / Math.PI;
        if (nextHue < 0)
            nextHue += 360;
        return {
            "hue": nextHue,
            "chroma": Math.min(1, distance / radius) * root.maxChroma
        };
    }

    function applyWheel(nextHue, nextChroma, live) {
        if (!root.commands || root.hueField.length === 0 || root.chromaField.length === 0)
            return;
        const fields = {};
        fields[root.hueField] = nextHue;
        fields[root.chromaField] = nextChroma;
        if (live)
            root.commands.previewDevelopNumbers(fields);
        else
            root.commands.setDevelopNumbers(fields);
    }

    function resetWheel() {
        if (!root.commands)
            return;
        const fields = {};
        fields[root.hueField] = 0;
        fields[root.chromaField] = 0;
        fields[root.luminanceField] = root.luminanceReset;
        root.commands.setDevelopNumbers(fields);
    }

    CustomLabel {
        Layout.fillWidth: true
        text: root.title
        horizontalAlignment: Text.AlignHCenter
        font.bold: true

        MouseArea {
            anchors.fill: parent
            enabled: root.editorEnabled
            onDoubleClicked: root.resetWheel()
        }
    }

    Canvas {
        id: wheel
        objectName: root.objectName.length ? root.objectName + "Disk" : ""
        Layout.alignment: Qt.AlignHCenter
        Layout.preferredWidth: Math.min(root.wheelDiameter, root.width)
        Layout.preferredHeight: Layout.preferredWidth
        width: Layout.preferredWidth
        height: Layout.preferredHeight
        antialiasing: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Component.onCompleted: requestPaint()

        onPaint: {
            const ctx = getContext("2d");
            const size = width;
            const cx = size / 2;
            const cy = size / 2;
            const radius = Math.max(1, size / 2 - 2);
            ctx.clearRect(0, 0, size, size);
            for (let angle = 0; angle < 360; ++angle) {
                ctx.beginPath();
                ctx.moveTo(cx, cy);
                ctx.arc(cx, cy, radius, (angle - 0.7) * Math.PI / 180, (angle + 1.7) * Math.PI / 180);
                ctx.closePath();
                const hue = (360 - angle) % 360;
                ctx.fillStyle = "hsl(" + hue + ", 100%, 50%)";
                ctx.fill();
            }
            const fade = ctx.createRadialGradient(cx, cy, 0, cx, cy, radius);
            fade.addColorStop(0, "rgba(255, 255, 255, 1)");
            fade.addColorStop(1, "rgba(255, 255, 255, 0)");
            ctx.beginPath();
            ctx.arc(cx, cy, radius, 0, Math.PI * 2);
            ctx.closePath();
            ctx.fillStyle = fade;
            ctx.fill();
            ctx.beginPath();
            ctx.arc(cx, cy, radius, 0, Math.PI * 2);
            ctx.strokeStyle = "rgba(255, 255, 255, 0.14)";
            ctx.lineWidth = 1;
            ctx.stroke();

            const chromaRatio = root.maxChroma <= 0 ? 0 : Math.min(1, Math.max(0, root.chroma / root.maxChroma));
            const markerRadius = chromaRatio * radius;
            const markerAngle = root.hue * Math.PI / 180;
            const mx = cx + Math.cos(markerAngle) * markerRadius;
            const my = cy - Math.sin(markerAngle) * markerRadius;
            ctx.beginPath();
            ctx.arc(mx, my, Math.max(4, Fonts.size6), 0, Math.PI * 2);
            ctx.fillStyle = chromaRatio > 0.04 ? "hsl(" + root.hue + ", 100%, 50%)" : "#d8d8d8";
            ctx.fill();
            ctx.strokeStyle = "#ffffff";
            ctx.lineWidth = 1.5;
            ctx.stroke();
        }

        MouseArea {
            anchors.fill: parent
            enabled: root.editorEnabled
            preventStealing: true

            function emitAt(px, py, live) {
                const next = root.polarFromPoint(px, py);
                if (root.liveReady)
                    root.applyWheel(next.hue, next.chroma, live);
            }

            onPressed: function (event) {
                emitAt(event.x, event.y, true);
            }
            onPositionChanged: function (event) {
                if (pressed)
                    emitAt(event.x, event.y, true);
            }
            onReleased: function (event) {
                emitAt(event.x, event.y, false);
            }
            onDoubleClicked: root.resetWheel()
        }
    }

    DevelopColorSlider {
        Layout.fillWidth: true
        title: "☀"
        from: root.luminanceFrom
        to: root.luminanceTo
        stepSize: root.luminanceStep
        displayDecimals: root.luminanceDecimals
        displayScale: root.luminanceDisplayScale
        resetValue: root.luminanceReset
        enabled: root.editorEnabled
        value: root.luminance
        trackGradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0
                color: "#050505"
            }
            GradientStop {
                position: 0.52
                color: "#6f6f6f"
            }
            GradientStop {
                position: 1
                color: "#f4f4f4"
            }
        }
        onValueEdited: function (value) {
            if (root.liveReady && root.commands)
                root.commands.previewDevelopNumber(root.luminanceField, value);
        }
        onValueCommitted: function (value) {
            if (root.commands)
                root.commands.setDevelopNumber(root.luminanceField, value);
        }
        onResetRequested: if (root.commands)
            root.commands.resetControl(root.luminanceField)
    }

    Connections {
        target: root
        function onHueChanged() {
            wheel.requestPaint();
        }
        function onChromaChanged() {
            wheel.requestPaint();
        }
        function onMaxChromaChanged() {
            wheel.requestPaint();
        }
    }
}
