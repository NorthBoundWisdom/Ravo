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
        if (!studio)
            return 1;
        if (studio.zoomMode === "actual" && studio.selectedWorkingWidth > 0)
            return Math.max(1, Math.round(studio.selectedWorkingWidth / devicePixelRatio));
        const width = Math.max(studio.previewViewportWidth, 1);
        return comparisonReady ? width * 2 : width;
    }

    function inspectSourceHeight() {
        if (!studio)
            return 1;
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

    readonly property rect navigatorVisible: {
        if (!studio || !scroller || !photoPlane)
            return Qt.rect(0, 0, 1, 1);
        if (studio.browseMode === "grid" || studio.browseMode === "survey" || photoPlane.width < 1 || scroller.width < 1)
            return Qt.rect(0, 0, 1, 1);
        const s = Math.max(inspectAnimScale, 0.0001);
        const ox = inspectAnimOriginX;
        const oy = inspectAnimOriginY;
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
        return Qt.rect(Math.max(0, Math.min(1, (visL - photoPlane.x) / w)), Math.max(0, Math.min(1, (visT - photoPlane.y) / h)), Math.max(0.02, Math.min(1, (visR - visL) / w)), Math.max(0.02, Math.min(1, (visB - visT) / h)));
    }

    function togglePhotoInspectZoom(stagePos) {
        if (!photoInspectEnabled || !studioActions || !studioActions.ids.viewToggleActualSize)
            return;
        if (inspectZoomAnimating)
            return;
        if (!scroller || !photoPlane || !previewStage)
            return;
        const goingToActual = studio.zoomMode !== "actual";
        const w = Math.max(1, photoPlane.width);
        const h = Math.max(1, photoPlane.height);
        const fx = (stagePos.x - photoPlane.x) / w;
        const fy = (stagePos.y - photoPlane.y) / h;
        const anchorX = stagePos.x - scroller.contentX;
        const anchorY = stagePos.y - scroller.contentY;
        if (goingToActual) {
            inspectViewportRestore = null;
            inspectViewportFocus = {
                "fx": fx,
                "fy": fy,
                "anchorX": anchorX,
                "anchorY": anchorY
            };
            savedInspectContentX = scroller.contentX;
            savedInspectContentY = scroller.contentY;
        } else {
            inspectViewportFocus = null;
            inspectViewportRestore = {
                "x": savedInspectContentX,
                "y": savedInspectContentY
            };
        }
        inspectZoomFrom = {
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
            "restoreX": savedInspectContentX,
            "restoreY": savedInspectContentY
        };
        inspectStageLockW = previewStage.width;
        inspectStageLockH = previewStage.height;
        inspectZoomPending = true;
        studioActions.run(studioActions.ids.viewToggleActualSize);
        if (inspectZoomPending)
            abortInspectZoomAnimation(null);
    }

    function beginInspectZoomAnimation(anim, scaleAnim, panXAnim, panYAnim) {
        inspectZoomPending = false;
        const from = inspectZoomFrom;
        if (!from || !scroller || !previewImage) {
            clearInspectZoomVisual(anim);
            applyPhotoViewportAfterZoom();
            return;
        }
        const targetStage = unlockedPhotoStageSize(studio.zoomMode, studio.zoomFactor);
        const targetPlane = photoPlaneRectForStage(targetStage.w, targetStage.h);
        const startW = Math.max(1, from.planeW);
        const sEnd = targetPlane.w / startW;
        if (!isFinite(sEnd) || sEnd <= 0 || !isFinite(targetPlane.w) || targetPlane.w < 1) {
            clearInspectZoomVisual(anim);
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
            clearInspectZoomVisual(anim);
            applyPhotoViewportAfterZoom();
            return;
        }
        inspectZoomCommit = {
            "x": targetX,
            "y": targetY
        };

        const scaleDelta = Math.abs(sEnd - 1);
        const panDelta = Math.abs(cEndX - scroller.contentX) + Math.abs(cEndY - scroller.contentY);
        if (scaleDelta < 0.01 && panDelta < 1) {
            commitInspectZoomAnimation(anim);
            return;
        }

        inspectAnimOriginX = ox;
        inspectAnimOriginY = oy;
        inspectAnimScale = 1;
        inspectZoomAnimating = true;
        const maxPanX = Math.max(0, scroller.contentWidth - scroller.width);
        const maxPanY = Math.max(0, scroller.contentHeight - scroller.height);
        if (scaleAnim) {
            scaleAnim.from = 1;
            scaleAnim.to = sEnd;
        }
        if (panXAnim) {
            panXAnim.from = scroller.contentX;
            panXAnim.to = Math.max(0, Math.min(maxPanX, cEndX));
        }
        if (panYAnim) {
            panYAnim.from = scroller.contentY;
            panYAnim.to = Math.max(0, Math.min(maxPanY, cEndY));
        }
        inspectZoomIgnoreStop = true;
        if (anim)
            anim.stop();
        inspectZoomIgnoreStop = false;
        if (anim)
            anim.start();
    }
}
