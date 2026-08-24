import QtQuick

// Neutral-gray night chrome. Property names must match GeoControls Theme.helper.
QtObject {
    readonly property int appearance: 1

    readonly property color windowColor: "#141414"
    readonly property color windowTextColor: "#e6e6e6"
    readonly property color baseColor: "#1c1c1c"
    readonly property color alternateBaseColor: "#171717"
    readonly property color textColor: "#e6e6e6"
    readonly property color buttonColor: "#2a2a2a"
    readonly property color buttonTextColor: "#e6e6e6"
    readonly property color lightColor: "#3d3d3d"
    readonly property color darkColor: "#0a0a0a"
    readonly property color midlightColor: "#333333"
    readonly property color midColor: "#4a4a4a"
    readonly property color shadowColor: "#99000000"
    readonly property color highlightColor: "#d0d0d0"
    readonly property color highlightedTextColor: "#111111"
    readonly property color placeholderTextColor: "#8a8a8a"
    readonly property color accentColor: highlightColor
    readonly property color linkColor: highlightColor
    readonly property color disabledTextColor: "#6a6a6a"
    readonly property color buttonPressedColor: "#1f1f1f"
    readonly property color buttonHoveredColor: "#333333"
    readonly property color buttonDisabledColor: "#1c1c1c"
    readonly property color dividerColor: "#3a3a3a"
    readonly property color railSurfaceColor: "#2a2a2a"
    readonly property color inputSurfaceColor: "#171717"
    readonly property color pageSurfaceColor: "#161616"
    readonly property color contentSurfaceColor: "#1a1a1a"
    readonly property color popupSurfaceColor: "#222222"
    readonly property color actionButtonColor: "#2a2a2a"
    readonly property color actionButtonHoveredColor: "#353535"
    readonly property color actionButtonPressedColor: "#1f1f1f"
    readonly property color actionButtonBorderColor: dividerColor
    readonly property color infoColor: "#9a9a9a"
    readonly property color successColor: "#67bd72"
    readonly property color warningColor: "#e9bd4f"
    readonly property color errorColor: "#d85a5a"

    // Copied from the GeoControls system font so CJK glyphs keep falling back.
    property font appFont: Qt.application.font
    property font monoFont: Qt.application.font
}
