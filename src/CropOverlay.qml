import QtQuick

// Rubber-band crop over the video. Stores a normalized rect of the picture
// (not the letterbox). Playback is not cropped — export applies the rect.
Item {
    id: root
    objectName: "cropOverlay"

    property color accent: "#FFD60A"
    property real contentX: 0
    property real contentY: 0
    property real contentW: 0
    property real contentH: 0
    property int videoWidth: 0
    property int videoHeight: 0

    property bool active: false
    property bool drawing: false
    property real cropX: 0
    property real cropY: 0
    property real cropW: 0
    property real cropH: 0
    // "" freeform, "portrait" 9:16, "landscape" 16:9, "square" 1:1
    property string aspectMode: ""

    readonly property bool hasContent: contentW > 1 && contentH > 1
    readonly property real areaX: hasContent ? contentX : 0
    readonly property real areaY: hasContent ? contentY : 0
    readonly property real areaW: hasContent ? contentW : width
    readonly property real areaH: hasContent ? contentH : height
    readonly property real boxX: areaX + cropX * areaW
    readonly property real boxY: areaY + cropY * areaH
    readonly property real boxW: cropW * areaW
    readonly property real boxH: cropH * areaH
    readonly property real lockedAspect: aspectMode === "portrait" ? 9 / 16
                                       : aspectMode === "landscape" ? 16 / 9
                                       : aspectMode === "square" ? 1
                                       : 0
    readonly property rect sourceRect: {
        if (!active || videoWidth <= 0 || videoHeight <= 0)
            return Qt.rect(0, 0, 0, 0);
        return Qt.rect(Math.round(cropX * videoWidth),
                       Math.round(cropY * videoHeight),
                       Math.round(cropW * videoWidth),
                       Math.round(cropH * videoHeight));
    }
    readonly property real handleSize: 10
    readonly property real handleHit: 16
    readonly property real minItemSize: 8
    readonly property real minVideoSize: 16

    property bool resizing: false
    property bool moving: false
    property bool scaleFromCenter: false
    property int resizeCorner: -1
    property bool pressOnDim: false
    property real moveOrigX: 0
    property real moveOrigY: 0
    property real moveOrigW: 0
    property real moveOrigH: 0
    property real moveStartVX: 0
    property real moveStartVY: 0
    property real pressItemX: 0
    property real pressItemY: 0
    property real anchorVX: 0
    property real anchorVY: 0
    property real centerVX: 0
    property real centerVY: 0
    property int hoverCursor: Qt.CrossCursor

    function clear() {
        active = false;
        drawing = false;
        resizing = false;
        moving = false;
        pressOnDim = false;
        scaleFromCenter = false;
        aspectMode = "";
        cropX = 0;
        cropY = 0;
        cropW = 0;
        cropH = 0;
        resizeCorner = -1;
    }

    function pixelRect() {
        return sourceRect;
    }

    function applyPreset(mode) {
        var vw = frameW();
        var vh = frameH();
        if (vw <= 0 || vh <= 0)
            return;
        var rw = vw;
        var rh = vh;
        if (mode === "portrait") {
            rh = vh;
            rw = rh * 9 / 16;
            if (rw > vw) {
                rw = vw;
                rh = rw * 16 / 9;
            }
        } else if (mode === "landscape") {
            rw = vw;
            rh = rw * 9 / 16;
            if (rh > vh) {
                rh = vh;
                rw = rh * 16 / 9;
            }
        } else if (mode === "square") {
            rw = rh = Math.min(vw, vh);
        } else {
            return;
        }
        aspectMode = mode;
        active = true;
        drawing = false;
        setFromVideo( (vw - rw) / 2, (vh - rh) / 2, rw, rh );
    }

    function frameW() {
        return videoWidth > 0 ? videoWidth : Math.max(1, areaW);
    }
    function frameH() {
        return videoHeight > 0 ? videoHeight : Math.max(1, areaH);
    }
    function clamp01(v) {
        return Math.max(0, Math.min(1, v));
    }
    function inContent(px, py) {
        return px >= areaX && px <= areaX + areaW && py >= areaY && py <= areaY + areaH;
    }
    function itemToNorm(px, py) {
        if (areaW <= 0 || areaH <= 0)
            return Qt.point(0, 0);
        return Qt.point(clamp01((px - areaX) / areaW), clamp01((py - areaY) / areaH));
    }
    function itemToVideo(px, py) {
        var n = itemToNorm(px, py);
        return Qt.point(n.x * frameW(), n.y * frameH());
    }
    function setFromVideo(x, y, w, h) {
        var vw = frameW();
        var vh = frameH();
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > vw) w = vw - x;
        if (y + h > vh) h = vh - y;
        if (w < 1 || h < 1)
            return;
        cropX = x / vw;
        cropY = y / vh;
        cropW = w / vw;
        cropH = h / vh;
    }
    function setFromItemCorners(x1, y1, x2, y2) {
        var left = Math.min(x1, x2);
        var right = Math.max(x1, x2);
        var top = Math.min(y1, y2);
        var bot = Math.max(y1, y2);
        left = Math.max(areaX, Math.min(left, areaX + areaW));
        right = Math.max(areaX, Math.min(right, areaX + areaW));
        top = Math.max(areaY, Math.min(top, areaY + areaH));
        bot = Math.max(areaY, Math.min(bot, areaY + areaH));
        if (areaW <= 0 || areaH <= 0)
            return;
        cropX = (left - areaX) / areaW;
        cropY = (top - areaY) / areaH;
        cropW = (right - left) / areaW;
        cropH = (bot - top) / areaH;
    }
    function insideBox(px, py) {
        return px >= boxX && px <= boxX + boxW && py >= boxY && py <= boxY + boxH;
    }
    function handlePos(i) {
        var cx = (i === 0 || i === 3) ? boxX : boxX + boxW;
        var cy = (i === 0 || i === 1) ? boxY : boxY + boxH;
        // Keep the grab target inside the overlay so a full-frame crop (L on
        // 16:9) still has clickable corners.
        var pad = handleSize / 2;
        cx = Math.min(Math.max(cx, pad), Math.max(pad, width - pad));
        cy = Math.min(Math.max(cy, pad), Math.max(pad, height - pad));
        return Qt.point(cx, cy);
    }
    function handleAt(px, py) {
        if (!active || drawing)
            return -1;
        var best = -1;
        var bestD = handleHit;
        for (var i = 0; i < 4; ++i) {
            var c = handlePos(i);
            var d = Math.hypot(px - c.x, py - c.y);
            if (d <= bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }
    function minBox(ratio) {
        var m = minVideoSize;
        if (ratio <= 0)
            return Qt.size(m, m);
        if (ratio >= 1)
            return Qt.size(m * ratio, m);
        return Qt.size(m, m / ratio);
    }
    function commitDraw() {
        drawing = false;
        if (boxW < minItemSize || boxH < minItemSize) {
            clear();
            return;
        }
        aspectMode = "";
        active = true;
    }
    function cursorFor(px, py) {
        var h = resizing ? resizeCorner : handleAt(px, py);
        if (h === 0 || h === 2)
            return Qt.SizeFDiagCursor;
        if (h === 1 || h === 3)
            return Qt.SizeBDiagCursor;
        if (drawing)
            return Qt.CrossCursor;
        if (moving || (active && insideBox(px, py)))
            return Qt.SizeAllCursor;
        if (active && !insideBox(px, py))
            return Qt.PointingHandCursor;
        if (!active)
            return Qt.CrossCursor;
        return Qt.ArrowCursor;
    }
    function resizeTo(px, py, centerScale) {
        var p = itemToVideo(px, py);
        if (centerScale)
            resizeFromCenter(p);
        else if (lockedAspect > 0)
            resizeLocked(p);
        else
            resizeFree(p);
    }
    function resizeFree(p) {
        setFromVideo(Math.min(anchorVX, p.x), Math.min(anchorVY, p.y),
                     Math.abs(p.x - anchorVX), Math.abs(p.y - anchorVY));
    }
    function resizeLocked(p) {
        var r = lockedAspect;
        var dx = p.x - anchorVX;
        var dy = p.y - anchorVY;
        var signX = dx >= 0 ? 1 : -1;
        var signY = dy >= 0 ? 1 : -1;
        var adx = Math.abs(dx);
        var ady = Math.abs(dy);
        var w;
        var h;
        if (adx < 0.5 && ady < 0.5) {
            var m = minBox(r);
            w = m.width;
            h = m.height;
        } else if (ady * r <= adx) {
            h = ady;
            w = h * r;
        } else {
            w = adx;
            h = w / r;
        }
        fitLocked(signX, signY, w, h);
    }
    function fitLocked(signX, signY, w, h) {
        var r = lockedAspect;
        var vw = frameW();
        var vh = frameH();
        var maxW = signX >= 0 ? (vw - anchorVX) : anchorVX;
        var maxH = signY >= 0 ? (vh - anchorVY) : anchorVY;
        maxW = Math.max(0, maxW);
        maxH = Math.max(0, maxH);
        if (w > maxW) {
            w = maxW;
            h = w / r;
        }
        if (h > maxH) {
            h = maxH;
            w = h * r;
        }
        var min = minBox(r);
        if (w < min.width || h < min.height) {
            w = Math.min(min.width, maxW);
            h = w / r;
            if (h > maxH) {
                h = maxH;
                w = h * r;
            }
        }
        var x = signX >= 0 ? anchorVX : anchorVX - w;
        var y = signY >= 0 ? anchorVY : anchorVY - h;
        setFromVideo(x, y, w, h);
    }
    function resizeFromCenter(p) {
        var cx = centerVX;
        var cy = centerVY;
        var vw = frameW();
        var vh = frameH();
        var maxHw = Math.min(cx, vw - cx);
        var maxHh = Math.min(cy, vh - cy);
        var dx = Math.abs(p.x - cx);
        var dy = Math.abs(p.y - cy);
        var hw;
        var hh;
        var r = lockedAspect;
        if (r > 0) {
            if (dy * r <= dx) {
                hh = dy;
                hw = hh * r;
            } else {
                hw = dx;
                hh = hw / r;
            }
            if (hw > maxHw) {
                hw = maxHw;
                hh = hw / r;
            }
            if (hh > maxHh) {
                hh = maxHh;
                hw = hh * r;
            }
            var min = minBox(r);
            hw = Math.max(hw, min.width / 2);
            hh = Math.max(hh, min.height / 2);
            if (hw > maxHw) {
                hw = maxHw;
                hh = hw / r;
            }
            if (hh > maxHh) {
                hh = maxHh;
                hw = hh * r;
            }
        } else {
            hw = Math.min(Math.max(dx, minVideoSize / 2), maxHw);
            hh = Math.min(Math.max(dy, minVideoSize / 2), maxHh);
        }
        setFromVideo(cx - hw, cy - hh, hw * 2, hh * 2);
    }
    function beginResize(corner) {
        resizing = true;
        resizeCorner = corner;
        var opp = (corner + 2) % 4;
        var vx = cropX * frameW();
        var vy = cropY * frameH();
        var vw = cropW * frameW();
        var vh = cropH * frameH();
        var corners = [
            Qt.point(vx, vy),
            Qt.point(vx + vw, vy),
            Qt.point(vx + vw, vy + vh),
            Qt.point(vx, vy + vh)
        ];
        anchorVX = corners[opp].x;
        anchorVY = corners[opp].y;
        centerVX = vx + vw / 2;
        centerVY = vy + vh / 2;
    }
    function beginMove(px, py) {
        moving = true;
        var n = itemToNorm(px, py);
        moveStartVX = n.x;
        moveStartVY = n.y;
        moveOrigX = cropX;
        moveOrigY = cropY;
        moveOrigW = cropW;
        moveOrigH = cropH;
    }
    function moveTo(px, py) {
        var n = itemToNorm(px, py);
        var x = moveOrigX + (n.x - moveStartVX);
        var y = moveOrigY + (n.y - moveStartVY);
        cropX = Math.max(0, Math.min(x, 1 - moveOrigW));
        cropY = Math.max(0, Math.min(y, 1 - moveOrigH));
    }

    // Dim the region outside the committed crop.
    Rectangle {
        visible: root.active && !root.drawing
        x: root.areaX
        y: root.areaY
        width: root.areaW
        height: Math.max(0, root.boxY - root.areaY)
        color: "#99000000"
    }
    Rectangle {
        visible: root.active && !root.drawing
        x: root.areaX
        y: root.boxY + root.boxH
        width: root.areaW
        height: Math.max(0, root.areaY + root.areaH - (root.boxY + root.boxH))
        color: "#99000000"
    }
    Rectangle {
        visible: root.active && !root.drawing
        x: root.areaX
        y: root.boxY
        width: Math.max(0, root.boxX - root.areaX)
        height: root.boxH
        color: "#99000000"
    }
    Rectangle {
        visible: root.active && !root.drawing
        x: root.boxX + root.boxW
        y: root.boxY
        width: Math.max(0, root.areaX + root.areaW - (root.boxX + root.boxW))
        height: root.boxH
        color: "#99000000"
    }

    Canvas {
        id: borderCanvas
        anchors.fill: parent
        visible: root.drawing || (root.active && root.cropW > 0 && root.cropH > 0)
        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);
            if (root.cropW <= 0 || root.cropH <= 0)
                return;
            ctx.strokeStyle = root.accent;
            ctx.lineWidth = 1.5;
            ctx.setLineDash([6, 4]);
            ctx.strokeRect(root.boxX + 0.5, root.boxY + 0.5,
                           Math.max(0, root.boxW - 1), Math.max(0, root.boxH - 1));
        }
    }

    Repeater {
        model: 4
        Rectangle {
            visible: root.active && !root.drawing
            width: root.handleSize
            height: root.handleSize
            radius: 1
            color: root.accent
            x: {
                root.boxX; root.boxW; root.width;
                return root.handlePos(index).x - width / 2;
            }
            y: {
                root.boxY; root.boxH; root.height;
                return root.handlePos(index).y - height / 2;
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.enabled
        hoverEnabled: true
        preventStealing: true
        cursorShape: root.hoverCursor
        onPositionChanged: (mouse) => {
            root.hoverCursor = root.cursorFor(mouse.x, mouse.y);
            if (root.drawing) {
                root.setFromItemCorners(root.pressItemX, root.pressItemY, mouse.x, mouse.y);
                return;
            }
            if (root.resizing) {
                if (mouse.modifiers & Qt.ShiftModifier)
                    root.scaleFromCenter = true;
                root.resizeTo(mouse.x, mouse.y, root.scaleFromCenter);
                return;
            }
            if (root.moving)
                root.moveTo(mouse.x, mouse.y);
        }
        onPressed: (mouse) => {
            root.pressItemX = mouse.x;
            root.pressItemY = mouse.y;
            root.pressOnDim = false;
            var handle = root.handleAt(mouse.x, mouse.y);
            if (handle >= 0) {
                root.beginResize(handle);
                root.scaleFromCenter = !!(mouse.modifiers & Qt.ShiftModifier);
                root.hoverCursor = root.cursorFor(mouse.x, mouse.y);
                return;
            }
            if (root.active) {
                if (root.insideBox(mouse.x, mouse.y)) {
                    root.beginMove(mouse.x, mouse.y);
                    root.hoverCursor = Qt.SizeAllCursor;
                    return;
                }
                root.pressOnDim = true;
                return;
            }
            if (!root.inContent(mouse.x, mouse.y))
                return;
            root.drawing = true;
            root.aspectMode = "";
            root.setFromItemCorners(mouse.x, mouse.y, mouse.x, mouse.y);
            root.hoverCursor = Qt.CrossCursor;
        }
        onReleased: (mouse) => {
            if (root.drawing) {
                root.setFromItemCorners(root.pressItemX, root.pressItemY, mouse.x, mouse.y);
                root.commitDraw();
            } else if (root.resizing) {
                root.resizeTo(mouse.x, mouse.y, root.scaleFromCenter);
                root.resizing = false;
                root.scaleFromCenter = false;
                root.resizeCorner = -1;
            } else if (root.moving) {
                root.moveTo(mouse.x, mouse.y);
                root.moving = false;
            } else if (root.pressOnDim) {
                var dx = mouse.x - root.pressItemX;
                var dy = mouse.y - root.pressItemY;
                if (Math.hypot(dx, dy) < root.minItemSize)
                    root.clear();
            }
            root.pressOnDim = false;
            root.hoverCursor = root.cursorFor(mouse.x, mouse.y);
        }
        onCanceled: {
            if (root.drawing)
                root.clear();
            root.resizing = false;
            root.moving = false;
            root.scaleFromCenter = false;
            root.pressOnDim = false;
            root.resizeCorner = -1;
        }
    }

    onBoxXChanged: borderCanvas.requestPaint()
    onBoxYChanged: borderCanvas.requestPaint()
    onBoxWChanged: borderCanvas.requestPaint()
    onBoxHChanged: borderCanvas.requestPaint()
    onAccentChanged: borderCanvas.requestPaint()
    onDrawingChanged: borderCanvas.requestPaint()
    onActiveChanged: borderCanvas.requestPaint()
    onWidthChanged: borderCanvas.requestPaint()
    onHeightChanged: borderCanvas.requestPaint()
}
