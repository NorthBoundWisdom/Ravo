pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Color · Advanced")
    sectionId: "color"
    initialExpanded: false
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        DevelopAdvancedLooks {
            panel: sectionRoot.panel
        }
        DevelopAdvancedToning {
            panel: sectionRoot.panel
        }
        DevelopAdvancedColorOps {
            panel: sectionRoot.panel
        }
    }
}
