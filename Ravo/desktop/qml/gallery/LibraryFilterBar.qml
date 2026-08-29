import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0

Item {
    id: root
    property var presenter
    property var commands
    property var colorChoices: []
    property var swatchColor: function (name) {
        return Theme.midColor;
    }

    property var extraFilters: []
    readonly property bool hasPresenter: presenter !== null && presenter !== undefined

    implicitHeight: Math.max(Fonts.toolbarHeight, Fonts.inputFieldHeight + Fonts.size12)

    function extraOpen(id) {
        if (root.extraFilters.indexOf(id) >= 0)
            return true;
        if (!root.hasPresenter)
            return false;
        if (id === "search")
            return root.presenter.filterText.length > 0;
        if (id === "type")
            return root.presenter.mediaFilter !== "any";
        if (id === "edits")
            return root.presenter.editFilter !== "any";
        if (id === "color")
            return root.presenter.colorFilters.length > 0;
        if (id === "rejected")
            return root.presenter.rejectFilter !== "include";
        return false;
    }

    function addExtra(id) {
        if (root.extraFilters.indexOf(id) >= 0)
            return;
        root.extraFilters = root.extraFilters.concat([id]);
    }

    function removeExtra(id) {
        root.extraFilters = root.extraFilters.filter(function (item) {
            return item !== id;
        });
        if (!root.commands)
            return;
        if (id === "search")
            root.commands.setTextFilter("");
        else if (id === "type")
            root.commands.setMediaFilter("any");
        else if (id === "edits")
            root.commands.setEditFilter("any");
        else if (id === "rejected")
            root.commands.run(root.commands.ids.librarySetRejectFilter, "include");
        else if (id === "color" && root.hasPresenter) {
            const colors = root.presenter.colorFilters.slice();
            for (let i = 0; i < colors.length; ++i)
                root.commands.run(root.commands.ids.libraryToggleColorFilter, colors[i]);
        }
    }

    function setRatingExact(value) {
        if (!root.commands)
            return;
        const already = root.hasPresenter && root.presenter.ratingFilterMode === "exact" &&
                        root.presenter.ratingFilterValue === value;
        if (already)
            root.commands.run(root.commands.ids.librarySetRatingFilter, {"mode": "any", "value": 0});
        else
            root.commands.run(root.commands.ids.librarySetRatingFilter, {"mode": "exact", "value": value});
    }

    function ratingStarActive(star) {
        if (!root.hasPresenter)
            return false;
        if (root.presenter.ratingFilterMode === "exact")
            return root.presenter.ratingFilterValue >= star && root.presenter.ratingFilterValue > 0;
        if (root.presenter.ratingFilterMode === "min")
            return root.presenter.ratingFilterValue >= star;
        return false;
    }

    readonly property bool ratingUnratedActive: root.hasPresenter &&
                                                root.presenter.ratingFilterMode === "exact" &&
                                                root.presenter.ratingFilterValue === 0

    component FilterCloseButton: CustomButton {
        display: AbstractButton.IconOnly
        icon.source: "qrc:/GeoControls/icons/Close.svg"
        tooltipText: qsTr("Remove filter")
        implicitWidth: Fonts.iconButtonSize
        implicitHeight: Fonts.iconButtonSize
        Layout.preferredWidth: implicitWidth
        Layout.preferredHeight: implicitHeight
        defaultPadding: 0
    }

    component FilterMenuItem: MenuItem {
        id: item
        implicitHeight: visible ? Math.max(Fonts.listItemHeight, Fonts.size24) : 0
        height: implicitHeight
        leftPadding: Fonts.size12
        rightPadding: Fonts.size12
        contentItem: Text {
            text: item.text
            font: Fonts.standardFont
            color: item.enabled ? Theme.textColor : Theme.disabledTextColor
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            color: item.highlighted ? Theme.buttonHoveredColor : Theme.popupSurfaceColor
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: Fonts.smallSpacing

        CustomCheckBox {
            id: filterToggle
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Filter")
            checked: false
            onCheckedChanged: {
                if (checked)
                    return;
                root.extraFilters = [];
                if (root.hasPresenter && root.presenter.filtersActive && root.commands)
                    root.commands.run(root.commands.ids.libraryClearFilters);
            }
        }

        Connections {
            target: root.presenter
            function onFilterChanged() {
                if (root.hasPresenter && root.presenter.filtersActive)
                    filterToggle.checked = true;
            }
        }

        Flickable {
            id: filterScroller
            visible: filterToggle.checked
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.HorizontalFlick
            contentWidth: filterRow.implicitWidth
            contentHeight: height

            RowLayout {
                id: filterRow
                height: filterScroller.height
                spacing: Fonts.smallSpacing

                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: "\u2606"
                    font.pixelSize: Fonts.size16
                    color: root.ratingUnratedActive || unratedMouse.containsMouse ? Theme.warningColor : Theme.midColor
                    opacity: enabled ? 1 : 0.45
                    Accessible.name: qsTr("Unrated")
                    MouseArea {
                        id: unratedMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.setRatingExact(0)
                    }
                }

                Repeater {
                    model: 5
                    delegate: Text {
                        required property int index
                        readonly property int star: index + 1
                        Layout.alignment: Qt.AlignVCenter
                        text: "\u2605"
                        font.pixelSize: Fonts.size16
                        color: root.ratingStarActive(star) || starMouse.containsMouse ? Theme.warningColor : Theme.midColor
                        opacity: enabled ? 1 : 0.45
                        Accessible.name: qsTr("%1 star").arg(star)
                        MouseArea {
                            id: starMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: root.setRatingExact(parent.star)
                        }
                    }
                }

                RowLayout {
                    visible: root.extraOpen("search")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomTextField {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 150
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Search photos")
                        text: root.hasPresenter ? root.presenter.filterText : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setTextFilter(text)
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("search")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("type")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomComboBox {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 105
                        model: [qsTr("Any type"), qsTr("RAW"), qsTr("JPEG"), qsTr("PNG"), qsTr("TIFF")]
                        currentIndex: root.hasPresenter && root.presenter.mediaFilter === "raw" ? 1
                                      : root.hasPresenter && root.presenter.mediaFilter === "jpeg" ? 2
                                      : root.hasPresenter && root.presenter.mediaFilter === "png" ? 3
                                      : root.hasPresenter && root.presenter.mediaFilter === "tiff" ? 4 : 0
                        onActivated: function (index) {
                            if (root.commands)
                                root.commands.setMediaFilter(["any", "raw", "jpeg", "png", "tiff"][index]);
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("type")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("edits")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomComboBox {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 115
                        model: [qsTr("Any edits"), qsTr("Edited"), qsTr("Unedited")]
                        currentIndex: root.hasPresenter && root.presenter.editFilter === "edited" ? 1
                                      : root.hasPresenter && root.presenter.editFilter === "unedited" ? 2 : 0
                        onActivated: function (index) {
                            if (root.commands)
                                root.commands.setEditFilter(index === 1 ? "edited" : (index === 2 ? "unedited" : "any"));
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("edits")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("color")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    Repeater {
                        model: root.colorChoices
                        delegate: Rectangle {
                            required property string modelData
                            Layout.alignment: Qt.AlignVCenter
                            width: 18
                            height: 18
                            radius: 9
                            color: root.swatchColor(modelData)
                            border.width: root.hasPresenter && root.presenter.colorFilters.indexOf(modelData) >= 0 ? 2 : 1
                            border.color: root.hasPresenter && root.presenter.colorFilters.indexOf(modelData) >= 0 ? Theme.textColor : Theme.dividerColor
                            MouseArea {
                                anchors.fill: parent
                                onClicked: if (root.commands)
                                    root.commands.run(root.commands.ids.libraryToggleColorFilter, modelData)
                            }
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("color")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("rejected")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomComboBox {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 120
                        model: [qsTr("Include"), qsTr("Exclude"), qsTr("Only")]
                        currentIndex: root.hasPresenter && root.presenter.rejectFilter === "exclude" ? 1
                                      : root.hasPresenter && root.presenter.rejectFilter === "only" ? 2 : 0
                        onActivated: function (index) {
                            if (root.commands)
                                root.commands.run(root.commands.ids.librarySetRejectFilter,
                                                  index === 1 ? "exclude" : (index === 2 ? "only" : "include"));
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("rejected")
                    }
                }

                CustomButton {
                    id: addFilterButton
                    Layout.alignment: Qt.AlignVCenter
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:/GeoControls/icons/Plus.svg"
                    tooltipText: qsTr("Add filter")
                    enabled: !root.extraOpen("search") || !root.extraOpen("type") || !root.extraOpen("edits") ||
                             !root.extraOpen("color") || !root.extraOpen("rejected")
                    implicitWidth: Fonts.iconButtonSize
                    implicitHeight: Fonts.iconButtonSize
                    Layout.preferredWidth: implicitWidth
                    Layout.preferredHeight: implicitHeight
                    defaultPadding: 0
                    onClicked: addFilterMenu.popup()

                    Menu {
                        id: addFilterMenu
                        y: addFilterButton.height + Fonts.size2
                        modal: true
                        dim: false
                        padding: Fonts.size4
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                        palette.window: Theme.popupSurfaceColor
                        palette.windowText: Theme.textColor
                        palette.text: Theme.textColor
                        palette.highlight: Theme.buttonHoveredColor
                        palette.highlightedText: Theme.textColor
                        background: Rectangle {
                            implicitWidth: 180
                            color: Theme.popupSurfaceColor
                            border.color: Theme.dividerColor
                            border.width: 1
                            radius: 4
                        }
                        FilterMenuItem {
                            text: qsTr("Search")
                            visible: !root.extraOpen("search")
                            onTriggered: root.addExtra("search")
                        }
                        FilterMenuItem {
                            text: qsTr("Type")
                            visible: !root.extraOpen("type")
                            onTriggered: root.addExtra("type")
                        }
                        FilterMenuItem {
                            text: qsTr("Edits")
                            visible: !root.extraOpen("edits")
                            onTriggered: root.addExtra("edits")
                        }
                        FilterMenuItem {
                            text: qsTr("Color")
                            visible: !root.extraOpen("color")
                            onTriggered: root.addExtra("color")
                        }
                        FilterMenuItem {
                            text: qsTr("Rejected")
                            visible: !root.extraOpen("rejected")
                            onTriggered: root.addExtra("rejected")
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: !filterToggle.checked
            Layout.preferredWidth: 0
            visible: !filterToggle.checked
        }

        CustomComboBox {
            Layout.alignment: Qt.AlignVCenter
            model: [qsTr("Import time"), qsTr("Capture time"), qsTr("Filename"), qsTr("Rating"), qsTr("File size")]
            Layout.preferredWidth: 140
            currentIndex: root.hasPresenter && root.presenter.sortField === "captured" ? 1
                          : root.hasPresenter && root.presenter.sortField === "name" ? 2
                          : root.hasPresenter && root.presenter.sortField === "rating" ? 3
                          : root.hasPresenter && root.presenter.sortField === "size" ? 4 : 0
            onActivated: function (index) {
                if (!root.commands || !root.hasPresenter)
                    return;
                const field = ["imported", "captured", "name", "rating", "size"][index];
                root.commands.run(root.commands.ids.librarySetSort,
                                  {"field": field, "direction": root.presenter.sortDirection});
            }
        }
        CustomButton {
            Layout.alignment: Qt.AlignVCenter
            text: root.hasPresenter && root.presenter.sortDirection === "asc" ? qsTr("Asc") : qsTr("Desc")
            onClicked: if (root.commands && root.hasPresenter)
                root.commands.run(root.commands.ids.librarySetSort, {
                                      "field": root.presenter.sortField,
                                      "direction": root.presenter.sortDirection === "asc" ? "desc" : "asc"
                                  })
        }
    }
}
