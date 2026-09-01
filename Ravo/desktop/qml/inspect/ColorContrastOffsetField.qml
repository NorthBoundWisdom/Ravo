pragma Translator: "DevelopPanel"

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

RowLayout {
    required property var panel
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
        enabled: panel.hasSelection
        validator: DoubleValidator {
            bottom: -3.4028234663852886e38
            top: 3.4028234663852886e38
            decimals: 9
            notation: DoubleValidator.ScientificNotation
        }
        text: panel.hasPresenter ? Number(panel.presenter.editColorContrast[modelData.key]).toString() : "0"
        onEditingCommitted: function (committedText) {
            const parsed = Number(committedText);
            if (Number.isFinite(parsed) && panel.commands)
                panel.commands.setDevelopNumber(modelData.field, parsed);
        }
    }
    CustomButton {
        text: qsTr("Reset")
        enabled: panel.hasSelection
        onClicked: if (panel.commands)
            panel.commands.resetControl(modelData.field)
    }
}
