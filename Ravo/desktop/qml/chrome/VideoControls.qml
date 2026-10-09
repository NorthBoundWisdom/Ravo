import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    objectName: "videoPlaybackControls"
    required property var presenter
    required property var commands
    function timeText(milliseconds) {
        return Math.floor(milliseconds / 60000) + ":" + ("0" + Math.floor(milliseconds / 1000) % 60).slice(-2);
    }
    implicitHeight: controls.implicitHeight + Fonts.size8 * 2
    color: Theme.toolbarSurfaceColor
    radius: ControlState.radiusSmall
    ColumnLayout {
        id: controls
        anchors.fill: parent
        anchors.margins: Fonts.size8
        spacing: Fonts.size4
        RowLayout {
            Layout.fillWidth: true
            Item {
                implicitWidth: playButton.implicitWidth
                implicitHeight: playButton.implicitHeight
                CustomButton {
                    id: playButton
                    anchors.fill: parent
                    objectName: "videoPlayPause"
                    readonly property string commandId: root.presenter.state === "playing" ? "studio.video.pause" : "studio.video.play"
                    readonly property var playAction: root.commands.controller.action(commandId)
                    text: playAction.title
                    enabled: playAction.enabled
                    tooltipText: playAction.disabledReason
                    onClicked: root.commands.run(commandId)
                }
                HoverHandler {
                    id: disabledPlayHover
                    enabled: !playButton.enabled
                }
                CustomToolTip {
                    visible: disabledPlayHover.hovered && !playButton.enabled
                    text: playButton.playAction.disabledReason
                }
            }
            CustomLabel {
                text: root.timeText(root.presenter.position) + " / " + root.timeText(root.presenter.duration)
            }
            CustomSlider {
                objectName: "videoPosition"
                Layout.fillWidth: true
                showTitle: false
                showValueLabel: false
                showStepButton: false
                from: 0
                to: Math.max(1, root.presenter.duration)
                stepSize: 1
                value: root.presenter.position
                enabled: root.presenter.duration > 0
                onValueCommitted: function (value) {
                    root.commands.run("studio.video.seek", Math.round(value));
                }
            }
            CustomCheckBox {
                objectName: "videoMute"
                text: qsTranslate("StudioCommands", "Mute Video")
                checked: root.presenter.muted
                onClicked: root.commands.run("studio.video.mute", checked)
            }
            CustomSlider {
                objectName: "videoVolume"
                Layout.preferredWidth: Fonts.size80
                showTitle: false
                showValueLabel: false
                showStepButton: false
                from: 0
                to: 1
                stepSize: 0.01
                value: root.presenter.volume
                tooltipText: qsTranslate("StudioCommands", "Video Volume")
                onValueCommitted: function (value) {
                    root.commands.run("studio.video.volume", value);
                }
            }
        }
        CustomLabel {
            Layout.fillWidth: true
            visible: root.presenter.warningsText.length > 0
            text: root.presenter.warningsText
            wrapMode: Text.Wrap
            color: Theme.placeholderTextColor
        }
        CustomLabel {
            Layout.fillWidth: true
            visible: root.presenter.error.length > 0
            text: root.presenter.error
            wrapMode: Text.Wrap
            color: Theme.placeholderTextColor
        }
    }
}
