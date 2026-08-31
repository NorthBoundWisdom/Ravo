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

    component DevelopSection: CustomEditPanel {
        id: sectionPanel
        showAddButton: false
        showDeleteButton: false
        showApplyButton: false
        actionsNeedEditing: false
        actionButtonsEnabled: root.hasSelection
        editing: false
        effectIndicator: true
        effectActiveTooltip: qsTr("Click to bypass this panel")
        effectBypassedTooltip: qsTr("Click to enable this panel")
        resetTooltip: qsTr("Reset this section")
        property string sectionId
        function syncEffectLamp() {
            modified = root.hasPresenter && sectionId.length && root.presenter.sectionModified(sectionId);
            effectEnabled = !root.hasPresenter || !sectionId.length || root.presenter.sectionEffectEnabled(sectionId);
        }
        Connections {
            target: root.presenter
            function onEditChanged() {
                sectionPanel.syncEffectLamp();
            }
            function onSelectionChanged() {
                sectionPanel.syncEffectLamp();
            }
        }
        Component.onCompleted: syncEffectLamp()
        onSectionIdChanged: syncEffectLamp()
        onReset: function () {
            if (root.commands && sectionId.length)
                root.commands.resetSection(sectionId);
        }
        onEffectEnabledToggled: function (enabled) {
            if (root.commands && sectionId.length)
                root.commands.setSectionEnabled(sectionId, enabled);
        }
    }

    component CurveOptionButton: CustomButton {
        id: optionButton
        property bool selected: false
        property color selectionColor: Theme.highlightColor

        defaultHeight: Fonts.inputFieldHeight
        defaultPadding: Fonts.size6
        buttonTextColor: selected ? selectionColor : Theme.buttonTextColor
        highlightedTextColor: selectionColor
        font: selected ? Fonts.makeBoldFont(Fonts.standardFont) : Fonts.standardFont

        background: Rectangle {
            color: !optionButton.enabled ? Theme.buttonDisabledColor : optionButton.pressed ? Qt.alpha(optionButton.selectionColor, 0.32) : optionButton.hovered ? Qt.alpha(optionButton.selectionColor, 0.24) : optionButton.selected ? Qt.alpha(optionButton.selectionColor, 0.16) : Theme.baseColor
            border.color: optionButton.selected ? optionButton.selectionColor : Theme.midColor
            border.width: optionButton.selected ? Fonts.size2 : Fonts.size1
            radius: Fonts.size2
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
        onValueEdited: function (value) {
            if (root.liveReady && root.commands)
                root.commands.previewDevelopNumber(fieldName, value);
        }
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
        onValueEdited: function (value) {
            if (root.liveReady && root.commands)
                root.commands.previewDevelopNumber(modelData.field, value);
        }
        onValueCommitted: function (value) {
            if (root.commands)
                root.commands.setDevelopNumber(modelData.field, value);
        }
        onResetRequested: if (root.commands)
            root.commands.resetControl(modelData.field)
    }

    component MaskEditor: ColumnLayout {
        id: maskEditor
        required property var mask
        Layout.fillWidth: true
        spacing: Fonts.smallSpacing

        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Mask")
            font.bold: true
            wrapMode: Text.WordWrap
        }
        CustomLabel {
            Layout.fillWidth: true
            text: maskEditor.mask.status !== undefined ? maskEditor.mask.status : ""
            wrapMode: Text.WordWrap
        }
        CustomLabel {
            Layout.fillWidth: true
            text: qsTr("Mask kind")
        }
        CustomComboBox {
            objectName: "maskKind"
            Layout.fillWidth: true
            model: maskEditor.mask.kindChoices !== undefined ? maskEditor.mask.kindChoices : []
            currentIndex: maskEditor.mask.kindIndex !== undefined ? maskEditor.mask.kindIndex : 0
            visible: currentIndex >= 0
            enabled: root.hasSelection && (maskEditor.mask.editable || !maskEditor.mask.attached)
            onActivated: if (root.commands)
                root.commands.setDevelopNumber(maskEditor.mask.kindField, currentIndex)
        }
        CustomLabel {
            Layout.fillWidth: true
            visible: maskEditor.mask.attached === true && maskEditor.mask.kindIndex < 0
            text: maskEditor.mask.kindLabel !== undefined ? maskEditor.mask.kindLabel : ""
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: maskEditor.mask.attached === true
            CustomCheckBox {
                Layout.fillWidth: true
                text: qsTr("Invert mask")
                enabled: root.hasSelection && maskEditor.mask.editable === true
                checked: maskEditor.mask.inverted === true
                onToggled: if (root.liveReady && root.commands && maskEditor.mask.editable === true)
                    root.commands.setDevelopNumber(maskEditor.mask.invertedField, checked ? 1 : 0)
            }
            CustomButton {
                text: qsTr("Reset")
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onClicked: if (root.commands)
                    root.commands.resetControl(maskEditor.mask.invertedField)
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: maskEditor.mask.selectorsVisible === true
            spacing: Fonts.smallSpacing

            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Source")
            }
            CustomComboBox {
                Layout.fillWidth: true
                model: maskEditor.mask.sourceChoices !== undefined ? maskEditor.mask.sourceChoices : []
                currentIndex: maskEditor.mask.sourceIndex !== undefined ? maskEditor.mask.sourceIndex : 0
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onActivated: if (root.commands)
                    root.commands.setDevelopNumber(maskEditor.mask.sourceField, currentIndex)
            }
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Channel")
            }
            CustomComboBox {
                Layout.fillWidth: true
                model: maskEditor.mask.channelChoices !== undefined ? maskEditor.mask.channelChoices : []
                currentIndex: maskEditor.mask.channelIndex !== undefined ? maskEditor.mask.channelIndex : 0
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onActivated: if (root.commands)
                    root.commands.setDevelopNumber(maskEditor.mask.channelField, currentIndex)
            }
        }
        Repeater {
            model: maskEditor.mask.numericControls !== undefined ? maskEditor.mask.numericControls : []
            delegate: CustomSlider {
                required property var modelData
                Layout.fillWidth: true
                title: modelData.title
                from: modelData.min
                to: modelData.max
                stepSize: modelData.step
                validatorDecimals: modelData.decimals
                showReset: true
                resetValue: modelData.reset
                delayedCommit: true
                visible: modelData.visible
                enabled: root.hasSelection && maskEditor.mask.editable === true && modelData.visible
                value: maskEditor.mask[modelData.key] !== undefined ? maskEditor.mask[modelData.key] : modelData.reset
                onValueEdited: function (value) {
                    if (root.liveReady && root.commands && maskEditor.mask.editable === true && modelData.visible)
                        root.commands.previewDevelopNumber(modelData.field, value);
                }
                onValueCommitted: function (value) {
                    if (root.commands)
                        root.commands.setDevelopNumber(modelData.field, value);
                }
                onResetRequested: if (root.commands)
                    root.commands.resetControl(modelData.field)
            }
        }
        CustomCheckBox {
            Layout.fillWidth: true
            visible: maskEditor.mask.attached === true
            text: qsTr("Show mask overlay")
            enabled: root.hasSelection
            checked: root.hasPresenter && root.presenter.maskOverlayVisible && root.presenter.maskOverlayTarget === maskEditor.mask.target
            onToggled: if (root.hasPresenter)
                root.presenter.setMaskOverlay(maskEditor.mask.target, checked)
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: maskEditor.mask.groupVisible === true
            spacing: Fonts.smallSpacing
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Group child")
            }
            CustomComboBox {
                Layout.fillWidth: true
                model: {
                    const count = maskEditor.mask.childCount !== undefined ? maskEditor.mask.childCount : 0;
                    const items = [];
                    for (let i = 0; i < count; ++i)
                        items.push(qsTr("Child %1").arg(i + 1));
                    return items;
                }
                currentIndex: maskEditor.mask.childIndex !== undefined ? maskEditor.mask.childIndex : 0
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onActivated: if (root.commands)
                    root.commands.setDevelopNumber(maskEditor.mask.childIndexField, currentIndex)
            }
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Child kind")
            }
            CustomComboBox {
                Layout.fillWidth: true
                model: maskEditor.mask.childKindChoices !== undefined ? maskEditor.mask.childKindChoices : []
                currentIndex: maskEditor.mask.childKindIndex !== undefined ? maskEditor.mask.childKindIndex : 0
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onActivated: if (root.commands && maskEditor.mask.childKindValues)
                    root.commands.setDevelopNumber(maskEditor.mask.childKindField, maskEditor.mask.childKindValues[currentIndex])
            }
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Combine")
            }
            CustomComboBox {
                Layout.fillWidth: true
                model: maskEditor.mask.operatorChoices !== undefined ? maskEditor.mask.operatorChoices : []
                currentIndex: maskEditor.mask.childOperatorIndex !== undefined ? maskEditor.mask.childOperatorIndex : 0
                enabled: root.hasSelection && maskEditor.mask.editable === true && maskEditor.mask.childIndex > 0
                onActivated: if (root.commands)
                    root.commands.setDevelopNumber(maskEditor.mask.childOperatorField, currentIndex)
            }
            CustomCheckBox {
                Layout.fillWidth: true
                text: qsTr("Invert child")
                enabled: root.hasSelection && maskEditor.mask.editable === true
                checked: maskEditor.mask.childInverted === true
                onToggled: if (root.commands)
                    root.commands.setDevelopNumber(maskEditor.mask.childInvertedField, checked ? 1 : 0)
            }
            RowLayout {
                Layout.fillWidth: true
                CustomButton {
                    text: qsTr("Add circle")
                    enabled: root.hasSelection && maskEditor.mask.editable === true
                    onClicked: if (root.commands)
                        root.commands.setDevelopNumber(maskEditor.mask.addChildField, 3)
                }
                CustomButton {
                    text: qsTr("Remove child")
                    enabled: root.hasSelection && maskEditor.mask.editable === true && maskEditor.mask.childCount > 1
                    onClicked: if (root.commands)
                        root.commands.setDevelopNumber(maskEditor.mask.removeChildField, 1)
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: maskEditor.mask.pointsVisible === true
            spacing: Fonts.smallSpacing
            CustomLabel {
                Layout.fillWidth: true
                text: qsTr("Point")
            }
            CustomComboBox {
                Layout.fillWidth: true
                model: {
                    const count = maskEditor.mask.pointCount !== undefined ? maskEditor.mask.pointCount : 0;
                    const items = [];
                    for (let i = 0; i < count; ++i)
                        items.push(qsTr("Point %1").arg(i + 1));
                    return items;
                }
                currentIndex: maskEditor.mask.pointIndex !== undefined ? maskEditor.mask.pointIndex : 0
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onActivated: if (root.commands)
                    root.commands.setDevelopNumber(maskEditor.mask.pointIndexField, currentIndex)
            }
            RowLayout {
                Layout.fillWidth: true
                CustomButton {
                    text: qsTr("Add point")
                    enabled: root.hasSelection && maskEditor.mask.editable === true
                    onClicked: if (root.commands)
                        root.commands.setDevelopNumber(maskEditor.mask.addPointField, 1)
                }
                CustomButton {
                    text: qsTr("Remove point")
                    enabled: root.hasSelection && maskEditor.mask.editable === true && maskEditor.mask.pointCount > 2
                    onClicked: if (root.commands)
                        root.commands.setDevelopNumber(maskEditor.mask.removePointField, 1)
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: maskEditor.mask.attached === true
            CustomButton {
                text: qsTr("Reset to all")
                enabled: root.hasSelection && maskEditor.mask.editable === true
                onClicked: if (root.commands)
                    root.commands.resetControl(maskEditor.mask.kindField)
            }
            CustomButton {
                text: qsTr("Detach mask")
                enabled: root.hasSelection && maskEditor.mask.canDetach === true
                onClicked: if (root.commands)
                    root.commands.resetControl(maskEditor.mask.detachField)
            }
        }
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
            enabled: root.hasSelection && root.hasPresenter && root.presenter.editColorChecker.patchCount > 0
            validator: DoubleValidator {
                bottom: -3.402823466e38
                top: 3.402823466e38
                decimals: 9
                notation: DoubleValidator.ScientificNotation
            }
            text: root.hasPresenter ? Number(root.presenter.editColorChecker[modelData.key]).toString() : "0"
            onEditingCommitted: function (committedText) {
                const parsed = Number(committedText);
                if (Number.isFinite(parsed) && root.commands)
                    root.commands.setDevelopNumber(modelData.field, parsed);
            }
        }
        CustomButton {
            text: qsTr("Reset")
            enabled: root.hasSelection && root.hasPresenter && root.presenter.editColorChecker.patchCount > 0
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
            text: root.hasPresenter ? Number(root.presenter.editColorContrast[modelData.key]).toString() : "0"
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

        DevelopSection {
            title: qsTr("Light")
            sectionId: "light"
            ColumnLayout {
                Layout.fillWidth: true
                width: parent.width
                DevelopSection {
                    title: qsTr("White Balance")
                    sectionId: "whiteBalance"
                    collapsible: false
                    animateHeight: false
                    borderWidth: 0
                    borderRadius: 0
                    padding: 0
                    panelColor: "transparent"
                    titleBarColor: "transparent"
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
                        CustomCheckBox {
                            objectName: "whiteBalancePickActive"
                            text: qsTr("Pick white on photo")
                            enabled: root.hasSelection && root.hasPresenter && root.presenter.editWhiteBalance.canPick
                            checked: root.hasPresenter && root.presenter.whiteBalancePickActive
                            onToggled: if (root.commands)
                                root.commands.setWhiteBalancePickActive(checked)
                        }
                        CustomLabel {
                            Layout.fillWidth: true
                            visible: root.hasPresenter && root.presenter.whiteBalancePickActive
                            text: qsTr("Click a neutral patch in the photo. RAW only; Perspective and Canvas must be off.")
                            wrapMode: Text.WordWrap
                            opacity: 0.75
                        }
                        Repeater {
                            model: [
                                {
                                    "title": qsTr("Red coefficient"),
                                    "key": "red",
                                    "field": "whiteBalanceRed"
                                },
                                {
                                    "title": qsTr("Green coefficient"),
                                    "key": "green",
                                    "field": "whiteBalanceGreen"
                                },
                                {
                                    "title": qsTr("Blue coefficient"),
                                    "key": "blue",
                                    "field": "whiteBalanceBlue"
                                },
                                {
                                    "title": qsTr("Fourth coefficient"),
                                    "key": "fourth",
                                    "field": "whiteBalanceFourth"
                                }
                            ]
                            delegate: CustomSlider {
                                required property var modelData
                                Layout.fillWidth: true
                                visible: root.hasPresenter && (root.presenter.editWhiteBalance.modeIndex === 3 || root.presenter.editWhiteBalance.hasCoefficients)
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
                                onValueEdited: function (value) {
                                    if (root.liveReady && root.commands)
                                        root.commands.previewDevelopNumber(modelData.field, value);
                                }
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
                    visible: !root.hasPresenter || root.presenter.editExposureParams.modeIndex === 0
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editExposureParams.exposureEv : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("exposure", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("exposure", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("exposure")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("sigmoidContrast", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("sigmoidContrast", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("sigmoidContrast")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("contrast", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("highlights", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("shadows", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("whites", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("blacks", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("blacks", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("blacks")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Exposure mode")
                    font.bold: true
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Manual"), qsTr("Deflicker")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editExposureParams.modeIndex : 0
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("exposureBlack", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("exposureBlack", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("exposureBlack")
                }
                CustomCheckBox {
                    text: qsTr("Compensate exposure bias")
                    visible: !root.hasPresenter || root.presenter.editExposureParams.modeIndex === 0
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editExposureParams.compensateExposureBias
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("exposureCompensateBias", checked ? 1 : 0)
                }
                CustomCheckBox {
                    text: qsTr("Compensate highlight preservation")
                    visible: !root.hasPresenter || root.presenter.editExposureParams.modeIndex === 0
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editExposureParams.compensateHighlightPreservation
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("exposureCompensateHighlight", checked ? 1 : 0)
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
                    visible: root.hasPresenter && root.presenter.editExposureParams.modeIndex === 1
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editExposureParams.deflickerPercentile : 50
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("exposureDeflickerPercentile", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("exposureDeflickerPercentile", value);
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
                    visible: root.hasPresenter && root.presenter.editExposureParams.modeIndex === 1
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editExposureParams.deflickerTargetEv : -4
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("exposureDeflickerTarget", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("sigmoidSkew", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("sigmoidHuePreservation", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("sigmoidHuePreservation", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("sigmoidHuePreservation")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("gamma", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("gamma", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("gamma")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("RGB levels")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("RGB, linked"), qsTr("RGB, independent")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editRgbLevels.modeIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("rgbLevelsMode", currentIndex)
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    visible: !root.hasPresenter || root.presenter.editRgbLevels.modeIndex === 0
                    model: [qsTr("None"), qsTr("Luminance"), qsTr("Max RGB"), qsTr("Average RGB"), qsTr("Sum RGB"), qsTr("Norm RGB"), qsTr("Basic power")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editRgbLevels.preserveIndex : 1
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("rgbLevelsPreserve", currentIndex)
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Black"),
                            "key": "black",
                            "field": "rgbLevelsBlack",
                            "reset": 0
                        },
                        {
                            "title": qsTr("Grey"),
                            "key": "grey",
                            "field": "rgbLevelsGrey",
                            "reset": 0.5
                        },
                        {
                            "title": qsTr("White"),
                            "key": "white",
                            "field": "rgbLevelsWhite",
                            "reset": 1
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.title
                        from: 0
                        to: 1
                        stepSize: 0.001
                        validatorDecimals: 3
                        showReset: true
                        resetValue: modelData.reset
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editRgbLevels[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
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
                        {
                            "title": qsTr("Green black"),
                            "key": "blackG",
                            "field": "rgbLevelsBlackG",
                            "reset": 0
                        },
                        {
                            "title": qsTr("Green grey"),
                            "key": "greyG",
                            "field": "rgbLevelsGreyG",
                            "reset": 0.5
                        },
                        {
                            "title": qsTr("Green white"),
                            "key": "whiteG",
                            "field": "rgbLevelsWhiteG",
                            "reset": 1
                        },
                        {
                            "title": qsTr("Blue black"),
                            "key": "blackB",
                            "field": "rgbLevelsBlackB",
                            "reset": 0
                        },
                        {
                            "title": qsTr("Blue grey"),
                            "key": "greyB",
                            "field": "rgbLevelsGreyB",
                            "reset": 0.5
                        },
                        {
                            "title": qsTr("Blue white"),
                            "key": "whiteB",
                            "field": "rgbLevelsWhiteB",
                            "reset": 1
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        visible: root.hasPresenter && root.presenter.editRgbLevels.modeIndex === 1
                        title: modelData.title
                        from: 0
                        to: 1
                        stepSize: 0.001
                        validatorDecimals: 3
                        showReset: true
                        resetValue: modelData.reset
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editRgbLevels[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl(modelData.field)
                    }
                }
                CustomButton {
                    text: qsTr("Reset RGB levels")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("rgbLevels")
                }
            }
        }
        DevelopSection {
            title: qsTr("Curves")
            sectionId: "curves"
            ColumnLayout {
                id: curveControls
                Layout.fillWidth: true
                width: parent.width
                spacing: Fonts.smallSpacing
                property bool editRegions: false
                readonly property bool rgbFamily: !root.hasPresenter || root.presenter.editCurve.familyIndex === 0
                readonly property bool masterChannel: !root.hasPresenter || root.presenter.editCurve.channel === 0
                readonly property bool regionsAvailable: rgbFamily && masterChannel && (!root.hasPresenter || root.presenter.editCurve.linked)
                readonly property color activeCurveColor: {
                    if (!root.hasPresenter || root.presenter.editCurve.channel === 0)
                        return Theme.textColor;
                    if (root.presenter.editCurve.familyIndex === 1)
                        return root.presenter.editCurve.channel === 1 ? "#d97bdc" : "#64c7c9";
                    if (root.presenter.editCurve.channel === 1)
                        return "#ed6a70";
                    if (root.presenter.editCurve.channel === 2)
                        return "#65c982";
                    return "#6f9df4";
                }
                readonly property string activeChannelLabel: {
                    if (!root.hasPresenter)
                        return qsTr("RGB");
                    if (root.presenter.editCurve.familyIndex === 1)
                        return [qsTr("Master"), qsTr("a"), qsTr("b")][root.presenter.editCurve.channel];
                    return [qsTr("RGB"), qsTr("Red"), qsTr("Green"), qsTr("Blue")][root.presenter.editCurve.channel];
                }

                onRegionsAvailableChanged: if (!regionsAvailable)
                    editRegions = false

                RowLayout {
                    objectName: "curveFamily"
                    Layout.fillWidth: true
                    spacing: Fonts.size6

                    CustomLabel {
                        text: qsTr("Curve")
                        opacity: 0.72
                        Layout.preferredWidth: Fonts.size60
                    }
                    CurveOptionButton {
                        objectName: "curveFamilyRgb"
                        Layout.fillWidth: true
                        text: qsTr("RGB")
                        selected: curveControls.rgbFamily
                        enabled: root.hasSelection
                        onClicked: {
                            curveControls.editRegions = false;
                            if (root.hasPresenter)
                                root.presenter.setCurveFamily(0);
                        }
                    }
                    CurveOptionButton {
                        objectName: "curveFamilyTone"
                        Layout.fillWidth: true
                        text: qsTr("Tone")
                        selected: !curveControls.rgbFamily
                        enabled: root.hasSelection
                        onClicked: {
                            curveControls.editRegions = false;
                            if (root.hasPresenter)
                                root.presenter.setCurveFamily(1);
                        }
                    }
                }

                RowLayout {
                    objectName: "curveChannel"
                    Layout.fillWidth: true
                    spacing: Fonts.size6

                    CustomLabel {
                        text: qsTr("Channel")
                        opacity: 0.72
                        Layout.preferredWidth: Fonts.size60
                    }
                    Repeater {
                        model: curveControls.rgbFamily ? [
                            {
                                "title": qsTr("RGB"),
                                "color": Theme.textColor
                            },
                            {
                                "title": qsTr("R"),
                                "color": "#ed6a70"
                            },
                            {
                                "title": qsTr("G"),
                                "color": "#65c982"
                            },
                            {
                                "title": qsTr("B"),
                                "color": "#6f9df4"
                            }
                        ] : [
                            {
                                "title": qsTr("Master"),
                                "color": Theme.textColor
                            },
                            {
                                "title": qsTr("a"),
                                "color": "#d97bdc"
                            },
                            {
                                "title": qsTr("b"),
                                "color": "#64c7c9"
                            }
                        ]
                        delegate: CurveOptionButton {
                            required property int index
                            required property var modelData
                            Layout.fillWidth: true
                            text: modelData.title
                            selectionColor: modelData.color
                            selected: (root.hasPresenter ? root.presenter.editCurve.channel : 0) === index
                            enabled: root.hasSelection
                            onClicked: {
                                curveControls.editRegions = false;
                                if (root.hasPresenter)
                                    root.presenter.setCurveChannel(index);
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Fonts.size6

                    CustomLabel {
                        text: qsTr("Edit")
                        opacity: 0.72
                        Layout.preferredWidth: Fonts.size60
                    }
                    CurveOptionButton {
                        objectName: "curvePointMode"
                        Layout.fillWidth: true
                        text: qsTr("Point")
                        selected: !curveControls.editRegions
                        enabled: root.hasSelection
                        onClicked: curveControls.editRegions = false
                    }
                    CurveOptionButton {
                        objectName: "curveParametricMode"
                        Layout.fillWidth: true
                        visible: curveControls.regionsAvailable
                        text: qsTr("Parametric")
                        selected: curveControls.editRegions
                        enabled: root.hasSelection
                        onClicked: curveControls.editRegions = true
                    }
                    CustomButton {
                        objectName: "resetActiveCurve"
                        text: qsTr("Reset curve")
                        defaultHeight: Fonts.inputFieldHeight
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.resetControl(curveControls.rgbFamily ? "rgbCurve" : "toneCurve")
                    }
                }

                ToneCurveEditor {
                    id: curveEditor
                    objectName: "curveEditor"
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(Fonts.size200, Math.min(Fonts.size300, width * 0.72))
                    editorEnabled: root.hasSelection
                    curveColor: curveControls.activeCurveColor
                    channelLabel: curveControls.activeChannelLabel
                    showRegionSplits: curveControls.editRegions && curveControls.regionsAvailable
                    regionSplits: root.hasPresenter ? [root.presenter.editCurve.split0, root.presenter.editCurve.split1, root.presenter.editCurve.split2] : [0.25, 0.5, 0.75]
                    histogramMode: root.hasPresenter ? root.presenter.editCurve.histogramMode : "rgb"
                    histogramRed: root.hasPresenter ? root.presenter.scopeHistogramRed : []
                    histogramGreen: root.hasPresenter ? root.presenter.scopeHistogramGreen : []
                    histogramBlue: root.hasPresenter ? root.presenter.scopeHistogramBlue : []
                    histogramLuma: root.hasPresenter ? root.presenter.scopeHistogramLuma : []
                    histogramMax: root.hasPresenter ? root.presenter.scopeHistogramMax : 0
                    points: root.hasPresenter ? root.presenter.editCurvePoints : [
                        {
                            "x": 0,
                            "y": 0
                        },
                        {
                            "x": 1,
                            "y": 1
                        }
                    ]
                    samples: root.hasPresenter ? root.presenter.editCurveSamples : []
                    onCurveEdited: function (points) {
                        if (root.commands)
                            root.commands.previewCurve(root.hasPresenter && root.presenter.editCurve.familyIndex === 1 ? "tone" : "rgb", root.hasPresenter ? root.presenter.editCurve.channel : 0, points);
                    }
                    onCurveCommitted: function (points) {
                        if (root.commands)
                            root.commands.setCurve(root.hasPresenter && root.presenter.editCurve.familyIndex === 1 ? "tone" : "rgb", root.hasPresenter ? root.presenter.editCurve.channel : 0, points);
                    }
                }
                CustomLabel {
                    text: qsTr("Drag points to reshape. Click to add. Double-click or Delete removes an interior point. Arrow keys nudge the selected point.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    visible: !curveControls.editRegions
                    opacity: 0.75
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: curveControls.editRegions && curveControls.regionsAvailable
                    spacing: Fonts.smallSpacing

                    CustomLabel {
                        text: qsTr("Parametric regions")
                        font: Fonts.makeBoldFont(Fonts.standardFont)
                        Layout.fillWidth: true
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Shadows"),
                                "key": "parametricShadows",
                                "field": "rgbCurveShadows"
                            },
                            {
                                "title": qsTr("Darks"),
                                "key": "parametricDarks",
                                "field": "rgbCurveDarks"
                            },
                            {
                                "title": qsTr("Lights"),
                                "key": "parametricLights",
                                "field": "rgbCurveLights"
                            },
                            {
                                "title": qsTr("Highlights"),
                                "key": "parametricHighlights",
                                "field": "rgbCurveHighlights"
                            }
                        ]
                        delegate: CustomSlider {
                            required property var modelData
                            Layout.fillWidth: true
                            title: modelData.title
                            from: -1
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: true
                            resetValue: 0
                            delayedCommit: true
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editCurve[modelData.key] : 0
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl(modelData.field)
                        }
                    }
                }

                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Curve settings")
                    expanded: false

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Fonts.size8
                        rowSpacing: Fonts.size8

                        CustomLabel {
                            text: qsTr("Interpolation")
                            opacity: 0.72
                        }
                        CustomComboBox {
                            objectName: "curveInterpolation"
                            Layout.fillWidth: true
                            model: [qsTr("Monotonic"), qsTr("Centripetal"), qsTr("Cubic")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editCurve.interpolationIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber(curveControls.rgbFamily ? "rgbCurveInterpolation" : "toneCurveInterpolation", currentIndex)
                        }

                        CustomLabel {
                            visible: !curveControls.rgbFamily
                            text: qsTr("Working space")
                            opacity: 0.72
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            visible: !curveControls.rgbFamily
                            model: [qsTr("RGB, linked"), qsTr("Lab"), qsTr("XYZ"), qsTr("Lab independent"), qsTr("sRGB"), qsTr("Linear RGB")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editCurve.workingSpaceIndex : 0
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber("toneCurveWorkingSpace", currentIndex)
                        }

                        CustomLabel {
                            visible: curveControls.masterChannel && (!root.hasPresenter || root.presenter.editCurve.linked)
                            text: qsTr("Preserve colors")
                            opacity: 0.72
                        }
                        CustomComboBox {
                            Layout.fillWidth: true
                            visible: curveControls.masterChannel && (!root.hasPresenter || root.presenter.editCurve.linked)
                            model: [qsTr("None"), qsTr("Luminance"), qsTr("Max RGB"), qsTr("Average RGB"), qsTr("Sum RGB"), qsTr("Norm RGB"), qsTr("Basic power")]
                            enabled: root.hasSelection
                            currentIndex: root.hasPresenter ? root.presenter.editCurve.preserveIndex : 1
                            onActivated: if (root.commands)
                                root.commands.setDevelopNumber(curveControls.rgbFamily ? "rgbCurvePreserve" : "toneCurvePreserve", currentIndex)
                        }

                        CustomCheckBox {
                            Layout.columnSpan: 2
                            text: qsTr("Compensate middle grey")
                            visible: curveControls.rgbFamily
                            enabled: root.hasSelection
                            checked: root.hasPresenter && root.presenter.editCurve.compensate
                            onToggled: if (root.commands)
                                root.commands.setDevelopNumber("rgbCurveCompensate", checked ? 1 : 0)
                        }

                        ColumnLayout {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            visible: curveControls.regionsAvailable
                            spacing: Fonts.smallSpacing

                            CustomLabel {
                                text: qsTr("Region boundaries")
                                font: Fonts.makeBoldFont(Fonts.standardFont)
                                Layout.fillWidth: true
                            }
                            Repeater {
                                model: [
                                    {
                                        "title": qsTr("Shadows / Darks"),
                                        "key": "split0",
                                        "field": "rgbCurveSplit0",
                                        "reset": 0.25
                                    },
                                    {
                                        "title": qsTr("Darks / Lights"),
                                        "key": "split1",
                                        "field": "rgbCurveSplit1",
                                        "reset": 0.5
                                    },
                                    {
                                        "title": qsTr("Lights / Highlights"),
                                        "key": "split2",
                                        "field": "rgbCurveSplit2",
                                        "reset": 0.75
                                    }
                                ]
                                delegate: CustomSlider {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    title: modelData.title
                                    from: 0.05
                                    to: 0.95
                                    stepSize: 0.01
                                    validatorDecimals: 2
                                    showReset: true
                                    resetValue: modelData.reset
                                    delayedCommit: true
                                    enabled: root.hasSelection
                                    value: root.hasPresenter ? root.presenter.editCurve[modelData.key] : modelData.reset
                                    onValueEdited: function (value) {
                                        if (root.liveReady && root.commands)
                                            root.commands.previewDevelopNumber(modelData.field, value);
                                    }
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
                }
            }
        }
        DevelopSection {
            title: qsTr("Color Equalizer")
            sectionId: "colorEqualizer"
            ColumnLayout {
                Layout.fillWidth: true
                width: parent.width
                CustomComboBox {
                    id: colorEqChannel
                    objectName: "colorEqChannel"
                    Layout.fillWidth: true
                    model: [qsTr("Saturation"), qsTr("Hue"), qsTr("Lightness")]
                    enabled: root.hasSelection
                    currentIndex: 0
                }
                Repeater {
                    id: colorEqBands
                    model: root.hasPresenter ? root.presenter.editColorEqBands : []
                    delegate: CustomSlider {
                        required property var modelData
                        objectName: "colorEqBand" + modelData.index
                        Layout.fillWidth: true
                        title: modelData.title
                        from: colorEqChannel.currentIndex === 1 ? -0.5 : -1
                        to: colorEqChannel.currentIndex === 1 ? 0.5 : 1
                        stepSize: colorEqChannel.currentIndex === 1 ? 0.005 : 0.05
                        validatorDecimals: colorEqChannel.currentIndex === 1 ? 3 : 2
                        showReset: true
                        resetValue: 0
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: colorEqChannel.currentIndex === 0 ? modelData.sat : colorEqChannel.currentIndex === 1 ? modelData.hue : modelData.light
                        readonly property string fieldName: colorEqChannel.currentIndex === 0 ? modelData.satField : colorEqChannel.currentIndex === 1 ? modelData.hueField : modelData.lightField
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(fieldName, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(fieldName, value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl(fieldName)
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                    text: qsTr("Eight hue bands match a Lightroom HSL mixer: Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta.")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vibrance", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("saturation", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("saturation", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("saturation")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Velvia")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                CustomCheckBox {
                    objectName: "velviaEnabled"
                    text: qsTr("Enable Velvia")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editVelviaParams.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("velviaEnabled", checked ? 1 : 0)
                }
                CustomSlider {
                    objectName: "velviaStrength"
                    Layout.fillWidth: true
                    title: qsTr("Strength")
                    from: 0
                    to: 100
                    stepSize: 1
                    validatorDecimals: 1
                    showReset: true
                    resetValue: 25
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVelviaParams.strength : 25
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("velviaStrength", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("velviaStrength", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("velviaStrength")
                }
                CustomSlider {
                    objectName: "velviaBias"
                    Layout.fillWidth: true
                    title: qsTr("Mid-tones bias")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 1
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVelviaParams.bias : 1
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("velviaBias", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("velviaBias", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("velviaBias")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.72
                    visible: root.hasPresenter && root.presenter.editVelviaParams.masked
                    text: qsTr("Loaded Velvia mask is preserved but edited outside this panel.")
                }
                CustomButton {
                    text: qsTr("Disable and reset Velvia")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("velvia")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("3D LUT")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    CustomTextField {
                        objectName: "lut3dFile"
                        Layout.fillWidth: true
                        maximumLength: 4096
                        showEmptyIndicator: true
                        showClipIndicator: true
                        placeholderText: qsTr("Choose a .cube file")
                        enabled: root.hasSelection
                        text: root.hasPresenter ? root.presenter.editLut3d.filePath : ""
                        onEditingCommitted: function (committedText) {
                            if (root.commands)
                                root.commands.setDevelopText("lut3dFile", committedText);
                        }
                    }
                    CustomButton {
                        text: qsTr("Choose…")
                        enabled: root.hasSelection
                        onClicked: lut3dDialog.openDialog()
                    }
                }
                CustomCheckBox {
                    objectName: "lut3dEnabled"
                    text: qsTr("Enable 3D LUT")
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editLut3d.hasFile
                    checked: root.hasPresenter && root.presenter.editLut3d.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("lut3dEnabled", checked ? 1 : 0)
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Input color space")
                }
                CustomComboBox {
                    objectName: "lut3dInputSpace"
                    Layout.fillWidth: true
                    model: root.hasPresenter ? root.presenter.editLut3d.spaceChoices : []
                    currentIndex: root.hasPresenter ? root.presenter.editLut3d.inputSpaceIndex : 0
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editLut3d.hasFile
                    Accessible.name: qsTr("3D LUT input color space")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("lut3dInputSpaceIndex", index);
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Output color space")
                }
                CustomComboBox {
                    objectName: "lut3dOutputSpace"
                    Layout.fillWidth: true
                    model: root.hasPresenter ? root.presenter.editLut3d.spaceChoices : []
                    currentIndex: root.hasPresenter ? root.presenter.editLut3d.outputSpaceIndex : 0
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editLut3d.hasFile
                    Accessible.name: qsTr("3D LUT output color space")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("lut3dOutputSpaceIndex", index);
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Interpolation")
                }
                CustomComboBox {
                    objectName: "lut3dInterpolation"
                    Layout.fillWidth: true
                    model: root.hasPresenter ? root.presenter.editLut3d.interpolationChoices : []
                    currentIndex: root.hasPresenter ? root.presenter.editLut3d.interpolationIndex : 0
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editLut3d.hasFile
                    Accessible.name: qsTr("3D LUT interpolation")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("lut3dInterpolationIndex", index);
                    }
                }
                CustomSlider {
                    objectName: "lut3dStrength"
                    Layout.fillWidth: true
                    title: qsTr("Strength")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 1
                    delayedCommit: true
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editLut3d.hasFile
                    value: root.hasPresenter ? root.presenter.editLut3d.strength : 1
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("lut3dStrength", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("lut3dStrength", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("lut3dStrength")
                }
                CustomButton {
                    text: qsTr("Disable and reset 3D LUT")
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editLut3d.present
                    onClicked: if (root.commands)
                        root.commands.resetControl("lut3d")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Color Balance RGB · linear sRGB D50 / Filmlight Yrg")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Fonts.size6
                    ColorGradeWheel {
                        objectName: "colorBalanceShadowsWheel"
                        title: qsTr("Shadows")
                        hueField: "colorBalanceShadowsHue"
                        chromaField: "colorBalanceShadowsChroma"
                        luminanceField: "colorBalanceShadowsY"
                        hue: root.hasPresenter ? root.presenter.editColorBalanceRgb.shadowsHue : 0
                        chroma: root.hasPresenter ? root.presenter.editColorBalanceRgb.shadowsChroma : 0
                        luminance: root.hasPresenter ? root.presenter.editColorBalanceRgb.shadowsY : 0
                        maxChroma: 0.5
                        luminanceFrom: -1
                        luminanceTo: 1
                        luminanceStep: 0.01
                        luminanceDecimals: 2
                        editorEnabled: root.hasSelection
                        commands: root.commands
                        liveReady: root.liveReady
                    }
                    ColorGradeWheel {
                        objectName: "colorBalanceMidtonesWheel"
                        title: qsTr("Midtones")
                        hueField: "colorBalanceMidtonesHue"
                        chromaField: "colorBalanceMidtonesChroma"
                        luminanceField: "colorBalanceMidtonesY"
                        hue: root.hasPresenter ? root.presenter.editColorBalanceRgb.midtonesHue : 0
                        chroma: root.hasPresenter ? root.presenter.editColorBalanceRgb.midtonesChroma : 0
                        luminance: root.hasPresenter ? root.presenter.editColorBalanceRgb.midtonesY : 0
                        maxChroma: 0.1
                        luminanceFrom: -0.25
                        luminanceTo: 0.25
                        luminanceStep: 0.005
                        luminanceDecimals: 3
                        editorEnabled: root.hasSelection
                        commands: root.commands
                        liveReady: root.liveReady
                    }
                    ColorGradeWheel {
                        objectName: "colorBalanceHighlightsWheel"
                        title: qsTr("Highlights")
                        hueField: "colorBalanceHighlightsHue"
                        chromaField: "colorBalanceHighlightsChroma"
                        luminanceField: "colorBalanceHighlightsY"
                        hue: root.hasPresenter ? root.presenter.editColorBalanceRgb.highlightsHue : 0
                        chroma: root.hasPresenter ? root.presenter.editColorBalanceRgb.highlightsChroma : 0
                        luminance: root.hasPresenter ? root.presenter.editColorBalanceRgb.highlightsY : 0
                        maxChroma: 0.2
                        luminanceFrom: -0.5
                        luminanceTo: 0.5
                        luminanceStep: 0.01
                        luminanceDecimals: 2
                        editorEnabled: root.hasSelection
                        commands: root.commands
                        liveReady: root.liveReady
                    }
                }
                CustomButton {
                    text: qsTr("Reset Color Balance RGB")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("colorBalance")
                }
                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Color Balance RGB · more")
                    expanded: false
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
                            {
                                "title": qsTr("Global · Luminance"),
                                "key": "globalY",
                                "field": "colorBalanceGlobalY",
                                "minimum": -0.05,
                                "maximum": 0.05,
                                "reset": 0,
                                "step": 0.001,
                                "decimals": 3
                            },
                            {
                                "title": qsTr("Global · Chroma"),
                                "key": "globalChroma",
                                "field": "colorBalanceGlobalChroma",
                                "minimum": 0,
                                "maximum": 0.01,
                                "reset": 0,
                                "step": 0.0001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Global · Hue"),
                                "key": "globalHue",
                                "field": "colorBalanceGlobalHue",
                                "minimum": 0,
                                "maximum": 360,
                                "reset": 0,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Shadows · Luminance"),
                                "key": "shadowsY",
                                "field": "colorBalanceShadowsY",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Shadows · Chroma"),
                                "key": "shadowsChroma",
                                "field": "colorBalanceShadowsChroma",
                                "minimum": 0,
                                "maximum": 0.5,
                                "reset": 0,
                                "step": 0.005,
                                "decimals": 3
                            },
                            {
                                "title": qsTr("Shadows · Hue"),
                                "key": "shadowsHue",
                                "field": "colorBalanceShadowsHue",
                                "minimum": 0,
                                "maximum": 360,
                                "reset": 0,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Midtones · Luminance"),
                                "key": "midtonesY",
                                "field": "colorBalanceMidtonesY",
                                "minimum": -0.25,
                                "maximum": 0.25,
                                "reset": 0,
                                "step": 0.005,
                                "decimals": 3
                            },
                            {
                                "title": qsTr("Midtones · Chroma"),
                                "key": "midtonesChroma",
                                "field": "colorBalanceMidtonesChroma",
                                "minimum": 0,
                                "maximum": 0.1,
                                "reset": 0,
                                "step": 0.001,
                                "decimals": 3
                            },
                            {
                                "title": qsTr("Midtones · Hue"),
                                "key": "midtonesHue",
                                "field": "colorBalanceMidtonesHue",
                                "minimum": 0,
                                "maximum": 360,
                                "reset": 0,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Highlights · Luminance"),
                                "key": "highlightsY",
                                "field": "colorBalanceHighlightsY",
                                "minimum": -0.5,
                                "maximum": 0.5,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Highlights · Chroma"),
                                "key": "highlightsChroma",
                                "field": "colorBalanceHighlightsChroma",
                                "minimum": 0,
                                "maximum": 0.2,
                                "reset": 0,
                                "step": 0.002,
                                "decimals": 3
                            },
                            {
                                "title": qsTr("Highlights · Hue"),
                                "key": "highlightsHue",
                                "field": "colorBalanceHighlightsHue",
                                "minimum": 0,
                                "maximum": 360,
                                "reset": 0,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Shadows fall-off"),
                                "key": "shadowsFalloff",
                                "field": "colorBalanceShadowsFalloff",
                                "minimum": 0,
                                "maximum": 3,
                                "reset": 1,
                                "step": 0.05,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Highlights fall-off"),
                                "key": "highlightsFalloff",
                                "field": "colorBalanceHighlightsFalloff",
                                "minimum": 0,
                                "maximum": 3,
                                "reset": 1,
                                "step": 0.05,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Mask grey fulcrum"),
                                "key": "maskGreyFulcrum",
                                "field": "colorBalanceMaskGreyFulcrum",
                                "minimum": 0.000001,
                                "maximum": 1,
                                "reset": 0.1845,
                                "step": 0.001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("White fulcrum · EV"),
                                "key": "whiteFulcrumEv",
                                "field": "colorBalanceWhiteFulcrumEv",
                                "minimum": -2,
                                "maximum": 2,
                                "reset": 0,
                                "step": 0.05,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Grey fulcrum"),
                                "key": "greyFulcrum",
                                "field": "colorBalanceGreyFulcrum",
                                "minimum": 0.1,
                                "maximum": 0.5,
                                "reset": 0.1845,
                                "step": 0.001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Chroma · Global"),
                                "key": "chromaGlobal",
                                "field": "colorBalanceChromaGlobal",
                                "minimum": -0.5,
                                "maximum": 0.5,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Chroma · Shadows"),
                                "key": "chromaShadows",
                                "field": "colorBalanceChromaShadows",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Chroma · Midtones"),
                                "key": "chromaMidtones",
                                "field": "colorBalanceChromaMidtones",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Chroma · Highlights"),
                                "key": "chromaHighlights",
                                "field": "colorBalanceChromaHighlights",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Saturation · Global"),
                                "key": "saturationGlobal",
                                "field": "colorBalanceSaturationGlobal",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Saturation · Shadows"),
                                "key": "saturationShadows",
                                "field": "colorBalanceSaturationShadows",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Saturation · Midtones"),
                                "key": "saturationMidtones",
                                "field": "colorBalanceSaturationMidtones",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Saturation · Highlights"),
                                "key": "saturationHighlights",
                                "field": "colorBalanceSaturationHighlights",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Brilliance · Global"),
                                "key": "brillianceGlobal",
                                "field": "colorBalanceBrillianceGlobal",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Brilliance · Shadows"),
                                "key": "brillianceShadows",
                                "field": "colorBalanceBrillianceShadows",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Brilliance · Midtones"),
                                "key": "brillianceMidtones",
                                "field": "colorBalanceBrillianceMidtones",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Brilliance · Highlights"),
                                "key": "brillianceHighlights",
                                "field": "colorBalanceBrillianceHighlights",
                                "minimum": -1,
                                "maximum": 1,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Vibrance"),
                                "key": "vibrance",
                                "field": "colorBalanceVibrance",
                                "minimum": -0.5,
                                "maximum": 0.5,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Hue rotation"),
                                "key": "hueRotation",
                                "field": "colorBalanceHueRotation",
                                "minimum": -180,
                                "maximum": 180,
                                "reset": 0,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Contrast"),
                                "key": "contrast",
                                "field": "colorBalanceContrast",
                                "minimum": -0.5,
                                "maximum": 0.5,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            }
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
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl(modelData.field)
                        }
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Split Toning")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                CustomCheckBox {
                    objectName: "splitToningEnabled"
                    text: qsTr("Enable Split Toning")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editSplitToning.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("splitToningEnabled", checked ? 1 : 0)
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Split Toning mix")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: false
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSplitToning.mix : 1
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitMix", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitMix", value);
                    }
                }
                HueSlider {
                    Layout.fillWidth: true
                    title: qsTr("Shadow hue")
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSplitShadowsHue : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitShadowsHue", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitShadowsHue", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("splitShadowsHue")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Shadow saturation")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: false
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSplitToning.shadowSaturation : 0.5
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitShadowSaturation", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitShadowSaturation", value);
                    }
                }
                HueSlider {
                    Layout.fillWidth: true
                    title: qsTr("Highlight hue")
                    showReset: true
                    resetValue: 0.2
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSplitHighlightsHue : 0.2
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitHighlightsHue", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitHighlightsHue", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("splitHighlightsHue")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Highlight saturation")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: false
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSplitToning.highlightSaturation : 0.5
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitHighlightSaturation", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitHighlightSaturation", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitBalance", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitBalance", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("splitBalance")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Midtone compression")
                    from: 0
                    to: 100
                    stepSize: 1
                    validatorDecimals: 1
                    showReset: false
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSplitToning.compress : 33
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("splitCompress", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("splitCompress", value);
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.72
                    visible: root.hasPresenter && root.presenter.editSplitToning.masked
                    text: qsTr("Loaded Split Toning mask is preserved but edited outside this panel.")
                }
                CustomButton {
                    text: qsTr("Disable and reset Split Toning")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("splitToning")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Monochrome")
                    font.bold: true
                    wrapMode: Text.WordWrap
                }
                CustomCheckBox {
                    objectName: "monochromeEnabled"
                    text: qsTr("Enable Monochrome")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editMonochromeFilter.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("monochromeEnabled", checked ? 1 : 0)
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Filter a*"),
                            "key": "filterA",
                            "field": "monochromeFilterA",
                            "from": -128,
                            "to": 128,
                            "reset": 0,
                            "step": 1,
                            "decimals": 1
                        },
                        {
                            "title": qsTr("Filter b*"),
                            "key": "filterB",
                            "field": "monochromeFilterB",
                            "from": -128,
                            "to": 128,
                            "reset": 0,
                            "step": 1,
                            "decimals": 1
                        },
                        {
                            "title": qsTr("Filter size"),
                            "key": "size",
                            "field": "monochromeSize",
                            "from": 0.5,
                            "to": 3,
                            "reset": 2,
                            "step": 0.1,
                            "decimals": 1
                        },
                        {
                            "title": qsTr("Keep highlights"),
                            "key": "highlights",
                            "field": "monochromeHighlights",
                            "from": 0,
                            "to": 1,
                            "reset": 0,
                            "step": 0.01,
                            "decimals": 2
                        },
                        {
                            "title": qsTr("Monochrome mix"),
                            "key": "mix",
                            "field": "monochromeMix",
                            "from": 0,
                            "to": 1,
                            "reset": 1,
                            "step": 0.01,
                            "decimals": 2
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.title
                        from: modelData.from
                        to: modelData.to
                        stepSize: modelData.step
                        validatorDecimals: modelData.decimals
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editMonochromeFilter[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.72
                    visible: root.hasPresenter && root.presenter.editMonochromeFilter.masked
                    text: qsTr("Loaded Monochrome mask is preserved but edited outside this panel.")
                }
                CustomButton {
                    text: qsTr("Disable and reset Monochrome")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("monochrome")
                }
                Expander {
                    Layout.fillWidth: true
                    title: qsTr("Color · Advanced")
                    expanded: false
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
                        model: [qsTr("IT8 skin tones"), qsTr("Expanded color checker"), qsTr("Helmholtz/Kohlrausch monochrome"), qsTr("Fuji Astia emulation"), qsTr("Fuji Classic Chrome emulation"), qsTr("Fuji Monochrome emulation"), qsTr("Fuji Provia emulation"), qsTr("Fuji Velvia emulation")]
                        enabled: root.hasSelection
                        currentIndex: root.hasPresenter ? root.presenter.editColorChecker.presetIndex : -1
                        onActivated: if (root.commands)
                            root.commands.setDevelopNumber("colorCheckerPreset", currentIndex)
                    }
                    CustomComboBox {
                        Layout.fillWidth: true
                        model: {
                            const labels = [];
                            const count = root.hasPresenter ? root.presenter.editColorChecker.patchCount : 0;
                            for (let index = 0; index < count; ++index)
                                labels.push(qsTr("Patch %1").arg(index + 1));
                            return labels;
                        }
                        enabled: root.hasSelection && root.hasPresenter && root.presenter.editColorChecker.patchCount > 0
                        currentIndex: root.hasPresenter ? root.presenter.editColorChecker.patchIndex : -1
                        onActivated: if (root.commands)
                            root.commands.setDevelopNumber("colorCheckerPatch", currentIndex)
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Source · L*"),
                                "key": "sourceL",
                                "field": "colorCheckerSourceL"
                            },
                            {
                                "title": qsTr("Source · a*"),
                                "key": "sourceA",
                                "field": "colorCheckerSourceA"
                            },
                            {
                                "title": qsTr("Source · b*"),
                                "key": "sourceB",
                                "field": "colorCheckerSourceB"
                            },
                            {
                                "title": qsTr("Target · L*"),
                                "key": "targetL",
                                "field": "colorCheckerTargetL"
                            },
                            {
                                "title": qsTr("Target · a*"),
                                "key": "targetA",
                                "field": "colorCheckerTargetA"
                            },
                            {
                                "title": qsTr("Target · b*"),
                                "key": "targetB",
                                "field": "colorCheckerTargetB"
                            }
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
                        text: root.hasPresenter && root.presenter.editLegacyColorBalance.enabled ? qsTr("Enabled") : qsTr("Inactive until edited")
                        opacity: 0.75
                    }
                    CustomComboBox {
                        Layout.fillWidth: true
                        model: [qsTr("Lift / Gamma / Gain"), qsTr("Slope / Offset / Power")]
                        enabled: root.hasSelection
                        currentIndex: root.hasPresenter ? root.presenter.editLegacyColorBalance.modeIndex : 1
                        onActivated: if (root.commands)
                            root.commands.setDevelopNumber("legacyColorBalanceMode", currentIndex)
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Lift · Factor"),
                                "key": "liftFactor",
                                "field": "legacyColorBalanceLiftFactor",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.0001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Lift · Red"),
                                "key": "liftRed",
                                "field": "legacyColorBalanceLiftRed",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Lift · Green"),
                                "key": "liftGreen",
                                "field": "legacyColorBalanceLiftGreen",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Lift · Blue"),
                                "key": "liftBlue",
                                "field": "legacyColorBalanceLiftBlue",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Gamma · Factor"),
                                "key": "gammaFactor",
                                "field": "legacyColorBalanceGammaFactor",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.0001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Gamma · Red"),
                                "key": "gammaRed",
                                "field": "legacyColorBalanceGammaRed",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Gamma · Green"),
                                "key": "gammaGreen",
                                "field": "legacyColorBalanceGammaGreen",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Gamma · Blue"),
                                "key": "gammaBlue",
                                "field": "legacyColorBalanceGammaBlue",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Gain · Factor"),
                                "key": "gainFactor",
                                "field": "legacyColorBalanceGainFactor",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.0001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Gain · Red"),
                                "key": "gainRed",
                                "field": "legacyColorBalanceGainRed",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Gain · Green"),
                                "key": "gainGreen",
                                "field": "legacyColorBalanceGainGreen",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Gain · Blue"),
                                "key": "gainBlue",
                                "field": "legacyColorBalanceGainBlue",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.00001,
                                "decimals": 5
                            },
                            {
                                "title": qsTr("Input saturation"),
                                "key": "inputSaturation",
                                "field": "legacyColorBalanceInputSaturation",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.0001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Contrast"),
                                "key": "contrast",
                                "field": "legacyColorBalanceContrast",
                                "minimum": 0.01,
                                "maximum": 1.99,
                                "reset": 1,
                                "step": 0.0001,
                                "decimals": 4
                            },
                            {
                                "title": qsTr("Contrast fulcrum (%)"),
                                "key": "greyFulcrum",
                                "field": "legacyColorBalanceGreyFulcrum",
                                "minimum": 0.1,
                                "maximum": 100,
                                "reset": 18,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Output saturation"),
                                "key": "outputSaturation",
                                "field": "legacyColorBalanceOutputSaturation",
                                "minimum": 0,
                                "maximum": 2,
                                "reset": 1,
                                "step": 0.0001,
                                "decimals": 4
                            }
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
                            value: root.hasPresenter ? root.presenter.editLegacyColorBalance[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
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
                        text: qsTr("Color Correction · D50 Lab")
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    CustomCheckBox {
                        objectName: "colorCorrectionEnabled"
                        text: qsTr("Enable Color Correction")
                        enabled: root.hasSelection
                        checked: root.hasPresenter && root.presenter.editColorCorrection.enabled
                        onToggled: if (root.liveReady && root.commands)
                            root.commands.setDevelopNumber("colorCorrectionEnabled", checked ? 1 : 0)
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Highlights · a*"),
                                "key": "highlightA",
                                "field": "colorCorrectionHighlightA",
                                "minimum": -40,
                                "maximum": 40,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Highlights · b*"),
                                "key": "highlightB",
                                "field": "colorCorrectionHighlightB",
                                "minimum": -40,
                                "maximum": 40,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Shadows · a*"),
                                "key": "shadowA",
                                "field": "colorCorrectionShadowA",
                                "minimum": -40,
                                "maximum": 40,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Shadows · b*"),
                                "key": "shadowB",
                                "field": "colorCorrectionShadowB",
                                "minimum": -40,
                                "maximum": 40,
                                "reset": 0,
                                "step": 0.01,
                                "decimals": 2
                            },
                            {
                                "title": qsTr("Saturation"),
                                "key": "saturation",
                                "field": "colorCorrectionSaturation",
                                "minimum": -3,
                                "maximum": 3,
                                "reset": 1,
                                "step": 0.01,
                                "decimals": 2
                            }
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
                            value: root.hasPresenter ? root.presenter.editColorCorrection[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
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
                            {
                                "title": "a* ×",
                                "key": "aSteepness",
                                "field": "colorContrastASteepness",
                                "minimum": 0,
                                "maximum": 5,
                                "reset": 1
                            },
                            {
                                "title": "b* ×",
                                "key": "bSteepness",
                                "field": "colorContrastBSteepness",
                                "minimum": 0,
                                "maximum": 5,
                                "reset": 1
                            }
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
                            value: root.hasPresenter ? root.presenter.editColorContrast[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
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
                            {
                                "title": "a* +",
                                "key": "aOffset",
                                "field": "colorContrastAOffset"
                            },
                            {
                                "title": "b* +",
                                "key": "bOffset",
                                "field": "colorContrastBOffset"
                            }
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
                    CustomLabel {
                        Layout.fillWidth: true
                        text: qsTr("Color Harmonizer")
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    CustomCheckBox {
                        objectName: "colorHarmonizerEnabled"
                        text: qsTr("Enable Color Harmonizer")
                        enabled: root.hasSelection
                        checked: root.hasPresenter && root.presenter.editColorHarmonizer.enabled
                        onToggled: if (root.liveReady && root.commands)
                            root.commands.setDevelopNumber("colorHarmonizerEnabled", checked ? 1 : 0)
                    }
                    CustomComboBox {
                        objectName: "colorHarmonizerRuleIndex"
                        Layout.fillWidth: true
                        model: root.hasPresenter ? root.presenter.editColorHarmonizer.ruleChoices : []
                        enabled: root.hasSelection
                        currentIndex: root.hasPresenter ? root.presenter.editColorHarmonizer.ruleIndex : 3
                        onActivated: if (root.commands)
                            root.commands.setDevelopNumber("colorHarmonizerRuleIndex", currentIndex)
                    }
                    Repeater {
                        model: root.hasPresenter ? root.presenter.editColorHarmonizer.sharedControls : []
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
                            visible: modelData.visible
                            enabled: root.hasSelection
                            value: root.hasPresenter ? root.presenter.editColorHarmonizer[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl(modelData.field)
                        }
                    }
                    CustomSlider {
                        Layout.fillWidth: true
                        readonly property var nodeControl: root.hasPresenter ? root.presenter.editColorHarmonizer.customNodeControl : ({})
                        title: nodeControl.title !== undefined ? nodeControl.title : qsTr("Custom nodes")
                        from: nodeControl.minimum !== undefined ? nodeControl.minimum : 2
                        to: nodeControl.maximum !== undefined ? nodeControl.maximum : 4
                        stepSize: nodeControl.step !== undefined ? nodeControl.step : 1
                        validatorDecimals: nodeControl.decimals !== undefined ? nodeControl.decimals : 0
                        showReset: true
                        resetValue: nodeControl.reset !== undefined ? nodeControl.reset : 4
                        delayedCommit: true
                        visible: root.hasPresenter ? nodeControl.visible : false
                        enabled: root.hasSelection && root.hasPresenter && root.presenter.editColorHarmonizer.customRule
                        value: root.hasPresenter ? root.presenter.editColorHarmonizer.customNodeCount : 4
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(nodeControl.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(nodeControl.field, value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl(nodeControl.field)
                    }
                    Repeater {
                        model: root.hasPresenter ? root.presenter.editColorHarmonizer.customHueControls : []
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
                            visible: modelData.visible
                            enabled: root.hasSelection && modelData.visible
                            value: root.hasPresenter ? root.presenter.editColorHarmonizer[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl(modelData.field)
                        }
                    }
                    Repeater {
                        model: root.hasPresenter ? root.presenter.editColorHarmonizer.nodeSaturationControls : []
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
                            visible: modelData.visible
                            enabled: root.hasSelection && modelData.visible
                            value: root.hasPresenter ? root.presenter.editColorHarmonizer[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl(modelData.field)
                        }
                    }
                    CustomButton {
                        text: qsTr("Disable and reset Color Harmonizer")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.resetControl("colorHarmonizer")
                    }
                    MaskEditor {
                        objectName: "colorHarmonizerMaskEditor"
                        mask: root.hasPresenter ? root.presenter.editColorHarmonizerMask : ({})
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        text: qsTr("Color Reconstruction")
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    CustomCheckBox {
                        objectName: "colorReconstructionEnabled"
                        text: qsTr("Enable Color Reconstruction")
                        enabled: root.hasSelection
                        checked: root.hasPresenter && root.presenter.editColorReconstruction.enabled
                        onToggled: if (root.liveReady && root.commands)
                            root.commands.setDevelopNumber("colorReconstructionEnabled", checked ? 1 : 0)
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        text: qsTr("Precedence")
                        wrapMode: Text.WordWrap
                    }
                    CustomComboBox {
                        objectName: "colorReconstructionPrecedence"
                        Layout.fillWidth: true
                        model: root.hasPresenter ? root.presenter.editColorReconstruction.precedenceChoices : []
                        enabled: root.hasSelection
                        currentIndex: root.hasPresenter ? root.presenter.editColorReconstruction.precedenceIndex : 0
                        onActivated: if (root.commands)
                            root.commands.setDevelopNumber("colorReconstructionPrecedenceIndex", currentIndex)
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Threshold"),
                                "key": "threshold",
                                "field": "colorReconstructionThreshold",
                                "minimum": 50,
                                "maximum": 150,
                                "reset": 100,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Spatial extent"),
                                "key": "spatial",
                                "field": "colorReconstructionSpatial",
                                "minimum": 0,
                                "maximum": 1000,
                                "reset": 400,
                                "step": 1,
                                "decimals": 1
                            },
                            {
                                "title": qsTr("Range extent"),
                                "key": "range",
                                "field": "colorReconstructionRange",
                                "minimum": 0,
                                "maximum": 50,
                                "reset": 10,
                                "step": 0.1,
                                "decimals": 1
                            }
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
                            value: root.hasPresenter ? root.presenter.editColorReconstruction[modelData.key] : modelData.reset
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                            onResetRequested: if (root.commands)
                                root.commands.resetControl(modelData.field)
                        }
                    }
                    CustomSlider {
                        Layout.fillWidth: true
                        title: qsTr("Hue")
                        from: 0
                        to: 360
                        stepSize: 0.1
                        validatorDecimals: 1
                        showReset: true
                        resetValue: 237.6
                        delayedCommit: true
                        visible: root.hasPresenter && root.presenter.editColorReconstruction.precedenceIndex === 2
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editColorReconstruction.hueDegrees : 237.6
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber("colorReconstructionHueDegrees", value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber("colorReconstructionHueDegrees", value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl("colorReconstructionHueDegrees")
                    }
                    CustomButton {
                        text: qsTr("Disable and reset Color Reconstruction")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.resetControl("colorReconstruction")
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        text: qsTr("Color Zones")
                        font.bold: true
                        wrapMode: Text.WordWrap
                    }
                    CustomCheckBox {
                        objectName: "colorZonesEnabled"
                        text: qsTr("Enable Color Zones")
                        enabled: root.hasSelection
                        checked: root.hasPresenter && root.presenter.editColorZones.enabled
                        onToggled: if (root.liveReady && root.commands)
                            root.commands.setDevelopNumber("colorZonesEnabled", checked ? 1 : 0)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        CustomComboBox {
                            objectName: "colorZonesSelectBy"
                            Layout.fillWidth: true
                            textRole: "label"
                            model: root.hasPresenter ? root.presenter.editColorZones.selectByChoices : []
                            currentIndex: root.hasPresenter ? root.presenter.editColorZones.selectByIndex : 2
                            Accessible.name: qsTr("Color Zones select by")
                            onActivated: function (index) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorZonesSelectByIndex", model[index].index);
                            }
                        }
                        CustomComboBox {
                            objectName: "colorZonesBand"
                            Layout.fillWidth: true
                            textRole: "label"
                            model: root.hasPresenter ? root.presenter.editColorZones.bandChoices : []
                            currentIndex: root.hasPresenter ? root.presenter.editColorZones.bandIndex : 0
                            Accessible.name: qsTr("Color Zones band")
                            onActivated: function (index) {
                                if (root.commands)
                                    root.commands.setDevelopNumber("colorZonesBandIndex", model[index].index);
                            }
                        }
                    }
                    CustomSlider {
                        Layout.fillWidth: true
                        title: qsTr("Color Zones mix")
                        from: -200
                        to: 200
                        stepSize: 1
                        validatorDecimals: 0
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editColorZones.strength : 0
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber("colorZonesStrength", value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber("colorZonesStrength", value);
                        }
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Lightness curve"),
                                "key": "lightness",
                                "field": "colorZonesLightness"
                            },
                            {
                                "title": qsTr("Chroma curve"),
                                "key": "chroma",
                                "field": "colorZonesChroma"
                            },
                            {
                                "title": qsTr("Hue curve"),
                                "key": "hue",
                                "field": "colorZonesHue"
                            }
                        ]
                        delegate: CustomSlider {
                            required property var modelData
                            Layout.fillWidth: true
                            title: modelData.title
                            from: 0
                            to: 1
                            stepSize: 0.01
                            validatorDecimals: 2
                            showReset: false
                            delayedCommit: true
                            enabled: root.hasSelection && root.hasPresenter && root.presenter.editColorZones.editable
                            value: root.hasPresenter ? root.presenter.editColorZones[modelData.key] : 0.5
                            onValueEdited: function (value) {
                                if (root.liveReady && root.commands)
                                    root.commands.previewDevelopNumber(modelData.field, value);
                            }
                            onValueCommitted: function (value) {
                                if (root.commands)
                                    root.commands.setDevelopNumber(modelData.field, value);
                            }
                        }
                    }
                    Repeater {
                        model: [
                            {
                                "title": qsTr("Lightness interpolation"),
                                "key": "lightnessInterpolationIndex",
                                "field": "colorZonesLightnessInterpolationIndex"
                            },
                            {
                                "title": qsTr("Chroma interpolation"),
                                "key": "chromaInterpolationIndex",
                                "field": "colorZonesChromaInterpolationIndex"
                            },
                            {
                                "title": qsTr("Hue interpolation"),
                                "key": "hueInterpolationIndex",
                                "field": "colorZonesHueInterpolationIndex"
                            }
                        ]
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            CustomLabel {
                                Layout.fillWidth: true
                                text: modelData.title
                            }
                            CustomComboBox {
                                Layout.preferredWidth: Fonts.standardFontMetrics.averageCharacterWidth * 20
                                textRole: "label"
                                model: root.hasPresenter ? root.presenter.editColorZones.interpolationChoices : []
                                currentIndex: root.hasPresenter ? root.presenter.editColorZones[modelData.key] : 1
                                enabled: root.hasSelection
                                onActivated: function (index) {
                                    if (root.commands)
                                        root.commands.setDevelopNumber(modelData.field, model[index].index);
                                }
                            }
                        }
                    }
                    CustomLabel {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.72
                        visible: root.hasPresenter && (!root.presenter.editColorZones.editable || root.presenter.editColorZones.masked)
                        text: root.hasPresenter && root.presenter.editColorZones.masked ? qsTr("Loaded Color Zones mask is preserved but edited outside this panel.") : qsTr("Loaded custom-node curves are preserved; reset Color Zones to use the eight-band editor.")
                    }
                    CustomButton {
                        text: qsTr("Disable and reset Color Zones")
                        enabled: root.hasSelection
                        onClicked: if (root.commands)
                            root.commands.resetControl("colorZones")
                    }
                }
            }
        }
        DevelopSection {
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
                    delegate: PrimariesSlider {}
                }
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
                    Layout.fillWidth: true
                    spacing: Fonts.size6
                    CustomButton {
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:/GeoControls/icons/RotateCcw.svg"
                        tooltipText: qsTr("Rotate Left")
                        enabled: root.hasSelection
                        implicitWidth: Fonts.iconButtonSize
                        implicitHeight: Fonts.iconButtonSize
                        Layout.preferredWidth: implicitWidth
                        Layout.preferredHeight: implicitHeight
                        Layout.fillWidth: true
                        defaultPadding: 0
                        onClicked: if (root.commands)
                            root.commands.rotateLeft.trigger()
                    }
                    CustomButton {
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:/GeoControls/icons/RotateCw.svg"
                        tooltipText: qsTr("Rotate Right")
                        enabled: root.hasSelection
                        implicitWidth: Fonts.iconButtonSize
                        implicitHeight: Fonts.iconButtonSize
                        Layout.preferredWidth: implicitWidth
                        Layout.preferredHeight: implicitHeight
                        Layout.fillWidth: true
                        defaultPadding: 0
                        onClicked: if (root.commands)
                            root.commands.rotateRight.trigger()
                    }
                    CustomButton {
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:/GeoControls/icons/FlipHorizontal.svg"
                        tooltipText: qsTr("Flip Horizontal")
                        enabled: root.hasSelection
                        implicitWidth: Fonts.iconButtonSize
                        implicitHeight: Fonts.iconButtonSize
                        Layout.preferredWidth: implicitWidth
                        Layout.preferredHeight: implicitHeight
                        Layout.fillWidth: true
                        defaultPadding: 0
                        onClicked: if (root.commands)
                            root.commands.flipHorizontal.trigger()
                    }
                    CustomButton {
                        display: AbstractButton.IconOnly
                        icon.source: "qrc:/GeoControls/icons/FlipVertical.svg"
                        tooltipText: qsTr("Flip Vertical")
                        enabled: root.hasSelection
                        implicitWidth: Fonts.iconButtonSize
                        implicitHeight: Fonts.iconButtonSize
                        Layout.preferredWidth: implicitWidth
                        Layout.preferredHeight: implicitHeight
                        Layout.fillWidth: true
                        defaultPadding: 0
                        onClicked: if (root.commands)
                            root.commands.flipVertical.trigger()
                    }
                }
                CustomButton {
                    Layout.fillWidth: true
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
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Fonts.size6
                    Repeater {
                        model: [
                            {
                                "label": qsTr("Auto"),
                                "mode": "full"
                            },
                            {
                                "label": qsTr("Vertical"),
                                "mode": "vertical"
                            },
                            {
                                "label": qsTr("Horizontal"),
                                "mode": "horizontal"
                            }
                        ]
                        delegate: CustomButton {
                            required property var modelData
                            Layout.fillWidth: true
                            text: modelData.label
                            enabled: root.hasSelection
                            tooltipText: qsTr("Analyze visible lines and apply a bounded perspective correction")
                            onClicked: if (root.commands)
                                root.commands.autoPerspective(modelData.mode)
                        }
                    }
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Angle"),
                            "key": "angle",
                            "field": "straighten",
                            "minimum": -45,
                            "maximum": 45,
                            "step": 0.1,
                            "decimals": 1
                        },
                        {
                            "title": qsTr("Vertical"),
                            "key": "vertical",
                            "field": "perspectiveVertical",
                            "minimum": -2,
                            "maximum": 2,
                            "step": 0.01,
                            "decimals": 2
                        },
                        {
                            "title": qsTr("Horizontal"),
                            "key": "horizontal",
                            "field": "perspectiveHorizontal",
                            "minimum": -2,
                            "maximum": 2,
                            "step": 0.01,
                            "decimals": 2
                        },
                        {
                            "title": qsTr("Shear"),
                            "key": "shear",
                            "field": "perspectiveShear",
                            "minimum": -0.5,
                            "maximum": 0.5,
                            "step": 0.005,
                            "decimals": 3
                        }
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
                        resetValue: 0
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? (modelData.field === "straighten" ? root.presenter.editStraighten : root.presenter.editPerspective[modelData.key]) : 0
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl(modelData.field)
                    }
                }
                CustomCheckBox {
                    id: perspectiveConstrainCropBox
                    objectName: "perspectiveConstrainCrop"
                    text: qsTr("Constrain crop")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editPerspective.constrainCrop
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("perspectiveConstrainCrop", checked ? 1 : 0)
                }
                Connections {
                    target: root.presenter
                    function onEditChanged() {
                        const constrained = root.hasPresenter && root.presenter.editPerspective.constrainCrop;
                        if (perspectiveConstrainCropBox.checked !== constrained)
                            perspectiveConstrainCropBox.checked = constrained;
                    }
                    function onSelectionChanged() {
                        perspectiveConstrainCropBox.checked = root.hasPresenter && root.presenter.editPerspective.constrainCrop;
                    }
                }
                CustomComboBox {
                    objectName: "perspectiveInterpolation"
                    Layout.fillWidth: true
                    enabled: root.hasSelection
                    model: [qsTr("Bilinear — fast"), qsTr("Lanczos 2"), qsTr("Lanczos 3 — best quality")]
                    currentIndex: root.hasPresenter ? root.presenter.editPerspective.interpolationIndex : 2
                    Accessible.name: qsTr("Perspective interpolation")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("perspectiveInterpolationIndex", index);
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Fonts.size6
                    CustomComboBox {
                        Layout.fillWidth: true
                        model: ["free", "1:1", "3:2", "4:3", "5:4", "16:9"]
                        enabled: root.hasSelection
                        displayText: root.hasPresenter && root.presenter.cropAspect === "locked" ? qsTr("Custom") : currentText
                        currentIndex: {
                            const aspects = ["free", "1:1", "3:2", "4:3", "5:4", "16:9"];
                            const current = root.hasPresenter ? root.presenter.cropAspect : "free";
                            return aspects.indexOf(current);
                        }
                        onActivated: if (root.commands)
                            root.commands.setCropAspect(currentText)
                    }
                    CustomButton {
                        display: AbstractButton.IconOnly
                        checkable: true
                        checked: root.hasPresenter && root.presenter.cropAspect !== "free"
                        icon.source: checked ? "qrc:/GeoControls/icons/Lock.svg" : "qrc:/GeoControls/icons/Unlock.svg"
                        tooltipText: checked ? qsTr("Unlock aspect ratio") : qsTr("Lock aspect ratio")
                        enabled: root.hasSelection
                        implicitWidth: Fonts.iconButtonSize
                        implicitHeight: Fonts.iconButtonSize
                        Layout.preferredWidth: implicitWidth
                        Layout.preferredHeight: implicitHeight
                        defaultPadding: 0
                        onToggled: if (root.commands)
                            root.commands.setCropAspect(checked ? "locked" : "free")
                    }
                }
                CustomCheckBox {
                    id: canvasEnabledBox
                    objectName: "canvasEnabled"
                    text: qsTr("Enlarge Canvas")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editCanvasEnabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("canvasEnabled", checked ? 1 : 0)
                }
                Connections {
                    target: root.presenter
                    function onEditChanged() {
                        const enabled = root.hasPresenter && root.presenter.editCanvasEnabled;
                        if (canvasEnabledBox.checked !== enabled)
                            canvasEnabledBox.checked = enabled;
                    }
                    function onSelectionChanged() {
                        canvasEnabledBox.checked = root.hasPresenter && root.presenter.editCanvasEnabled;
                    }
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Canvas left (%)"),
                            "key": "left",
                            "field": "canvasLeft"
                        },
                        {
                            "title": qsTr("Canvas right (%)"),
                            "key": "right",
                            "field": "canvasRight"
                        },
                        {
                            "title": qsTr("Canvas top (%)"),
                            "key": "top",
                            "field": "canvasTop"
                        },
                        {
                            "title": qsTr("Canvas bottom (%)"),
                            "key": "bottom",
                            "field": "canvasBottom"
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? implicitHeight : 0
                        Layout.maximumHeight: visible ? 65535 : 0
                        visible: canvasEnabledBox.checked
                        title: modelData.title
                        from: 0
                        to: 100
                        stepSize: 0.1
                        validatorDecimals: 1
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editCanvas[modelData.key] : 0
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                    }
                }
                CustomComboBox {
                    objectName: "canvasColor"
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    Layout.maximumHeight: visible ? 65535 : 0
                    visible: canvasEnabledBox.checked
                    enabled: root.hasSelection
                    textRole: "label"
                    model: root.hasPresenter ? root.presenter.editCanvas.colorChoices : []
                    currentIndex: root.hasPresenter ? root.presenter.editCanvas.colorIndex : 0
                    Accessible.name: qsTr("Canvas color")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("canvasColorIndex", model[index].index);
                    }
                }
                CustomButton {
                    Layout.preferredHeight: visible ? implicitHeight : 0
                    Layout.maximumHeight: visible ? 65535 : 0
                    visible: canvasEnabledBox.checked
                    text: qsTr("Reset canvas")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("canvas")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("toneEqBlacks", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("toneEqShadows", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("toneEqMidtones", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("toneEqHighlights", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("toneEqWhites", value);
                    }
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
            title: qsTr("Graduated ND")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("graduatedDensity", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("graduatedRotation", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("graduatedRotation", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("graduatedRotation")
                }
                MaskEditor {
                    objectName: "graduatedMaskEditor"
                    mask: root.hasPresenter ? root.presenter.editGraduatedMask : ({})
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
                    from: -1
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVignette : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vignette", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("vignette", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("vignette")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Vignette midpoint")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0.8
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVignetteParams.midpoint : 0.8
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vignetteMidpoint", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("vignetteMidpoint", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("vignetteMidpoint")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Vignette feather")
                    from: 0.05
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0.5
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVignetteParams.falloff : 0.5
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vignetteFalloff", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("vignetteFalloff", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("vignetteFalloff")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Vignette roundness")
                    from: 0.5
                    to: 5
                    stepSize: 0.05
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 1
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVignetteParams.shape : 1
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vignetteShape", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("vignetteShape", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("vignetteShape")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Vignette center X")
                    from: -1
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVignetteParams.centerX : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vignetteCenterX", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("vignetteCenterX", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("vignetteCenterX")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Vignette center Y")
                    from: -1
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editVignetteParams.centerY : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("vignetteCenterY", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("vignetteCenterY", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("vignetteCenterY")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("bloom", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("soften", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("dehaze", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("dehaze", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("dehaze")
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Distance")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0.2
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editDehazeDistance : 0.2
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("dehazeDistance", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("dehazeDistance", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("dehazeDistance")
                }
                CustomCheckBox {
                    text: qsTr("Adaptive window scale")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editDehazeAdaptive
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("dehazeAdaptive", checked ? 1 : 0)
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Output Dither / Posterize")
                    font.bold: true
                }
                CustomCheckBox {
                    objectName: "outputDitherEnabled"
                    text: qsTr("Enable output dither")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editOutputDither.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("outputDitherEnabled", checked ? 1 : 0)
                }
                CustomComboBox {
                    id: outputDitherMethodCombo
                    objectName: "outputDitherMethod"
                    Layout.fillWidth: true
                    enabled: root.hasSelection
                    textRole: "label"
                    model: root.hasPresenter ? root.presenter.editOutputDither.methodChoices : []
                    currentIndex: root.hasPresenter ? root.presenter.editOutputDither.methodIndex : 10
                    Accessible.name: qsTr("Output dither method")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("outputDitherMethodIndex", model[index].index);
                    }
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("Random damping (dB)")
                    from: root.hasPresenter ? root.presenter.editOutputDither.dampingMinimum : -200
                    to: root.hasPresenter ? root.presenter.editOutputDither.dampingMaximum : 0
                    stepSize: 0.1
                    validatorDecimals: 1
                    showReset: true
                    resetValue: -100
                    delayedCommit: true
                    visible: root.hasPresenter && root.presenter.editOutputDither.dampingVisible
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editOutputDither.dampingDb : -100
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("outputDitherDamping", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("outputDitherDamping", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("outputDitherDamping")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.72
                    text: qsTr("Auto dithers integer exports; previews and float output are only clipped.")
                }
                CustomButton {
                    text: qsTr("Reset output dither")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("outputDither")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Frame / Border")
                    font.bold: true
                }
                CustomCheckBox {
                    objectName: "outputFrameEnabled"
                    text: qsTr("Enable frame")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editOutputFrame.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("outputFrameEnabled", checked ? 1 : 0)
                }
                RowLayout {
                    Layout.fillWidth: true
                    CustomComboBox {
                        objectName: "outputFrameOrientation"
                        Layout.fillWidth: true
                        textRole: "label"
                        model: root.hasPresenter ? root.presenter.editOutputFrame.orientationChoices : []
                        currentIndex: root.hasPresenter ? root.presenter.editOutputFrame.orientationIndex : 0
                        Accessible.name: qsTr("Frame orientation")
                        onActivated: function (index) {
                            if (root.commands)
                                root.commands.setDevelopNumber("outputFrameOrientationIndex", model[index].index);
                        }
                    }
                    CustomComboBox {
                        objectName: "outputFrameBasis"
                        Layout.fillWidth: true
                        textRole: "label"
                        model: root.hasPresenter ? root.presenter.editOutputFrame.basisChoices : []
                        currentIndex: root.hasPresenter ? root.presenter.editOutputFrame.basisIndex : 0
                        Accessible.name: qsTr("Frame size basis")
                        onActivated: function (index) {
                            if (root.commands)
                                root.commands.setDevelopNumber("outputFrameBasisIndex", model[index].index);
                        }
                    }
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Outer aspect (-1 constant, 0 image)"),
                            "key": "aspect",
                            "field": "outputFrameAspect",
                            "from": -1,
                            "to": 3,
                            "reset": -1
                        },
                        {
                            "title": qsTr("Border size"),
                            "key": "size",
                            "field": "outputFrameSize",
                            "from": 0,
                            "to": 0.5,
                            "reset": 0.1
                        },
                        {
                            "title": qsTr("Horizontal position"),
                            "key": "positionH",
                            "field": "outputFramePositionH",
                            "from": 0,
                            "to": 1,
                            "reset": 0.5
                        },
                        {
                            "title": qsTr("Vertical position"),
                            "key": "positionV",
                            "field": "outputFramePositionV",
                            "from": 0,
                            "to": 1,
                            "reset": 0.5
                        },
                        {
                            "title": qsTr("Frame line size"),
                            "key": "lineSize",
                            "field": "outputFrameLineSize",
                            "from": 0,
                            "to": 1,
                            "reset": 0
                        },
                        {
                            "title": qsTr("Frame line offset"),
                            "key": "lineOffset",
                            "field": "outputFrameLineOffset",
                            "from": 0,
                            "to": 1,
                            "reset": 0.5
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.title
                        from: modelData.from
                        to: modelData.to
                        stepSize: 0.01
                        validatorDecimals: 2
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editOutputFrame[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                    }
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Border red"),
                            "key": "borderRed",
                            "field": "outputFrameBorderRed",
                            "reset": 1
                        },
                        {
                            "title": qsTr("Border green"),
                            "key": "borderGreen",
                            "field": "outputFrameBorderGreen",
                            "reset": 1
                        },
                        {
                            "title": qsTr("Border blue"),
                            "key": "borderBlue",
                            "field": "outputFrameBorderBlue",
                            "reset": 1
                        },
                        {
                            "title": qsTr("Frame red"),
                            "key": "lineRed",
                            "field": "outputFrameLineRed",
                            "reset": 0
                        },
                        {
                            "title": qsTr("Frame green"),
                            "key": "lineGreen",
                            "field": "outputFrameLineGreen",
                            "reset": 0
                        },
                        {
                            "title": qsTr("Frame blue"),
                            "key": "lineBlue",
                            "field": "outputFrameLineBlue",
                            "reset": 0
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.title
                        from: 0
                        to: 1
                        stepSize: 0.01
                        validatorDecimals: 2
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editOutputFrame[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                    }
                }
                CustomButton {
                    text: qsTr("Reset frame")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("outputFrame")
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Text Watermark")
                    font.bold: true
                }
                CustomCheckBox {
                    objectName: "watermarkEnabled"
                    text: qsTr("Enable watermark")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editWatermark.enabled
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("watermarkEnabled", checked ? 1 : 0)
                }
                RowLayout {
                    Layout.fillWidth: true
                    CustomLabel {
                        text: qsTr("Text")
                    }
                    CustomTextField {
                        objectName: "watermarkText"
                        Layout.fillWidth: true
                        maximumLength: 256
                        showEmptyIndicator: false
                        showClipIndicator: false
                        enabled: root.hasSelection
                        text: root.hasPresenter ? root.presenter.editWatermark.text : "RAVO"
                        onEditingCommitted: function (committedText) {
                            if (root.commands)
                                root.commands.setDevelopText("watermarkText", committedText);
                        }
                    }
                }
                CustomComboBox {
                    objectName: "watermarkAlignment"
                    Layout.fillWidth: true
                    textRole: "label"
                    model: root.hasPresenter ? root.presenter.editWatermark.alignmentChoices : []
                    currentIndex: root.hasPresenter ? root.presenter.editWatermark.alignmentIndex : 8
                    Accessible.name: qsTr("Watermark alignment")
                    onActivated: function (index) {
                        if (root.commands)
                            root.commands.setDevelopNumber("watermarkAlignmentIndex", model[index].index);
                    }
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Watermark opacity"),
                            "key": "opacity",
                            "field": "watermarkOpacity",
                            "from": 0,
                            "to": 1,
                            "reset": 0.5,
                            "step": 0.01
                        },
                        {
                            "title": qsTr("Text height (% short side)"),
                            "key": "scale",
                            "field": "watermarkScale",
                            "from": 0.5,
                            "to": 50,
                            "reset": 8,
                            "step": 0.5
                        },
                        {
                            "title": qsTr("Horizontal offset"),
                            "key": "offsetX",
                            "field": "watermarkOffsetX",
                            "from": -1,
                            "to": 1,
                            "reset": 0,
                            "step": 0.01
                        },
                        {
                            "title": qsTr("Vertical offset"),
                            "key": "offsetY",
                            "field": "watermarkOffsetY",
                            "from": -1,
                            "to": 1,
                            "reset": 0,
                            "step": 0.01
                        },
                        {
                            "title": qsTr("Watermark rotation"),
                            "key": "rotation",
                            "field": "watermarkRotation",
                            "from": -180,
                            "to": 180,
                            "reset": 0,
                            "step": 1
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.title
                        from: modelData.from
                        to: modelData.to
                        stepSize: modelData.step
                        validatorDecimals: modelData.step < 1 ? 2 : 0
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editWatermark[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                    }
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Watermark red"),
                            "key": "red",
                            "field": "watermarkRed",
                            "reset": 1
                        },
                        {
                            "title": qsTr("Watermark green"),
                            "key": "green",
                            "field": "watermarkGreen",
                            "reset": 1
                        },
                        {
                            "title": qsTr("Watermark blue"),
                            "key": "blue",
                            "field": "watermarkBlue",
                            "reset": 1
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.title
                        from: 0
                        to: 1
                        stepSize: 0.01
                        validatorDecimals: 2
                        showReset: false
                        delayedCommit: true
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editWatermark[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber(modelData.field, value);
                        }
                    }
                }
                CustomLabel {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.72
                    text: qsTr("Portable fixed 5×7 text. Supported tokens: {stem}, {asset_id}.")
                }
                CustomButton {
                    text: qsTr("Reset watermark")
                    enabled: root.hasSelection
                    onClicked: if (root.commands)
                        root.commands.resetControl("watermark")
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
                    title: qsTr("Texture")
                    from: -100
                    to: 100
                    stepSize: 1
                    validatorDecimals: 0
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editTexture.strength * 50 : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("texture", value / 50);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("texture", value / 50);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("texture")
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
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editTexture.detailThreshold : 0.2
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber("textureDetailThreshold", value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber("textureDetailThreshold", value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl("textureDetailThreshold")
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
                        enabled: root.hasSelection
                        value: root.hasPresenter ? root.presenter.editTexture.iterations : 1
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber("textureIterations", value);
                        }
                        onValueCommitted: function (value) {
                            if (root.commands)
                                root.commands.setDevelopNumber("textureIterations", value);
                        }
                        onResetRequested: if (root.commands)
                            root.commands.resetControl("textureIterations")
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
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSharpen : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("sharpen", value);
                    }
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
                    to: 8
                    stepSize: 0.1
                    validatorDecimals: 1
                    showReset: true
                    resetValue: 2
                    delayedCommit: true
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSharpenRadius : 2
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("sharpenRadius", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("sharpenRadius", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("sharpenRadius")
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
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editSharpenThreshold : 0.5
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("sharpenThreshold", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("sharpenThreshold", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("sharpenThreshold")
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
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editDenoise : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("denoise", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("denoise", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("denoise")
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
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editDenoiseChroma : 1
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("denoiseChroma", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("denoiseChroma", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("denoiseChroma")
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
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editDenoiseRadius : 1
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("denoiseRadius", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("denoiseRadius", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("denoiseRadius")
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
                        onValueEdited: function (value) {
                            retouchEditor.draftSourceY = value;
                        }
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        visible: retouchEditor.draftMode === 2
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
                        onValueEdited: function (value) {
                            retouchEditor.draftBlurRadius = value;
                        }
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        visible: retouchEditor.draftMode === 3
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
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
                        enabled: root.hasSelection
                        onValueEdited: function (value) {
                            retouchEditor.draftFillBrightness = value;
                        }
                    }
                    Button {
                        Layout.fillWidth: true
                        text: qsTr("Add retouch region")
                        enabled: root.hasSelection && root.commands
                        onClicked: root.commands.addRetouchRegion({
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
                        text: qsTr("Regions: %1").arg(root.hasPresenter ? root.presenter.editRetouch.regionCount : 0)
                        opacity: 0.72
                    }
                    Repeater {
                        model: root.hasPresenter ? root.presenter.editRetouch.regions : []
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
                                enabled: root.hasSelection && root.commands
                                onClicked: root.commands.removeRetouchRegion(modelData.index)
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
                    enabled: root.hasSelection
                    value: root.hasPresenter ? root.presenter.editClarity : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("clarity", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("grain", value);
                    }
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
            title: qsTr("RAW Repair / Denoise / Lens")
            sectionId: "raw"
            ColumnLayout {
                Layout.fillWidth: true
                width: parent.width
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Demosaicing")
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Auto — RCD / Markesteijn 3"), qsTr("PPG — Bayer compatibility"), qsTr("Markesteijn 1 — X-Trans fast"), qsTr("Markesteijn 3 — X-Trans quality")]
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.selectedMediaType === "image/x-raw"
                    currentIndex: root.hasPresenter ? root.presenter.editDemosaicModeIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("demosaicModeIndex", currentIndex)
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: qsTr("Auto selects RCD for Bayer and Markesteijn 3-pass for X-Trans. Explicit sensor-mismatched modes fail instead of changing algorithms.")
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                }
                CustomSlider {
                    Layout.fillWidth: true
                    title: qsTr("RAW wavelet denoise")
                    from: 0
                    to: 1
                    stepSize: 0.01
                    validatorDecimals: 2
                    showReset: true
                    resetValue: 0
                    delayedCommit: true
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.selectedMediaType === "image/x-raw"
                    value: root.hasPresenter ? root.presenter.editRawDenoiseThreshold : 0
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("rawDenoiseThreshold", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("rawDenoiseThreshold", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("rawDenoiseThreshold")
                }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("hotPixelsStrength", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("hotPixelsThreshold", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("rawCaIterations", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("rawHighlights", value);
                    }
                    onValueCommitted: function (value) {
                        if (root.commands)
                            root.commands.setDevelopNumber("rawHighlights", value);
                    }
                    onResetRequested: if (root.commands)
                        root.commands.resetControl("rawHighlights")
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("lensK1", value);
                    }
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
                    onValueEdited: function (value) {
                        if (root.liveReady && root.commands)
                            root.commands.previewDevelopNumber("lensVignetting", value);
                    }
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
            title: qsTr("Input Profile")
            sectionId: "inputProfile"
            ColumnLayout {
                Layout.fillWidth: true
                width: parent.width
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Source metadata"), qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"), qsTr("Linear ProPhoto RGB"), qsTr("Display P3"), qsTr("HLG P3")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editInputColor.inputProfileIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("inputProfile", currentIndex)
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Linear ProPhoto RGB"), qsTr("Display P3"), qsTr("Adobe RGB")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editInputColor.workingProfileIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("workingProfile", currentIndex)
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Perceptual"), qsTr("Relative colorimetric"), qsTr("Saturation"), qsTr("Absolute colorimetric")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editInputColor.intentIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("renderingIntent", currentIndex)
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("No gamut clipping"), qsTr("Clip to sRGB"), qsTr("Clip to Adobe RGB"), qsTr("Clip to linear Rec709"), qsTr("Clip to linear Rec2020")]
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
                    text: root.hasPresenter ? qsTr("%1 → %2").arg(root.presenter.editInputColor.inputProfile).arg(root.presenter.editInputColor.workingProfile) : ""
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
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editProfileGamma.enabled
                    currentIndex: root.hasPresenter ? root.presenter.editProfileGamma.modeIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("profileGammaModeIndex", currentIndex)
                }
                Repeater {
                    model: [
                        {
                            "title": qsTr("Dynamic range"),
                            "key": "dynamicRange",
                            "field": "profileGammaDynamicRange",
                            "minimum": 0.01,
                            "maximum": 32,
                            "reset": 10,
                            "step": 0.01,
                            "decimals": 2
                        },
                        {
                            "title": qsTr("Middle gray luma"),
                            "key": "greyPoint",
                            "field": "profileGammaGreyPoint",
                            "minimum": 0.1,
                            "maximum": 100,
                            "reset": 18,
                            "step": 0.1,
                            "decimals": 1
                        },
                        {
                            "title": qsTr("Black relative exposure"),
                            "key": "shadowsRange",
                            "field": "profileGammaShadowsRange",
                            "minimum": -16,
                            "maximum": 16,
                            "reset": -5,
                            "step": 0.05,
                            "decimals": 2
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        visible: root.hasPresenter && root.presenter.editProfileGamma.modeIndex === 0
                        title: modelData.title
                        from: modelData.minimum
                        to: modelData.maximum
                        stepSize: modelData.step
                        validatorDecimals: modelData.decimals
                        showReset: true
                        resetValue: modelData.reset
                        delayedCommit: true
                        enabled: root.hasSelection && root.hasPresenter && root.presenter.editProfileGamma.enabled
                        value: root.hasPresenter ? root.presenter.editProfileGamma[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
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
                        {
                            "title": qsTr("Linear part"),
                            "key": "linear",
                            "field": "profileGammaLinear",
                            "minimum": 0,
                            "maximum": 1,
                            "reset": 0.1,
                            "step": 0.0001,
                            "decimals": 4
                        },
                        {
                            "title": qsTr("Gamma exponent"),
                            "key": "gamma",
                            "field": "profileGammaGamma",
                            "minimum": 0,
                            "maximum": 1,
                            "reset": 0.45,
                            "step": 0.0001,
                            "decimals": 4
                        }
                    ]
                    delegate: CustomSlider {
                        required property var modelData
                        Layout.fillWidth: true
                        visible: root.hasPresenter && root.presenter.editProfileGamma.modeIndex === 1
                        title: modelData.title
                        from: modelData.minimum
                        to: modelData.maximum
                        stepSize: modelData.step
                        validatorDecimals: modelData.decimals
                        showReset: true
                        resetValue: modelData.reset
                        delayedCommit: true
                        enabled: root.hasSelection && root.hasPresenter && root.presenter.editProfileGamma.enabled
                        value: root.hasPresenter ? root.presenter.editProfileGamma[modelData.key] : modelData.reset
                        onValueEdited: function (value) {
                            if (root.liveReady && root.commands)
                                root.commands.previewDevelopNumber(modelData.field, value);
                        }
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
                    model: [qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"), qsTr("Linear ProPhoto RGB"), qsTr("PQ Rec2020"), qsTr("HLG Rec2020"), qsTr("PQ P3"), qsTr("HLG P3"), qsTr("Display P3")]
                    enabled: root.hasSelection
                    currentIndex: root.hasPresenter ? root.presenter.editOutputColor.outputProfileIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("outputProfile", currentIndex)
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Perceptual"), qsTr("Relative colorimetric"), qsTr("Saturation"), qsTr("Absolute colorimetric")]
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
                    model: [qsTr("sRGB"), qsTr("Adobe RGB"), qsTr("Linear Rec709"), qsTr("Linear Rec2020"), qsTr("Rec709"), qsTr("Linear ProPhoto RGB"), qsTr("PQ Rec2020"), qsTr("HLG Rec2020"), qsTr("PQ P3"), qsTr("HLG P3"), qsTr("Display P3")]
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editOutputColor.proofModeIndex !== 0
                    currentIndex: root.hasPresenter ? root.presenter.editOutputColor.proofProfileIndex : 0
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("proofProfile", currentIndex)
                }
                CustomComboBox {
                    Layout.fillWidth: true
                    model: [qsTr("Perceptual"), qsTr("Relative colorimetric"), qsTr("Saturation"), qsTr("Absolute colorimetric")]
                    enabled: root.hasSelection && root.hasPresenter && root.presenter.editOutputColor.proofModeIndex !== 0
                    currentIndex: root.hasPresenter ? root.presenter.editOutputColor.proofIntentIndex : 1
                    onActivated: if (root.commands)
                        root.commands.setDevelopNumber("proofIntent", currentIndex)
                }
                CustomCheckBox {
                    text: qsTr("Black-point compensation")
                    enabled: root.hasSelection
                    checked: root.hasPresenter && root.presenter.editOutputColor.blackPointCompensation
                    onToggled: if (root.liveReady && root.commands)
                        root.commands.setDevelopNumber("outputBlackPointCompensation", checked ? 1 : 0)
                }
                CustomLabel {
                    Layout.fillWidth: true
                    text: root.hasPresenter ? qsTr("%1 · %2 · proof %3").arg(root.presenter.editOutputColor.outputProfile).arg(root.presenter.editOutputColor.proofMode).arg(root.presenter.editOutputColor.proofProfile) : ""
                    wrapMode: Text.WordWrap
                    opacity: 0.75
                }
            }
        }
    }
}
