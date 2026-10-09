import QtQuick
import QtQuick.Controls
import GeoControls 1.0

CustomMenu {
    id: root

    menuWidth: 240

    modal: true
    dim: false
    overlap: 0
    padding: Fonts.size4
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: Overlay.overlay

    delegate: StudioContextMenuItem {}
}
