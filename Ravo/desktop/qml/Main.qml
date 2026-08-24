import QtQuick
import QtQuick.Layouts
import QtQuick.Window
import GeoControls 1.0
import GeoControls.AppShell 1.0

Window {
    id: window
    width: 1280
    height: 800
    visible: true
    color: Theme.windowColor
    title: studio.catalogOpen ? ("Ravo Studio — " + studio.catalogPath) : "Ravo Studio"

    readonly property var colorChoices: ["red", "yellow", "green", "blue", "purple"]
    readonly property var colorSwatches: ({
        "none": Theme.midColor,
        "red": "#d85a5a",
        "yellow": "#e9bd4f",
        "green": "#67bd72",
        "blue": "#4e8cd7",
        "purple": "#9b6ad8"
    })

    function openCreateLibraryDialog() {
        createDialog.currentFolder = studio.defaultCatalogFolder
        createDialog.initialSelectedFile = studio.defaultCatalogFile
        createDialog.openDialog()
    }

    function openOpenLibraryDialog() {
        openDialog.currentFolder = studio.defaultCatalogFolder
        openDialog.initialSelectedFile = studio.defaultCatalogExists() ? studio.defaultCatalogFile : ""
        openDialog.openDialog()
    }

    function openImportDialog() {
        importDialog.currentFolder = studio.defaultCatalogFolder
        importDialog.openDialog()
    }

    function openImportFolderDialog() {
        importFolderDialog.currentFolder = studio.defaultCatalogFolder
        importFolderDialog.openDialog()
    }

    function startLibrarySession() {
        if (studio.catalogOpen)
            return
        if (studio.defaultCatalogExists())
            studio.openCatalog(studio.defaultCatalogFile)
        else
            openCreateLibraryDialog()
    }

    function swatchColor(name) {
        return window.colorSwatches[name] || Theme.midColor
    }

    Component.onCompleted: Qt.callLater(startLibrarySession)

    Shortcut { sequence: "G"; onActivated: studio.returnToGrid() }
    Shortcut { sequence: "E"; onActivated: studio.openLoupe() }
    Shortcut { sequence: "D"; onActivated: studio.openDevelop() }
    Shortcut { sequence: "Z"; enabled: studio.browseMode === "develop"; onActivated: studio.undoEdit() }
    Shortcut { sequence: "Shift+Z"; enabled: studio.browseMode === "develop"; onActivated: studio.redoEdit() }
    Shortcut { sequence: "\\"; enabled: studio.browseMode === "develop"; onActivated: studio.toggleBeforeAfter() }
    Shortcut { sequence: "Return"; onActivated: studio.openLoupe() }
    Shortcut { sequence: "Enter"; onActivated: studio.openLoupe() }
    Shortcut { sequence: "Esc"; onActivated: studio.returnToGrid() }
    Shortcut { sequence: "Left"; onActivated: studio.selectPrevious() }
    Shortcut { sequence: "Right"; onActivated: studio.selectNext() }
    Shortcut { sequence: "F"; onActivated: studio.setZoomMode("fit") }
    Shortcut { sequence: "Shift+1"; onActivated: studio.setZoomMode("actual") }
    Shortcut { sequence: "0"; onActivated: studio.setRating(0) }
    Shortcut { sequence: "1"; onActivated: studio.setRating(1) }
    Shortcut { sequence: "2"; onActivated: studio.setRating(2) }
    Shortcut { sequence: "3"; onActivated: studio.setRating(3) }
    Shortcut { sequence: "4"; onActivated: studio.setRating(4) }
    Shortcut { sequence: "5"; onActivated: studio.setRating(5) }
    Shortcut { sequence: "X"; onActivated: studio.toggleRejected() }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Fonts.toolbarHeight
            color: Qt.lighter(Theme.windowColor, 1.1)

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                spacing: Fonts.smallSpacing

                EqualActionButtonRow {
                    CustomButton {
                        text: qsTr("Create Library…")
                        icon.source: "qrc:/GeoControls/icons/Plus.svg"
                        tooltipText: qsTr("Create a new photo library")
                        onClicked: openCreateLibraryDialog()
                    }
                    CustomButton {
                        text: qsTr("Open Library…")
                        icon.source: "qrc:/GeoControls/icons/Open.svg"
                        tooltipText: qsTr("Open an existing photo library")
                        onClicked: openOpenLibraryDialog()
                    }
                    CustomButton {
                        text: qsTr("Import…")
                        icon.source: "qrc:/GeoControls/icons/Load.svg"
                        tooltipText: qsTr("Import photos")
                        enabled: studio.catalogOpen && !studio.busy
                        onClicked: openImportDialog()
                    }
                    CustomButton {
                        text: qsTr("Import Folder…")
                        icon.source: "qrc:/GeoControls/icons/Scene.svg"
                        tooltipText: qsTr("Import a folder of photos")
                        enabled: studio.catalogOpen && !studio.busy
                        onClicked: openImportFolderDialog()
                    }
                }

                Item { Layout.fillWidth: true }

                SegmentedControl {
                    model: [qsTr("Grid"), qsTr("Loupe"), qsTr("Edit")]
                    currentIndex: studio.browseMode === "develop" ? 2 : (studio.browseMode === "loupe" ? 1 : 0)
                    enabled: studio.catalogOpen
                    onActivated: function (index) {
                        if (index === 0)
                            studio.returnToGrid()
                        else if (index === 1)
                            studio.openLoupe()
                        else
                            studio.openDevelop()
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Fonts.toolbarHeight
            color: Theme.contentSurfaceColor
            visible: studio.catalogOpen

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                spacing: Fonts.smallSpacing

                CustomLabel { text: qsTr("Rating") }
                CustomComboBox {
                    id: ratingFilter
                    model: [qsTr("Any"), qsTr("≥ 1"), qsTr("≥ 2"), qsTr("≥ 3"), qsTr("≥ 4"), qsTr("≥ 5"),
                            qsTr("Exact 0"), qsTr("Exact 1"), qsTr("Exact 2"), qsTr("Exact 3"), qsTr("Exact 4"), qsTr("Exact 5")]
                    Layout.preferredWidth: 140
                    onActivated: function (index) {
                        if (index === 0)
                            studio.setRatingFilter("any", 0)
                        else if (index <= 5)
                            studio.setRatingFilter("min", index)
                        else
                            studio.setRatingFilter("exact", index - 6)
                    }
                }

                CustomLabel { text: qsTr("Color") }
                Repeater {
                    model: window.colorChoices
                    delegate: Rectangle {
                        required property string modelData
                        width: 18
                        height: 18
                        radius: 9
                        color: window.swatchColor(modelData)
                        border.width: studio.colorFilters.indexOf(modelData) >= 0 ? 2 : 1
                        border.color: studio.colorFilters.indexOf(modelData) >= 0 ? Theme.textColor : Theme.dividerColor
                        MouseArea {
                            anchors.fill: parent
                            onClicked: studio.toggleColorFilter(modelData)
                        }
                    }
                }

                CustomLabel { text: qsTr("Rejected") }
                CustomComboBox {
                    model: [qsTr("Include"), qsTr("Exclude"), qsTr("Only")]
                    Layout.preferredWidth: 120
                    currentIndex: studio.rejectFilter === "exclude" ? 1 : (studio.rejectFilter === "only" ? 2 : 0)
                    onActivated: function (index) {
                        studio.setRejectFilter(index === 1 ? "exclude" : (index === 2 ? "only" : "include"))
                    }
                }

                CustomButton {
                    text: qsTr("Clear filters")
                    enabled: studio.filtersActive
                    onClicked: studio.clearFilters()
                }

                Item { Layout.fillWidth: true }

                CustomComboBox {
                    model: [qsTr("Import time"), qsTr("Filename"), qsTr("Rating")]
                    Layout.preferredWidth: 140
                    currentIndex: studio.sortField === "name" ? 1 : (studio.sortField === "rating" ? 2 : 0)
                    onActivated: function (index) {
                        const field = index === 1 ? "name" : (index === 2 ? "rating" : "imported")
                        studio.setSort(field, studio.sortDirection)
                    }
                }
                CustomButton {
                    text: studio.sortDirection === "asc" ? qsTr("Asc") : qsTr("Desc")
                    onClicked: studio.setSort(studio.sortField, studio.sortDirection === "asc" ? "desc" : "asc")
                }
                CustomLabel { text: qsTr("Size") }
                CustomSlider {
                    from: 120
                    to: 260
                    stepSize: 10
                    value: studio.thumbnailSize
                    showTitle: false
                    showStepButton: false
                    Layout.preferredWidth: 140
                    delayedCommit: true
                    commitDelay: 40
                    onValueCommitted: function (value) { studio.thumbnailSize = Math.round(value) }
                }
            }
        }

        InfoBar {
            Layout.fillWidth: true
            Layout.margins: Fonts.size8
            visible: studio.errorText.length > 0
            severity: "error"
            title: qsTr("Error")
            message: studio.errorText
            closable: false
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            GridView {
                id: grid
                anchors.fill: parent
                anchors.margins: Fonts.size8
                visible: studio.browseMode === "grid"
                clip: true
                model: studio.assets
                cellWidth: studio.thumbnailSize + Fonts.size16
                cellHeight: studio.thumbnailSize + Fonts.size36
                cacheBuffer: cellHeight * 4
                onVisibleChanged: if (visible && studio.selectedIndex >= 0)
                    positionViewAtIndex(studio.selectedIndex, GridView.Contain)
                Connections {
                    target: studio
                    function onBrowseModeChanged() {
                        if (studio.browseMode === "grid" && studio.selectedIndex >= 0)
                            grid.positionViewAtIndex(studio.selectedIndex, GridView.Contain)
                    }
                    function onSelectionChanged() {
                        if (studio.browseMode === "grid" && studio.selectedIndex >= 0)
                            grid.positionViewAtIndex(studio.selectedIndex, GridView.Contain)
                    }
                }
                delegate: Item {
                    id: tile
                    required property string assetId
                    required property string displayName
                    required property int rating
                    required property string colorLabel
                    required property bool rejected
                    required property url thumbnailUrl
                    required property string thumbnailState
                    required property string importState
                    required property bool hasEdits
                    width: grid.cellWidth
                    height: grid.cellHeight

                    Component.onCompleted: studio.ensureThumbnail(assetId)

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: Fonts.size4
                        color: Theme.pageSurfaceColor
                        border.width: assetId === studio.selectedAssetId ? 2 : 1
                        border.color: assetId === studio.selectedAssetId ? Theme.highlightColor : Theme.dividerColor

                        Image {
                            anchors.fill: parent
                            anchors.bottomMargin: Fonts.size22
                            anchors.margins: Fonts.size4
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: false
                            source: thumbnailUrl
                            visible: thumbnailUrl.toString().length > 0
                        }

                        CustomLabel {
                            anchors.centerIn: parent
                            visible: thumbnailUrl.toString().length === 0
                            text: thumbnailState === "failed" ? qsTr("Failed") :
                                  (importState === "missing" || thumbnailState === "missing" ?
                                   qsTr("Missing") : qsTr("Loading…"))
                            color: Theme.placeholderTextColor
                        }

                        CustomLabel {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.margins: Fonts.size4
                            text: "★".repeat(rating) + "☆".repeat(5 - rating)
                            font.pixelSize: Fonts.size12
                        }

                        Rectangle {
                            width: 10
                            height: 10
                            radius: 5
                            visible: colorLabel !== "none"
                            color: window.swatchColor(colorLabel)
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: Fonts.size6
                        }

                        Rectangle {
                            visible: importState === "missing" || thumbnailState === "missing"
                            anchors.top: parent.top
                            anchors.left: parent.left
                            anchors.margins: Fonts.size6
                            width: 58
                            height: 18
                            radius: 4
                            color: "#c47b16"
                            CustomLabel {
                                anchors.centerIn: parent
                                text: qsTr("Missing")
                                color: "#ffffff"
                                font.pixelSize: Fonts.size10
                            }
                        }

                        Rectangle {
                            visible: hasEdits
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: Fonts.size6
                            width: 48
                            height: 18
                            radius: 4
                            color: Theme.highlightColor
                            CustomLabel {
                                anchors.centerIn: parent
                                text: qsTr("Edit")
                                color: "#ffffff"
                                font.pixelSize: Fonts.size10
                            }
                        }

                        Rectangle {
                            visible: rejected
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: hasEdits ? Fonts.size6 + 52 : Fonts.size6
                            width: 56
                            height: 18
                            radius: 4
                            color: "#aa3333"
                            CustomLabel {
                                anchors.centerIn: parent
                                text: qsTr("Reject")
                                color: "#ffffff"
                                font.pixelSize: Fonts.size10
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: studio.selectAsset(assetId)
                            onDoubleClicked: {
                                studio.selectAsset(assetId)
                                studio.openLoupe()
                            }
                        }
                    }
                }

                CustomLabel {
                    anchors.centerIn: parent
                    visible: grid.count === 0
                    text: !studio.catalogOpen ? qsTr("No library open.") :
                          (studio.filtersActive ? qsTr("No photos match the current filters.") :
                           qsTr("No photos imported yet."))
                    color: Theme.placeholderTextColor
                }
            }

            RowLayout {
                anchors.fill: parent
                visible: studio.browseMode !== "grid"
                spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: Fonts.toolbarHeight
                    color: Qt.lighter(Theme.windowColor, 1.1)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Fonts.standardMargin
                        anchors.rightMargin: Fonts.standardMargin
                        spacing: Fonts.smallSpacing
                        SegmentedControl {
                            model: [qsTr("Fit"), qsTr("Fill"), qsTr("100%")]
                            currentIndex: studio.zoomMode === "fill" ? 1 : (studio.zoomMode === "actual" ? 2 : 0)
                            enabled: studio.previewUrl.toString().length > 0
                            onActivated: function (index) {
                                studio.setZoomMode(index === 1 ? "fill" : (index === 2 ? "actual" : "fit"))
                            }
                        }
                        CustomLabel {
                            text: studio.previewLoading ? qsTr("Loading preview…") :
                                  (Math.round(studio.zoomFactor * 100) + "%")
                            color: Theme.placeholderTextColor
                        }
                        Item { Layout.fillWidth: true }
                        CustomButton { text: qsTr("Previous"); onClicked: studio.selectPrevious() }
                        CustomButton { text: qsTr("Next"); onClicked: studio.selectNext() }
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Flickable {
                        id: scroller
                        anchors.fill: parent
                        clip: true
                        contentWidth: previewImage.width
                        contentHeight: previewImage.height
                        boundsBehavior: Flickable.StopAtBounds

                        Image {
                            id: previewImage
                            asynchronous: true
                            cache: false
                            source: studio.previewUrl
                            fillMode: Image.PreserveAspectFit
                            width: {
                                if (studio.zoomMode === "fit")
                                    return scroller.width
                                if (studio.zoomMode === "fill")
                                    return Math.max(scroller.width, implicitWidth * (scroller.height / Math.max(implicitHeight, 1)))
                                if (studio.zoomMode === "actual")
                                    return implicitWidth
                                return implicitWidth * studio.zoomFactor
                            }
                            height: {
                                if (studio.zoomMode === "fit")
                                    return scroller.height
                                if (studio.zoomMode === "fill")
                                    return Math.max(scroller.height, implicitHeight * (scroller.width / Math.max(implicitWidth, 1)))
                                if (studio.zoomMode === "actual")
                                    return implicitHeight
                                return implicitHeight * studio.zoomFactor
                            }
                        }

                        WheelHandler {
                            onWheel: function (event) {
                                studio.adjustZoom(event.angleDelta.y)
                                event.accepted = true
                            }
                        }
                    }

                    CustomLabel {
                        anchors.centerIn: parent
                        visible: previewImage.source.toString().length === 0 && !studio.previewLoading
                        text: studio.selectedImportState === "missing" ? qsTr("Original file is missing.") :
                              (studio.catalogOpen ? qsTr("Select a photo to inspect.") : qsTr("Create or open a library."))
                        color: Theme.placeholderTextColor
                    }

                    Rectangle {
                        visible: studio.selectedImportState === "missing"
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.margins: Fonts.size8
                        width: 86
                        height: 22
                        radius: 4
                        color: "#c47b16"
                        z: 2
                        CustomLabel {
                            anchors.centerIn: parent
                            text: qsTr("Missing file")
                            color: "#ffffff"
                            font.pixelSize: Fonts.size12
                        }
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 92
                    orientation: ListView.Horizontal
                    clip: true
                    model: studio.assets
                    spacing: Fonts.size4
                    currentIndex: studio.selectedIndex
                    highlightMoveDuration: 0
                    delegate: Item {
                        required property string assetId
                        required property url thumbnailUrl
                        required property string importState
                        required property string thumbnailState
                        width: 88
                        height: 84
                        Component.onCompleted: studio.ensureThumbnail(assetId)
                        Rectangle {
                            anchors.fill: parent
                            color: Theme.pageSurfaceColor
                            border.width: assetId === studio.selectedAssetId ? 2 : 1
                            border.color: assetId === studio.selectedAssetId ? Theme.highlightColor : Theme.dividerColor
                            Image {
                                anchors.fill: parent
                                anchors.margins: 2
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: false
                                source: thumbnailUrl
                            }
                            Rectangle {
                                visible: importState === "missing" || thumbnailState === "missing"
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 4
                                width: 10
                                height: 10
                                radius: 5
                                color: "#c47b16"
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: studio.selectAsset(assetId)
                            }
                        }
                    }
                }
            }

                Rectangle {
                    visible: studio.browseMode === "develop"
                    Layout.preferredWidth: 320
                    Layout.fillHeight: true
                    color: Theme.contentSurfaceColor

                    Flickable {
                        anchors.fill: parent
                        clip: true
                        contentWidth: width
                        contentHeight: developColumn.implicitHeight

                        ColumnLayout {
                            id: developColumn
                            width: parent.width
                            spacing: Fonts.smallSpacing

                            RowLayout {
                                CustomButton {
                                    text: qsTr("Undo")
                                    enabled: studio.canUndo
                                    onClicked: studio.undoEdit()
                                }
                                CustomButton {
                                    text: qsTr("Redo")
                                    enabled: studio.canRedo
                                    onClicked: studio.redoEdit()
                                }
                                CustomButton {
                                    text: studio.beforeAfter ? qsTr("After") : qsTr("Before")
                                    onClicked: studio.toggleBeforeAfter()
                                }
                                CustomButton {
                                    text: qsTr("Reset all")
                                    onClicked: studio.resetAllEdits()
                                }
                            }

                            CustomLabel { text: qsTr("Geometry"); font.bold: true }
                            RowLayout {
                                CustomButton { text: qsTr("Rotate L"); onClicked: studio.rotateLeft() }
                                CustomButton { text: qsTr("Rotate R"); onClicked: studio.rotateRight() }
                                CustomButton { text: qsTr("Reset"); onClicked: studio.resetSection("geometry") }
                            }
                            CustomSlider {
                                title: qsTr("Crop X")
                                from: 0; to: 0.9; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editCropX
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("cropX", value) }
                            }
                            CustomSlider {
                                title: qsTr("Crop Y")
                                from: 0; to: 0.9; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editCropY
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("cropY", value) }
                            }
                            CustomSlider {
                                title: qsTr("Crop W")
                                from: 0.1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editCropWidth
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("cropWidth", value) }
                            }
                            CustomSlider {
                                title: qsTr("Crop H")
                                from: 0.1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editCropHeight
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("cropHeight", value) }
                            }

                            CustomLabel { text: qsTr("White Balance"); font.bold: true }
                            CustomSlider {
                                title: qsTr("Temp")
                                from: 2000; to: 12000; stepSize: 50
                                value: studio.editTemperature
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("temperature", value) }
                            }
                            CustomSlider {
                                title: qsTr("Tint")
                                from: -150; to: 150; stepSize: 1
                                value: studio.editTint
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("tint", value) }
                            }
                            CustomButton { text: qsTr("Reset WB"); onClicked: studio.resetSection("whiteBalance") }

                            CustomLabel { text: qsTr("Light"); font.bold: true }
                            CustomSlider {
                                title: qsTr("Exposure")
                                from: -5; to: 5; stepSize: 0.05; validatorDecimals: 2
                                value: studio.editExposure
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("exposure", value) }
                            }
                            CustomSlider {
                                title: qsTr("Contrast")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editContrast
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("contrast", value) }
                            }
                            CustomSlider {
                                title: qsTr("Highlights")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editHighlights
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("highlights", value) }
                            }
                            CustomSlider {
                                title: qsTr("Shadows")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editShadows
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("shadows", value) }
                            }
                            CustomSlider {
                                title: qsTr("Whites")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editWhites
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("whites", value) }
                            }
                            CustomSlider {
                                title: qsTr("Blacks")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editBlacks
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("blacks", value) }
                            }
                            CustomButton { text: qsTr("Reset light"); onClicked: studio.resetSection("light") }

                            CustomLabel { text: qsTr("Color"); font.bold: true }
                            CustomSlider {
                                title: qsTr("Vibrance")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editVibrance
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("vibrance", value) }
                            }
                            CustomSlider {
                                title: qsTr("Saturation")
                                from: -1; to: 1; stepSize: 0.01; validatorDecimals: 2
                                value: studio.editSaturation
                                delayedCommit: true
                                onValueCommitted: function (value) { studio.setDevelopNumber("saturation", value) }
                            }
                            CustomButton { text: qsTr("Reset color"); onClicked: studio.resetSection("color") }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Fonts.toolbarHeight
            color: Qt.lighter(Theme.windowColor, 1.1)
            visible: studio.catalogOpen && studio.selectedAssetId.length > 0

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                spacing: Fonts.size8

                RatingControl {
                    rating: studio.selectedRating
                    onRatingChangedByUser: function (value) { studio.setRating(value) }
                }
                Repeater {
                    model: ["none"].concat(window.colorChoices)
                    delegate: Rectangle {
                        required property string modelData
                        width: 18
                        height: 18
                        radius: 9
                        color: window.swatchColor(modelData)
                        border.width: studio.selectedColorLabel === modelData ? 2 : 1
                        border.color: Theme.textColor
                        MouseArea {
                            anchors.fill: parent
                            onClicked: studio.setColorLabel(modelData)
                        }
                    }
                }
                CustomButton {
                    text: studio.selectedRejected ? qsTr("Unreject") : qsTr("Reject")
                    onClicked: studio.toggleRejected()
                }
                Item { Layout.fillWidth: true }
                CustomLabel {
                    text: studio.statusText
                    elide: Text.ElideMiddle
                    Layout.maximumWidth: 480
                }
            }
        }

        MainStatusBar {
            Layout.fillWidth: true
            statusText: studio.statusText
            viewerText: studio.previewLoading ? qsTr("Loading preview…") :
                        (studio.selectedAssetId.length > 0 ? (studio.visibleCount + " photos") : "")
        }
    }

    QmlFileDialogPage {
        id: createDialog
        dialogTitle: qsTr("Create Library")
        dialogMode: "save"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studio.createCatalogFromPath(filePath)
        }
    }

    QmlFileDialogPage {
        id: openDialog
        dialogTitle: qsTr("Open Library")
        dialogMode: "open"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studio.openCatalogFromPath(filePath)
        }
    }

    QmlFileDialogPage {
        id: importDialog
        dialogTitle: qsTr("Import Photos")
        dialogMode: "openFiles"
        nameFilters: [
            "Photos (*.png *.jpg *.jpeg *.JPG *.JPEG *.tif *.tiff *.bmp *.gif *.webp *.arw *.ARW *.cr2 *.CR2 *.cr3 *.CR3 *.nef *.NEF *.dng *.DNG *.raf *.orf *.rw2)",
            "All files (*)"
        ]
        onFileAccepted: function (filePath, selectedFilter, filePaths) {
            studio.importFilePaths(filePaths)
        }
    }

    FolderDialogPage {
        id: importFolderDialog
        dialogTitle: qsTr("Import Folder")
        onFolderAccepted: function (folderPath) {
            studio.importFolderFromPath(folderPath)
        }
    }
}
