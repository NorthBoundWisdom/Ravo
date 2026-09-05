import QtQuick
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property string title
    property bool expanded: true
    default property alias sectionContent: body.data
    spacing: Fonts.size8
    CustomButton {
        Layout.fillWidth: true
        text: (root.expanded ? "▾  " : "▸  ") + root.title
        Accessible.name: root.title
        onClicked: root.expanded = !root.expanded
    }
    ColumnLayout {
        id: body
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.size4
        Layout.rightMargin: Fonts.size4
        visible: root.expanded
        spacing: Fonts.size8
    }
}
