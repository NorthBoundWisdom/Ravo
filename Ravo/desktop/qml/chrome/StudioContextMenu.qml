import QtQuick
import QtQuick.Controls
import GeoControls 1.0

Menu {
    id: root

    property int menuWidth: 240

    modal: true
    dim: false
    overlap: 0
    padding: Fonts.size4
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: Overlay.overlay

    palette.window: Theme.popupSurfaceColor
    palette.windowText: Theme.textColor
    palette.base: Theme.popupSurfaceColor
    palette.text: Theme.textColor
    palette.button: Theme.popupSurfaceColor
    palette.buttonText: Theme.textColor
    palette.highlight: Theme.buttonHoveredColor
    palette.highlightedText: Theme.textColor
    palette.mid: Theme.dividerColor

    background: Rectangle {
        implicitWidth: root.menuWidth
        color: Theme.popupSurfaceColor
        border.color: Theme.dividerColor
        border.width: 1
        radius: 4
    }

    delegate: StudioContextMenuItem {}
}
