import QtQuick
import "Format.js" as Format

// The edited video: its clips in play order, each framed with a handle at
// both ends over its own filmstrip, laid over their footage so what's cut
// out stays on the track, dimmed, between them. Drag a handle to trim the
// clip into its source, double-click a clip to split it, double-click the
// line between two pieces of the same stretch to join them, hold a clip to
// move it, and hover a clip to remove it. "t" is seconds along the sequence;
// a clip's in and out are seconds in its source.
Item {
    id: root
    implicitHeight: 76
    // Time runs left to right in every language, so the timeline isn't mirrored.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    readonly property var timeline: backend.timeline
    readonly property var clips: timeline.clips
    readonly property real durationSec: timeline.duration
    property real playheadSec: 0
    property color accent: "#FFD60A"
    property color accentForeground: "black"
    property bool zoomed: false
    // Frozen at zoom time, so the view doesn't shift under an edit.
    property real viewStartSec: 0
    property real viewEndSec: 0

    // The track is a map of the footage: each video laid out whole, from its
    // start to its end, and each clip over its own footage in it. A clip
    // that picks up later in the video of the one before shares its stretch
    // of track; any other clip starts its video over, after the rest of the
    // one before. So a cut leaves its footage on the track, dimmed, instead
    // of closing up, a trim moves no other clip, and dragging a handle back
    // over the cut restores it. Playback and export still skip the cut.
    // Track seconds, used for x below, are seconds along this layout, not
    // the sequence; origin is where a clip's video starts on it.
    function layoutOf(list) {
        var result = [], origin = 0
        for (var i = 0; i < list.length; ++i) {
            var clip = list[i], before = list[i - 1]
            // Within a millisecond, as playback and export carry on.
            if (before && !(before.source === clip.source && clip.in > before.out - 0.001))
                origin = videoEnd(result[i - 1])
            result.push({ start: origin + clip.in, end: origin + clip.out, origin: origin, source: clip.source })
        }
        return result
    }
    readonly property var layout: layoutOf(clips)
    // Where the video of a clip placed at at ends on the track.
    function videoEnd(at) { return at.origin + timeline.sourceDurations[at.source] }
    // Where the zoomed-out track ends: the end of the last clip's video. A
    // trim only moves footage between a clip and the dimmed cuts around it,
    // so this holds unless the trim changes which clips carry on from the one
    // before; adding, removing or moving clips refits it.
    readonly property real footageEnd: layout.length ? videoEnd(layout[layout.length - 1]) : 0
    // An edit that leaves no clip in the zoomed view would leave the bar
    // without one, so it zooms back out. Mid-trim the layout changes live;
    // that waits for the drop.
    function settleView() {
        if (zoomed && !layout.some(at => at.end > viewStartSec && at.start < viewEndSec)) zoomed = false
    }
    onLayoutChanged: if (!trimming) settleView()
    // Something is being dragged, so playback must not move the playhead.
    readonly property bool interacting: area.mode !== 0
    readonly property bool trimming: area.mode === 1

    readonly property real handleW: 14
    // The clip frames' border; the filmstrip sits exactly inside it.
    readonly property int frameW: 3
    readonly property real frameRadius: theme.radius
    // The stretch of the track currently shown, in track seconds.
    readonly property real windowStart: zoomed ? viewStartSec : 0
    // A drag holds the length it started with, so the handle stays under the pointer.
    readonly property real windowEnd: zoomed ? viewEndSec : trimming ? videoEnd(area.frozen[area.frozen.length - 1]) : footageEnd
    readonly property real windowLen: Math.max(windowEnd - windowStart, 0.001)
    readonly property real pxPerSec: width / windowLen
    readonly property color film: "#1c1c1e"
    // Frames are decoded at twice the strip's height, for sharpness on HiDPI.
    readonly property int thumbHeight: 140

    // A filmstrip of video source placed with its start at track time
    // origin, from source time from to to, as far as it's on screen. Frames
    // are placed by source time from originX, so they hold still while a
    // handle slides over them.
    function filmOf(source, origin, from, to) {
        return {
            video: backend.videos[source], originX: xForTime(origin),
            from: Math.max(from, windowStart - origin), to: Math.min(to, windowEnd - origin)
        }
    }

    // Where each clip draws, by index, as {start, end} in track seconds, with
    // origin, where its source's start would sit, and its source. Normally
    // the layout. Dragging a handle keeps every other clip where it was until
    // release, and the dragged clip's footage where it was, so only its edge
    // moves; moving a clip opens a space where it will land.
    readonly property var positions: {
        if (area.mode === 1)
            return area.frozen.map((was, i) => ({ start: was.origin + clips[i].in, end: was.origin + clips[i].out, origin: was.origin, source: was.source }))
        var result = []
        if (area.mode === 4) {
            // Laid out as if dropped, the moving clip itself under the pointer.
            var order = clips.slice(), moving = order.splice(area.clipIndex, 1)[0]
            order.splice(area.moveTarget, 0, moving)
            var landed = layoutOf(order)
            for (var j = 0, k = 0; j < clips.length; ++j) {
                if (j === area.clipIndex) { result.push(null); continue }
                if (k === area.moveTarget) ++k
                result.push(landed[k++])
            }
            var length = moving.out - moving.in
            var at = Math.max(0, Math.min(root.timeForX(area.mouseX) - area.grabOffset, footageEnd - length))
            result[area.clipIndex] = { start: at, end: at + length, origin: at - moving.in, source: moving.source }
            return result
        }
        return layout
    }

    // Each stretch of track showing one video whole, as {source, origin,
    // end}, and the footage cut from it, dimmed, as {start, end}: all but its
    // clips. Each video's filmstrip is laid out once, under its clips and
    // cuts, so what a drag uncovers is already there and nothing reloads.
    // Read from positions alone: mid-drag, the clips change a beat before the
    // positions catch up, and mixing the two would make the film jump. A
    // moving clip carries its own filmstrip, and the cuts are hidden.
    readonly property var runs: {
        var result = []
        if (area.mode === 4) return result
        for (var i = 0; i < positions.length; ++i) {
            var at = positions[i], run = result[result.length - 1]
            if (!run || run.source !== at.source || run.origin !== at.origin)
                result.push(run = { source: at.source, origin: at.origin, clips: [] })
            run.clips.push(at)
            run.end = videoEnd(at)
        }
        return result
    }
    readonly property var cuts: {
        var result = []
        for (var r = 0; r < runs.length; ++r) {
            var from = runs[r].origin
            for (var i = 0; i < runs[r].clips.length; ++i) {
                var at = runs[r].clips[i]
                if (at.start > from) result.push({ start: from, end: at.start })
                from = Math.max(from, at.end)
            }
            if (runs[r].end > from) result.push({ start: from, end: runs[r].end })
        }
        return result
    }
    // Ints, so the Repeaters only rebuild on a real change, not on each
    // re-evaluation a drag causes.
    readonly property int runCount: runs.length
    readonly property int cutCount: cuts.length

    // Asks for the frame at a source time in clip index, e.g. under a dragged handle.
    signal scrubClip(int index, real sourceTime)
    signal scrub(real seconds)

    // Track seconds where sequence time t is shown, and back. Off any clip,
    // a track time goes to the start of the next clip, or the end of the edit.
    function trackTime(t) {
        var i = timeline.clipAt(t), clip = clips[i], at = layout[i]
        return clip && at ? at.start + Math.max(0, Math.min(t - clip.start, clip.end - clip.start)) : 0
    }
    function sequenceTime(track) {
        for (var i = 0; i < clips.length; ++i) {
            if (track < layout[i].start) return clips[i].start
            if (track <= layout[i].end) return clips[i].start + track - layout[i].start
        }
        return durationSec
    }
    function xForTime(track) { return durationSec <= 0 ? 0 : (track - windowStart) * pxPerSec }
    function timeForX(x) {
        if (width <= 0 || durationSec <= 0) return 0
        return windowStart + Math.max(0, Math.min(1, x / width)) * windowLen
    }
    // Z frames the clip under the playhead with some slack; again zooms out.
    // If the clip changed since the last zoom, Z zooms again on it instead.
    function toggleZoom() {
        if (durationSec <= 0) return
        var at = layout[timeline.clipAt(playheadSec)]
        var slack = (at.end - at.start) / 8
        var newStart = Math.max(0, at.start - slack), newEnd = Math.min(footageEnd, at.end + slack)
        if (zoomed && newStart === viewStartSec && newEnd === viewEndSec) zoomed = false
        else { viewStartSec = newStart; viewEndSec = newEnd; zoomed = true }
    }
    // Seconds of source per filmstrip frame: frames about as wide as they are
    // tall at 16:9, on a fixed ladder of steps so frames stay cached across
    // edits and zooms.
    readonly property real frameStep: {
        var wanted = (height - 2 * frameW) * 16 / 9 / Math.max(pxPerSec, 0.001)
        var steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1800]
        for (var i = 0; i < steps.length; ++i) if (steps[i] >= wanted) return steps[i]
        return 3600
    }

    Rectangle {
        id: track
        anchors.fill: parent; anchors.topMargin: root.frameW; anchors.bottomMargin: root.frameW
        radius: Math.max(0, root.frameRadius - root.frameW); color: root.film
    }

    component Handle: Rectangle {
        property bool leading: true
        x: leading ? 0 : parent.width - width
        width: Math.min(root.handleW, parent.width / 2); height: parent.height
        topLeftRadius: leading ? root.frameRadius : 0; bottomLeftRadius: topLeftRadius
        topRightRadius: leading ? 0 : root.frameRadius; bottomRightRadius: topRightRadius
        color: root.accent
        Rectangle { anchors.centerIn: parent; width: 2; height: 16; radius: 1; color: root.film }
    }

    // Frames on the step ladder from film.from to film.to, each at its source
    // time. offsetX is this item's own x on the track.
    component Film: Item {
        id: filmItem
        property var film: ({ video: null, from: 0, to: 0 })
        property real offsetX: 0
        readonly property int first: Math.floor(film.from / root.frameStep)
        // An int property only signals a real change. Fed straight from the
        // expression, the Repeater would take every re-evaluation, as a drag
        // causes, for a new model and rebuild every frame.
        readonly property int count: film.video ? Math.max(0, Math.ceil(film.to / root.frameStep) - first) : 0
        Repeater {
            model: filmItem.count
            Image {
                required property int index
                readonly property real time: (filmItem.first + index) * root.frameStep
                x: Math.round(filmItem.film.originX + time * root.pxPerSec) - filmItem.offsetX
                width: Math.ceil(root.frameStep * root.pxPerSec) + 1; height: filmItem.height
                // One decode size whatever the layout, so frames are extracted and cached once.
                sourceSize.height: root.thumbHeight
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                source: "image://thumbs/" + filmItem.film.video.thumbKey + "/" + Math.round(time * 1000)
            }
        }
    }

    // Each video's filmstrip, its cuts dimmed, and over them one accent
    // frame per clip, with its handles inside the frame. Clipped, so
    // zoomed-out-of-view clips never draw over the buttons beside it.
    Item {
        anchors.fill: parent
        clip: true
        Repeater {
            model: root.runCount
            Item {
                id: runItem
                objectName: "run"
                required property int index
                // Briefly past the end while the count changes.
                readonly property var run: root.runs[index] || { source: 0, origin: 0, end: 0 }
                x: Math.round(root.xForTime(run.origin)); width: Math.round(root.xForTime(run.end)) - x
                y: root.frameW; height: root.height - 2 * root.frameW
                clip: true
                Film { anchors.fill: parent; film: root.filmOf(runItem.run.source, runItem.run.origin, 0, runItem.run.end - runItem.run.origin); offsetX: runItem.x }
            }
        }
        Repeater {
            model: root.cutCount
            Rectangle {
                objectName: "cut"
                required property int index
                readonly property var cut: root.cuts[index] || { start: 0, end: 0 }
                x: Math.round(root.xForTime(cut.start)); width: Math.round(root.xForTime(cut.end)) - x
                y: root.frameW; height: root.height - 2 * root.frameW
                color: root.film; opacity: 0.7
            }
        }
        Repeater {
            model: root.clips.length
            Item {
                id: clipItem
                objectName: "clip"
                required property int index
                // Briefly past the end while the clip count changes.
                readonly property var at: root.positions[index] || { start: 0, end: 0, origin: 0, source: 0 }
                readonly property bool hovered: area.clipHovered === index
                readonly property bool moving: area.mode === 4 && area.clipIndex === index
                // Whole pixels, so handles meet the filmstrip without an antialiased seam.
                x: Math.round(root.xForTime(at.start)); width: Math.max(4, Math.round(root.xForTime(at.end)) - x)
                height: root.height
                z: moving || (area.mode === 1 && area.clipIndex === index) ? 1 : 0
                opacity: moving ? 0.85 : 1
                // While clips move, each carries its own footage.
                Film {
                    anchors.fill: parent; anchors.margins: root.frameW
                    clip: true
                    film: area.mode === 4 ? root.filmOf(clipItem.at.source, clipItem.at.origin, clipItem.at.start - clipItem.at.origin, clipItem.at.end - clipItem.at.origin) : ({ video: null, from: 0, to: 0 })
                    offsetX: clipItem.x + root.frameW
                }
                Rectangle {
                    anchors.fill: parent
                    radius: root.frameRadius; color: "transparent"
                    border.color: root.accent; border.width: root.frameW
                }
                Handle { leading: true }
                Handle { leading: false }
                // Remove this clip. Only offered when another clip would remain.
                Rectangle {
                    id: remove
                    visible: clipItem.hovered && root.clips.length > 1 && clipItem.width > 60 && area.mode === 0
                    anchors.top: parent.top; anchors.right: parent.right
                    anchors.topMargin: 7; anchors.rightMargin: root.handleW + 4
                    width: 20; height: 20; radius: 10
                    readonly property bool hot: area.removeHovered
                    color: hot ? root.accent : "#e0202023"
                    Text {
                        anchors.centerIn: parent; anchors.verticalCenterOffset: -1
                        text: "×"; font.pixelSize: 16; font.weight: Font.DemiBold
                        color: remove.hot ? root.accentForeground : "white"
                    }
                }
            }
        }

        // The playhead, held between the handles of the clip it's in so it
        // never hides under one.
        Rectangle {
            visible: root.durationSec > 0 && area.mode !== 4
                && root.trackTime(root.playheadSec) >= root.windowStart && root.trackTime(root.playheadSec) <= root.windowEnd
            x: {
                if (area.mode === 1) {
                    var at = root.positions[area.clipIndex]
                    var edge = Math.round(root.xForTime(area.leftEdge ? at.start : at.end))
                    return area.leftEdge ? edge + root.handleW : edge - root.handleW - width
                }
                var px = root.xForTime(root.trackTime(root.playheadSec)) - 1
                // Read before the clip list catches up with an edit, it can be missing.
                var at = root.layout[root.timeline.clipAt(root.playheadSec)]
                if (at) {
                    var from = Math.round(root.xForTime(at.start)) + root.handleW
                    var to = Math.round(root.xForTime(at.end)) - root.handleW - width
                    if (to >= from) px = Math.max(from, Math.min(px, to))
                }
                return Math.max(0, Math.min(root.width - width, px))
            }
            y: track.y
            width: 2; height: track.height; color: "white"
        }
    }

    // Hint above the timeline: the source time under a dragged handle, or what the pointer can do.
    Rectangle {
        readonly property bool dragging: root.trimming
        readonly property string hint: area.splitHovered >= 0 ? qsTr("Double-click to join")
                                      : area.clipHovered >= 0 && !area.onHandle
                                        ? (root.clips.length > 1 ? qsTr("Double-click to split · hold to move") : qsTr("Double-click to split")) : ""
        visible: root.enabled && (dragging || (area.mode === 0 && area.containsMouse && !area.removeHovered && hint !== ""))
        width: label.implicitWidth + 20; height: dragging ? 32 : 26
        radius: Math.min(4, theme.radius); color: "#2c2c2f"
        x: {
            var at = dragging ? root.positions[area.clipIndex] : null
            var anchor = dragging ? root.xForTime(area.leftEdge ? at.start : at.end) : area.mouseX
            return Math.max(0, Math.min(root.width - width, anchor - width / 2))
        }
        y: -height - 8
        Text {
            id: label; anchors.centerIn: parent; color: "white"
            text: parent.dragging ? Format.fmt(area.activeTime) : parent.hint
            font.pixelSize: parent.dragging ? 15 : 12
            font.family: parent.dragging ? "monospace" : Qt.application.font.family
            font.weight: parent.dragging ? Font.DemiBold : Font.Normal
        }
    }

    Timer {
        id: hold
        interval: 350
        onTriggered: area.pickUp()
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        // 0 none, 1 dragging a handle, 2 scrubbing, 3 pressing a clip's ×, 4 moving a clip
        property int mode: 0
        property int clipIndex: -1
        property bool leftEdge: true
        // The source time under the dragged handle.
        property real activeTime: 0
        // A clip's end is where the next one starts, so an end handle shows, and
        // leaves the playhead on, a hair inside: this clip's last frame.
        readonly property real edgeFrameTime: activeTime - (leftEdge ? 0 : 0.001)
        property int clipHovered: -1
        property int splitHovered: -1
        property bool onHandle: false
        property bool removeHovered: false
        // Snapshots taken when a drag begins.
        property var frozen: []
        property real dragStartPlayhead: 0
        property real pressX: 0
        property real grabOffset: 0
        // Where a moving clip would land: its index once dropped.
        readonly property int moveTarget: {
            if (mode !== 4) return -1
            var moving = root.clips[clipIndex]
            var middle = root.timeForX(mouseX) - grabOffset + (moving.out - moving.in) / 2
            var target = 0
            for (var i = 0; i < root.clips.length; ++i)
                if (i !== clipIndex && (root.layout[i].start + root.layout[i].end) / 2 < middle) ++target
            return target
        }

        // The hovered clip's × sits in its top-right corner, inside the handle.
        function onRemove(x, y) {
            if (clipHovered < 0 || root.clips.length < 2) return false
            var at = root.positions[clipHovered]
            var right = root.xForTime(at.end) - root.handleW - 4
            var width = root.xForTime(at.end) - root.xForTime(at.start)
            return width > 60 && x >= right - 20 && x <= right && y >= 7 && y <= 27
        }

        // Where x lands: a handle of some clip, or inside a clip. Between two
        // clips, the side of the line decides which clip is grabbed.
        function hitTest(x) {
            for (var i = 0; i < root.clips.length; ++i) {
                var a = root.xForTime(root.positions[i].start), b = root.xForTime(root.positions[i].end)
                var w = Math.min(root.handleW, (b - a) / 2) + 2
                var left = x >= a - 3 && x <= a + w
                var right = !left && x >= b - w && x <= b + 3
                if (right && i + 1 < root.clips.length && x > b
                    && root.xForTime(root.positions[i + 1].start) - b < 1) { i += 1; left = true; right = false }
                if (left || right) {
                    var boundary = left ? i - 1 : i
                    var split = root.timeline.canJoin(boundary) ? boundary : -1
                    return { handle: true, clip: i, left: left, split: split }
                }
                if (x > a && x < b) return { handle: false, clip: i, split: -1 }
            }
            return { handle: false, clip: -1, split: -1 }
        }
        // Handles catch on neighbouring clip edges and the playhead as it was
        // when the drag began; the drag moves the playhead along, so snapping
        // to it live would catch on itself.
        function snap(t) {
            var best = t, bestDistance = 8
            function consider(s) {
                var d = Math.abs(root.xForTime(s) - root.xForTime(t))
                if (d < bestDistance) { best = s; bestDistance = d }
            }
            for (var j = 0; j < frozen.length; ++j)
                if (j !== clipIndex) { consider(frozen[j].start); consider(frozen[j].end) }
            consider(dragStartPlayhead)
            return best
        }
        function seek(t) { root.playheadSec = t; root.scrub(t) }
        function hover(x, y) {
            var hit = hitTest(x)
            onHandle = hit.handle
            clipHovered = hit.clip
            splitHovered = hit.split
            removeHovered = onRemove(x, y)
            cursorShape = removeHovered ? Qt.PointingHandCursor : hit.handle ? Qt.SizeHorCursor : Qt.ArrowCursor
        }
        function pickUp() {
            if (mode !== 2 || clipIndex < 0 || root.clips.length < 2) return
            grabOffset = root.timeForX(mouseX) - root.positions[clipIndex].start
            mode = 4
            cursorShape = Qt.ClosedHandCursor
        }

        onPositionChanged: mouse => {
            if (root.durationSec <= 0) return
            var t = root.timeForX(mouse.x)
            if (mode === 0) { hover(mouse.x, mouse.y); return }
            if (mode === 2) {
                // Moving before the hold fires makes it a scrub, not a pick-up.
                if (Math.abs(mouse.x - pressX) > 4) hold.stop()
                // Over a cut, the playhead waits at the next clip's start.
                seek(root.sequenceTime(t))
                return
            }
            if (mode !== 1) return
            // Unclamped, so the outer handles can reach past the timeline's
            // ends to restore footage trimmed off earlier.
            var free = root.windowStart + mouse.x / Math.max(root.pxPerSec, 0.001)
            root.timeline.moveEdge(clipIndex, leftEdge, snap(free) - frozen[clipIndex].origin)
            activeTime = leftEdge ? root.clips[clipIndex].in : root.clips[clipIndex].out
            root.scrubClip(clipIndex, edgeFrameTime)
        }
        onPressed: mouse => {
            if (root.durationSec <= 0) return
            if (removeHovered) { mode = 3; return }
            var hit = hitTest(mouse.x)
            clipIndex = hit.clip
            if (hit.handle) {
                frozen = root.layout
                dragStartPlayhead = root.trackTime(root.playheadSec)
                leftEdge = hit.left
                activeTime = leftEdge ? root.clips[clipIndex].in : root.clips[clipIndex].out
                mode = 1
                root.timeline.beginGesture()
                // Show the edge's frame from the press, not only once dropped.
                root.scrubClip(clipIndex, edgeFrameTime)
            } else {
                mode = 2; pressX = mouse.x
                seek(root.sequenceTime(root.timeForX(mouse.x)))
                if (hit.clip >= 0) hold.restart()
            }
        }
        onReleased: {
            hold.stop()
            if (mode === 1) {
                root.timeline.endGesture()
                // The playhead stays on the frame the handle was dropped at.
                root.playheadSec = root.timeline.clipStart(clipIndex) + edgeFrameTime - root.clips[clipIndex].in
                root.settleView()
            }
            if (mode === 3 && onRemove(mouseX, mouseY)) root.timeline.removeClip(clipHovered)
            if (mode === 4) {
                var target = moveTarget
                root.timeline.moveClip(clipIndex, target)
                mode = 0
                seek(root.timeline.clipStart(target))
            }
            mode = 0; clipIndex = -1
            // A drag can end off the bar, and then no exit follows to clear it.
            if (containsMouse) hover(mouseX, mouseY)
            else unhover()
        }
        onCanceled: {
            hold.stop()
            if (mode === 1) { root.timeline.endGesture(); root.settleView() }
            mode = 0; clipIndex = -1
        }
        onDoubleClicked: mouse => {
            if (onRemove(mouse.x, mouse.y)) return
            var hit = hitTest(mouse.x)
            if (hit.split >= 0) root.timeline.joinClips(hit.split)
            else if (hit.clip >= 0 && !hit.handle) root.timeline.split(root.sequenceTime(root.timeForX(mouse.x)))
            hover(mouse.x, mouse.y)
        }
        function unhover() { clipHovered = -1; splitHovered = -1; removeHovered = false; onHandle = false }
        onExited: unhover()
    }
}
