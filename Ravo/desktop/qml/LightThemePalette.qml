import QtQuick

// Explicit light-neutral photography workspace. Names match GeoControls Theme.helper.
QtObject {
    readonly property int appearance: 0

    readonly property color windowColor: "#C6C6C6"
    readonly property color windowTextColor: "#202020"
    readonly property color baseColor: "#CECECE"
    readonly property color alternateBaseColor: "#C2C2C2"
    readonly property color textColor: "#202020"
    readonly property color buttonColor: "#D4D4D4"
    readonly property color buttonTextColor: "#202020"
    readonly property color lightColor: "#E2E2E2"
    readonly property color darkColor: "#8F8F8F"
    readonly property color midlightColor: "#CBCBCB"
    readonly property color midColor: "#A5A5A5"
    readonly property color shadowColor: "#66000000"
    readonly property color highlightColor: "#5A5A5A"
    readonly property color highlightedTextColor: "#F2F2F2"
    readonly property color placeholderTextColor: "#686868"
    readonly property color accentColor: highlightColor
    readonly property color linkColor: highlightColor
    readonly property color disabledTextColor: "#858585"
    readonly property color buttonPressedColor: "#BDBDBD"
    readonly property color buttonHoveredColor: "#DEDEDE"
    readonly property color buttonDisabledColor: "#C7C7C7"
    readonly property color dividerColor: "#9A9A9A"
    readonly property color splitHandleColor: "#B0B0B0"
    readonly property color railSurfaceColor: "#C1C1C1"
    readonly property color inputSurfaceColor: "#DCDCDC"
    readonly property color pageSurfaceColor: "#BDBDBD"
    readonly property color contentSurfaceColor: "#B5B5B5"
    readonly property color popupSurfaceColor: "#D2D2D2"
    readonly property color actionButtonColor: "#D0D0D0"
    readonly property color actionButtonHoveredColor: "#DCDCDC"
    readonly property color actionButtonPressedColor: "#BABABA"
    readonly property color actionButtonBorderColor: "#999999"
    readonly property color infoColor: "#686868"
    readonly property color successColor: "#67BD72"
    readonly property color warningColor: "#E9BD4F"
    readonly property color errorColor: "#D85A5A"

    readonly property color photographicMiddleGray: "#767676"
    readonly property color toolbarSurfaceColor: "#CECECE"
    readonly property color viewerToolbarColor: "#C1C1C1"
    readonly property color imageSurroundColor: photographicMiddleGray
    readonly property color selectedBorderColor: "#202020"
    readonly property color selectedSecondaryBorderColor: "#686868"

    property font appFont: Qt.application.font
    property font monoFont: Qt.application.font
}
