import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    color: Theme.windowColor

    signal closeRequested

    focus: visible
    Keys.onEscapePressed: root.closeRequested()

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Fonts.standardMargin
        spacing: Fonts.size12

        RowLayout {
            Layout.fillWidth: true
            CustomButton {
                text: qsTr("Back")
                icon.source: "qrc:/GeoControls/icons/Undo.svg"
                onClicked: root.closeRequested()
            }
            CustomLabel {
                text: qsTr("Settings")
                font.bold: true
                font.pixelSize: Fonts.size18
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.dividerColor
        }

        CustomLabel {
            text: qsTr("Appearance")
            font.bold: true
        }

        RowLayout {
            spacing: Fonts.size12
            CustomLabel {
                text: qsTr("Night mode")
            }
            CustomSwitch {
                id: nightSwitch
                model: [qsTr("Off"), qsTr("On")]
                enabled: root.presenter !== null && root.presenter !== undefined
                onActivated: function (index) {
                    if (root.presenter)
                        root.presenter.nightMode = index === 1;
                }
            }
        }

        Binding {
            target: nightSwitch
            property: "currentIndex"
            value: root.presenter && root.presenter.nightMode ? 1 : 0
        }

        CustomLabel {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.placeholderTextColor
            text: qsTr("Use a dark studio palette for browsing and editing. This preference is saved on this computer.")
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
