import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Item {
    id: root

    property string title: ""
    property double value: 0
    property double from: -1
    property double to: 1
    property double stepSize: 0.01
    property double resetValue: 0
    property double displayScale: 1
    property int displayDecimals: 0
    property Gradient trackGradient: null
    property bool delayedCommit: true
    property int commitDelay: 30
    readonly property double visualValue: slider.value

    signal valueEdited(double value)
    signal valueCommitted(double value)
    signal resetRequested

    Layout.fillWidth: true
    implicitWidth: Fonts.size180
    implicitHeight: content.implicitHeight

    property bool _syncing: false
    property var _pausedFlickable: null
    property bool _pausedFlickableWasInteractive: false

    function almostEqual(left, right) {
        return Math.abs(left - right) < Math.max(1e-6, Math.abs(root.stepSize) * 0.5);
    }

    function applyIncomingValue() {
        if (root._syncing || slider.pressed)
            return;
        if (!root.almostEqual(slider.value, root.value)) {
            root._syncing = true;
            slider.value = root.value;
            root._syncing = false;
        }
    }

    function findAncestorFlickable(item) {
        let current = item;
        while (current) {
            if (current.flickableDirection !== undefined && current.interactive !== undefined && current.contentY !== undefined)
                return current;
            current = current.parent;
        }
        return null;
    }

    function pauseAncestorFlickable() {
        if (root._pausedFlickable)
            return;
        const flickable = root.findAncestorFlickable(root.parent);
        if (!flickable)
            return;
        root._pausedFlickable = flickable;
        root._pausedFlickableWasInteractive = flickable.interactive;
        flickable.interactive = false;
    }

    function resumeAncestorFlickable() {
        if (!root._pausedFlickable)
            return;
        root._pausedFlickable.interactive = root._pausedFlickableWasInteractive;
        root._pausedFlickable = null;
        root._pausedFlickableWasInteractive = false;
    }

    function requestCommit() {
        if (!root.enabled || root._syncing)
            return;
        if (root.delayedCommit)
            commitTimer.restart();
        else
            root.valueCommitted(slider.value);
    }

    function reset() {
        if (!root.enabled || root.almostEqual(slider.value, root.resetValue))
            return;
        root.resetRequested();
    }

    onValueChanged: applyIncomingValue()
    onFromChanged: applyIncomingValue()
    onToChanged: applyIncomingValue()
    Component.onCompleted: applyIncomingValue()
    Component.onDestruction: resumeAncestorFlickable()

    Timer {
        id: commitTimer
        interval: root.commitDelay
        repeat: false
        onTriggered: root.valueCommitted(slider.value)
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        spacing: Fonts.size4

        RowLayout {
            Layout.fillWidth: true

            CustomLabel {
                Layout.fillWidth: true
                text: root.title
                color: root.enabled ? Theme.textColor : Theme.disabledTextColor
                elide: Text.ElideRight

                MouseArea {
                    anchors.fill: parent
                    enabled: root.enabled
                    onDoubleClicked: root.reset()
                }
            }

            CustomLabel {
                text: Number(root.visualValue * root.displayScale).toFixed(root.displayDecimals)
                color: root.enabled ? Theme.textColor : Theme.disabledTextColor
                horizontalAlignment: Text.AlignRight
                font.bold: true
            }
        }

        Slider {
            id: slider
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.size20
            from: root.from
            to: root.to
            stepSize: root.stepSize
            live: true
            snapMode: Slider.SnapAlways
            enabled: root.enabled
            Accessible.name: root.title

            onMoved: {
                if (pressed)
                    root.valueEdited(value);
                else
                    root.requestCommit();
            }
            onPressedChanged: {
                if (pressed) {
                    root.pauseAncestorFlickable();
                    return;
                }
                root.resumeAncestorFlickable();
                root.requestCommit();
            }

            background: Rectangle {
                x: slider.leftPadding
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: slider.availableWidth
                height: slider.pressed || handleHover.hovered ? Fonts.size6 : Fonts.size4
                radius: height / 2
                color: Theme.buttonPressedColor
                gradient: root.trackGradient
                opacity: root.enabled ? 1 : 0.45

                Rectangle {
                    width: slider.visualPosition * parent.width
                    height: parent.height
                    radius: parent.radius
                    color: Theme.midlightColor
                    visible: root.trackGradient === null
                }

                Behavior on height {
                    NumberAnimation {
                        duration: 90
                    }
                }
            }

            handle: Rectangle {
                x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
                y: slider.topPadding + slider.availableHeight / 2 - height / 2
                width: Fonts.size16
                height: width
                radius: width / 2
                color: root.enabled ? "#f4f4f4" : Theme.disabledTextColor
                border.color: "#33ffffff"
                border.width: Fonts.size1
                scale: slider.pressed || handleHover.hovered ? 1.12 : 1

                HoverHandler {
                    id: handleHover
                }

                Behavior on scale {
                    NumberAnimation {
                        duration: 90
                    }
                }
            }
        }
    }
}
