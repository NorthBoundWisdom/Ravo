import QtQuick
import QtQuick.Controls
import GeoControls 1.0

MenuSeparator {
    padding: Fonts.size4

    contentItem: Rectangle {
        implicitHeight: 1
        color: Theme.dividerColor
    }

    background: Rectangle {
        color: Theme.popupSurfaceColor
    }
}
