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
    readonly property bool localEditing: hasPresenter && presenter.localEditing === true
    property bool showAdvancedInstances: false
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

    LocalAdjustmentWorkspace {
        panel: root
        Layout.fillWidth: true
    }

    Repeater {
        model: [root.hasPresenter && root.presenter.activeLocalId !== undefined ? root.presenter.activeLocalId : ""]
        delegate: DevelopAdjustmentStack {
            required property var modelData
            enabled: modelData === (root.hasPresenter && root.presenter.activeLocalId !== undefined ? root.presenter.activeLocalId : "")
            panel: root
            Layout.fillWidth: true
        }
    }
}
