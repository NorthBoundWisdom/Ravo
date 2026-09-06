pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: stackRoot
    required property var panel
    Layout.fillWidth: true
    Layout.leftMargin: Fonts.standardMargin
    Layout.rightMargin: Fonts.standardMargin
    Layout.bottomMargin: Fonts.size12
    spacing: Fonts.smallSpacing

    DevelopLightSection {
        panel: stackRoot.panel
    }
    DevelopCurvesSection {
        panel: stackRoot.panel
    }
    DevelopColorCoreSection {
        panel: stackRoot.panel
    }
    DevelopColorEqualizerSection {
        panel: stackRoot.panel
    }
    DevelopColorAdvancedSection {
        panel: stackRoot.panel
    }
    DevelopPrimariesSection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
    DevelopGeometrySection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
    DevelopToneEqualizerSection {
        panel: stackRoot.panel
    }
    DevelopGraduatedSection {
        panel: stackRoot.panel
    }
    DevelopEffectsSection {
        panel: stackRoot.panel
    }
    DevelopDetailSection {
        panel: stackRoot.panel
    }
    DevelopRawSection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
    DevelopCalibrationSection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
    DevelopInputProfileSection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
    DevelopProfileGammaSection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
    DevelopOutputProfileSection {
        visible: !stackRoot.panel.localEditing
        panel: stackRoot.panel
    }
}
