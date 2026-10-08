import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0
import "../chrome" as Chrome

Rectangle {
    id: root
    property var presenter
    property var commands

    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool histogramMode: hasPresenter && presenter.inspect.scopeMode === "histogram"
    readonly property bool waveformMode: hasPresenter && presenter.inspect.scopeMode === "waveform"
    readonly property bool paradeMode: !hasPresenter || presenter.inspect.scopeMode === "parade"
    readonly property bool vectorscopeMode: hasPresenter && presenter.inspect.scopeMode === "vectorscope"
    readonly property bool splitMode: hasPresenter && presenter.inspect.scopeMode === "split"

    color: Theme.imageSurroundColor
    implicitHeight: Fonts.scaledUiSize(120)

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.dividerColor
        z: 2
    }

    Item {
        id: plot
        anchors.fill: parent
        anchors.leftMargin: Fonts.size8
        anchors.rightMargin: Fonts.size8
        anchors.topMargin: Fonts.size4
        anchors.bottomMargin: Fonts.size4

        Canvas {
            id: histogramCanvas
            anchors.fill: parent
            visible: root.histogramMode
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const w = width;
                const h = height;
                ctx.fillStyle = "#1a1a1a";
                ctx.fillRect(0, 0, w, h);
                ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.18);
                ctx.lineWidth = 1;
                for (let g = 1; g < 4; ++g) {
                    const x = w * g / 4;
                    const y = h * g / 4;
                    ctx.beginPath();
                    ctx.moveTo(x, 0);
                    ctx.lineTo(x, h);
                    ctx.moveTo(0, y);
                    ctx.lineTo(w, y);
                    ctx.stroke();
                }
                if (!root.hasPresenter || root.presenter.inspect.scopeHistogramMax <= 0)
                    return;
                const maxv = root.presenter.inspect.scopeHistogramMax;
                function drawChannel(values, color) {
                    ctx.beginPath();
                    ctx.moveTo(0, h);
                    for (let k = 0; k < 256; ++k) {
                        const count = values[k] || 0;
                        const y = h - (maxv > 0 ? count / maxv : 0) * (h - 2);
                        ctx.lineTo(k / 255 * w, y);
                    }
                    ctx.lineTo(w, h);
                    ctx.closePath();
                    ctx.fillStyle = color;
                    ctx.fill();
                }
                ctx.globalCompositeOperation = "lighter";
                drawChannel(root.presenter.inspect.scopeHistogramRed, Qt.rgba(1, 0.15, 0.12, 0.55));
                drawChannel(root.presenter.inspect.scopeHistogramGreen, Qt.rgba(0.15, 1, 0.18, 0.55));
                drawChannel(root.presenter.inspect.scopeHistogramBlue, Qt.rgba(0.2, 0.4, 1, 0.55));
            }
        }

        Image {
            anchors.fill: parent
            visible: !root.histogramMode
            fillMode: Image.Stretch
            asynchronous: false
            cache: false
            source: !root.hasPresenter ? "" : root.waveformMode ? root.presenter.inspect.scopeWaveformUrl : root.paradeMode ? root.presenter.inspect.scopeParadeUrl : root.vectorscopeMode ? root.presenter.inspect.scopeVectorscopeUrl : root.presenter.inspect.scopeSplitUrl
            opacity: 0.95
        }

        Repeater {
            model: 8
            Rectangle {
                required property int index
                visible: root.waveformMode || root.paradeMode
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                y: parent.height * (index + 1) / 9
                color: index === 0 || index === 4 ? Qt.rgba(1, 1, 1, 0.35) : Qt.rgba(1, 1, 1, 0.12)
            }
        }

        Repeater {
            model: 2
            Rectangle {
                required property int index
                visible: root.paradeMode
                width: 1
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                x: parent.width * (index + 1) / 3
                color: Qt.rgba(1, 1, 1, 0.28)
            }
        }

        Canvas {
            anchors.fill: parent
            visible: root.vectorscopeMode
            onPaint: {
                const ctx = getContext("2d");
                ctx.reset();
                const cx = width / 2;
                const cy = height / 2;
                const radius = Math.min(width, height) * 0.46;
                ctx.strokeStyle = Qt.rgba(1, 1, 1, 0.25);
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.arc(cx, cy, radius, 0, 2 * Math.PI);
                ctx.moveTo(cx - radius, cy);
                ctx.lineTo(cx + radius, cy);
                ctx.moveTo(cx, cy - radius);
                ctx.lineTo(cx, cy + radius);
                ctx.stroke();
            }
        }
    }

    component ScopeModeItem: Chrome.StudioContextMenuItem {
        id: item
        required property string modeId
        checkable: true
        checked: root.hasPresenter ? root.presenter.inspect.scopeMode === modeId : modeId === "parade"
        onTriggered: {
            if (root.commands)
                root.commands.run(root.commands.ids.viewSetScopeMode, item.modeId);
        }
    }

    CustomButton {
        id: scopeModeButton
        z: 4
        width: Fonts.scaledUiSize(24)
        height: Fonts.scaledUiSize(24)
        anchors.left: plot.left
        anchors.top: plot.top
        anchors.leftMargin: Fonts.size2
        anchors.topMargin: Fonts.size2
        enabled: root.hasPresenter
        Accessible.name: qsTr("Scope type")
        tooltipText: qsTr("Scope type")
        text: "\u25BC"
        defaultPadding: 0
        onClicked: scopeModeMenu.popup()

        Chrome.StudioContextMenu {
            id: scopeModeMenu
            fitToContent: true
            ScopeModeItem {
                text: qsTr("Histogram")
                modeId: "histogram"
            }
            ScopeModeItem {
                text: qsTr("Waveform")
                modeId: "waveform"
            }
            ScopeModeItem {
                text: qsTr("Parade")
                modeId: "parade"
            }
            ScopeModeItem {
                text: qsTr("Vectorscope")
                modeId: "vectorscope"
            }
            ScopeModeItem {
                text: qsTr("Split")
                modeId: "split"
            }
        }
    }

    Connections {
        target: root.presenter ? root.presenter.inspect : null
        function onScopesChanged() {
            histogramCanvas.requestPaint();
        }
    }

    onWidthChanged: histogramCanvas.requestPaint()
    onHeightChanged: histogramCanvas.requestPaint()
}
