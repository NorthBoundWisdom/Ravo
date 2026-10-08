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
    readonly property double visualValue: slider.visualValue

    signal valueEdited(double value)
    signal valueCommitted(double value)
    signal resetRequested

    Layout.fillWidth: true
    implicitWidth: Fonts.size180
    implicitHeight: content.implicitHeight

    ColumnLayout {
        id: content
        anchors.fill: parent
        spacing: Fonts.size4
        RowLayout {
            Layout.fillWidth: true
            CustomLabel {
                Layout.fillWidth: true
                text: root.title
                elide: Text.ElideRight
                MouseArea {
                    anchors.fill: parent
                    enabled: root.enabled
                    onDoubleClicked: root.resetRequested()
                }
            }
            CustomLabel {
                text: Number(root.visualValue * root.displayScale).toFixed(root.displayDecimals)
                horizontalAlignment: Text.AlignRight
            }
        }
        CustomSlider {
            id: slider
            Layout.fillWidth: true
            title: root.title
            showTitle: false
            showValueLabel: false
            showStepButton: false
            from: root.from
            to: root.to
            stepSize: root.stepSize
            value: root.value
            trackGradient: root.trackGradient
            delayedCommit: root.delayedCommit
            commitDelay: root.commitDelay
            onValueEdited: function (value) {
                root.valueEdited(value);
            }
            onValueCommitted: function (value) {
                root.valueCommitted(value);
            }
        }
    }
}
