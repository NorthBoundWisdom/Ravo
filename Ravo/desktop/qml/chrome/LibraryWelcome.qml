import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

// Presentation only: the presenter owns library work and the registered actions
// own dialog requests. The entrance never delays opening a library.
Rectangle {
    id: root
    required property var presenter
    required property var commands
    property bool animationsEnabled: true
    readonly property bool motionActive: visible && animationsEnabled
    property real entranceProgress: 1
    color: Theme.windowColor

    onMotionActiveChanged: {
        entrance.stop();
        entranceProgress = 1;
        if (motionActive) {
            entranceProgress = 0;
            entrance.start();
        }
    }

    NumberAnimation {
        id: entrance
        target: root
        property: "entranceProgress"
        from: 0
        to: 1
        duration: 520
        easing.type: Easing.OutCubic
    }

    Flickable {
        id: scroll
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, content.implicitHeight + Fonts.scaledUiSize(32) * 2)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            x: (scroll.width - width) / 2
            y: Math.max(Fonts.scaledUiSize(32), (scroll.height - implicitHeight) / 2)
            width: Math.max(0, Math.min(Fonts.scaledUiSize(520), scroll.width - Fonts.size24 * 2))
            spacing: Fonts.size24

            Image {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Fonts.scaledUiSize(root.height < Fonts.scaledUiSize(440) ? 88 : 120)
                Layout.preferredHeight: Layout.preferredWidth
                source: "qrc:/ravo/studio/icons/AppIcon.png"
                sourceSize.width: 256
                sourceSize.height: 256
                fillMode: Image.PreserveAspectFit
                mipmap: true
                opacity: root.entranceProgress
                scale: 0.9 + 0.1 * root.entranceProgress
                Accessible.ignored: true
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Fonts.size12
                opacity: 0.6 + 0.4 * root.entranceProgress

                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTranslate("Main", "Ravo Studio")
                    font.pixelSize: Fonts.scaledUiSize(32)
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.textColor
                }

                CustomLabel {
                    Layout.fillWidth: true
                    text: root.presenter.busy ? root.presenter.statusText : qsTranslate("Main", "Create or open a library to import photos.")
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.placeholderTextColor
                }
            }

            Item {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Fonts.scaledUiSize(192)
                Layout.preferredHeight: Fonts.size8
                visible: root.presenter.busy
                Accessible.role: Accessible.ProgressBar
                Accessible.name: root.presenter.statusText

                Rectangle {
                    id: progressTrack
                    anchors.centerIn: parent
                    width: parent.width
                    height: Fonts.scaledUiSize(3)
                    radius: height / 2
                    color: Theme.alternateBaseColor
                    clip: true

                    Rectangle {
                        id: sweep
                        width: parent.width * 0.45
                        height: parent.height
                        radius: parent.radius
                        x: (parent.width - width) / 2
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop {
                                position: 0
                                color: "#079fdf"
                            }
                            GradientStop {
                                position: 0.65
                                color: "#05cbc7"
                            }
                            GradientStop {
                                position: 1
                                color: "#ffbd22"
                            }
                        }

                        NumberAnimation on x {
                            from: -sweep.width
                            to: progressTrack.width
                            duration: 1400
                            loops: Animation.Infinite
                            running: root.motionActive && root.presenter.busy
                            easing.type: Easing.InOutSine
                        }
                    }
                }
            }

            GridLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.fillWidth: true
                columns: content.width < Fonts.scaledUiSize(400) ? 1 : 2
                columnSpacing: Fonts.size12
                rowSpacing: Fonts.size12
                visible: !root.presenter.busy

                CustomButton {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    defaultHeight: Fonts.scaledUiSize(44)
                    action: root.commands.createLibrary
                    buttonColor: Theme.highlightColor
                    buttonTextColor: Theme.highlightedTextColor
                    hoveredColor: Qt.darker(Theme.highlightColor, 1.1)
                    pressedColor: Qt.darker(Theme.highlightColor, 1.2)
                    Accessible.name: text
                }

                CustomButton {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 1
                    defaultHeight: Fonts.scaledUiSize(44)
                    action: root.commands.openLibrary
                    Accessible.name: text
                }
            }
        }
    }
}
