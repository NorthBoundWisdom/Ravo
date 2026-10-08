pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

ColumnLayout {
    id: root
    property var presenter
    property var commands
    required property Flickable scrollViewport
    property bool liveReady: false
    property string workspace: "edit"
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined
    readonly property bool hasSelection: hasPresenter && presenter.selectedAssetId.length > 0
    readonly property bool localEditing: hasPresenter && presenter.develop.localEditing === true
    readonly property bool cropPinned: hasPresenter && presenter.develop.cropToolActive
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

    Repeater {
        model: [root.hasPresenter && root.presenter.develop.activeLocalId !== undefined ? root.presenter.develop.activeLocalId : ""]
        delegate: DevelopAdjustmentStack {
            required property var modelData
            enabled: modelData === (root.hasPresenter && root.presenter.develop.activeLocalId !== undefined ? root.presenter.develop.activeLocalId : "")
            panel: root
            visible: root.workspace === "edit" || (root.workspace === "local" && root.localEditing)
            Layout.fillWidth: true
        }
    }
}
