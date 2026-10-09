import QtQuick
import QtQuick.Controls
import GeoControls 1.0

CustomMenu {
    id: root

    menuWidth: 200

    modal: false
    dim: false
    overlap: 0
    padding: Fonts.size4

    delegate: StudioContextMenuItem {}
}
