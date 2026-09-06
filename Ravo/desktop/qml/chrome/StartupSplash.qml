import QtQuick
import QtQuick.Window
import GeoControls 1.0

Window {
    id: splash
    objectName: "startupSplash"
    required property var presenter
    property bool animationsEnabled: true
    width: Fonts.scaledUiSize(560)
    height: Fonts.scaledUiSize(320)
    flags: Qt.SplashScreen | Qt.FramelessWindowHint
    transientParent: null
    color: "transparent"
    title: qsTranslate("Main", "Ravo Studio")
    visible: false
    x: screen.virtualX + (screen.width - width) / 2
    y: screen.virtualY + (screen.height - height) / 2
    onClosing: Qt.quit()

    Rectangle {
        anchors.fill: parent
        radius: Fonts.size12
        color: "#202326"
        border.width: 1
        border.color: "#3b4146"
        clip: true

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.margins: 1
            width: Fonts.scaledUiSize(174)
            radius: Fonts.size12
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: "#113d4c"
                }
                GradientStop {
                    position: 1
                    color: "#17212b"
                }
            }

            Image {
                objectName: "startupLogo"
                anchors.centerIn: parent
                width: Fonts.scaledUiSize(124)
                height: width
                source: "qrc:/ravo/studio/icons/AppIcon.png"
                sourceSize: Qt.size(256, 256)
                fillMode: Image.PreserveAspectFit
                mipmap: true
                Accessible.ignored: true
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Fonts.scaledUiSize(212)
            anchors.rightMargin: Fonts.scaledUiSize(36)
            anchors.verticalCenter: parent.verticalCenter
            spacing: Fonts.size24

            Text {
                width: parent.width
                text: qsTranslate("Main", "Ravo Studio")
                font: Fonts.makeBoldFont(Fonts.makeScaledFont(Fonts.standardFont, 1.6))
                color: "#f2f4f5"
                wrapMode: Text.WordWrap
            }

            Text {
                width: parent.width
                text: splash.presenter.busy ? splash.presenter.statusText : ""
                font: Fonts.standardFont
                color: "#aab4bc"
                wrapMode: Text.WordWrap
            }

            Rectangle {
                id: track
                width: parent.width
                height: Fonts.scaledUiSize(3)
                radius: height / 2
                color: "#3b4146"
                clip: true
                Accessible.role: Accessible.ProgressBar
                Accessible.name: splash.presenter.statusText

                Rectangle {
                    id: sweep
                    width: track.width * 0.4
                    height: track.height
                    radius: track.radius
                    x: (track.width - width) / 2
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
                    // Render-thread animation continues during GUI publication.
                    XAnimator on x {
                        from: -sweep.width
                        to: track.width
                        duration: 1400
                        loops: Animation.Infinite
                        running: splash.visible && splash.animationsEnabled
                        easing.type: Easing.InOutSine
                    }
                }
            }
        }
    }
}
