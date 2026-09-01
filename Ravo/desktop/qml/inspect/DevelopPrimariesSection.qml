pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Camera Calibration")
    sectionId: "primaries"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        Repeater {
            model: [
                {
                    "title": qsTr("Shadow tint hue"),
                    "key": "achromaticTintHueDegrees",
                    "field": "primariesAchromaticHueDegrees",
                    "minimum": -180,
                    "maximum": 180,
                    "reset": 0,
                    "step": 0.1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Shadow tint purity"),
                    "key": "achromaticTintPurity",
                    "field": "primariesAchromaticPurity",
                    "minimum": 0,
                    "maximum": 0.5,
                    "reset": 0,
                    "step": 0.002,
                    "decimals": 3
                },
                {
                    "title": qsTr("Red hue"),
                    "key": "redHueDegrees",
                    "field": "primariesRedHueDegrees",
                    "minimum": -90,
                    "maximum": 90,
                    "reset": 0,
                    "step": 0.1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Red saturation"),
                    "key": "redPurity",
                    "field": "primariesRedPurity",
                    "minimum": 0.2,
                    "maximum": 3,
                    "reset": 1,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Green hue"),
                    "key": "greenHueDegrees",
                    "field": "primariesGreenHueDegrees",
                    "minimum": -90,
                    "maximum": 90,
                    "reset": 0,
                    "step": 0.1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Green saturation"),
                    "key": "greenPurity",
                    "field": "primariesGreenPurity",
                    "minimum": 0.2,
                    "maximum": 3,
                    "reset": 1,
                    "step": 0.01,
                    "decimals": 2
                },
                {
                    "title": qsTr("Blue hue"),
                    "key": "blueHueDegrees",
                    "field": "primariesBlueHueDegrees",
                    "minimum": -90,
                    "maximum": 90,
                    "reset": 0,
                    "step": 0.1,
                    "decimals": 1
                },
                {
                    "title": qsTr("Blue saturation"),
                    "key": "bluePurity",
                    "field": "primariesBluePurity",
                    "minimum": 0.2,
                    "maximum": 3,
                    "reset": 1,
                    "step": 0.01,
                    "decimals": 2
                }
            ]
            delegate: PrimariesSlider {
                panel: sectionRoot.panel
            }
        }
    }
}
