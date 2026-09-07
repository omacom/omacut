import QtQuick
import "Format.js" as Format

// A thumbnail filmstrip with two draggable handles and a scrubbable playhead.
// All times are in seconds.
Item {
    id: root
    implicitHeight: 76

    property real durationSec: 0
    property real startSec: 0
    property real endSec: 0
    property real playheadSec: 0
    property bool zoomed: false
    // Frozen at zoom time, so dragging a handle doesn't rescale the view under it.
    property real viewStartSec: 0
    property real viewEndSec: 0
    property int thumbCount: 0
    property int thumbReadyCount: 0
    property int thumbRevision: 0
    // True while the user is dragging anything, so the player won't fight the UI.
    property bool interacting: false
    property int activeMode: 0  // 0 none, 1 start, 2 end, 3 playhead
    readonly property bool trimmingRange: activeMode === 1 || activeMode === 2

    readonly property real handleW: 14
    readonly property real trackX: handleW
    readonly property real trackW: width - 2 * handleW
    // The stretch of the video the track currently shows.
    readonly property real windowStart: zoomed ? viewStartSec : 0
    readonly property real windowEnd: zoomed ? viewEndSec : durationSec
    readonly property real windowLen: Math.max(windowEnd - windowStart, 0.001)
    property color accent: "#FFD60A"
    readonly property color film: "#1c1c1e"
    readonly property real activeTime: activeMode === 1 ? startSec : endSec
    readonly property real activeHandleX: activeMode === 1
        ? xForTime(startSec) - handleW / 2
        : xForTime(endSec) + handleW / 2

    signal scrub(real seconds)

    function xForTime(t) {
        if (durationSec <= 0)
            return trackX;
        return trackX + ((t - windowStart) / windowLen) * trackW;
    }
    function timeForX(x) {
        if (trackW <= 0 || durationSec <= 0)
            return 0;
        var f = (x - trackX) / trackW;
        return windowStart + Math.max(0, Math.min(1, f)) * windowLen;
    }
    // Z zooms to a close-up where the selection fills 80% of the track, leaving
    // 10% slack on each side for fine tuning. If the selection changed since the
    // last zoom, Z zooms again on the new selection; only when zooming wouldn't
    // get any closer does it zoom back out to the whole video.
    function toggleZoom() {
        if (durationSec <= 0)
            return;
        var slack = (endSec - startSec) / 8;
        var newStart = Math.max(0, startSec - slack);
        var newEnd = Math.min(durationSec, endSec + slack);
        if (zoomed && newStart === viewStartSec && newEnd === viewEndSec) {
            zoomed = false;
            return;
        }
        viewStartSec = newStart;
        viewEndSec = newEnd;
        zoomed = true;
    }

    // ---- filmstrip ----
    Rectangle {
        id: track
        x: root.trackX
        y: 4
        width: root.trackW
        height: root.height - 8
        radius: 6
        color: root.film
        clip: true

        Row {
            anchors.fill: parent
            Repeater {
                model: root.thumbCount
                Image {
                    width: track.width / Math.max(root.thumbCount, 1)
                    height: track.height
                    // Bound the decoded texture to the strip height. The filmstrip
                    // has a fixed height, so this never thrashes on resize.
                    sourceSize.height: track.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: false
                    source: index < root.thumbReadyCount
                        ? "image://thumbs/" + root.thumbRevision + "/" + index
                        : ""
                }
            }
        }
    }

    // ---- dim outside the selection ----
    Rectangle {
        x: track.x
        y: track.y
        width: Math.max(0, root.xForTime(root.startSec) - track.x)
        height: track.height
        color: "#00000099"
    }
    Rectangle {
        x: root.xForTime(root.endSec)
        y: track.y
        width: Math.max(0, track.x + track.width - root.xForTime(root.endSec))
        height: track.height
        color: "#00000099"
    }

    // ---- yellow selection frame ----
    Rectangle {
        x: root.xForTime(root.startSec) - root.handleW
        y: 0
        width: (root.xForTime(root.endSec) - root.xForTime(root.startSec)) + 2 * root.handleW
        height: root.height
        radius: 8
        color: "transparent"
        border.color: root.accent
        border.width: 3
    }

    // ---- left/right handles ----
    Component {
        id: handle
        Rectangle {
            radius: 5
            color: root.accent
            Rectangle {  // grip line
                anchors.centerIn: parent
                width: 2
                height: 16
                radius: 1
                color: root.film
            }
        }
    }
    Loader {
        sourceComponent: handle
        x: root.xForTime(root.startSec) - root.handleW
        y: 0
        width: root.handleW
        height: root.height
    }
    Loader {
        sourceComponent: handle
        x: root.xForTime(root.endSec)
        y: 0
        width: root.handleW
        height: root.height
    }

    // ---- playhead ----
    Rectangle {
        visible: root.durationSec > 0
            && root.playheadSec >= root.windowStart && root.playheadSec <= root.windowEnd
        x: root.xForTime(root.playheadSec) - 1
        y: track.y
        width: 2
        height: track.height
        color: "white"
    }

    // ---- floating time bubble while adjusting trim handles ----
    Rectangle {
        id: timeBubble
        visible: root.trimmingRange
        // Follows the label, which grows an hours field past 3600s. The padding
        // keeps the old 82px bubble for an mm:ss.cc time.
        width: timeLabel.contentWidth + 10
        height: 32
        radius: 7
        x: Math.max(0, Math.min(root.width - width, root.activeHandleX - width / 2))
        y: -42
        color: "#2c2c2f"

        Text {
            id: timeLabel
            anchors.centerIn: parent
            text: Format.fmt(root.activeTime)
            color: "white"
            font.pixelSize: 15
            font.family: "monospace"
            font.weight: Font.DemiBold
        }
    }

    Rectangle {
        visible: root.trimmingRange
        x: root.activeHandleX - width / 2
        y: timeBubble.y + timeBubble.height
        width: 2
        height: Math.max(0, track.y - y)
        radius: 1
        color: "white"
    }

    // ---- interaction: one MouseArea, hit-test on press (mirrors the C++ logic) ----
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        property int mode: 0  // 0 none, 1 start, 2 end, 3 playhead

        function hitTest(x) {
            var x0 = root.xForTime(root.startSec);
            var x1 = root.xForTime(root.endSec);
            if (Math.abs(x - (x0 - root.handleW / 2)) <= root.handleW)
                return 1;
            if (Math.abs(x - (x1 + root.handleW / 2)) <= root.handleW)
                return 2;
            if (x >= x0 && x <= x1)
                return 3;
            return 0;
        }

        cursorShape: Qt.ArrowCursor

        onPositionChanged: function (mouse) {
            if (root.durationSec <= 0)
                return;
            if (mode === 0) {
                var h = hitTest(mouse.x);
                cursorShape = (h === 1 || h === 2) ? Qt.SizeHorCursor : Qt.ArrowCursor;
                return;
            }
            var t = root.timeForX(mouse.x);
            var minGap = Math.min(0.1, root.durationSec);
            if (mode === 1) {
                root.startSec = Math.min(t, root.endSec - minGap);
                root.playheadSec = root.startSec;
                root.scrub(root.startSec);
            } else if (mode === 2) {
                root.endSec = Math.max(t, root.startSec + minGap);
                root.playheadSec = root.endSec;
                root.scrub(root.endSec);
            } else {
                root.playheadSec = Math.max(root.startSec, Math.min(t, root.endSec));
                root.scrub(root.playheadSec);
            }
        }

        onPressed: function (mouse) {
            if (root.durationSec <= 0)
                return;
            mode = hitTest(mouse.x);
            root.activeMode = mode;
            root.interacting = mode !== 0;
            if (mode === 3) {
                root.playheadSec = Math.max(root.startSec, Math.min(root.timeForX(mouse.x), root.endSec));
                root.scrub(root.playheadSec);
            }
        }

        onReleased: {
            mode = 0;
            root.activeMode = 0;
            root.interacting = false;
        }
    }
}
