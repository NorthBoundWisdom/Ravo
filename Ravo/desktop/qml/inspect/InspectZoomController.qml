import QtQuick

// Transient inspect zoom/viewport orchestration. Identity/cancellation stay in C++.
QtObject {
    id: root

    property var studio
    property var scroller
    property var photoPlane
    property var previewImage
    property var previewStage
    property var studioActions
    property real devicePixelRatio: 1

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

    property bool comparisonReady: false

    readonly property bool photoInspectEnabled: {
        if (!studio || !previewImage)
            return false;
        if (studio.browseMode === "grid" || studio.browseMode === "survey" || (studio.browseMode === "develop" && studio.cropToolActive))
            return false;
        return previewImage.status === Image.Ready && studio.previewUrl.toString().length > 0;
    }

    function inspectSourceWidth() {
        if (studio.zoomMode === "actual" && studio.selectedWorkingWidth > 0)
            return Math.max(1, Math.round(studio.selectedWorkingWidth / devicePixelRatio));
        const width = Math.max(studio.previewViewportWidth, 1);
        return comparisonReady ? width * 2 : width;
    }

    function inspectSourceHeight() {
        if (studio.zoomMode === "actual" && studio.selectedWorkingHeight > 0)
            return Math.max(1, Math.round(studio.selectedWorkingHeight / devicePixelRatio));
        return Math.max(studio.previewViewportHeight, 1);
    }

    function centerPhotoViewportNow() {
        if (!scroller)
            return;
        const maxX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxY = Math.max(0, scroller.contentHeight - scroller.height);
        scroller.contentX = maxX / 2;
        scroller.contentY = maxY / 2;
    }

    function centerPhotoViewport() {
        if (!scroller)
            return;
        Qt.callLater(function () {
            root.centerPhotoViewportNow();
        });
    }

    function applyPhotoViewportAfterZoom() {
        Qt.callLater(function () {
            if (!scroller || !photoPlane)
                return;
            const maxX = Math.max(0, scroller.contentWidth - scroller.width);
            const maxY = Math.max(0, scroller.contentHeight - scroller.height);
            if (root.inspectViewportFocus) {
                const focus = root.inspectViewportFocus;
                root.inspectViewportFocus = null;
                scroller.contentX = Math.max(0, Math.min(maxX, photoPlane.x + focus.fx * photoPlane.width - focus.anchorX));
                scroller.contentY = Math.max(0, Math.min(maxY, photoPlane.y + focus.fy * photoPlane.height - focus.anchorY));
                return;
            }
            if (root.inspectViewportRestore) {
                const restore = root.inspectViewportRestore;
                root.inspectViewportRestore = null;
                scroller.contentX = Math.max(0, Math.min(maxX, restore.x));
                scroller.contentY = Math.max(0, Math.min(maxY, restore.y));
                return;
            }
            root.centerPhotoViewportNow();
        });
    }

    function unlockedPhotoStageSize(mode, factor) {
        const srcW = root.inspectSourceWidth();
        const srcH = root.inspectSourceHeight();
        if (mode === "fit")
            return {
                "w": scroller.width,
                "h": scroller.height
            };
        if (mode === "fill")
            return {
                "w": Math.max(scroller.width, srcW * (scroller.height / srcH)),
                "h": Math.max(scroller.height, srcH * (scroller.width / srcW))
            };
        if (mode === "actual")
            return {
                "w": srcW,
                "h": srcH
            };
        return {
            "w": Math.max(1, srcW * factor),
            "h": Math.max(1, srcH * factor)
        };
    }

    function photoPlaneRectForStage(stageW, stageH) {
        const srcW = root.inspectSourceWidth();
        const srcH = root.inspectSourceHeight();
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

    function clearInspectZoomVisual(anim) {
        inspectZoomAnimating = false;
        inspectZoomPending = false;
        inspectZoomFrom = null;
        inspectZoomCommit = null;
        if (anim)
            anim.stop();
        inspectAnimScale = 1;
        inspectStageLockW = -1;
        inspectStageLockH = -1;
    }

    function abortInspectZoomAnimation(anim) {
        clearInspectZoomVisual(anim);
        inspectViewportFocus = null;
        inspectViewportRestore = null;
    }

    function commitInspectZoomAnimation(anim) {
        const commit = inspectZoomCommit;
        clearInspectZoomVisual(anim);
        inspectViewportFocus = null;
        inspectViewportRestore = null;
        if (!commit || !scroller)
            return;
        const maxX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxY = Math.max(0, scroller.contentHeight - scroller.height);
        scroller.contentX = Math.max(0, Math.min(maxX, commit.x));
        scroller.contentY = Math.max(0, Math.min(maxY, commit.y));
    }

    function seekNavigatorViewport(nx, ny) {
        if (!studio || studio.browseMode === "grid" || studio.browseMode === "survey" || !photoPlane || photoPlane.width < 1)
            return;
        const maxX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxY = Math.max(0, scroller.contentHeight - scroller.height);
        scroller.contentX = Math.max(0, Math.min(maxX, photoPlane.x + nx * photoPlane.width));
        scroller.contentY = Math.max(0, Math.min(maxY, photoPlane.y + ny * photoPlane.height));
    }

    function inspectPointInPhoto(pos) {
        if (!photoPlane || photoPlane.width < 1 || photoPlane.height < 1)
            return false;
        return pos.x >= photoPlane.x && pos.x <= photoPlane.x + photoPlane.width && pos.y >= photoPlane.y && pos.y <= photoPlane.y + photoPlane.height;
    }

    signal beginZoomAnimationRequested
}
