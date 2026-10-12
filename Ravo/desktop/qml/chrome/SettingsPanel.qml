import QtQuick
import GeoControls 1.0

CustomEditPanel {
    showAddButton: false
    showDeleteButton: false
    showResetButton: false
    showApplyButton: false
    initialExpanded: true
    // Keep section scrolling stable when wrapping changes the form height.
    animateHeight: false
}
