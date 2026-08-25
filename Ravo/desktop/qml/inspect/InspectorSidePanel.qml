import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Rectangle {
    id: root
    property var presenter
    property var commands
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool developOpen: hasPresenter && presenter.browseMode === "develop"
    property bool liveReady: false

    Component.onCompleted: liveReady = true

    color: Theme.railSurfaceColor

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ScopePanel {
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.scaledUiSize(128)
            Layout.minimumHeight: Fonts.scaledUiSize(96)
            presenter: root.presenter
        }

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            contentWidth: width
            contentHeight: column.implicitHeight

            ColumnLayout {
                id: column
                width: parent.width
                spacing: Fonts.smallSpacing

                PhotoInfoPanel {
                    visible: !root.developOpen
                    Layout.fillWidth: true
                    presenter: root.presenter
                    commands: root.commands
                }

                DevelopPanel {
                    visible: root.developOpen
                    Layout.fillWidth: true
                    presenter: root.presenter
                    commands: root.commands
                    liveReady: root.liveReady
                }
            }
        }
    }
}
