pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

CustomButton {
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
