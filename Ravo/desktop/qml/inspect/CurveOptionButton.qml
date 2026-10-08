pragma Translator: DevelopPanel

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

SegmentedButton {
    id: optionButton
    property color selectionColor: Theme.highlightColor

    defaultHeight: Fonts.inputFieldHeight
    defaultPadding: Fonts.size6
    buttonTextColor: selected ? selectionColor : Theme.buttonTextColor
    highlightedTextColor: selectionColor
    font: selected ? Fonts.makeBoldFont(Fonts.standardFont) : Fonts.standardFont

    buttonColor: selected ? Qt.alpha(selectionColor, 0.16) : Theme.baseColor
    hoveredColor: Qt.alpha(selectionColor, 0.24)
    pressedColor: Qt.alpha(selectionColor, 0.32)
}
