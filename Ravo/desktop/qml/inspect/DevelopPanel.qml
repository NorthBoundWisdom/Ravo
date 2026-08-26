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

    component DevelopSection: CustomEditPanel {
        showAddButton: false
        showDeleteButton: false
        showApplyButton: false
        actionsNeedEditing: false
        actionButtonsEnabled: root.hasSelection
        editing: root.hasSelection
        resetTooltip: qsTr("Reset this section")
        property string sectionId
        onReset: function () {
            if (root.commands && sectionId.length)
                root.commands.resetSection(sectionId);
        }
    }

    component MixerSlider: CustomSlider {
        id: mixer
        required property string inputChannel
        property string fieldName
        property double currentValue: 0
        property double identityValue: 0
        readonly property color lowTrackColor: {
            if (inputChannel === "red")
                return "#3f9297";
            if (inputChannel === "green")
                return "#a65b9a";
            if (inputChannel === "blue")
                return "#c09a45";
            return Theme.midColor;
        }
        readonly property color highTrackColor: {
            if (inputChannel === "red")
                return "#d45c64";
            if (inputChannel === "green")
                return "#58b574";
            if (inputChannel === "blue")
                return "#5b83d1";
            return Theme.midColor;
        }
        Layout.fillWidth: true
        from: -2
        to: 2
        stepSize: 0.01
        validatorDecimals: 2
        showReset: true
        resetValue: identityValue
        delayedCommit: true
        enabled: root.hasSelection
        value: currentValue
        trackGradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop {
                position: 0
                color: mixer.lowTrackColor
            }
            GradientStop {
                position: 0.5
                color: Theme.midColor
            }
            GradientStop {
                position: 1
                color: mixer.highTrackColor
            }
        }
        onValueChanged: if (root.liveReady && root.commands)
                root.commands.previewDevelopNumber(fieldName, value)
        onValueCommitted: function (value) {
            if (root.commands)
                root.commands.setDevelopNumber(fieldName, value);
        }
        onResetRequested: if (root.commands)
            root.commands.resetControl(fieldName)
    }

    component PrimariesSlider: CustomSlider {
        required property var modelData
        Layout.fillWidth: true
        title: modelData.title
        from: modelData.minimum
        to: modelData.maximum
        stepSize: modelData.step
        validatorDecimals: modelData.decimals
        showReset: true
        resetValue: modelData.reset
        delayedCommit: true
        enabled: root.hasSelection
        value: root.hasPresenter ? root.presenter.editPrimaries[modelData.key] : modelData.reset
        onValueChanged: if (root.liveReady && root.commands)
                root.commands.previewDevelopNumber(modelData.field, value)
        onValueCommitted: function (value) {
            if (root.commands)
                root.commands.setDevelopNumber(modelData.field, value);
        }
        onResetRequested: if (root.commands)
            root.commands.resetControl(modelData.field)
    }

    component ColorCheckerNumberField: RowLayout {
        required property var modelData
        Layout.fillWidth: true
        spacing: Fonts.smallSpacing

        CustomLabel {
            Layout.fillWidth: true
            text: modelData.title
        }
        CustomTextField {
            Layout.preferredWidth: Fonts.standardFontMetrics.averageCharacterWidth * 12
            showEmptyIndicator: false
            showClipIndicator: false
            enabled: root.hasSelection && root.hasPresenter
                     && root.presenter.editColorChecker.patchCount > 0
            validator: DoubleValidator {
                bottom: -3.402823466e38
                top: 3.402823466e38
                decimals: 9
                notation: DoubleValidator.ScientificNotation
            }
            text: root.hasPresenter
                  ? Number(root.presenter.editColorChecker[modelData.key]).toString() : "0"
            onEditingCommitted: function (committedText) {
                const parsed = Number(committedText);
                if (Number.isFinite(parsed) && root.commands)
                    root.commands.setDevelopNumber(modelData.field, parsed);
            }
        }
        CustomButton {
            text: qsTr("Reset")
            enabled: root.hasSelection && root.hasPresenter
                     && root.presenter.editColorChecker.patchCount > 0
            onClicked: if (root.commands)
                root.commands.resetControl(modelData.field)
        }
    }

    component ColorContrastOffsetField: RowLayout {
        required property var modelData
        Layout.fillWidth: true
        spacing: Fonts.smallSpacing

        CustomLabel {
            Layout.fillWidth: true
            text: modelData.title
        }
        CustomTextField {
            Layout.preferredWidth: Fonts.standardFontMetrics.averageCharacterWidth * 12
            showEmptyIndicator: false
            showClipIndicator: false
            enabled: root.hasSelection
            validator: DoubleValidator {
                bottom: -3.4028234663852886e38
                top: 3.4028234663852886e38
                decimals: 9
                notation: DoubleValidator.ScientificNotation
            }
            text: root.hasPresenter
                  ? Number(root.presenter.editColorContrast[modelData.key]).toString() : "0"
            onEditingCommitted: function (committedText) {
                const parsed = Number(committedText);
                if (Number.isFinite(parsed) && root.commands)
                    root.commands.setDevelopNumber(modelData.field, parsed);
            }
        }
        CustomButton {
            text: qsTr("Reset")
            enabled: root.hasSelection
            onClicked: if (root.commands)
                root.commands.resetControl(modelData.field)
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

                RowLayout {
                    CustomButton {
                        text: qsTr("Undo")
                        enabled: root.hasPresenter && root.presenter.canUndo
                        onClicked: if (root.commands)
                            root.commands.undo.trigger()
                    }
                    CustomButton {
                        text: qsTr("Redo")
                        enabled: root.hasPresenter && root.presenter.canRedo
                        onClicked: if (root.commands)
                            root.commands.redo.trigger()
                    }
                    CustomButton {
                        text: root.hasPresenter && root.presenter.beforeAfter ? qsTr("After") : qsTr("Before")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.beforeAfter.trigger()
                    }
                    CustomButton {
                        text: qsTr("Reset all")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.resetEdits.trigger()
                    }
                }

                DevelopSection {
                    title: qsTr("Geometry")
                    sectionId: "geometry"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        spacing: Fonts.smallSpacing
                        RowLayout {
                            CustomButton {
                                text: qsTr("Rotate L")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.rotateLeft.trigger()
                            }
                            CustomButton {
                                text: qsTr("Rotate R")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.rotateRight.trigger()
                            }
                            CustomButton {
                                text: qsTr("Flip H")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.flipHorizontal.trigger()
                            }
                            CustomButton {
                                text: qsTr("Flip V")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.flipVertical.trigger()
                            }
                        }
                        CustomButton {
                            text: root.hasPresenter && root.presenter.cropToolActive ? qsTr("Done") : qsTr("Crop & Rotate")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.toggleCropTool()
                        }
                        CustomLabel {
                            text: qsTr("Drag the frame to crop. Drag outside it, or Option/Alt-drag, to straighten.")
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            opacity: 0.75
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: ["free", "1:1", "3:2", "4:3", "5:4", "16:9"]
                            enabled: root.hasSelection
                            currentIndex: {
                                const aspects = ["free", "1:1", "3:2", "4:3", "5:4", "16:9"];
                                const current = root.hasPresenter ? root.presenter.cropAspect : "free";
                                const index = aspects.indexOf(current);
                                return index < 0 ? 0 : index;
                            }
                            onActivated: if (root.commands)
                                root.commands.setCropAspect(currentText)
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Angle")
                            from: -45
                            to: 45
                            stepSize: 0.1
                            validatorDecimals: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editStraighten : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("straighten", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("straighten", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("straighten")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Input Profile")
                    sectionId: "inputProfile"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Source metadata"), qsTr("sRGB"), qsTr("Adobe RGB"),
                                qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"),
                                qsTr("Linear ProPhoto RGB"), qsTr("Display P3"), qsTr("HLG P3")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editInputColor.inputProfileIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("inputProfile", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Linear Rec709"), qsTr("Linear Rec2020"),
                                qsTr("Linear ProPhoto RGB"), qsTr("Display P3"), qsTr("Adobe RGB")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editInputColor.workingProfileIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("workingProfile", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Perceptual"), qsTr("Relative colorimetric"),
                                qsTr("Saturation"), qsTr("Absolute colorimetric")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editInputColor.intentIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("renderingIntent", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("No gamut clipping"), qsTr("Clip to sRGB"),
                                qsTr("Clip to Adobe RGB"), qsTr("Clip to linear Rec709"),
                                qsTr("Clip to linear Rec2020")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editInputColor.normalizeIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("gamutNormalize", currentIndex)
                        }
                        CustomCheckBox {
                            text: qsTr("RAW blue mapping")
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editInputColor.blueMapping
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("blueMapping", checked ? 1 : 0)
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: root.hasPresenter
                                ? qsTr("%1 → %2").arg(root.presenter.editInputColor.inputProfile)
                                    .arg(root.presenter.editInputColor.workingProfile)
                                : ""
                            wrapMode: Text.WordWrap
                            opacity: 0.75
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Unbreak input profile")
                    sectionId: "profileGamma"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomCheckBox {
                            text: qsTr("Enable correction")
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editProfileGamma.enabled
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("profileGammaEnabled", checked ? 1 : 0)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Logarithmic"), qsTr("Gamma")]
                            enabled: root.hasSelection && root.hasPresenter
                                && root.presenter.editProfileGamma.enabled
                            currentIndex: root.hasPresenter ? root.presenter.editProfileGamma.modeIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("profileGammaModeIndex", currentIndex)
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Dynamic range"), "key": "dynamicRange", "field": "profileGammaDynamicRange", "minimum": 0.01, "maximum": 32, "reset": 10, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Middle gray luma"), "key": "greyPoint", "field": "profileGammaGreyPoint", "minimum": 0.1, "maximum": 100, "reset": 18, "step": 0.1, "decimals": 1 },
                                { "title": qsTr("Black relative exposure"), "key": "shadowsRange", "field": "profileGammaShadowsRange", "minimum": -16, "maximum": 16, "reset": -5, "step": 0.05, "decimals": 2 }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                visible: root.hasPresenter
                                    && root.presenter.editProfileGamma.modeIndex === 0
                                title: modelData.title
                                from: modelData.minimum
                                to: modelData.maximum
                                stepSize: modelData.step
                                validatorDecimals: modelData.decimals
                                showReset: true
                                resetValue: modelData.reset
                                delayedCommit: true
                                enabled: root.hasSelection && root.hasPresenter
                                    && root.presenter.editProfileGamma.enabled
                                value: root.hasPresenter ? root.presenter.editProfileGamma[modelData.key] : modelData.reset
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Linear part"), "key": "linear", "field": "profileGammaLinear", "minimum": 0, "maximum": 1, "reset": 0.1, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Gamma exponent"), "key": "gamma", "field": "profileGammaGamma", "minimum": 0, "maximum": 1, "reset": 0.45, "step": 0.0001, "decimals": 4 }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                visible: root.hasPresenter
                                    && root.presenter.editProfileGamma.modeIndex === 1
                                title: modelData.title
                                from: modelData.minimum
                                to: modelData.maximum
                                stepSize: modelData.step
                                validatorDecimals: modelData.decimals
                                showReset: true
                                resetValue: modelData.reset
                                delayedCommit: true
                                enabled: root.hasSelection && root.hasPresenter
                                    && root.presenter.editProfileGamma.enabled
                                value: root.hasPresenter ? root.presenter.editProfileGamma[modelData.key] : modelData.reset
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Output & Soft Proof")
                    sectionId: "outputProfile"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"),
                                qsTr("Linear Rec2020"), qsTr("Rec709"),
                                qsTr("Linear ProPhoto RGB"), qsTr("PQ Rec2020"),
                                qsTr("HLG Rec2020"), qsTr("PQ P3"), qsTr("HLG P3"),
                                qsTr("Display P3")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editOutputColor.outputProfileIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("outputProfile", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Perceptual"), qsTr("Relative colorimetric"),
                                qsTr("Saturation"), qsTr("Absolute colorimetric")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editOutputColor.intentIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("outputRenderingIntent", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Proof off"), qsTr("Soft proof"), qsTr("Gamut warning")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editOutputColor.proofModeIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("proofMode", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"),
                                qsTr("Linear Rec2020"), qsTr("Rec709"),
                                qsTr("Linear ProPhoto RGB"), qsTr("PQ Rec2020"),
                                qsTr("HLG Rec2020"), qsTr("PQ P3"), qsTr("HLG P3"),
                                qsTr("Display P3")]
                            enabled: root.hasSelection && root.hasPresenter
                                && root.presenter.editOutputColor.proofModeIndex !== 0
                            currentIndex: root.hasPresenter ? root.presenter.editOutputColor.proofProfileIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("proofProfile", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Perceptual"), qsTr("Relative colorimetric"),
                                qsTr("Saturation"), qsTr("Absolute colorimetric")]
                            enabled: root.hasSelection && root.hasPresenter
                                && root.presenter.editOutputColor.proofModeIndex !== 0
                            currentIndex: root.hasPresenter ? root.presenter.editOutputColor.proofIntentIndex : 1
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("proofIntent", currentIndex)
                        }
                        CustomCheckBox {
                            text: qsTr("Black-point compensation")
                            enabled: root.hasSelection
                            checked: root.hasPresenter
                                && root.presenter.editOutputColor.blackPointCompensation
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("outputBlackPointCompensation",
                                    checked ? 1 : 0)
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: root.hasPresenter
                                ? qsTr("%1 · %2 · proof %3").arg(
                                    root.presenter.editOutputColor.outputProfile).arg(
                                    root.presenter.editOutputColor.proofMode).arg(
                                    root.presenter.editOutputColor.proofProfile)
                                : ""
                            wrapMode: Text.WordWrap
                            opacity: 0.75
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("White Balance")
                    sectionId: "whiteBalance"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("As shot"), qsTr("Camera reference"), qsTr("As shot → reference"), qsTr("Manual coefficients")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editWhiteBalance.modeIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("whiteBalanceMode", currentIndex)
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Automatic modes resolve camera metadata before demosaic. Manual values scale R, G1, B and G2/CYGM channel 4.")
                            wrapMode: Text.WordWrap
                            opacity: 0.75
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Red coefficient"), "key": "red", "field": "whiteBalanceRed" },
                                { "title": qsTr("Green coefficient"), "key": "green", "field": "whiteBalanceGreen" },
                                { "title": qsTr("Blue coefficient"), "key": "blue", "field": "whiteBalanceBlue" },
                                { "title": qsTr("Fourth coefficient"), "key": "fourth", "field": "whiteBalanceFourth" }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                visible: root.hasPresenter && (root.presenter.editWhiteBalance.modeIndex === 3
                                         || root.presenter.editWhiteBalance.hasCoefficients)
                                title: modelData.title
                                from: 0.000001
                                to: 8
                                stepSize: 0.01
                                validatorDecimals: 3
                                showReset: true
                                resetValue: 1
                                delayedCommit: true
                                enabled: root.hasSelection
                                value: root.hasPresenter ? root.presenter.editWhiteBalance[modelData.key] : 1
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Color Calibration")
                    sectionId: "calibration"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Output rows × input channels (linear sRGB, D50)")
                            wrapMode: Text.WordWrap
                            opacity: 0.75
                        }
                        MixerSlider {
                            title: qsTr("Red ← Red")
                            inputChannel: "red"
                            fieldName: "channelMixerRR"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerRR : 1
                            identityValue: 1
                        }
                        MixerSlider {
                            title: qsTr("Red ← Green")
                            inputChannel: "green"
                            fieldName: "channelMixerRG"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerRG : 0
                        }
                        MixerSlider {
                            title: qsTr("Red ← Blue")
                            inputChannel: "blue"
                            fieldName: "channelMixerRB"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerRB : 0
                        }
                        MixerSlider {
                            title: qsTr("Green ← Red")
                            inputChannel: "red"
                            fieldName: "channelMixerGR"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerGR : 0
                        }
                        MixerSlider {
                            title: qsTr("Green ← Green")
                            inputChannel: "green"
                            fieldName: "channelMixerGG"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerGG : 1
                            identityValue: 1
                        }
                        MixerSlider {
                            title: qsTr("Green ← Blue")
                            inputChannel: "blue"
                            fieldName: "channelMixerGB"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerGB : 0
                        }
                        MixerSlider {
                            title: qsTr("Blue ← Red")
                            inputChannel: "red"
                            fieldName: "channelMixerBR"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerBR : 0
                        }
                        MixerSlider {
                            title: qsTr("Blue ← Green")
                            inputChannel: "green"
                            fieldName: "channelMixerBG"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerBG : 0
                        }
                        MixerSlider {
                            title: qsTr("Blue ← Blue")
                            inputChannel: "blue"
                            fieldName: "channelMixerBB"
                            currentValue: root.hasPresenter ? root.presenter.editChannelMixerBB : 1
                            identityValue: 1
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("RGB Primaries")
                    sectionId: "primaries"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        Repeater {
                            model: [
                                { "title": qsTr("Achromatic tint hue"), "key": "achromaticTintHueDegrees", "field": "primariesAchromaticHueDegrees", "minimum": -180, "maximum": 180, "reset": 0, "step": 0.1, "decimals": 1 },
                                { "title": qsTr("Achromatic tint purity"), "key": "achromaticTintPurity", "field": "primariesAchromaticPurity", "minimum": 0, "maximum": 0.2, "reset": 0, "step": 0.002, "decimals": 3 },
                                { "title": qsTr("Red hue"), "key": "redHueDegrees", "field": "primariesRedHueDegrees", "minimum": -20, "maximum": 20, "reset": 0, "step": 0.1, "decimals": 1 },
                                { "title": qsTr("Red purity"), "key": "redPurity", "field": "primariesRedPurity", "minimum": 0.5, "maximum": 1.5, "reset": 1, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Green hue"), "key": "greenHueDegrees", "field": "primariesGreenHueDegrees", "minimum": -20, "maximum": 20, "reset": 0, "step": 0.1, "decimals": 1 },
                                { "title": qsTr("Green purity"), "key": "greenPurity", "field": "primariesGreenPurity", "minimum": 0.5, "maximum": 1.5, "reset": 1, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Blue hue"), "key": "blueHueDegrees", "field": "primariesBlueHueDegrees", "minimum": -20, "maximum": 20, "reset": 0, "step": 0.1, "decimals": 1 },
                                { "title": qsTr("Blue purity"), "key": "bluePurity", "field": "primariesBluePurity", "minimum": 0.5, "maximum": 1.5, "reset": 1, "step": 0.01, "decimals": 2 }
                            ]
                            delegate: PrimariesSlider {}
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Light")
                    sectionId: "light"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Exposure mode")
                            font.bold: true
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Manual"), qsTr("Deflicker")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter
                                ? root.presenter.editExposureParams.modeIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("exposureMode", currentIndex)
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Exposure black")
                            from: -0.1
                            to: 0.1
                            stepSize: 0.0001
                            validatorDecimals: 4
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editExposureParams.black : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("exposureBlack", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("exposureBlack", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("exposureBlack")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Exposure")
                            from: -3
                            to: 4
                            stepSize: 0.001
                            validatorDecimals: 3
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            visible: !root.hasPresenter
                                || root.presenter.editExposureParams.modeIndex === 0
                            enabled: root.hasSelection
                            value: root.hasPresenter
                                ? root.presenter.editExposureParams.exposureEv : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("exposure", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("exposure", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("exposure")
                        }
                        CustomCheckBox {
                            text: qsTr("Compensate exposure bias")
                            visible: !root.hasPresenter
                                || root.presenter.editExposureParams.modeIndex === 0
                            enabled: root.hasSelection
                            checked: root.hasPresenter
                                && root.presenter.editExposureParams.compensateExposureBias
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("exposureCompensateBias",
                                    checked ? 1 : 0)
                        }
                        CustomCheckBox {
                            text: qsTr("Compensate highlight preservation")
                            visible: !root.hasPresenter
                                || root.presenter.editExposureParams.modeIndex === 0
                            enabled: root.hasSelection
                            checked: root.hasPresenter
                                && root.presenter.editExposureParams.compensateHighlightPreservation
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("exposureCompensateHighlight",
                                    checked ? 1 : 0)
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Deflicker percentile")
                            from: 0
                            to: 100
                            stepSize: 0.1
                            validatorDecimals: 1
                            showReset: true
                            resetValue: 50
                            delayedCommit: true
                            visible: root.hasPresenter
                                && root.presenter.editExposureParams.modeIndex === 1
                            enabled: root.hasSelection
                            value: root.hasPresenter
                                ? root.presenter.editExposureParams.deflickerPercentile : 50
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(
                                        "exposureDeflickerPercentile", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(
                                        "exposureDeflickerPercentile", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("exposureDeflickerPercentile")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Deflicker target EV")
                            from: -18
                            to: 18
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: -4
                            delayedCommit: true
                            visible: root.hasPresenter
                                && root.presenter.editExposureParams.modeIndex === 1
                            enabled: root.hasSelection
                            value: root.hasPresenter
                                ? root.presenter.editExposureParams.deflickerTargetEv : -4
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("exposureDeflickerTarget", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("exposureDeflickerTarget", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("exposureDeflickerTarget")
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            text: qsTr("Sigmoid Display · Standard SDR")
                            font.bold: true
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Contrast")
                            from: 0.7
                            to: 3
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 1.5
                            delayedCommit: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSigmoidContrast : 1.5
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sigmoidContrast", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sigmoidContrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sigmoidContrast")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Skew")
                            from: -1
                            to: 1
                            stepSize: 0.02
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSigmoidSkew : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sigmoidSkew", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sigmoidSkew", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sigmoidSkew")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Preserve Hue")
                            from: 0
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            visible: root.hasPresenter && root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSigmoidHuePreservation : 1
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sigmoidHuePreservation", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sigmoidHuePreservation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sigmoidHuePreservation")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Contrast")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            visible: root.hasPresenter && !root.presenter.editSigmoidEnabled
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editContrast : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("contrast", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("contrast", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("contrast")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlights")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editHighlights : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("highlights", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("highlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("highlights")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Shadows")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editShadows : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("shadows", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("shadows", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("shadows")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Whites")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editWhites : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("whites", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("whites", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("whites")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Blacks")
                            from: -0.1
                            to: 0.1
                            stepSize: 0.001
                            validatorDecimals: 3
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editBlacks : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("blacks", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("blacks", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("blacks")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Gamma")
                            from: 0.2
                            to: 3
                            showReset: true
                            resetValue: 1
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGamma : 1
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("gamma", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("gamma", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("gamma")
                        }
                        CustomLabel {
                            text: qsTr("Tone Curve")
                            Layout.fillWidth: true
                        }
                        ToneCurveEditor {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 200
                            editorEnabled: root.hasSelection
                            points: root.hasPresenter ? root.presenter.editToneCurve : [
                                {
                                    "x": 0,
                                    "y": 0
                                },
                                {
                                    "x": 1,
                                    "y": 1
                                }
                            ]
                            samples: root.hasPresenter ? root.presenter.editToneCurveSamples : []
                            onCurveEdited: function (points) {
                                if (root.commands)
                                    root.commands.previewToneCurve(points);
                            }
                            onCurveCommitted: function (points) {
                                if (root.commands)
                                    root.commands.setToneCurve(points);
                            }
                        }
                        CustomLabel {
                            text: qsTr("Drag points to reshape. Click to add. Double-click an interior point to remove it.")
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            opacity: 0.75
                        }
                        CustomButton {
                            text: qsTr("Reset curve")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetControl("toneCurve")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Color")
                    sectionId: "color"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Vibrance")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVibrance : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("vibrance", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("vibrance", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("vibrance")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Saturation")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSaturation : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("saturation", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("saturation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("saturation")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Velvia")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVelvia : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("velvia", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("velvia", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("velvia")
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Color look-up table · D50 Lab")
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        CustomCheckBox {
                            text: qsTr("Enable color look-up table")
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editColorChecker.enabled
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("colorCheckerEnabled", checked ? 1 : 0)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [
                                qsTr("IT8 skin tones"),
                                qsTr("Expanded color checker"),
                                qsTr("Helmholtz/Kohlrausch monochrome"),
                                qsTr("Fuji Astia emulation"),
                                qsTr("Fuji Classic Chrome emulation"),
                                qsTr("Fuji Monochrome emulation"),
                                qsTr("Fuji Provia emulation"),
                                qsTr("Fuji Velvia emulation")
                            ]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter
                                          ? root.presenter.editColorChecker.presetIndex : -1
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("colorCheckerPreset", currentIndex)
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: {
                                const labels = [];
                                const count = root.hasPresenter
                                              ? root.presenter.editColorChecker.patchCount : 0;
                                for (let index = 0; index < count; ++index)
                                    labels.push(qsTr("Patch %1").arg(index + 1));
                                return labels;
                            }
                            enabled: root.hasSelection && root.hasPresenter
                                     && root.presenter.editColorChecker.patchCount > 0
                            currentIndex: root.hasPresenter
                                          ? root.presenter.editColorChecker.patchIndex : -1
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("colorCheckerPatch", currentIndex)
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Source · L*"), "key": "sourceL", "field": "colorCheckerSourceL" },
                                { "title": qsTr("Source · a*"), "key": "sourceA", "field": "colorCheckerSourceA" },
                                { "title": qsTr("Source · b*"), "key": "sourceB", "field": "colorCheckerSourceB" },
                                { "title": qsTr("Target · L*"), "key": "targetL", "field": "colorCheckerTargetL" },
                                { "title": qsTr("Target · a*"), "key": "targetA", "field": "colorCheckerTargetA" },
                                { "title": qsTr("Target · b*"), "key": "targetB", "field": "colorCheckerTargetB" }
                            ]
                            delegate: ColorCheckerNumberField {}
                        }
                        CustomButton {
                            text: qsTr("Disable and reset color look-up table")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetControl("colorChecker")
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Color Balance · legacy Lab / ProPhoto RGB")
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: root.hasPresenter && root.presenter.editLegacyColorBalance.enabled
                                  ? qsTr("Enabled")
                                  : qsTr("Inactive until edited")
                            opacity: 0.75
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("Lift / Gamma / Gain"), qsTr("Slope / Offset / Power")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter
                                          ? root.presenter.editLegacyColorBalance.modeIndex : 1
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("legacyColorBalanceMode", currentIndex)
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Lift · Factor"), "key": "liftFactor", "field": "legacyColorBalanceLiftFactor", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Lift · Red"), "key": "liftRed", "field": "legacyColorBalanceLiftRed", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Lift · Green"), "key": "liftGreen", "field": "legacyColorBalanceLiftGreen", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Lift · Blue"), "key": "liftBlue", "field": "legacyColorBalanceLiftBlue", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Gamma · Factor"), "key": "gammaFactor", "field": "legacyColorBalanceGammaFactor", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Gamma · Red"), "key": "gammaRed", "field": "legacyColorBalanceGammaRed", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Gamma · Green"), "key": "gammaGreen", "field": "legacyColorBalanceGammaGreen", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Gamma · Blue"), "key": "gammaBlue", "field": "legacyColorBalanceGammaBlue", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Gain · Factor"), "key": "gainFactor", "field": "legacyColorBalanceGainFactor", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Gain · Red"), "key": "gainRed", "field": "legacyColorBalanceGainRed", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Gain · Green"), "key": "gainGreen", "field": "legacyColorBalanceGainGreen", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Gain · Blue"), "key": "gainBlue", "field": "legacyColorBalanceGainBlue", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.00001, "decimals": 5 },
                                { "title": qsTr("Input saturation"), "key": "inputSaturation", "field": "legacyColorBalanceInputSaturation", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Contrast"), "key": "contrast", "field": "legacyColorBalanceContrast", "minimum": 0.01, "maximum": 1.99, "reset": 1, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Contrast fulcrum (%)"), "key": "greyFulcrum", "field": "legacyColorBalanceGreyFulcrum", "minimum": 0.1, "maximum": 100, "reset": 18, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Output saturation"), "key": "outputSaturation", "field": "legacyColorBalanceOutputSaturation", "minimum": 0, "maximum": 2, "reset": 1, "step": 0.0001, "decimals": 4 }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                title: modelData.title
                                from: modelData.minimum
                                to: modelData.maximum
                                stepSize: modelData.step
                                validatorDecimals: modelData.decimals
                                showReset: true
                                resetValue: modelData.reset
                                delayedCommit: true
                                enabled: root.hasSelection
                                value: root.hasPresenter
                                       ? root.presenter.editLegacyColorBalance[modelData.key]
                                       : modelData.reset
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                        CustomButton {
                            text: qsTr("Disable and reset legacy Color Balance")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetControl("legacyColorBalance")
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Color Balance RGB · linear sRGB D50 / Filmlight Yrg")
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: [qsTr("darktable UCS (2022)"), qsTr("JzAzBz (2021)")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editColorBalanceRgb.formulaIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("colorBalanceFormula", currentIndex)
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Global · Luminance"), "key": "globalY", "field": "colorBalanceGlobalY", "minimum": -0.05, "maximum": 0.05, "reset": 0, "step": 0.001, "decimals": 3 },
                                { "title": qsTr("Global · Chroma"), "key": "globalChroma", "field": "colorBalanceGlobalChroma", "minimum": 0, "maximum": 0.01, "reset": 0, "step": 0.0001, "decimals": 4 },
                                { "title": qsTr("Global · Hue"), "key": "globalHue", "field": "colorBalanceGlobalHue", "minimum": 0, "maximum": 360, "reset": 0, "step": 1, "decimals": 1 },
                                { "title": qsTr("Shadows · Luminance"), "key": "shadowsY", "field": "colorBalanceShadowsY", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Shadows · Chroma"), "key": "shadowsChroma", "field": "colorBalanceShadowsChroma", "minimum": 0, "maximum": 0.5, "reset": 0, "step": 0.005, "decimals": 3 },
                                { "title": qsTr("Shadows · Hue"), "key": "shadowsHue", "field": "colorBalanceShadowsHue", "minimum": 0, "maximum": 360, "reset": 0, "step": 1, "decimals": 1 },
                                { "title": qsTr("Midtones · Luminance"), "key": "midtonesY", "field": "colorBalanceMidtonesY", "minimum": -0.25, "maximum": 0.25, "reset": 0, "step": 0.005, "decimals": 3 },
                                { "title": qsTr("Midtones · Chroma"), "key": "midtonesChroma", "field": "colorBalanceMidtonesChroma", "minimum": 0, "maximum": 0.1, "reset": 0, "step": 0.001, "decimals": 3 },
                                { "title": qsTr("Midtones · Hue"), "key": "midtonesHue", "field": "colorBalanceMidtonesHue", "minimum": 0, "maximum": 360, "reset": 0, "step": 1, "decimals": 1 },
                                { "title": qsTr("Highlights · Luminance"), "key": "highlightsY", "field": "colorBalanceHighlightsY", "minimum": -0.5, "maximum": 0.5, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Highlights · Chroma"), "key": "highlightsChroma", "field": "colorBalanceHighlightsChroma", "minimum": 0, "maximum": 0.2, "reset": 0, "step": 0.002, "decimals": 3 },
                                { "title": qsTr("Highlights · Hue"), "key": "highlightsHue", "field": "colorBalanceHighlightsHue", "minimum": 0, "maximum": 360, "reset": 0, "step": 1, "decimals": 1 },
                                { "title": qsTr("Shadows fall-off"), "key": "shadowsFalloff", "field": "colorBalanceShadowsFalloff", "minimum": 0, "maximum": 3, "reset": 1, "step": 0.05, "decimals": 2 },
                                { "title": qsTr("Highlights fall-off"), "key": "highlightsFalloff", "field": "colorBalanceHighlightsFalloff", "minimum": 0, "maximum": 3, "reset": 1, "step": 0.05, "decimals": 2 },
                                { "title": qsTr("Mask grey fulcrum"), "key": "maskGreyFulcrum", "field": "colorBalanceMaskGreyFulcrum", "minimum": 0.000001, "maximum": 1, "reset": 0.1845, "step": 0.001, "decimals": 4 },
                                { "title": qsTr("White fulcrum · EV"), "key": "whiteFulcrumEv", "field": "colorBalanceWhiteFulcrumEv", "minimum": -2, "maximum": 2, "reset": 0, "step": 0.05, "decimals": 2 },
                                { "title": qsTr("Grey fulcrum"), "key": "greyFulcrum", "field": "colorBalanceGreyFulcrum", "minimum": 0.1, "maximum": 0.5, "reset": 0.1845, "step": 0.001, "decimals": 4 },
                                { "title": qsTr("Chroma · Global"), "key": "chromaGlobal", "field": "colorBalanceChromaGlobal", "minimum": -0.5, "maximum": 0.5, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Chroma · Shadows"), "key": "chromaShadows", "field": "colorBalanceChromaShadows", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Chroma · Midtones"), "key": "chromaMidtones", "field": "colorBalanceChromaMidtones", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Chroma · Highlights"), "key": "chromaHighlights", "field": "colorBalanceChromaHighlights", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Saturation · Global"), "key": "saturationGlobal", "field": "colorBalanceSaturationGlobal", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Saturation · Shadows"), "key": "saturationShadows", "field": "colorBalanceSaturationShadows", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Saturation · Midtones"), "key": "saturationMidtones", "field": "colorBalanceSaturationMidtones", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Saturation · Highlights"), "key": "saturationHighlights", "field": "colorBalanceSaturationHighlights", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Brilliance · Global"), "key": "brillianceGlobal", "field": "colorBalanceBrillianceGlobal", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Brilliance · Shadows"), "key": "brillianceShadows", "field": "colorBalanceBrillianceShadows", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Brilliance · Midtones"), "key": "brillianceMidtones", "field": "colorBalanceBrillianceMidtones", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Brilliance · Highlights"), "key": "brillianceHighlights", "field": "colorBalanceBrillianceHighlights", "minimum": -1, "maximum": 1, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Vibrance"), "key": "vibrance", "field": "colorBalanceVibrance", "minimum": -0.5, "maximum": 0.5, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Hue rotation"), "key": "hueRotation", "field": "colorBalanceHueRotation", "minimum": -180, "maximum": 180, "reset": 0, "step": 1, "decimals": 1 },
                                { "title": qsTr("Contrast"), "key": "contrast", "field": "colorBalanceContrast", "minimum": -0.5, "maximum": 0.5, "reset": 0, "step": 0.01, "decimals": 2 }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                title: modelData.title
                                from: modelData.minimum
                                to: modelData.maximum
                                stepSize: modelData.step
                                validatorDecimals: modelData.decimals
                                showReset: true
                                resetValue: modelData.reset
                                delayedCommit: true
                                enabled: root.hasSelection
                                value: root.hasPresenter ? root.presenter.editColorBalanceRgb[modelData.key] : modelData.reset
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Color Correction · D50 Lab")
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        CustomCheckBox {
                            objectName: "colorCorrectionEnabled"
                            text: qsTr("Enable Color Correction")
                            enabled: root.hasSelection
                            checked: root.hasPresenter
                                     && root.presenter.editColorCorrection.enabled
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("colorCorrectionEnabled", checked ? 1 : 0)
                        }
                        Repeater {
                            model: [
                                { "title": qsTr("Highlights · a*"), "key": "highlightA", "field": "colorCorrectionHighlightA", "minimum": -40, "maximum": 40, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Highlights · b*"), "key": "highlightB", "field": "colorCorrectionHighlightB", "minimum": -40, "maximum": 40, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Shadows · a*"), "key": "shadowA", "field": "colorCorrectionShadowA", "minimum": -40, "maximum": 40, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Shadows · b*"), "key": "shadowB", "field": "colorCorrectionShadowB", "minimum": -40, "maximum": 40, "reset": 0, "step": 0.01, "decimals": 2 },
                                { "title": qsTr("Saturation"), "key": "saturation", "field": "colorCorrectionSaturation", "minimum": -3, "maximum": 3, "reset": 1, "step": 0.01, "decimals": 2 }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                title: modelData.title
                                from: modelData.minimum
                                to: modelData.maximum
                                stepSize: modelData.step
                                validatorDecimals: modelData.decimals
                                showReset: true
                                resetValue: modelData.reset
                                delayedCommit: true
                                enabled: root.hasSelection
                                value: root.hasPresenter
                                       ? root.presenter.editColorCorrection[modelData.key]
                                       : modelData.reset
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                        CustomButton {
                            text: qsTr("Disable and reset Color Correction")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetControl("colorCorrection")
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            text: qsTr("Color contrast")
                            font.bold: true
                            wrapMode: Text.WordWrap
                        }
                        CustomCheckBox {
                            objectName: "colorContrastEnabled"
                            text: qsTr("Enable Color contrast")
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editColorContrast.enabled
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("colorContrastEnabled", checked ? 1 : 0)
                        }
                        Repeater {
                            model: [
                                { "title": "a* ×", "key": "aSteepness", "field": "colorContrastASteepness", "minimum": 0, "maximum": 5, "reset": 1 },
                                { "title": "b* ×", "key": "bSteepness", "field": "colorContrastBSteepness", "minimum": 0, "maximum": 5, "reset": 1 }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                title: modelData.title
                                from: modelData.minimum
                                to: modelData.maximum
                                stepSize: 0.01
                                validatorDecimals: 3
                                showReset: true
                                resetValue: modelData.reset
                                delayedCommit: true
                                enabled: root.hasSelection
                                value: root.hasPresenter
                                       ? root.presenter.editColorContrast[modelData.key]
                                       : modelData.reset
                                onValueChanged: if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value)
                                onValueCommitted: function (value) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, value);
                                }
                                onResetRequested: if (root.commands)
                                    root.commands.resetControl(modelData.field)
                            }
                        }
                        Repeater {
                            model: [
                                { "title": "a* +", "key": "aOffset", "field": "colorContrastAOffset" },
                                { "title": "b* +", "key": "bOffset", "field": "colorContrastBOffset" }
                            ]
                            delegate: ColorContrastOffsetField {}
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            CustomCheckBox {
                                Layout.fillWidth: true
                                text: qsTr("Allow extended chroma")
                                enabled: root.hasSelection
                                checked: root.hasPresenter && root.presenter.editColorContrast.unbound
                                onToggled: if (root.liveReady && root.commands)
                                    root.commands.setDevelopNumber("colorContrastUnbound", checked ? 1 : 0)
                            }
                            CustomButton {
                                text: qsTr("Reset")
                                enabled: root.hasSelection
                                onClicked: if (root.commands)
                                    root.commands.resetControl("colorContrastUnbound")
                            }
                        }
                        CustomButton {
                            text: qsTr("Disable and reset Color contrast")
                            enabled: root.hasSelection
                            onClicked: if (root.commands)
                                root.commands.resetControl("colorContrast")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Monochrome")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editMonochrome : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("monochrome", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("monochrome", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("monochrome")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Split amount")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitAmount : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitAmount", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitAmount", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitAmount")
                        }
                        HueSlider {
                            Layout.fillWidth: true
                            title: qsTr("Shadow hue")
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitShadowsHue : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitShadowsHue", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitShadowsHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitShadowsHue")
                        }
                        HueSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlight hue")
                            showReset: true
                            resetValue: 0.2
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitHighlightsHue : 0.2
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitHighlightsHue", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitHighlightsHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitHighlightsHue")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Split balance")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0.5
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSplitBalance : 0.5
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("splitBalance", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("splitBalance", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("splitBalance")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Detail")
                    sectionId: "detail"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Sharpen")
                            from: 0
                            to: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSharpen : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sharpen", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sharpen", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sharpen")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Radius")
                            from: 0
                            to: 4.8
                            stepSize: 0.1
                            validatorDecimals: 1
                            showReset: true
                            resetValue: 2
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSharpenRadius : 2
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("sharpenRadius", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("sharpenRadius", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("sharpenRadius")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Clarity")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editClarity : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("clarity", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("clarity", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("clarity")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Grain")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGrain : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("grain", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("grain", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("grain")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Effects")
                    sectionId: "effects"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Vignette")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editVignette : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("vignette", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("vignette", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("vignette")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Bloom")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editBloom : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("bloom", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("bloom", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("bloom")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Soften")
                            from: 0
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editSoften : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("soften", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("soften", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("soften")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Dehaze")
                            from: -1
                            to: 1
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editDehaze : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("dehaze", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("dehaze", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("dehaze")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("RAW Repair / Denoise / Lens")
                    sectionId: "raw"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Hot pixels")
                            from: 0
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editHotPixelsStrength : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("hotPixelsStrength", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("hotPixelsStrength", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("hotPixelsStrength")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Hot pixel threshold")
                            from: 0
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0.05
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editHotPixelsThreshold : 0.05
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("hotPixelsThreshold", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("hotPixelsThreshold", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("hotPixelsThreshold")
                        }
                        CustomCheckBox {
                            text: qsTr("Permissive (3 neighbours)")
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editHotPixelsPermissive
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("hotPixelsPermissive", checked ? 1 : 0)
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("RAW chromatic aberration")
                            from: 0
                            to: 5
                            stepSize: 1
                            validatorDecimals: 0
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editRawCaIterations : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("rawCaIterations", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("rawCaIterations", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("rawCaIterations")
                        }
                        CustomCheckBox {
                            text: qsTr("Avoid CA color shift")
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editRawCaAvoidShift
                            onToggled: if (root.liveReady && root.commands)
                                root.commands.setDevelopNumber("rawCaAvoidShift", checked ? 1 : 0)
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlight reconstruction")
                            from: 0
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editRawHighlights : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("rawHighlights", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("rawHighlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("rawHighlights")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Denoise")
                            from: 0
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editDenoise : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("denoise", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("denoise", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("denoise")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Lens distortion")
                            from: -1
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editLensK1 : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("lensK1", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("lensK1", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("lensK1")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Lens vignetting")
                            from: 0
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editLensVignetting : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("lensVignetting", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("lensVignetting", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("lensVignetting")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Tone equalizer")
                    sectionId: "toneEqual"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Blacks")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqBlacks : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqBlacks", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqBlacks", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqBlacks")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Shadows")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqShadows : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqShadows", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqShadows", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqShadows")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Midtones")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqMidtones : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqMidtones", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqMidtones", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqMidtones")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Highlights")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqHighlights : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqHighlights", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqHighlights", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqHighlights")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Whites")
                            from: -2
                            to: 2
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editToneEqWhites : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("toneEqWhites", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("toneEqWhites", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("toneEqWhites")
                        }
                    }
                }

                DevelopSection {
                    title: qsTr("Graduated ND / Color EQ")
                    sectionId: "graduated"
                    ColumnLayout {
                        Layout.fillWidth: true
                        width: parent.width
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Graduated density")
                            from: -4
                            to: 4
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGraduatedDensity : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("graduatedDensity", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("graduatedDensity", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("graduatedDensity")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Graduated rotation")
                            from: -180
                            to: 180
                            stepSize: 1
                            validatorDecimals: 0
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editGraduatedRotation : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("graduatedRotation", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("graduatedRotation", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("graduatedRotation")
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            model: ["0", "1", "2", "3", "4", "5", "6", "7"]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editColorEqBand : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("colorEqBand", Number(currentText))
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Band saturation")
                            from: -1
                            to: 1
                            stepSize: 0.05
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorEqSat : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("colorEqSat", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorEqSat", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorEqSat")
                        }
                        CustomSlider {
                            Layout.fillWidth: true
                            title: qsTr("Band hue")
                            from: -0.5
                            to: 0.5
                            stepSize: 0.005
                            validatorDecimals: 3
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorEqHue : 0
                            onValueChanged: if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber("colorEqHue", value)
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorEqHue", value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl("colorEqHue")
                        }
                    }
                }
            }
}
