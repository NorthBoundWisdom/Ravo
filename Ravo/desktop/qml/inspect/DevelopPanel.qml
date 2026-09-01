pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property var presenter
    property var commands
    property bool liveReady: false
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    spacing: Fonts.smallSpacing

    function openLut3dDialog() {
        lut3dDialog.openDialog();
    }

    QmlFileDialogPage {
        id: lut3dDialog
        dialogTitle: qsTr("Choose 3D LUT")
        dialogMode: "open"
        nameFilters: [qsTr("Cube LUT (*.cube *.CUBE)")]
        onFileAccepted: function (filePath) {
            if (root.commands)
                root.commands.setDevelopText("lut3dFile", filePath);
        }
    }

    CustomLabel {
        Layout.leftMargin: Fonts.standardMargin
        Layout.topMargin: Fonts.size8
        text: qsTr("Develop")
        font.bold: true
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Fonts.standardMargin
        Layout.rightMargin: Fonts.standardMargin
        Layout.bottomMargin: Fonts.size12
        spacing: Fonts.smallSpacing

        DevelopLightSection {
            panel: root
        }
        DevelopCurvesSection {
            panel: root
        }
        DevelopColorCoreSection {
            panel: root
        }
        DevelopColorEqualizerSection {
            panel: root
        }
        DevelopColorAdvancedSection {
            panel: root
        }
        DevelopPrimariesSection {
            panel: root
        }
        DevelopGeometrySection {
            panel: root
        }
        DevelopToneEqualizerSection {
            panel: root
        }
        DevelopGraduatedSection {
            panel: root
        }
        DevelopEffectsSection {
            panel: root
        }
        DevelopDetailSection {
            panel: root
        }
        DevelopRawSection {
            panel: root
        }
        DevelopCalibrationSection {
            panel: root
        }
        DevelopInputProfileSection {
            panel: root
        }
        DevelopProfileGammaSection {
            panel: root
        }
        DevelopOutputProfileSection {
            panel: root
        }
    }
}
