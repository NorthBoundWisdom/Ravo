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
            objectName: "cropScopePanel"
            Layout.fillWidth: true
            Layout.preferredHeight: Fonts.scaledUiSize(128)
            Layout.minimumHeight: Fonts.scaledUiSize(96)
            presenter: root.presenter
            commands: root.commands
        }

        Flickable {
            objectName: "pinnedCropPanel"
            visible: root.developOpen && developPanel.cropPinned
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(cropControls.implicitHeight + 2 * Fonts.standardMargin, root.height * 0.55)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            contentWidth: width
            contentHeight: cropControls.implicitHeight + 2 * Fonts.standardMargin
            DevelopCropControls {
                id: cropControls
                x: Fonts.standardMargin
                y: Fonts.standardMargin
                width: parent.width - 2 * Fonts.standardMargin
                panel: developPanel
                pinned: true
            }
        }

        Flickable {
            objectName: "developPanelScroller"
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
                    id: developPanel
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
