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
    title: studio.catalogOpen ? qsTr("Ravo Studio — %1").arg(studio.catalogPath) :
                                qsTr("Ravo Studio")
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
    property string removeConfirmationToken: ""
    property string deleteConfirmationToken: ""
    readonly property bool studioInteractive: !settingsOpen && !studioCommands.paletteOpen
    readonly property bool textInputActive: activeFocusItem &&
                                                   (activeFocusItem instanceof TextInput ||
                                                    activeFocusItem instanceof TextEdit)
    property string lastGalleryMode: "grid"
    property string pendingExportFormat: ""
    property var pendingExportOptions: ({})
    property string pendingExportFilenameTemplate: ""
    property string viewportAssetId: ""
    property var inspectViewportFocus: null
    property var inspectViewportRestore: null
    property var pendingInspectStagePos: null
    property var inspectZoomFrom: null
    property var inspectZoomCommit: null
    property real savedInspectContentX: 0
    property real savedInspectContentY: 0
    property bool inspectZoomPending: false
    property bool inspectZoomAnimating: false
    property bool inspectZoomIgnoreStop: false
    property real inspectStageLockW: -1
    property real inspectStageLockH: -1
    property real inspectAnimScale: 1
    property real inspectAnimOriginX: 0
    property real inspectAnimOriginY: 0
    readonly property int inspectZoomDurationMs: 240
    readonly property bool photoInspectEnabled: {
        if (studio.browseMode === "grid" ||
                (studio.browseMode === "develop" && studio.cropToolActive))
            return false;
        if (typeof previewImage === "undefined")
            return false;
        return previewImage.status === Image.Ready && studio.previewUrl.toString().length > 0;
    }
    readonly property rect navigatorVisible: {
        if (studio.browseMode === "grid" || typeof photoPlane === "undefined" || photoPlane.width < 1 || scroller.width < 1)
            return Qt.rect(0, 0, 1, 1);
        const s = Math.max(window.inspectAnimScale, 0.0001);
        const ox = window.inspectAnimOriginX;
        const oy = window.inspectAnimOriginY;
        const stageL = ox + (scroller.contentX - ox) / s;
        const stageT = oy + (scroller.contentY - oy) / s;
        const stageR = ox + (scroller.contentX + scroller.width - ox) / s;
        const stageB = oy + (scroller.contentY + scroller.height - oy) / s;
        const visL = Math.max(stageL, photoPlane.x);
        const visT = Math.max(stageT, photoPlane.y);
        const visR = Math.min(stageR, photoPlane.x + photoPlane.width);
        const visB = Math.min(stageB, photoPlane.y + photoPlane.height);
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

    function centerPhotoViewport() {
        if (typeof scroller === "undefined")
            return;
        Qt.callLater(function () {
            window.centerPhotoViewportNow();
        });
    }

    function centerPhotoViewportNow() {
        if (typeof scroller === "undefined")
            return;
        const maxX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxY = Math.max(0, scroller.contentHeight - scroller.height);
        scroller.contentX = maxX / 2;
        scroller.contentY = maxY / 2;
    }

    function applyPhotoViewportAfterZoom() {
        Qt.callLater(function () {
            if (typeof scroller === "undefined" || typeof photoPlane === "undefined")
                return;
            const maxX = Math.max(0, scroller.contentWidth - scroller.width);
            const maxY = Math.max(0, scroller.contentHeight - scroller.height);
            if (window.inspectViewportFocus) {
                const focus = window.inspectViewportFocus;
                window.inspectViewportFocus = null;
                scroller.contentX = Math.max(0, Math.min(maxX, photoPlane.x + focus.fx * photoPlane.width - focus.anchorX));
                scroller.contentY = Math.max(0, Math.min(maxY, photoPlane.y + focus.fy * photoPlane.height - focus.anchorY));
                return;
            }
            if (window.inspectViewportRestore) {
                const restore = window.inspectViewportRestore;
                window.inspectViewportRestore = null;
                scroller.contentX = Math.max(0, Math.min(maxX, restore.x));
                scroller.contentY = Math.max(0, Math.min(maxY, restore.y));
                return;
            }
            window.centerPhotoViewportNow();
        });
    }

    function unlockedPhotoStageSize(mode, factor) {
        const srcW = Math.max(previewImage.implicitWidth, 1);
        const srcH = Math.max(previewImage.implicitHeight, 1);
        if (mode === "fit")
            return { "w": scroller.width, "h": scroller.height };
        if (mode === "fill")
            return {
                "w": Math.max(scroller.width, srcW * (scroller.height / srcH)),
                "h": Math.max(scroller.height, srcH * (scroller.width / srcW))
            };
        if (mode === "actual")
            return { "w": srcW, "h": srcH };
        return { "w": Math.max(1, srcW * factor), "h": Math.max(1, srcH * factor) };
    }

    function photoPlaneRectForStage(stageW, stageH) {
        const srcW = Math.max(previewImage.implicitWidth, 1);
        const srcH = Math.max(previewImage.implicitHeight, 1);
        const contain = Math.min(stageW / srcW, stageH / srcH);
        const planeW = srcW * contain;
        const planeH = srcH * contain;
        return {
            "x": (stageW - planeW) / 2,
            "y": (stageH - planeH) / 2,
            "w": planeW,
            "h": planeH
        };
    }

    function clearInspectZoomVisual() {
        inspectZoomAnimating = false;
        inspectZoomPending = false;
        inspectZoomFrom = null;
        inspectZoomCommit = null;
        inspectZoomAnim.stop();
        inspectAnimScale = 1;
        inspectStageLockW = -1;
        inspectStageLockH = -1;
    }

    function abortInspectZoomAnimation() {
        clearInspectZoomVisual();
        inspectViewportFocus = null;
        inspectViewportRestore = null;
    }

    function commitInspectZoomAnimation() {
        const commit = inspectZoomCommit;
        inspectZoomAnimating = false;
        inspectZoomPending = false;
        inspectZoomFrom = null;
        inspectZoomCommit = null;
        inspectViewportFocus = null;
        inspectViewportRestore = null;
        inspectZoomAnim.stop();
        inspectAnimScale = 1;
        inspectStageLockW = -1;
        inspectStageLockH = -1;
        if (!commit || typeof scroller === "undefined")
            return;
        const maxX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxY = Math.max(0, scroller.contentHeight - scroller.height);
        scroller.contentX = Math.max(0, Math.min(maxX, commit.x));
        scroller.contentY = Math.max(0, Math.min(maxY, commit.y));
    }

    function beginInspectZoomAnimation() {
        inspectZoomPending = false;
        const from = inspectZoomFrom;
        if (!from || typeof scroller === "undefined" || typeof previewImage === "undefined") {
            clearInspectZoomVisual();
            applyPhotoViewportAfterZoom();
            return;
        }
        const targetStage = unlockedPhotoStageSize(studio.zoomMode, studio.zoomFactor);
        const targetPlane = photoPlaneRectForStage(targetStage.w, targetStage.h);
        const startW = Math.max(1, from.planeW);
        const sEnd = targetPlane.w / startW;
        if (!isFinite(sEnd) || sEnd <= 0 || !isFinite(targetPlane.w) || targetPlane.w < 1) {
            clearInspectZoomVisual();
            applyPhotoViewportAfterZoom();
            return;
        }

        let targetX = 0;
        let targetY = 0;
        if (from.goingToActual) {
            targetX = targetPlane.x + from.fx * targetPlane.w - from.anchorX;
            targetY = targetPlane.y + from.fy * targetPlane.h - from.anchorY;
        } else {
            targetX = from.restoreX;
            targetY = from.restoreY;
        }
        const maxTargetX = Math.max(0, targetStage.w - scroller.width);
        const maxTargetY = Math.max(0, targetStage.h - scroller.height);
        targetX = Math.max(0, Math.min(maxTargetX, targetX));
        targetY = Math.max(0, Math.min(maxTargetY, targetY));

        const ox = from.originX;
        const oy = from.originY;
        const cEndX = ox * (1 - sEnd) + from.planeX * sEnd - targetPlane.x + targetX;
        const cEndY = oy * (1 - sEnd) + from.planeY * sEnd - targetPlane.y + targetY;
        if (!isFinite(cEndX) || !isFinite(cEndY) || !isFinite(ox) || !isFinite(oy)) {
            clearInspectZoomVisual();
            applyPhotoViewportAfterZoom();
            return;
        }
        inspectZoomCommit = { "x": targetX, "y": targetY };

        const scaleDelta = Math.abs(sEnd - 1);
        const panDelta = Math.abs(cEndX - scroller.contentX) + Math.abs(cEndY - scroller.contentY);
        if (scaleDelta < 0.01 && panDelta < 1) {
            commitInspectZoomAnimation();
            return;
        }

        inspectAnimOriginX = ox;
        inspectAnimOriginY = oy;
        inspectAnimScale = 1;
        inspectZoomAnimating = true;
        const maxPanX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxPanY = Math.max(0, scroller.contentHeight - scroller.height);
        inspectZoomScaleAnim.from = 1;
        inspectZoomScaleAnim.to = sEnd;
        inspectZoomPanXAnim.from = scroller.contentX;
        inspectZoomPanXAnim.to = Math.max(0, Math.min(maxPanX, cEndX));
        inspectZoomPanYAnim.from = scroller.contentY;
        inspectZoomPanYAnim.to = Math.max(0, Math.min(maxPanY, cEndY));
        inspectZoomIgnoreStop = true;
        inspectZoomAnim.stop();
        inspectZoomIgnoreStop = false;
        inspectZoomAnim.start();
    }

    function inspectPointInPhoto(pos) {
        if (typeof photoPlane === "undefined" || photoPlane.width < 1 || photoPlane.height < 1)
            return false;
        return pos.x >= photoPlane.x && pos.x <= photoPlane.x + photoPlane.width &&
               pos.y >= photoPlane.y && pos.y <= photoPlane.y + photoPlane.height;
    }

    function togglePhotoInspectZoom(stagePos) {
        if (!window.photoInspectEnabled || !studioActions.ids.viewToggleActualSize)
            return;
        if (window.inspectZoomAnimating)
            return;
        const goingToActual = studio.zoomMode !== "actual";
        const w = Math.max(1, photoPlane.width);
        const h = Math.max(1, photoPlane.height);
        const fx = (stagePos.x - photoPlane.x) / w;
        const fy = (stagePos.y - photoPlane.y) / h;
        const anchorX = stagePos.x - scroller.contentX;
        const anchorY = stagePos.y - scroller.contentY;
        if (goingToActual) {
            window.inspectViewportRestore = null;
            window.inspectViewportFocus = {
                "fx": fx,
                "fy": fy,
                "anchorX": anchorX,
                "anchorY": anchorY
            };
            window.savedInspectContentX = scroller.contentX;
            window.savedInspectContentY = scroller.contentY;
        } else {
            window.inspectViewportFocus = null;
            window.inspectViewportRestore = {
                "x": window.savedInspectContentX,
                "y": window.savedInspectContentY
            };
        }
        window.inspectZoomFrom = {
            "goingToActual": goingToActual,
            "planeX": photoPlane.x,
            "planeY": photoPlane.y,
            "planeW": w,
            "planeH": h,
            "originX": stagePos.x,
            "originY": stagePos.y,
            "fx": fx,
            "fy": fy,
            "anchorX": anchorX,
            "anchorY": anchorY,
            "restoreX": window.savedInspectContentX,
            "restoreY": window.savedInspectContentY
        };
        window.inspectStageLockW = previewStage.width;
        window.inspectStageLockH = previewStage.height;
        window.inspectZoomPending = true;
        studioActions.run(studioActions.ids.viewToggleActualSize);
        if (window.inspectZoomPending)
            window.abortInspectZoomAnimation();
    }

    Timer {
        id: inspectClickTimer
        interval: Math.max(180, Qt.styleHints.mouseDoubleClickInterval)
        repeat: false
        onTriggered: {
            if (window.pendingInspectStagePos)
                window.togglePhotoInspectZoom(window.pendingInspectStagePos);
            window.pendingInspectStagePos = null;
        }
    }

    ParallelAnimation {
        id: inspectZoomAnim
        NumberAnimation {
            id: inspectZoomScaleAnim
            target: window
            property: "inspectAnimScale"
            duration: window.inspectZoomDurationMs
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            id: inspectZoomPanXAnim
            target: scroller
            property: "contentX"
            duration: window.inspectZoomDurationMs
            easing.type: Easing.OutCubic
        }
        NumberAnimation {
            id: inspectZoomPanYAnim
            target: scroller
            property: "contentY"
            duration: window.inspectZoomDurationMs
            easing.type: Easing.OutCubic
        }
        onStopped: {
            if (window.inspectZoomIgnoreStop)
                return;
            if (window.inspectZoomAnimating)
                window.commitInspectZoomAnimation();
        }
    }

    Connections {
        target: studio
        function onSelectionChanged() {
            if (window.viewportAssetId !== studio.selectedAssetId) {
                window.viewportAssetId = studio.selectedAssetId;
                window.abortInspectZoomAnimation();
                window.inspectViewportFocus = null;
                window.inspectViewportRestore = null;
                window.centerPhotoViewport();
            }
        }
        function onZoomChanged() {
            if (window.inspectZoomPending) {
                window.beginInspectZoomAnimation();
                return;
            }
            window.abortInspectZoomAnimation();
            window.applyPhotoViewportAfterZoom();
        }
        function onBrowseModeChanged() {
            window.abortInspectZoomAnimation();
            window.inspectViewportFocus = null;
            window.inspectViewportRestore = null;
            window.centerPhotoViewport();
        }
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

    function exportNameFilter(format) {
        if (format === "jpeg")
            return qsTr("JPEG (*.jpg *.jpeg)");
        if (format === "png")
            return qsTr("PNG (*.png)");
        if (format === "tiff")
            return qsTr("TIFF (*.tif *.tiff)");
        return qsTr("Original copy (*)");
    }

    function clearPendingExport() {
        window.pendingExportFormat = "";
        window.pendingExportOptions = ({});
        window.pendingExportFilenameTemplate = "";
    }

    function openExportDialog() {
        if (!studio.selectedAssetId.length)
            return;
        window.clearPendingExport();
        exportOptionsDialog.openForExport();
    }

    function openStyleSaveDialog() {
        styleSaveDialog.currentFolder = studio.defaultCatalogFolder;
        styleSaveDialog.initialSelectedFile = studio.selectedDisplayName + ".rstyle.json";
        styleSaveDialog.openDialog();
    }

    function openStyleApplyDialog() {
        styleApplyDialog.currentFolder = studio.defaultCatalogFolder;
        styleApplyDialog.openDialog();
    }

    function openPresetImportDialog() {
        presetImportDialog.currentFolder = studio.defaultCatalogFolder;
        presetImportDialog.openDialog();
    }

    function startLibrarySession() {
        if (studio.catalogOpen)
            return;
        if (studio.startupCatalogPath.length) {
            studio.openCatalogFromPath(studio.startupCatalogPath);
            return;
        }
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

    Binding {
        target: Theme
        property: "helper"
        value: studioPalette
    }

    StudioActions {
        id: studioActions
        controller: studioCommands
        presenter: studio
        windowHost: window
    }

    Binding {
        target: studioCommands
        property: "settingsOpen"
        value: window.settingsOpen
    }
    Binding {
        target: studioCommands
        property: "textInputActive"
        value: window.textInputActive
    }
    Binding {
        target: studioCommands
        property: "modalOpen"
        value: removeDialog.visible || deleteDiskDialog.visible || aboutDialog.visible ||
               exportOptionsDialog.visible
    }

    StudioCommandShortcuts {
        controller: studioCommands
    }

    menuBar: StudioMenuBar {
        id: studioMenuBar
        controller: studioCommands
    }

    Connections {
        target: studioCommands
        function onPresentationCommandRequested(id, argument) {
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
            else if (id === ids.styleSave)
                openStyleSaveDialog();
            else if (id === ids.styleApply)
                openStyleApplyDialog();
            else if (id === ids.presetImport)
                openPresetImportDialog();
            else if (id === ids.windowSettings)
                window.settingsOpen = true;
            else if (id === ids.windowClose)
                window.close();
            else if (id === ids.windowQuit)
                Qt.quit();
            else if (id === ids.windowAbout)
                openAboutDialog();
            else if (id === ids.photoRemove) {
                window.removeConfirmationToken = String(argument);
                askRemovePhoto();
            } else if (id === ids.photoRemoveFromDisk) {
                window.deleteConfirmationToken = String(argument);
                askDeleteFromDisk();
            } else if (id === "studio.window.dismiss")
                window.settingsOpen = false;
        }
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
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

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

                LibraryFilterBar {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    presenter: studio
                    commands: studioActions
                    colorChoices: window.colorChoices
                    swatchColor: window.swatchColor
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

                CustomButton {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("Assistant")
                    checkable: true
                    checked: studioCommands.assistantOpen
                    tooltipText: qsTr("Show or hide the Assistant panel")
                    Accessible.name: qsTr("Assistant")
                    onClicked: studioActions.trigger(studioActions.ids.windowAssistant)
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
                commands: studioActions
                viewRectX: navigatorVisible.x
                viewRectY: navigatorVisible.y
                viewRectW: navigatorVisible.width
                viewRectH: navigatorVisible.height
                onViewportSeeked: function (nx, ny) {
                    window.seekNavigatorViewport(nx, ny);
                }
            }

            Item {
                id: galleryStage
                SplitView.fillWidth: true
                SplitView.minimumWidth: 280

                readonly property int gridMinCell: 120
                readonly property int gridMaxCell: 320
                readonly property int gridScrollGutter: 14
                function fittedGridCell(availableWidth, preferred) {
                    const inner = Math.max(gridMinCell, availableWidth - gridScrollGutter);
                    const target = Math.min(gridMaxCell, Math.max(gridMinCell, preferred));
                    const cols = Math.max(1, Math.floor(inner / target));
                    return inner / cols;
                }
                function revealGridSelection() {
                    if (studio.browseMode !== "grid" || studio.selectedIndex < 0 || grid.count === 0)
                        return;
                    const cols = Math.max(1, Math.floor(grid.width / Math.max(1, grid.cellWidth)));
                    const row = Math.floor(studio.selectedIndex / cols);
                    const itemY = row * grid.cellHeight;
                    if (itemY < grid.contentY || itemY + grid.cellHeight > grid.contentY + grid.height)
                        grid.positionViewAtIndex(studio.selectedIndex, GridView.Contain);
                }

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
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    pixelAligned: true
                    model: studio.assets
                    cellWidth: galleryStage.fittedGridCell(width, studio.thumbnailSize + Fonts.size12)
                    cellHeight: cellWidth
                    cacheBuffer: cellHeight * 8
                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AlwaysOn
                        implicitWidth: 10
                    }
                    onVisibleChanged: if (visible)
                        galleryStage.revealGridSelection()
                    Connections {
                        target: studio
                        function onBrowseModeChanged() {
                            galleryStage.revealGridSelection();
                        }
                        function onSelectionChanged() {
                            galleryStage.revealGridSelection();
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

                        Component.onCompleted: if (tile.thumbnailState !== "ready")
                            studio.ensureThumbnail(tile.assetId)

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
                            onClicked: function (button, modifiers) {
                                studioActions.handlePhotoClick(tile.assetId, button, modifiers);
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
                        id: photoInspectLayer
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Flickable {
                            id: scroller
                            anchors.fill: parent
                            anchors.margins: Fonts.size8
                            clip: true
                            interactive: !(studio.browseMode === "develop" && studio.cropToolActive) && !window.inspectZoomAnimating
                            contentWidth: previewStage.width
                            contentHeight: previewStage.height
                            boundsBehavior: Flickable.StopAtBounds

                            Item {
                                id: previewStage
                                width: {
                                    if (window.inspectStageLockW >= 0)
                                        return window.inspectStageLockW;
                                    if (studio.zoomMode === "fit")
                                        return scroller.width;
                                    if (studio.zoomMode === "fill")
                                        return Math.max(scroller.width, previewImage.implicitWidth * (scroller.height / Math.max(previewImage.implicitHeight, 1)));
                                    if (studio.zoomMode === "actual")
                                        return Math.max(1, previewImage.implicitWidth);
                                    return Math.max(1, previewImage.implicitWidth * studio.zoomFactor);
                                }
                                height: {
                                    if (window.inspectStageLockH >= 0)
                                        return window.inspectStageLockH;
                                    if (studio.zoomMode === "fit")
                                        return scroller.height;
                                    if (studio.zoomMode === "fill")
                                        return Math.max(scroller.height, previewImage.implicitHeight * (scroller.width / Math.max(previewImage.implicitWidth, 1)));
                                    if (studio.zoomMode === "actual")
                                        return Math.max(1, previewImage.implicitHeight);
                                    return Math.max(1, previewImage.implicitHeight * studio.zoomFactor);
                                }
                                transform: Scale {
                                    origin.x: window.inspectAnimOriginX
                                    origin.y: window.inspectAnimOriginY
                                    xScale: window.inspectAnimScale
                                    yScale: window.inspectAnimScale
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

                                    HoverHandler {
                                        id: photoInspectHover
                                        enabled: window.photoInspectEnabled
                                        cursorShape: studio.whiteBalancePickActive ? Qt.CrossCursor : Qt.BlankCursor
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

                                TapHandler {
                                    id: photoSurfaceTap
                                    acceptedButtons: Qt.LeftButton
                                    enabled: studio.browseMode !== "grid"
                                    onTapped: function (eventPoint, button) {
                                        if (photoSurfaceTap.tapCount > 1)
                                            return;
                                        if (!window.photoInspectEnabled || !window.inspectPointInPhoto(eventPoint.position))
                                            return;
                                        if (studio.browseMode === "develop" && studio.whiteBalancePickActive) {
                                            const w = Math.max(1, photoPlane.width);
                                            const h = Math.max(1, photoPlane.height);
                                            studioActions.pickWhiteBalance((eventPoint.position.x - photoPlane.x) / w,
                                                                           (eventPoint.position.y - photoPlane.y) / h);
                                            return;
                                        }
                                        if (studio.browseMode === "loupe") {
                                            window.pendingInspectStagePos = eventPoint.position;
                                            inspectClickTimer.restart();
                                            return;
                                        }
                                        window.togglePhotoInspectZoom(eventPoint.position);
                                    }
                                    onDoubleTapped: function (eventPoint, button) {
                                        inspectClickTimer.stop();
                                        window.pendingInspectStagePos = null;
                                        if (studio.browseMode === "loupe")
                                            studioActions.openGallery("grid");
                                    }
                                }

                                CropOverlay {
                                    anchors.fill: parent
                                    visible: studio.browseMode === "develop" && studio.cropToolActive && photoPlane.width > 1
                                    imageX: photoPlane.x
                                    imageY: photoPlane.y
                                    imageWidth: photoPlane.width
                                    imageHeight: photoPlane.height
                                    imageRotation: photoPlane.rotation
                                    photoItem: photoPlane
                                    sourceWidth: studio.selectedWorkingWidth
                                    sourceHeight: studio.selectedWorkingHeight
                                    minShortEdgePixels: studio.cropMinShortEdgePixels
                                    minShortEdgeFraction: studio.cropMinShortEdgeFraction
                                    cropX: studio.editCropX
                                    cropY: studio.editCropY
                                    cropWidth: studio.editCropWidth
                                    cropHeight: studio.editCropHeight
                                    aspectRatio: studio.cropAspectRatio
                                    straighten: studio.editStraighten
                                    onCropEdited: function (x, y, w, h) {
                                        studioActions.previewCropRect(x, y, w, h);
                                    }
                                    onCropCommitted: function (x, y, w, h) {
                                        studioActions.setCropRect(x, y, w, h);
                                    }
                                    onStraightenEdited: function (degrees) {
                                        studioActions.previewDevelopNumber("straighten", degrees);
                                    }
                                    onStraightenCommitted: function (degrees) {
                                        studioActions.setDevelopNumber("straighten", degrees);
                                    }
                                }
                            }

                            WheelHandler {
                                onWheel: function (event) {
                                    window.abortInspectZoomAnimation();
                                    window.inspectViewportFocus = null;
                                    window.inspectViewportRestore = null;
                                    studioActions.run(studioActions.ids.viewAdjustZoom, event.angleDelta.y);
                                    event.accepted = true;
                                }
                            }
                        }

                        Canvas {
                            id: magnifierCursor
                            width: 28
                            height: 28
                            z: 20
                            antialiasing: true
                            visible: photoInspectHover.hovered && window.photoInspectEnabled && !studio.whiteBalancePickActive
                            property bool zoomOut: studio.zoomMode === "actual"
                            x: {
                                if (!visible)
                                    return 0;
                                return photoPlane.mapToItem(photoInspectLayer, photoInspectHover.point.position.x, photoInspectHover.point.position.y).x - 11;
                            }
                            y: {
                                if (!visible)
                                    return 0;
                                return photoPlane.mapToItem(photoInspectLayer, photoInspectHover.point.position.x, photoInspectHover.point.position.y).y - 11;
                            }
                            onZoomOutChanged: requestPaint()
                            Component.onCompleted: requestPaint()
                            onPaint: {
                                const ctx = getContext("2d");
                                ctx.reset();
                                const cx = 11;
                                const cy = 11;
                                const r = 7;
                                ctx.lineCap = "round";
                                ctx.lineJoin = "round";
                                ctx.strokeStyle = "#111111";
                                ctx.lineWidth = 3.6;
                                ctx.beginPath();
                                ctx.arc(cx, cy, r, 0, Math.PI * 2);
                                ctx.moveTo(cx + r * 0.72, cy + r * 0.72);
                                ctx.lineTo(22, 22);
                                ctx.stroke();
                                ctx.strokeStyle = "#f4f4f4";
                                ctx.lineWidth = 1.8;
                                ctx.beginPath();
                                ctx.arc(cx, cy, r, 0, Math.PI * 2);
                                ctx.moveTo(cx + r * 0.72, cy + r * 0.72);
                                ctx.lineTo(22, 22);
                                ctx.stroke();
                                ctx.beginPath();
                                ctx.moveTo(cx - 3.2, cy);
                                ctx.lineTo(cx + 3.2, cy);
                                if (!magnifierCursor.zoomOut) {
                                    ctx.moveTo(cx, cy - 3.2);
                                    ctx.lineTo(cx, cy + 3.2);
                                }
                                ctx.stroke();
                            }
                        }

                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: window.showPhotoMenu()
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
            viewerText: studio.previewLoading ? qsTr("Loading preview…") :
                                                (studio.selectedAssetId.length > 0 ?
                                                     qsTr("%1 photos").arg(studio.visibleCount) : "")
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
        languageManager: studioLanguage
        assistant: studioAssistant
        onCloseRequested: window.settingsOpen = false
    }

    AssistantPanel {
        id: assistantPanel
        assistant: studioAssistant
        presenter: studio
        windowHost: window
        z: 30
        visible: studioCommands.assistantOpen
        onCloseRequested: studioCommands.assistantOpen = false
    }

    StudioCommandPalette {
        id: commandPalette
        controller: studioCommands
        windowHost: window
        z: 100
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
                studioActions.run(studioActions.ids.photoRemoveConfirmed,
                                  window.removeConfirmationToken);
            studioCommands.cancelPendingConfirmation(window.removeConfirmationToken);
            window.removeConfirmationToken = "";
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
                studioActions.run(studioActions.ids.photoRemoveFromDiskConfirmed,
                                  window.deleteConfirmationToken);
            studioCommands.cancelPendingConfirmation(window.deleteConfirmationToken);
            window.deleteConfirmationToken = "";
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
            studioActions.run(studioActions.ids.libraryCreatePath, filePath);
        }
    }

    QmlFileDialogPage {
        id: openDialog
        dialogTitle: qsTr("Open Library")
        dialogMode: "open"
        nameFilters: ["Ravo catalog (*.sqlite)"]
        onFileAccepted: function (filePath) {
            studioActions.run(studioActions.ids.libraryOpenPath, filePath);
        }
    }

    QmlFileDialogPage {
        id: importDialog
        dialogTitle: qsTr("Import Photos")
        dialogMode: "openFiles"
        nameFilters: ["Photos (*.png *.jpg *.jpeg *.JPG *.JPEG *.tif *.tiff *.bmp *.gif *.webp *.arw *.ARW *.cr2 *.CR2 *.cr3 *.CR3 *.nef *.NEF *.dng *.DNG *.raf *.orf *.rw2)", "All files (*)"]
        onFileAccepted: function (filePath, selectedFilter, filePaths) {
            studioActions.run(studioActions.ids.libraryImportPaths, filePaths);
        }
    }

    FolderDialogPage {
        id: importFolderDialog
        dialogTitle: qsTr("Import Folder")
        onFolderAccepted: function (folderPath) {
            studioActions.run(studioActions.ids.libraryImportFolderPath, folderPath);
        }
    }

    ExportOptionsDialog {
        id: exportOptionsDialog
        parentItem: window.contentItem
        presenter: studio
        onExportAccepted: function (format, options, filenameTemplate) {
            window.pendingExportFormat = format;
            window.pendingExportOptions = options;
            window.pendingExportFilenameTemplate = filenameTemplate;
            if (studio.selectedCount > 1) {
                exportBatchDialog.currentFolder = studio.defaultCatalogFolder;
                exportBatchDialog.openDialog();
            } else {
                exportDialog.nameFilters = [window.exportNameFilter(format)];
                exportDialog.currentFolder = studio.defaultCatalogFolder;
                exportDialog.initialSelectedFile = studio.selectedDisplayName;
                exportDialog.openDialog();
            }
        }
        onExportCanceled: window.clearPendingExport()
    }

    QmlFileDialogPage {
        id: exportDialog
        dialogTitle: qsTr("Save Export")
        dialogMode: "save"
        nameFilters: [qsTr("PNG (*.png)")]
        onFileAccepted: function (filePath) {
            const format = window.pendingExportFormat;
            const options = window.pendingExportOptions;
            window.clearPendingExport();
            studioActions.run(studioActions.ids.libraryExportWrite, {
                    "path": filePath,
                    "format": format,
                    "options": options
                });
        }
        onFileRejected: window.clearPendingExport()
    }

    FolderDialogPage {
        id: exportBatchDialog
        dialogTitle: qsTr("Select Batch Export Folder")
        onFolderAccepted: function (folderPath) {
            const format = window.pendingExportFormat;
            const options = window.pendingExportOptions;
            const filenameTemplate = window.pendingExportFilenameTemplate;
            window.clearPendingExport();
            studioActions.run(studioActions.ids.libraryExportBatchWrite, {
                    "directory": folderPath,
                    "filenameTemplate": filenameTemplate,
                    "format": format,
                    "options": options
                });
        }
        onFolderRejected: window.clearPendingExport()
    }

    QmlFileDialogPage {
        id: styleSaveDialog
        dialogTitle: qsTr("Save Recipe Style")
        dialogMode: "save"
        nameFilters: [qsTr("Ravo recipe style (*.rstyle.json)")]
        onFileAccepted: function (filePath) {
            studioActions.run(studioActions.ids.styleSavePath, filePath);
        }
    }

    QmlFileDialogPage {
        id: styleApplyDialog
        dialogTitle: qsTr("Apply Recipe Style")
        dialogMode: "open"
        nameFilters: [qsTr("Ravo recipe style (*.rstyle.json)"), qsTr("Lightroom preset (*.xmp)")]
        onFileAccepted: function (filePath) {
            studioActions.run(studioActions.ids.styleApplyPath, filePath);
        }
    }

    QmlFileDialogPage {
        id: presetImportDialog
        dialogTitle: qsTr("Import Preset")
        dialogMode: "open"
        nameFilters: [qsTr("Lightroom preset (*.xmp)"), qsTr("Ravo recipe style (*.rstyle.json)")]
        onFileAccepted: function (filePath) {
            studioActions.run(studioActions.ids.presetImportPath, filePath);
        }
    }
}
