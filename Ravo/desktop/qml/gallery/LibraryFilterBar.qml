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

    function matchingFacetCount(entries, predicate) {
        for (let i = 0; i < entries.length; ++i) {
            if (predicate(entries[i]))
                return entries[i].count;
        }
        return 0;
    }

    readonly property int cameraFacetCount: !root.hasPresenter || root.presenter.cameraFilter.length === 0 ? -1 : root.matchingFacetCount(root.presenter.cameraFacets, function (entry) {
        return (entry.cameraMake || "") === root.presenter.cameraMakeFilter && (entry.cameraModel || "") === root.presenter.cameraModelFilter;
    })
    readonly property int lensFacetCount: !root.hasPresenter || root.presenter.lensFilter.length === 0 ? -1 : root.matchingFacetCount(root.presenter.lensFacets, function (entry) {
        return Math.abs(entry.focalLengthMm - Number(root.presenter.lensFilter)) < 0.000000001;
    })
    readonly property int lensNameFacetCount: !root.hasPresenter || (root.presenter.lensMakeFilter.length === 0 && root.presenter.lensModelFilter.length === 0) ? -1 : root.matchingFacetCount(root.presenter.lensNameFacets, function (entry) {
        return (entry.lensMake || "") === root.presenter.lensMakeFilter && (entry.lensModel || "") === root.presenter.lensModelFilter;
    })
    readonly property int captureDateFacetCount: !root.hasPresenter || root.presenter.captureDateFilter.length === 0 ? -1 : root.matchingFacetCount(root.presenter.captureDateFacets, function (entry) {
        return entry.captureDate === root.presenter.captureDateFilter;
    })
    readonly property int locationFacetCount: !root.hasPresenter || root.presenter.locationFilter.length === 0 ? -1 : root.presenter.sublocationFilter.length > 0 ? root.matchingFacetCount(root.presenter.sublocationFacets, function (entry) {
        return entry.key === root.presenter.sublocationFilter;
    }) : root.presenter.cityFilter.length > 0 ? root.matchingFacetCount(root.presenter.cityFacets, function (entry) {
        return entry.key === root.presenter.cityFilter;
    }) : root.presenter.provinceStateFilter.length > 0 ? root.matchingFacetCount(root.presenter.provinceStateFacets, function (entry) {
        return entry.key === root.presenter.provinceStateFilter;
    }) : root.matchingFacetCount(root.presenter.countryFacets, function (entry) {
        return entry.key === root.presenter.countryFilter;
    })

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
        if (id === "cullFlag")
            return root.presenter.cullFlagFilter !== "any";
        if (id === "cullSuggestion")
            return root.presenter.cullSuggestionFilter !== "none";
        if (id === "camera")
            return root.presenter.cameraFilter.length > 0;
        if (id === "lens")
            return root.presenter.lensFilter.length > 0;
        if (id === "lensName")
            return root.presenter.lensMakeFilter.length > 0 || root.presenter.lensModelFilter.length > 0;
        if (id === "captureDate")
            return root.presenter.captureDateFilter.length > 0;
        if (id === "location")
            return root.presenter.locationFilter.length > 0;
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
        else if (id === "cullFlag" && root.hasPresenter)
            root.presenter.setCullFlagFilter("any");
        else if (id === "cullSuggestion" && root.hasPresenter)
            root.presenter.setCullSuggestionFilter("none");
        else if (id === "camera")
            root.commands.setCameraFacetFilter("", "");
        else if (id === "lens")
            root.commands.setLensFacetFilter("");
        else if (id === "lensName")
            root.commands.setLensNameFacetFilter("", "");
        else if (id === "captureDate")
            root.commands.setCaptureDateFacetFilter("");
        else if (id === "location")
            root.commands.setLocationFacetFilter("", "", "", "");
        else if (id === "color" && root.hasPresenter) {
            const colors = root.presenter.colorFilters.slice();
            for (let i = 0; i < colors.length; ++i)
                root.commands.run(root.commands.ids.libraryToggleColorFilter, colors[i]);
        }
    }

    function setRatingExact(value) {
        if (!root.commands)
            return;
        const already = root.hasPresenter && root.presenter.ratingFilterMode === "exact" && root.presenter.ratingFilterValue === value;
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
        if (root.presenter.ratingFilterMode === "exact")
            return root.presenter.ratingFilterValue >= star && root.presenter.ratingFilterValue > 0;
        if (root.presenter.ratingFilterMode === "min")
            return root.presenter.ratingFilterValue >= star;
        return false;
    }

    readonly property bool ratingUnratedActive: root.hasPresenter && root.presenter.ratingFilterMode === "exact" && root.presenter.ratingFilterValue === 0

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
                        currentIndex: root.hasPresenter && root.presenter.mediaFilter === "raw" ? 1 : root.hasPresenter && root.presenter.mediaFilter === "jpeg" ? 2 : root.hasPresenter && root.presenter.mediaFilter === "png" ? 3 : root.hasPresenter && root.presenter.mediaFilter === "tiff" ? 4 : 0
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
                        currentIndex: root.hasPresenter && root.presenter.editFilter === "edited" ? 1 : root.hasPresenter && root.presenter.editFilter === "unedited" ? 2 : 0
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
                        currentIndex: root.hasPresenter && root.presenter.rejectFilter === "exclude" ? 1 : root.hasPresenter && root.presenter.rejectFilter === "only" ? 2 : 0
                        onActivated: function (index) {
                            if (root.commands)
                                root.commands.run(root.commands.ids.librarySetRejectFilter, index === 1 ? "exclude" : (index === 2 ? "only" : "include"));
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("rejected")
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
                        currentIndex: root.hasPresenter && root.presenter.cullFlagFilter === "picked" ? 1 : root.hasPresenter && root.presenter.cullFlagFilter === "rejected" ? 2 : root.hasPresenter && root.presenter.cullFlagFilter === "unreviewed" ? 3 : 0
                        onActivated: function (index) {
                            if (!root.hasPresenter)
                                return;
                            const mode = index === 1 ? "picked" : index === 2 ? "rejected" : index === 3 ? "unreviewed" : "any";
                            root.presenter.setCullFlagFilter(mode);
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("cullFlag")
                    }
                }

                RowLayout {
                    objectName: "cullSuggestionFilterChips"
                    visible: root.extraOpen("cullSuggestion")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomComboBox {
                        objectName: "cullSuggestionFilterCombo"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 170
                        model: [qsTr("No suggestion"), qsTr("Exact byte duplicate"), qsTr("Near duplicate (heuristic)"), qsTr("Burst")]
                        currentIndex: root.hasPresenter && root.presenter.cullSuggestionFilter === "exact_duplicate" ? 1 : root.hasPresenter && root.presenter.cullSuggestionFilter === "near_duplicate" ? 2 : root.hasPresenter && root.presenter.cullSuggestionFilter === "burst" ? 3 : 0
                        onActivated: function (index) {
                            if (!root.hasPresenter)
                                return;
                            const mode = index === 1 ? "exact_duplicate" : index === 2 ? "near_duplicate" : index === 3 ? "burst" : "none";
                            root.presenter.setCullSuggestionFilter(mode);
                        }
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("cullSuggestion")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("camera")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomTextField {
                        id: cameraMakeField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 110
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Camera make")
                        text: root.hasPresenter ? root.presenter.cameraMakeFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setCameraFacetFilter(text, cameraModelField.text)
                    }
                    CustomTextField {
                        id: cameraModelField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 130
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Camera model")
                        text: root.hasPresenter ? root.presenter.cameraModelFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setCameraFacetFilter(cameraMakeField.text, text)
                    }
                    Text {
                        visible: root.cameraFacetCount >= 0
                        text: qsTr("%1 photos").arg(root.cameraFacetCount)
                        color: Theme.midColor
                        font: Fonts.standardFont
                        Layout.alignment: Qt.AlignVCenter
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("camera")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("lens")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomTextField {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Focal mm")
                        text: root.hasPresenter ? root.presenter.lensFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLensFacetFilter(text)
                    }
                    Text {
                        visible: root.lensFacetCount >= 0
                        text: qsTr("%1 photos").arg(root.lensFacetCount)
                        color: Theme.midColor
                        font: Fonts.standardFont
                        Layout.alignment: Qt.AlignVCenter
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("lens")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("lensName")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomTextField {
                        id: lensMakeField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 110
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Lens make")
                        text: root.hasPresenter ? root.presenter.lensMakeFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLensNameFacetFilter(text, lensModelField.text)
                    }
                    CustomTextField {
                        id: lensModelField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 150
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Lens model")
                        text: root.hasPresenter ? root.presenter.lensModelFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLensNameFacetFilter(lensMakeField.text, text)
                    }
                    Text {
                        visible: root.lensNameFacetCount >= 0
                        text: qsTr("%1 photos").arg(root.lensNameFacetCount)
                        color: Theme.midColor
                        font: Fonts.standardFont
                        Layout.alignment: Qt.AlignVCenter
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("lensName")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("captureDate")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomTextField {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 130
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("YYYY:MM:DD")
                        text: root.hasPresenter ? root.presenter.captureDateFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setCaptureDateFacetFilter(text)
                    }
                    Text {
                        visible: root.captureDateFacetCount >= 0
                        text: qsTr("%1 photos").arg(root.captureDateFacetCount)
                        color: Theme.midColor
                        font: Fonts.standardFont
                        Layout.alignment: Qt.AlignVCenter
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("captureDate")
                    }
                }

                RowLayout {
                    visible: root.extraOpen("location")
                    spacing: Fonts.size2
                    Layout.alignment: Qt.AlignVCenter
                    CustomTextField {
                        id: locationCountryField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Country")
                        text: root.hasPresenter ? root.presenter.countryFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLocationFacetFilter(text, locationProvinceField.text, locationCityField.text, locationSublocationField.text)
                    }
                    CustomTextField {
                        id: locationProvinceField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("State")
                        text: root.hasPresenter ? root.presenter.provinceStateFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLocationFacetFilter(locationCountryField.text, text, locationCityField.text, locationSublocationField.text)
                    }
                    CustomTextField {
                        id: locationCityField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 90
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("City")
                        text: root.hasPresenter ? root.presenter.cityFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLocationFacetFilter(locationCountryField.text, locationProvinceField.text, text, locationSublocationField.text)
                    }
                    CustomTextField {
                        id: locationSublocationField
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: 100
                        Layout.preferredHeight: Fonts.inputFieldHeight
                        showEmptyIndicator: false
                        showClipIndicator: false
                        alignRightWhenFocused: false
                        placeholderText: qsTr("Sublocation")
                        text: root.hasPresenter ? root.presenter.sublocationFilter : ""
                        onEditingFinished: if (root.commands)
                            root.commands.setLocationFacetFilter(locationCountryField.text, locationProvinceField.text, locationCityField.text, text)
                    }
                    Text {
                        visible: root.locationFacetCount >= 0
                        text: qsTr("%1 photos").arg(root.locationFacetCount)
                        color: Theme.midColor
                        font: Fonts.standardFont
                        Layout.alignment: Qt.AlignVCenter
                    }
                    FilterCloseButton {
                        onClicked: root.removeExtra("location")
                    }
                }

                CustomButton {
                    id: addFilterButton
                    Layout.alignment: Qt.AlignVCenter
                    display: AbstractButton.IconOnly
                    icon.source: "qrc:/GeoControls/icons/Plus.svg"
                    tooltipText: qsTr("Add filter")
                    enabled: !root.extraOpen("search") || !root.extraOpen("type") || !root.extraOpen("edits") || !root.extraOpen("color") || !root.extraOpen("rejected") || !root.extraOpen("cullFlag") || !root.extraOpen("cullSuggestion") || !root.extraOpen("camera") || !root.extraOpen("lens") || !root.extraOpen("lensName") || !root.extraOpen("captureDate") || !root.extraOpen("location")
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
                        FilterMenuItem {
                            text: qsTr("Camera")
                            visible: !root.extraOpen("camera")
                            onTriggered: root.addExtra("camera")
                        }
                        FilterMenuItem {
                            text: qsTr("Lens")
                            visible: !root.extraOpen("lens")
                            onTriggered: root.addExtra("lens")
                        }
                        FilterMenuItem {
                            text: qsTr("Lens name")
                            visible: !root.extraOpen("lensName")
                            onTriggered: root.addExtra("lensName")
                        }
                        FilterMenuItem {
                            text: qsTr("Capture date")
                            visible: !root.extraOpen("captureDate")
                            onTriggered: root.addExtra("captureDate")
                        }
                        FilterMenuItem {
                            text: qsTr("Location")
                            visible: !root.extraOpen("location")
                            onTriggered: root.addExtra("location")
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
            currentIndex: root.hasPresenter && root.presenter.sortField === "captured" ? 1 : root.hasPresenter && root.presenter.sortField === "name" ? 2 : root.hasPresenter && root.presenter.sortField === "rating" ? 3 : root.hasPresenter && root.presenter.sortField === "size" ? 4 : 0
            onActivated: function (index) {
                if (!root.commands || !root.hasPresenter)
                    return;
                const field = ["imported", "captured", "name", "rating", "size"][index];
                root.commands.run(root.commands.ids.librarySetSort, {
                    "field": field,
                    "direction": root.presenter.sortDirection
                });
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
