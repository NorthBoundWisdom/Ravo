import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import GeoControls 1.0
import GeoControls.AppShell 1.0

ApplicationWindow {
    id: window
    width: 1440
    height: 900
    visible: true
    color: Theme.windowColor
    title: studio.catalogOpen ? ("Ravo Studio — " + studio.catalogPath) : "Ravo Studio"
    palette.window: Theme.windowColor
    palette.windowText: Theme.windowTextColor
    palette.base: Theme.baseColor
    palette.alternateBase: Theme.alternateBaseColor
    palette.text: Theme.textColor
    palette.button: Theme.buttonColor
    palette.buttonText: Theme.buttonTextColor
    palette.light: Theme.lightColor
    palette.midlight: Theme.midlightColor
    palette.mid: Theme.midColor
    palette.dark: Theme.darkColor
    palette.shadow: Theme.shadowColor
    palette.highlight: Theme.highlightColor
    palette.highlightedText: Theme.highlightedTextColor
    palette.placeholderText: Theme.placeholderTextColor
    palette.accent: Theme.accentColor

    property bool settingsOpen: false
    readonly property bool studioInteractive: !settingsOpen
    property string lastGalleryMode: "grid"
    readonly property rect navigatorVisible: {
        if (studio.browseMode === "grid" || typeof photoPlane === "undefined" || photoPlane.width < 1 || scroller.width < 1)
            return Qt.rect(0, 0, 1, 1);
        const visL = Math.max(scroller.contentX, photoPlane.x);
        const visT = Math.max(scroller.contentY, photoPlane.y);
        const visR = Math.min(scroller.contentX + scroller.width, photoPlane.x + photoPlane.width);
        const visB = Math.min(scroller.contentY + scroller.height, photoPlane.y + photoPlane.height);
        const w = Math.max(1, photoPlane.width);
        const h = Math.max(1, photoPlane.height);
        return Qt.rect(Math.max(0, Math.min(1, (visL - photoPlane.x) / w)),
                       Math.max(0, Math.min(1, (visT - photoPlane.y) / h)),
                       Math.max(0.02, Math.min(1, (visR - visL) / w)),
                       Math.max(0.02, Math.min(1, (visB - visT) / h)));
    }

    function seekNavigatorViewport(nx, ny) {
        if (studio.browseMode === "grid" || typeof photoPlane === "undefined" || photoPlane.width < 1)
            return;
        const maxX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxY = Math.max(0, scroller.contentHeight - scroller.height);
        scroller.contentX = Math.max(0, Math.min(maxX, photoPlane.x + nx * photoPlane.width));
        scroller.contentY = Math.max(0, Math.min(maxY, photoPlane.y + ny * photoPlane.height));
    }

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
        createDialog.currentFolder = studio.defaultCatalogFolder;
        createDialog.initialSelectedFile = studio.defaultCatalogFile;
        createDialog.openDialog();
    }

    function openOpenLibraryDialog() {
        openDialog.currentFolder = studio.defaultCatalogFolder;
        openDialog.initialSelectedFile = studio.defaultCatalogExists() ? studio.defaultCatalogFile : "";
        openDialog.openDialog();
    }

    function openImportDialog() {
        importDialog.currentFolder = studio.defaultCatalogFolder;
        importDialog.openDialog();
    }

    function openImportFolderDialog() {
        importFolderDialog.currentFolder = studio.defaultCatalogFolder;
        importFolderDialog.openDialog();
    }

    function openExportDialog() {
        if (!studio.selectedAssetId.length)
            return;
        exportDialog.currentFolder = studio.defaultCatalogFolder;
        exportDialog.initialSelectedFile = studio.selectedDisplayName;
        exportDialog.openDialog();
    }

    function startLibrarySession() {
        if (studio.catalogOpen)
            return;
        if (studio.defaultCatalogExists())
            studio.openCatalog(studio.defaultCatalogFile);
        else
            openCreateLibraryDialog();
    }

    function swatchColor(name) {
        return window.colorSwatches[name] || Theme.midColor;
    }

    function openAboutDialog() {
        aboutDialog.openWithButtons();
    }

    function showPhotoMenu() {
        photoMenu.popup();
    }

    function askRemovePhoto() {
        if (!studio.selectedAssetId.length)
            return;
        removeDialog.messageText = studio.selectedCount > 1 ? qsTr("Remove %1 photos from the library? Original files on disk will not be deleted.").arg(studio.selectedCount) : qsTr("Remove this photo from the library? The original file on disk will not be deleted.");
        removeDialog.openWithButtons();
    }

    function askDeleteFromDisk() {
        if (!studio.canDeleteFromDisk)
            return;
        deleteDiskDialog.messageText = studio.selectedCount > 1 ? qsTr("Permanently delete %1 original files from disk and remove them from the library? This cannot be undone.").arg(studio.selectedCount) : qsTr("Permanently delete the original file from disk and remove it from the library? This cannot be undone.");
        deleteDiskDialog.openWithButtons();
    }

    function applyAppearance() {
        studioPalette.appFont = Qt.application.font;
        studioPalette.monoFont = Qt.application.font;
        Theme.helper = studioPalette;
        Theme.colorsChanged();
        Theme.fontsChanged();
    }

    DarkThemePalette {
        id: studioPalette
    }

    StudioActions {
        id: studioActions
        presenter: studio
        windowHost: window
    }

    Component.onCompleted: {
        applyAppearance();
        Qt.callLater(startLibrarySession);
    }

    Connections {
        target: studio
        function onBrowseModeChanged() {
            if (studio.browseMode === "grid" || studio.browseMode === "loupe")
                window.lastGalleryMode = studio.browseMode;
        }
        function onUiCommandRequested(id) {
            const ids = studioActions.ids;
            if (id === ids.libraryCreate)
                openCreateLibraryDialog();
            else if (id === ids.libraryOpen)
                openOpenLibraryDialog();
            else if (id === ids.libraryImportFiles)
                openImportDialog();
            else if (id === ids.libraryImportFolder)
                openImportFolderDialog();
            else if (id === ids.libraryExport)
                openExportDialog();
            else if (id === ids.windowSettings)
                window.settingsOpen = true;
            else if (id === ids.windowClose)
                window.close();
            else if (id === ids.windowQuit)
                Qt.quit();
            else if (id === ids.windowAbout)
                openAboutDialog();
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(studioMenuBar.implicitHeight, Fonts.menuBarHeight)
            color: Theme.windowColor

            StudioMenuBar {
                id: studioMenuBar
                width: parent.width
                actions: studioActions
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.dividerColor
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Math.max(Fonts.toolbarHeight, Fonts.inputFieldHeight + Fonts.size12)
            Layout.preferredHeight: implicitHeight
            color: Theme.toolbarSurfaceColor
            visible: studio.catalogOpen
            clip: true

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.dividerColor
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Fonts.standardMargin
                anchors.rightMargin: Fonts.standardMargin
                spacing: Fonts.smallSpacing

                CustomCheckBox {
                    id: filterToggle
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Filter")
                    checked: false
                    onCheckedChanged: {
                        if (!checked && studio.filtersActive)
                            studio.clearFilters();
                    }
                }

                CustomLabel {
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    text: qsTr("Rating")
                }
                CustomComboBox {
                    id: ratingFilter
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    model: [qsTr("Any"), qsTr("≥ 1"), qsTr("≥ 2"), qsTr("≥ 3"), qsTr("≥ 4"), qsTr("≥ 5"), qsTr("Exact 0"), qsTr("Exact 1"), qsTr("Exact 2"), qsTr("Exact 3"), qsTr("Exact 4"), qsTr("Exact 5")]
                    Layout.preferredWidth: 140
                    onActivated: function (index) {
                        if (index === 0)
                            studio.setRatingFilter("any", 0);
                        else if (index <= 5)
                            studio.setRatingFilter("min", index);
                        else
                            studio.setRatingFilter("exact", index - 6);
                    }
                }

                CustomLabel {
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    text: qsTr("Color")
                }
                Repeater {
                    model: window.colorChoices
                    delegate: Rectangle {
                        required property string modelData
                        Layout.alignment: Qt.AlignVCenter
                        enabled: filterToggle.checked
                        opacity: enabled ? 1 : 0.45
                        width: 18
                        height: 18
                        radius: 9
                        color: window.swatchColor(modelData)
                        border.width: studio.colorFilters.indexOf(modelData) >= 0 ? 2 : 1
                        border.color: studio.colorFilters.indexOf(modelData) >= 0 ? Theme.textColor : Theme.dividerColor
                        MouseArea {
                            anchors.fill: parent
                            enabled: filterToggle.checked
                            onClicked: studio.toggleColorFilter(modelData)
                        }
                    }
                }

                CustomLabel {
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    text: qsTr("Rejected")
                }
                CustomComboBox {
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    model: [qsTr("Include"), qsTr("Exclude"), qsTr("Only")]
                    Layout.preferredWidth: 120
                    currentIndex: studio.rejectFilter === "exclude" ? 1 : (studio.rejectFilter === "only" ? 2 : 0)
                    onActivated: function (index) {
                        studio.setRejectFilter(index === 1 ? "exclude" : (index === 2 ? "only" : "include"));
                    }
                }

                CustomButton {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Clear filters")
                    enabled: filterToggle.checked && studio.filtersActive
                    onClicked: studio.clearFilters()
                }

                CustomComboBox {
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    model: [qsTr("Import time"), qsTr("Filename"), qsTr("Rating")]
                    Layout.preferredWidth: 140
                    currentIndex: studio.sortField === "name" ? 1 : (studio.sortField === "rating" ? 2 : 0)
                    onActivated: function (index) {
                        const field = index === 1 ? "name" : (index === 2 ? "rating" : "imported");
                        studio.setSort(field, studio.sortDirection);
                    }
                }
                CustomButton {
                    Layout.alignment: Qt.AlignVCenter
                    enabled: filterToggle.checked
                    text: studio.sortDirection === "asc" ? qsTr("Asc") : qsTr("Desc")
                    onClicked: studio.setSort(studio.sortField, studio.sortDirection === "asc" ? "desc" : "asc")
                }

                Item {
                    Layout.fillWidth: true
                }

                SegmentedControl {
                    Layout.alignment: Qt.AlignVCenter
                    model: [qsTr("Gallery"), qsTr("Edit")]
                    currentIndex: studio.browseMode === "develop" ? 1 : 0
                    enabled: studio.catalogOpen
                    onActivated: function (index) {
                        if (index === 1) {
                            studioActions.run(studioActions.ids.viewDevelop);
                            return;
                        }
                        studioActions.openGallery(window.lastGalleryMode);
                    }
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

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            visible: studio.catalogOpen
            handle: Rectangle {
                implicitWidth: 1
                implicitHeight: 1
                color: SplitHandle.pressed || SplitHandle.hovered ? Theme.midColor : Theme.splitHandleColor
            }

            LibrarySidePanel {
                SplitView.preferredWidth: 240
                SplitView.minimumWidth: 160
                presenter: studio
                viewRectX: navigatorVisible.x
                viewRectY: navigatorVisible.y
                viewRectW: navigatorVisible.width
                viewRectH: navigatorVisible.height
                onViewportSeeked: function (nx, ny) {
                    window.seekNavigatorViewport(nx, ny);
                }
            }

            Item {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 280

                Rectangle {
                    anchors.fill: parent
                    color: studio.browseMode === "grid" ? Theme.contentSurfaceColor : Theme.imageSurroundColor
                }

                GridView {
                    id: grid
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: galleryReviewBar.top
                    anchors.margins: Fonts.size8
                    visible: studio.browseMode === "grid"
                    clip: true
                    model: studio.assets
                    cellWidth: studio.thumbnailSize + Fonts.size12
                    cellHeight: studio.thumbnailSize + Fonts.size12
                    cacheBuffer: cellHeight * 4
                    onVisibleChanged: if (visible && studio.selectedIndex >= 0)
                        positionViewAtIndex(studio.selectedIndex, GridView.Contain)
                    Connections {
                        target: studio
                        function onBrowseModeChanged() {
                            if (studio.browseMode === "grid" && studio.selectedIndex >= 0)
                                grid.positionViewAtIndex(studio.selectedIndex, GridView.Contain);
                        }
                        function onSelectionChanged() {
                            if (studio.browseMode === "grid" && studio.selectedIndex >= 0)
                                grid.positionViewAtIndex(studio.selectedIndex, GridView.Contain);
                        }
                    }
                    delegate: Item {
                        id: tile
                        required property string assetId
                        required property string displayName
                        required property string mediaType
                        required property int rating
                        required property string colorLabel
                        required property bool rejected
                        required property url thumbnailUrl
                        required property string thumbnailState
                        required property string importState
                        required property bool hasEdits
                        required property bool selected
                        required property int pixelWidth
                        required property int pixelHeight
                        required property int index
                        width: grid.cellWidth
                        height: grid.cellHeight

                        Component.onCompleted: studio.ensureThumbnail(tile.assetId)

                        ThumbnailCell {
                            anchors.fill: parent
                            thumbnailUrl: tile.thumbnailUrl
                            thumbnailState: tile.thumbnailState
                            importState: tile.importState
                            rating: tile.rating
                            colorLabel: tile.colorLabel
                            rejected: tile.rejected
                            hasEdits: tile.hasEdits
                            selected: tile.selected
                            current: tile.assetId === studio.selectedAssetId
                            sequenceNumber: tile.index + 1
                            displayName: tile.displayName
                            mediaType: tile.mediaType
                            pixelWidth: tile.pixelWidth
                            pixelHeight: tile.pixelHeight
                            swatchColor: window.swatchColor
                            onClicked: function (mouse) {
                                studioActions.handlePhotoClick(tile.assetId, mouse);
                            }
                            onDoubleClicked: studioActions.handlePhotoDoubleClick(tile.assetId)
                        }
                    }

                    CustomLabel {
                        anchors.centerIn: parent
                        visible: grid.count === 0
                        text: studio.filtersActive ? qsTr("No photos match the current filters.") : qsTr("No photos imported yet.")
                        color: Theme.placeholderTextColor
                    }
                }

                ColumnLayout {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: galleryReviewBar.top
                    visible: studio.browseMode !== "grid"
                    spacing: 0

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Flickable {
                            id: scroller
                            anchors.fill: parent
                            clip: true
                            interactive: !(studio.browseMode === "develop" && studio.cropToolActive)
                            contentWidth: previewStage.width
                            contentHeight: previewStage.height
                            boundsBehavior: Flickable.StopAtBounds

                            Item {
                                id: previewStage
                                width: {
                                    if (studio.zoomMode === "fit")
                                        return scroller.width;
                                    if (studio.zoomMode === "fill")
                                        return Math.max(scroller.width, previewImage.implicitWidth * (scroller.height / Math.max(previewImage.implicitHeight, 1)));
                                    if (studio.zoomMode === "actual")
                                        return Math.max(1, previewImage.implicitWidth);
                                    return Math.max(1, previewImage.implicitWidth * studio.zoomFactor);
                                }
                                height: {
                                    if (studio.zoomMode === "fit")
                                        return scroller.height;
                                    if (studio.zoomMode === "fill")
                                        return Math.max(scroller.height, previewImage.implicitHeight * (scroller.width / Math.max(previewImage.implicitWidth, 1)));
                                    if (studio.zoomMode === "actual")
                                        return Math.max(1, previewImage.implicitHeight);
                                    return Math.max(1, previewImage.implicitHeight * studio.zoomFactor);
                                }

                                readonly property real sourceW: Math.max(previewImage.implicitWidth, 1)
                                readonly property real sourceH: Math.max(previewImage.implicitHeight, 1)
                                readonly property real containScale: Math.min(width / sourceW, height / sourceH)
                                readonly property real baseW: sourceW * containScale
                                readonly property real baseH: sourceH * containScale
                                readonly property real rotateScale: {
                                    if (!(studio.browseMode === "develop" && studio.cropToolActive && studio.cropGuideReady))
                                        return 1;
                                    const rad = Math.abs(studio.editStraighten) * Math.PI / 180;
                                    const c = Math.cos(rad);
                                    const s = Math.sin(rad);
                                    const aabbW = c * baseW + s * baseH;
                                    const aabbH = s * baseW + c * baseH;
                                    return Math.min(width / Math.max(aabbW, 1), height / Math.max(aabbH, 1));
                                }

                                Item {
                                    id: photoPlane
                                    width: previewStage.baseW * previewStage.rotateScale
                                    height: previewStage.baseH * previewStage.rotateScale
                                    x: (previewStage.width - width) / 2
                                    y: (previewStage.height - height) / 2
                                    rotation: studio.browseMode === "develop" && studio.cropToolActive && studio.cropGuideReady ? studio.editStraighten : 0
                                    transformOrigin: Item.Center
                                    antialiasing: true

                                    Image {
                                        id: previewImage
                                        anchors.fill: parent
                                        asynchronous: false
                                        cache: false
                                        source: studio.previewUrl
                                        fillMode: Image.Stretch
                                        smooth: true
                                        antialiasing: true
                                    }

                                    Rectangle {
                                        anchors.fill: parent
                                        color: "transparent"
                                        border.width: 1
                                        border.color: Qt.rgba(1, 1, 1, 0.92)
                                        visible: studio.browseMode === "develop" && studio.cropToolActive && Math.abs(studio.editStraighten) > 0.04
                                        antialiasing: true
                                    }
                                }

                                CropOverlay {
                                    anchors.fill: parent
                                    visible: studio.browseMode === "develop" && studio.cropToolActive && studio.cropGuideReady && photoPlane.width > 1
                                    sourceWidth: photoPlane.width
                                    sourceHeight: photoPlane.height
                                    cropX: studio.editCropX
                                    cropY: studio.editCropY
                                    cropWidth: studio.editCropWidth
                                    cropHeight: studio.editCropHeight
                                    onCropCommitted: function (x, y, w, h) {
                                        studioActions.setCropRect(x, y, w, h);
                                    }
                                }
                            }

                            WheelHandler {
                                onWheel: function (event) {
                                    studio.adjustZoom(event.angleDelta.y);
                                    event.accepted = true;
                                }
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            acceptedButtons: Qt.RightButton
                            onClicked: window.showPhotoMenu()
                        }

                        CustomLabel {
                            anchors.centerIn: parent
                            visible: previewImage.source.toString().length === 0 && !studio.previewLoading
                            text: studio.selectedImportState === "missing" ? qsTr("Original file is missing.") : qsTr("Select a photo to inspect.")
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
                }

                GalleryReviewBar {
                    id: galleryReviewBar
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    presenter: studio
                    commands: studioActions
                    colorChoices: window.colorChoices
                    swatchColor: window.swatchColor
                }
            }

            InspectorSidePanel {
                SplitView.preferredWidth: 320
                SplitView.minimumWidth: 260
                presenter: studio
                commands: studioActions
            }
        }

        FilmStripBar {
            visible: studio.catalogOpen
            Layout.fillWidth: true
            Layout.preferredHeight: 108
            Layout.minimumHeight: 88
            presenter: studio
            commands: studioActions
            swatchColor: window.swatchColor
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: !studio.catalogOpen
            color: Theme.windowColor
            CustomLabel {
                anchors.centerIn: parent
                text: qsTr("Create or open a library to import photos.")
                color: Theme.placeholderTextColor
            }
        }

        MainStatusBar {
            Layout.fillWidth: true
            statusText: studio.statusText
            viewerText: studio.previewLoading ? qsTr("Loading preview…") : (studio.selectedAssetId.length > 0 ? (studio.visibleCount + " photos") : "")
        }
    }

    PhotoContextMenu {
        id: photoMenu
        commands: studioActions
    }

    SettingsPage {
        anchors.fill: parent
        visible: window.settingsOpen
        z: 20
        presenter: studio
        onCloseRequested: window.settingsOpen = false
    }

    MessageDialog {
        id: removeDialog
        parentItem: window.contentItem
        titleText: qsTr("Remove from Catalog")
        messageText: qsTr("Remove this photo from the library? The original file on disk will not be deleted.")
        buttons: [qsTr("Cancel"), qsTr("Remove")]
        defaultButtonText: qsTr("Remove")
        onFinished: function (buttonText) {
            if (buttonText === qsTr("Remove"))
                studioActions.run(studioActions.ids.photoRemove);
        }
    }

    MessageDialog {
        id: deleteDiskDialog
        parentItem: window.contentItem
        titleText: qsTr("Delete from Disk")
        messageText: qsTr("Permanently delete the original file from disk and remove it from the library? This cannot be undone.")
        buttons: [qsTr("Cancel"), qsTr("Delete")]
        defaultButtonText: qsTr("Delete")
        onFinished: function (buttonText) {
            if (buttonText === qsTr("Delete"))
                studioActions.run(studioActions.ids.photoRemoveFromDisk);
        }
    }

    MessageDialog {
        id: aboutDialog
        parentItem: window.contentItem
        titleText: qsTr("About Ravo Studio")
        messageText: qsTr("Ravo Studio is a local photo library and editor.")
        buttons: [qsTr("OK")]
        defaultButtonText: qsTr("OK")
    }

    QmlFileDialogPage {
        id: createDialog
        dialogTitle: qsTr("Create Library")
        dialogMode: "save"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studio.createCatalogFromPath(filePath);
        }
    }

    QmlFileDialogPage {
        id: openDialog
        dialogTitle: qsTr("Open Library")
        dialogMode: "open"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studio.openCatalogFromPath(filePath);
        }
    }

    QmlFileDialogPage {
        id: importDialog
        dialogTitle: qsTr("Import Photos")
        dialogMode: "openFiles"
        nameFilters: ["Photos (*.png *.jpg *.jpeg *.JPG *.JPEG *.tif *.tiff *.bmp *.gif *.webp *.arw *.ARW *.cr2 *.CR2 *.cr3 *.CR3 *.nef *.NEF *.dng *.DNG *.raf *.orf *.rw2)", "All files (*)"]
        onFileAccepted: function (filePath, selectedFilter, filePaths) {
            studio.importFilePaths(filePaths);
        }
    }

    FolderDialogPage {
        id: importFolderDialog
        dialogTitle: qsTr("Import Folder")
        onFolderAccepted: function (folderPath) {
            studio.importFolderFromPath(folderPath);
        }
    }

    QmlFileDialogPage {
        id: exportDialog
        dialogTitle: qsTr("Export Photo")
        dialogMode: "save"
        nameFilters: ["JPEG (*.jpg *.jpeg)", "PNG (*.png)", "TIFF (*.tif *.tiff)", "Original copy (*)"]
        onFileAccepted: function (filePath, selectedFilter) {
            studioActions.run(studioActions.ids.libraryExportWrite, {
                    "path": filePath,
                    "filter": selectedFilter
                });
        }
    }
}
