pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

DevelopSection {
    id: sectionRoot
    title: qsTr("Detail")
    sectionId: "detail"
    ColumnLayout {
        Layout.fillWidth: true
        width: parent.width
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Texture")
            from: -100
            to: 100
            stepSize: 1
            validatorDecimals: 0
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editTexture.strength * 50 : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("texture", value / 50);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("texture", value / 50);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("texture")
        }
        Expander {
            Layout.fillWidth: true
            title: qsTr("Texture · more")
            expanded: false
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Texture scale")
                from: 0.01
                to: 10
                stepSize: 0.01
                validatorDecimals: 2
                showReset: true
                resetValue: 0.2
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editTexture.detailThreshold : 0.2
                onValueEdited: function (value) {
                    if (panel.liveReady && panel.commands)
                        panel.commands.previewDevelopNumber("textureDetailThreshold", value);
                }
                onValueCommitted: function (value) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber("textureDetailThreshold", value);
                }
                onResetRequested: if (panel.commands)
                    panel.commands.resetControl("textureDetailThreshold")
            }
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Texture iterations")
                from: 1
                to: 5
                stepSize: 1
                validatorDecimals: 0
                showReset: true
                resetValue: 1
                delayedCommit: true
                enabled: panel.hasSelection
                value: panel.hasPresenter ? panel.presenter.editTexture.iterations : 1
                onValueEdited: function (value) {
                    if (panel.liveReady && panel.commands)
                        panel.commands.previewDevelopNumber("textureIterations", value);
                }
                onValueCommitted: function (value) {
                    if (panel.commands)
                        panel.commands.setDevelopNumber("textureIterations", value);
                }
                onResetRequested: if (panel.commands)
                    panel.commands.resetControl("textureIterations")
            }
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Sharpen")
            from: 0
            to: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSharpen : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("sharpen", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("sharpen", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("sharpen")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Radius")
            from: 0
            to: 8
            stepSize: 0.1
            validatorDecimals: 1
            showReset: true
            resetValue: 2
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSharpenRadius : 2
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("sharpenRadius", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("sharpenRadius", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("sharpenRadius")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Masking")
            from: 0
            to: 100
            stepSize: 0.1
            validatorDecimals: 1
            showReset: true
            resetValue: 0.5
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editSharpenThreshold : 0.5
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("sharpenThreshold", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("sharpenThreshold", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("sharpenThreshold")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Luminance denoise")
            from: 0
            to: 1
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editDenoise : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("denoise", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("denoise", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("denoise")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Color denoise")
            from: 0
            to: 1
            stepSize: 0.05
            validatorDecimals: 2
            showReset: true
            resetValue: 1
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editDenoiseChroma : 1
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("denoiseChroma", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("denoiseChroma", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("denoiseChroma")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Denoise radius")
            from: 0.5
            to: 8
            stepSize: 0.1
            validatorDecimals: 1
            showReset: true
            resetValue: 1
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editDenoiseRadius : 1
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("denoiseRadius", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("denoiseRadius", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("denoiseRadius")
        }
        ColumnLayout {
            id: retouchEditor
            Layout.fillWidth: true
            spacing: 6
            property int draftMode: 1
            property real draftCenterX: 0.5
            property real draftCenterY: 0.5
            property real draftRadius: 0.08
            property real draftFeather: 0.03
            property real draftOpacity: 1.0
            property real draftSourceX: 0.35
            property real draftSourceY: 0.5
            property int draftBlurType: 0
            property real draftBlurRadius: 10.0
            property int draftFillMode: 0
            property real draftFillR: 0.5
            property real draftFillG: 0.5
            property real draftFillB: 0.5
            property real draftFillBrightness: 0.0

            Label {
                text: qsTr("Retouch")
                font.weight: Font.DemiBold
            }
            ComboBox {
                id: retouchMode
                Layout.fillWidth: true
                enabled: panel.hasSelection
                model: [qsTr("Clone"), qsTr("Heal"), qsTr("Blur"), qsTr("Fill")]
                currentIndex: retouchEditor.draftMode
                onActivated: retouchEditor.draftMode = currentIndex
                Accessible.name: qsTr("Retouch mode")
            }
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Target X")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftCenterX
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftCenterX = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Target Y")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftCenterY
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftCenterY = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Spot radius")
                from: 0.01
                to: 0.5
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftRadius
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftRadius = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Spot feather")
                from: 0
                to: 0.5
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftFeather
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftFeather = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                title: qsTr("Spot opacity")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftOpacity
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftOpacity = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode < 2
                title: qsTr("Source X")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftSourceX
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftSourceX = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode < 2
                title: qsTr("Source Y")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftSourceY
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftSourceY = value;
                }
            }
            ComboBox {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 2
                enabled: panel.hasSelection
                model: [qsTr("Gaussian"), qsTr("Bilateral")]
                currentIndex: retouchEditor.draftBlurType
                onActivated: retouchEditor.draftBlurType = currentIndex
                Accessible.name: qsTr("Blur type")
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 2
                title: qsTr("Blur radius")
                from: 0.1
                to: 200
                stepSize: 0.1
                validatorDecimals: 1
                value: retouchEditor.draftBlurRadius
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftBlurRadius = value;
                }
            }
            ComboBox {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 3
                enabled: panel.hasSelection
                model: [qsTr("Erase"), qsTr("Color")]
                currentIndex: retouchEditor.draftFillMode
                onActivated: retouchEditor.draftFillMode = currentIndex
                Accessible.name: qsTr("Fill mode")
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 3 && retouchEditor.draftFillMode === 1
                title: qsTr("Fill red")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftFillR
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftFillR = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 3 && retouchEditor.draftFillMode === 1
                title: qsTr("Fill green")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftFillG
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftFillG = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 3 && retouchEditor.draftFillMode === 1
                title: qsTr("Fill blue")
                from: 0
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftFillB
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftFillB = value;
                }
            }
            CustomSlider {
                Layout.fillWidth: true
                visible: retouchEditor.draftMode === 3
                title: qsTr("Fill brightness")
                from: -1
                to: 1
                stepSize: 0.01
                validatorDecimals: 2
                value: retouchEditor.draftFillBrightness
                enabled: panel.hasSelection
                onValueEdited: function (value) {
                    retouchEditor.draftFillBrightness = value;
                }
            }
            Button {
                Layout.fillWidth: true
                text: qsTr("Add retouch region")
                enabled: panel.hasSelection && panel.commands
                onClicked: panel.commands.addRetouchRegion({
                    "mode": ["clone", "heal", "blur", "fill"][retouchEditor.draftMode],
                    "centerX": retouchEditor.draftCenterX,
                    "centerY": retouchEditor.draftCenterY,
                    "radius": retouchEditor.draftRadius,
                    "feather": retouchEditor.draftFeather,
                    "opacity": retouchEditor.draftOpacity,
                    "sourceX": retouchEditor.draftSourceX,
                    "sourceY": retouchEditor.draftSourceY,
                    "blurType": retouchEditor.draftBlurType === 0 ? "gaussian" : "bilateral",
                    "blurRadius": retouchEditor.draftBlurRadius,
                    "fillMode": retouchEditor.draftFillMode === 0 ? "erase" : "color",
                    "fillR": retouchEditor.draftFillR,
                    "fillG": retouchEditor.draftFillG,
                    "fillB": retouchEditor.draftFillB,
                    "fillBrightness": retouchEditor.draftFillBrightness
                })
            }
            Label {
                text: qsTr("Regions: %1").arg(panel.hasPresenter ? panel.presenter.editRetouch.regionCount : 0)
                opacity: 0.72
            }
            Repeater {
                model: panel.hasPresenter ? panel.presenter.editRetouch.regions : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: modelData.mode + " · " + modelData.maskKind
                        elide: Text.ElideRight
                    }
                    Button {
                        text: qsTr("Remove")
                        enabled: panel.hasSelection && panel.commands
                        onClicked: panel.commands.removeRetouchRegion(modelData.index)
                    }
                }
            }
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Clarity")
            from: -1
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editClarity : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("clarity", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("clarity", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("clarity")
        }
        CustomSlider {
            Layout.fillWidth: true
            title: qsTr("Grain")
            from: 0
            to: 1
            showReset: true
            resetValue: 0
            delayedCommit: true
            enabled: panel.hasSelection
            value: panel.hasPresenter ? panel.presenter.editGrain : 0
            onValueEdited: function (value) {
                if (panel.liveReady && panel.commands)
                    panel.commands.previewDevelopNumber("grain", value);
            }
            onValueCommitted: function (value) {
                if (panel.commands)
                    panel.commands.setDevelopNumber("grain", value);
            }
            onResetRequested: if (panel.commands)
                panel.commands.resetControl("grain")
        }
    }
}
