import QtQuick

// Charcoal-gray night workspace. Property names must match GeoControls Theme.helper.
QtObject {
    readonly property int appearance: 1

    readonly property color windowColor: "#242424"
    readonly property color windowTextColor: "#E2E2E2"
    readonly property color baseColor: "#2D2D2D"
    readonly property color alternateBaseColor: "#282828"
    readonly property color textColor: "#E2E2E2"
    readonly property color buttonColor: "#404040"
    readonly property color buttonTextColor: "#E6E6E6"
    readonly property color lightColor: "#5A5A5A"
    readonly property color darkColor: "#181818"
    readonly property color midlightColor: "#4B4B4B"
    readonly property color midColor: "#5C5C5C"
    readonly property color shadowColor: "#99000000"
    readonly property color highlightColor: "#C8C8C8"
    readonly property color highlightedTextColor: "#1A1A1A"
    readonly property color placeholderTextColor: "#999999"
    readonly property color accentColor: highlightColor
    readonly property color linkColor: highlightColor
    readonly property color disabledTextColor: "#707070"
    readonly property color buttonPressedColor: "#353535"
    readonly property color buttonHoveredColor: "#4B4B4B"
    readonly property color buttonDisabledColor: "#323232"
    readonly property color dividerColor: "#4D4D4D"
    readonly property color splitHandleColor: "#4D4D4D"
    readonly property color railSurfaceColor: "#353535"
    readonly property color inputSurfaceColor: "#303030"
    readonly property color pageSurfaceColor: "#292929"
    readonly property color contentSurfaceColor: "#303030"
    readonly property color popupSurfaceColor: "#3A3A3A"
    readonly property color actionButtonColor: "#404040"
    readonly property color actionButtonHoveredColor: "#4C4C4C"
    readonly property color actionButtonPressedColor: "#343434"
    readonly property color actionButtonBorderColor: "#555555"
    readonly property color infoColor: "#999999"
    readonly property color successColor: "#67BD72"
    readonly property color warningColor: "#E9BD4F"
    readonly property color errorColor: "#D85A5A"

    readonly property color photographicMiddleGray: "#767676"
    readonly property color toolbarSurfaceColor: "#2D2D2D"
    readonly property color viewerToolbarColor: "#353535"
    readonly property color imageSurroundColor: photographicMiddleGray
    readonly property color selectedBorderColor: "#E2E2E2"
    readonly property color selectedSecondaryBorderColor: "#999999"

    property font appFont: Qt.application.font
    property font monoFont: Qt.application.font
}
