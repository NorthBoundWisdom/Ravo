import QtQuick

// Single Lightroom-like studio palette. Property names match GeoControls Theme.helper.
QtObject {
    readonly property int appearance: 1

    readonly property color windowColor: "#1c1c1c"
    readonly property color windowTextColor: "#e6e6e6"
    readonly property color baseColor: "#2b2b2b"
    readonly property color alternateBaseColor: "#323232"
    readonly property color textColor: "#e6e6e6"
    readonly property color buttonColor: "#3a3a3a"
    readonly property color buttonTextColor: "#e8e8e8"
    readonly property color lightColor: "#5a5a5a"
    readonly property color darkColor: "#121212"
    readonly property color midlightColor: "#404040"
    readonly property color midColor: "#5c5c5c"
    readonly property color shadowColor: "#99000000"
    readonly property color highlightColor: "#c8c8c8"
    readonly property color highlightedTextColor: "#1a1a1a"
    readonly property color placeholderTextColor: "#9a9a9a"
    readonly property color accentColor: highlightColor
    readonly property color linkColor: highlightColor
    readonly property color disabledTextColor: "#6e6e6e"
    readonly property color buttonPressedColor: "#2f2f2f"
    readonly property color buttonHoveredColor: "#4a4a4a"
    readonly property color buttonDisabledColor: "#2a2a2a"
    readonly property color dividerColor: "#141414"
    readonly property color splitHandleColor: "#141414"
    readonly property color railSurfaceColor: "#2a2a2a"
    readonly property color inputSurfaceColor: "#333333"
    readonly property color pageSurfaceColor: "#252525"
    readonly property color contentSurfaceColor: "#3c3c3c"
    readonly property color popupSurfaceColor: "#3a3a3a"
    readonly property color actionButtonColor: "#3a3a3a"
    readonly property color actionButtonHoveredColor: "#4a4a4a"
    readonly property color actionButtonPressedColor: "#2f2f2f"
    readonly property color actionButtonBorderColor: "#555555"
    readonly property color infoColor: "#9a9a9a"
    readonly property color successColor: "#67BD72"
    readonly property color warningColor: "#E9BD4F"
    readonly property color errorColor: "#D85A5A"

    readonly property color photographicMiddleGray: "#767676"
    readonly property color toolbarSurfaceColor: "#2b2b2b"
    readonly property color viewerToolbarColor: "#2a2a2a"
    readonly property color imageSurroundColor: photographicMiddleGray
    readonly property color selectedBorderColor: "#dcdcdc"
    readonly property color selectedSecondaryBorderColor: "#9a9a9a"

    property font appFont: Qt.application.font
    property font monoFont: Qt.application.font
}
