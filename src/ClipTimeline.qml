import QtQuick
import "Format.js" as Format

// Source ranges are shown adjacent, with a small visual gutter for each pair
// of independent handles. Playback positions remain source timestamps.
Item {
    id: root
    implicitHeight: 76
    required property var clips
    property real durationSec: 0
    property color accent: "#FFD60A"
    property int thumbCount: 0
    property int thumbReadyCount: 0
    property int thumbRevision: 0
    property real playheadSec: 0
    property real startSec: 0
    property real endSec: 0
    property bool interacting: false
    property bool trimmingRange: false
    property bool zoomed: false
    property real viewStartSec: 0
    property real viewEndSec: 0
    property var ranges: []
    property bool syncing: false
    readonly property int selected: clips.selectedIndex
    readonly property real windowStart: zoomed ? viewStartSec : 0
    readonly property real windowEnd: zoomed ? viewEndSec : durationSec
    readonly property real totalLength: clips.duration
    readonly property int visibleCount: zoomed && selected >= 0 ? 1 : ranges.length
    readonly property real pixelsPerSecond: Math.max(1, filmstrip.contentWidth - visibleCount * 32) /
        Math.max(0.001, zoomed && selected >= 0 ? endSec - startSec : totalLength)
    signal scrub(real seconds)

    function sync() {
        syncing = true;
        ranges = clips.snapshot();
        var range = selected >= 0 ? ranges[selected] : null;
        startSec = range ? range.sourceStartSec : 0;
        endSec = range ? range.sourceEndSec : 0;
        syncing = false;
    }
    function resizeSelection() {
        if (syncing || selected < 0) return;
        clips.resizeClip(selected, startSec, endSec);
        sync();
    }
    onStartSecChanged: resizeSelection()
    onEndSecChanged: resizeSelection()
    Component.onCompleted: sync()
    Connections {
        target: root.clips
        function onChanged() { root.sync(); }
        function onSelectionChanged() { root.sync(); }
    }
    function timelinePosition(source) {
        var offset = 0;
        for (var i = 0; i < ranges.length; ++i) {
            var c = ranges[i];
            if (source < c.sourceEndSec || i === ranges.length - 1)
                return offset + Math.max(0, Math.min(c.sourceEndSec - c.sourceStartSec, source - c.sourceStartSec));
            offset += c.sourceEndSec - c.sourceStartSec;
        }
        return 0;
    }
    function toggleZoom() {
        if (selected < 0) return;
        var slack = (endSec - startSec) / 8;
        var a = Math.max(0, startSec - slack);
        var b = Math.min(durationSec, endSec + slack);
        if (zoomed && a === viewStartSec && b === viewEndSec) zoomed = false;
        else { viewStartSec = a; viewEndSec = b; zoomed = true; }
    }

    Text {
        anchors.bottom: parent.top
        anchors.bottomMargin: 6
        anchors.horizontalCenter: parent.horizontalCenter
        visible: root.trimmingRange
        text: Format.fmt(root.playheadSec)
        color: root.accent
        font.pixelSize: 14
        font.family: "monospace"
    }
    Text {
        anchors.centerIn: parent
        visible: root.ranges.length === 0
        text: "Empty timeline — reopen the video to start again"
        color: "#b8b8bc"
        font.pixelSize: 12
    }
    Flickable {
        id: filmstrip
        anchors.fill: parent
        contentWidth: Math.max(width, root.visibleCount * 48)
        contentHeight: height
        clip: true
        interactive: !root.interacting
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.HorizontalFlick
    Row {
        width: filmstrip.contentWidth
        height: root.height
        spacing: 4
        Repeater {
            model: root.clips
            delegate: Item {
                id: segment
                required property int index
                required property real sourceStartSec
                required property real sourceEndSec
                required property real lengthSec
                required property real timelineStartSec
                visible: !root.zoomed || index === root.selected
                width: visible ? lengthSec * root.pixelsPerSecond + 28 : 0
                height: root.height
                readonly property bool selected: index === root.selected
                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: "#252529"
                    border.width: segment.selected ? 3 : 1
                    border.color: segment.selected ? root.accent : "#77777c"
                }
                Image {
                    anchors { fill: parent; leftMargin: 14; rightMargin: 14; topMargin: 4; bottomMargin: 4 }
                    source: root.thumbReadyCount > 0 ? "image://thumbs/" + root.thumbRevision + "/" +
                        Math.max(0, Math.min(root.thumbReadyCount - 1, Math.floor(((segment.sourceStartSec - root.windowStart) / Math.max(root.windowEnd - root.windowStart, 0.001)) * root.thumbCount))) : ""
                    sourceSize.height: root.height
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    cache: false
                    opacity: segment.selected ? 0.8 : 0.5
                }
                Text {
                    anchors.centerIn: parent
                    visible: segment.width > 100
                    text: Format.fmt(segment.lengthSec)
                    color: "white"
                    style: Text.Outline
                    styleColor: "black"
                    font.pixelSize: 12
                }
                Rectangle {
                    visible: root.playheadSec >= segment.sourceStartSec && root.playheadSec <= segment.sourceEndSec
                    x: 14 + (root.playheadSec - segment.sourceStartSec) / Math.max(segment.lengthSec, 0.001) * (segment.width - 28)
                    y: 4
                    height: parent.height - 8
                    width: 2
                    color: "white"
                }
                MouseArea {
                    anchors { fill: parent; leftMargin: 14; rightMargin: 14 }
                    function seek(mouse) {
                        root.playheadSec = segment.sourceStartSec + Math.max(0, Math.min(1, mouse.x / Math.max(width, 1))) * segment.lengthSec;
                        root.scrub(root.playheadSec);
                    }
                    onPressed: function(mouse) {
                        root.clips.select(segment.index);
                        root.interacting = true;
                        seek(mouse);
                    }
                    onPositionChanged: function(mouse) { if (pressed) seek(mouse); }
                    onReleased: root.interacting = false
                    onCanceled: root.interacting = false
                }
                Repeater {
                    model: 2
                    delegate: Rectangle {
                        id: handle
                        required property int index
                        readonly property bool leftEdge: index === 0
                        x: leftEdge ? 0 : segment.width - width
                        width: 14
                        height: segment.height
                        radius: 5
                        color: segment.selected ? root.accent : "#77777c"
                        Rectangle { anchors.centerIn: parent; width: 2; height: 18; color: "#202024" }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.SizeHorCursor
                            property real pressX
                            property real originalStart
                            property real originalEnd
                            property real scale
                            onPressed: function(mouse) {
                                root.clips.select(segment.index);
                                pressX = mapToItem(root, mouse.x, mouse.y).x;
                                originalStart = segment.sourceStartSec;
                                originalEnd = segment.sourceEndSec;
                                scale = root.pixelsPerSecond;
                                root.interacting = true;
                                root.trimmingRange = true;
                            }
                            onPositionChanged: function(mouse) {
                                if (!pressed) return;
                                var delta = (mapToItem(root, mouse.x, mouse.y).x - pressX) / scale;
                                var low = segment.index === 0 ? 0 : root.ranges[segment.index - 1].sourceEndSec;
                                var high = segment.index === root.ranges.length - 1 ? root.durationSec : root.ranges[segment.index + 1].sourceStartSec;
                                var gap = Math.min(0.1, root.durationSec);
                                var a = handle.leftEdge ? Math.max(low, Math.min(originalStart + delta, originalEnd - gap)) : originalStart;
                                var b = handle.leftEdge ? originalEnd : Math.min(high, Math.max(originalEnd + delta, originalStart + gap));
                                if (root.clips.resizeClip(segment.index, a, b)) {
                                    root.playheadSec = handle.leftEdge ? a : b;
                                    root.scrub(root.playheadSec);
                                }
                            }
                            onReleased: { root.interacting = false; root.trimmingRange = false; }
                            onCanceled: { root.interacting = false; root.trimmingRange = false; }
                        }
                    }
                }
            }
        }
    }
    }
}
