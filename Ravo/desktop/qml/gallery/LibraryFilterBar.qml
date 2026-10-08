import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0
import "../chrome" as Chrome

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
        if (id === "type")
            return root.presenter.library.mediaFilter !== "any";
        if (id === "color")
            return root.presenter.library.colorFilters.length > 0;
        if (id === "cullFlag")
            return root.presenter.library.cullFlagFilter !== "any";
        return false;
    }

    function addExtra(id) {
        if (root.extraFilters.indexOf(id) < 0)
            root.extraFilters = root.extraFilters.concat([id]);
    }

    function removeExtra(id) {
        root.extraFilters = root.extraFilters.filter(function (item) {
            return item !== id;
        });
        if (!root.commands)
            return;
        if (id === "type")
            root.commands.setMediaFilter("any");
        else if (id === "cullFlag" && root.hasPresenter)
            root.presenter.library.setCullFlagFilter("any");
        else if (id === "color" && root.hasPresenter) {
            const colors = root.presenter.library.colorFilters.slice();
            for (let i = 0; i < colors.length; ++i)
                root.commands.run(root.commands.ids.libraryToggleColorFilter, colors[i]);
        }
    }

    function setRatingExact(value) {
        if (!root.commands)
            return;
        const already = root.hasPresenter && root.presenter.library.ratingFilterMode === "exact" && root.presenter.library.ratingFilterValue === value;
        if (already)
            root.commands.run(root.commands.ids.librarySetRatingFilter, {
                "mode": "any",
                "value": 0
            });
        else
            root.commands.run(root.commands.ids.librarySetRatingFilter, {
                "mode": "exact",
                "value": value
            });
    }

    function ratingStarActive(star) {
        if (!root.hasPresenter)
            return false;
        if (root.presenter.library.ratingFilterMode === "exact")
            return root.presenter.library.ratingFilterValue >= star && root.presenter.library.ratingFilterValue > 0;
        if (root.presenter.library.ratingFilterMode === "min")
            return root.presenter.library.ratingFilterValue >= star;
        return false;
    }

    readonly property bool ratingUnratedActive: root.hasPresenter && root.presenter.library.ratingFilterMode === "exact" && root.presenter.library.ratingFilterValue === 0

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

    component FilterMenuItem: Chrome.StudioContextMenuItem {
        id: item
        implicitHeight: visible ? Math.max(Fonts.listItemHeight, Fonts.size24) : 0
        height: implicitHeight
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
                    visible: root.extraOpen("type")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomComboBox {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 105
                        model: [qsTr("Any type"), qsTr("RAW"), qsTr("JPEG"), qsTr("PNG"), qsTr("TIFF")]
                        currentIndex: root.hasPresenter && root.presenter.library.mediaFilter === "raw" ? 1 : root.hasPresenter && root.presenter.library.mediaFilter === "jpeg" ? 2 : root.hasPresenter && root.presenter.library.mediaFilter === "png" ? 3 : root.hasPresenter && root.presenter.library.mediaFilter === "tiff" ? 4 : 0
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
                            border.width: root.hasPresenter && root.presenter.library.colorFilters.indexOf(modelData) >= 0 ? 2 : 1
                            border.color: root.hasPresenter && root.presenter.library.colorFilters.indexOf(modelData) >= 0 ? Theme.textColor : Theme.dividerColor
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
                    objectName: "cullFlagFilterChips"
                    visible: root.extraOpen("cullFlag")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomComboBox {
                        objectName: "cullFlagFilterCombo"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 140
                        model: [qsTr("Any review"), qsTr("Picked"), qsTr("Rejected"), qsTr("Unreviewed")]
                        currentIndex: root.hasPresenter && root.presenter.library.cullFlagFilter === "picked" ? 1 : root.hasPresenter && root.presenter.library.cullFlagFilter === "rejected" ? 2 : root.hasPresenter && root.presenter.library.cullFlagFilter === "unreviewed" ? 3 : 0
                        onActivated: function (index) {
                            if (!root.hasPresenter)
                                return;
                            const mode = index === 1 ? "picked" : index === 2 ? "rejected" : index === 3 ? "unreviewed" : "any";
                            root.presenter.library.setCullFlagFilter(mode);
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("cullFlag")
                    }
                }

                CustomButton {
                    id: addFilterButton
                    Layout.alignment: Qt.AlignVCenter
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:/GeoControls/icons/Plus.svg"
                    tooltipText: qsTr("Add filter")
                    enabled: !root.extraOpen("type") || !root.extraOpen("color") || !root.extraOpen("cullFlag")
                    implicitWidth: Fonts.iconButtonSize
                    implicitHeight: Fonts.iconButtonSize
                    Layout.preferredWidth: implicitWidth
                    Layout.preferredHeight: implicitHeight
                    defaultPadding: 0
                    onClicked: addFilterMenu.popup()

                    Chrome.StudioContextMenu {
                        id: addFilterMenu
                        fitToContent: true

                        FilterMenuItem {
                            text: qsTr("Type")
                            visible: !root.extraOpen("type")
                            onTriggered: root.addExtra("type")
                        }

                        FilterMenuItem {
                            text: qsTr("Flag")
                            visible: !root.extraOpen("cullFlag")
                            onTriggered: root.addExtra("cullFlag")
                        }
                        FilterMenuItem {
                            text: qsTr("Color")
                            visible: !root.extraOpen("color")
                            onTriggered: root.addExtra("color")
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
            currentIndex: root.hasPresenter && root.presenter.library.sortField === "captured" ? 1 : root.hasPresenter && root.presenter.library.sortField === "name" ? 2 : root.hasPresenter && root.presenter.library.sortField === "rating" ? 3 : root.hasPresenter && root.presenter.library.sortField === "size" ? 4 : 0
            onActivated: function (index) {
                if (!root.commands || !root.hasPresenter)
                    return;
                const field = ["imported", "captured", "name", "rating", "size"][index];
                root.commands.run(root.commands.ids.librarySetSort, {
                    "field": field,
                    "direction": root.presenter.library.sortDirection
                });
            }
        }
        CustomButton {
            Layout.alignment: Qt.AlignVCenter
            text: root.hasPresenter && root.presenter.library.sortDirection === "asc" ? qsTr("Asc") : qsTr("Desc")
            onClicked: if (root.commands && root.hasPresenter)
                root.commands.run(root.commands.ids.librarySetSort, {
                    "field": root.presenter.library.sortField,
                    "direction": root.presenter.library.sortDirection === "asc" ? "desc" : "asc"
                })
        }
    }
}
