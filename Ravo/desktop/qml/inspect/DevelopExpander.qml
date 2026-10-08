import QtQuick
import GeoControls 1.0

Expander {
    id: control

    // The filled header covers the inherited Rectangle's border. Keep the
    // outline above both header and body, outside the default content layout.
    readonly property Item outline: Rectangle {
        parent: control
        anchors.fill: parent
        z: 1
        color: "transparent"
        radius: control.radius
        border.color: control.border.color
        border.width: control.border.width
    }
}
