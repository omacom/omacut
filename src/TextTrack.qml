import QtQuick
import "Format.js" as Format

// The texts' own track, above the clips and on the same time scale: each text
// is a strip from where it starts showing to where it stops. Drag a strip to
// move the text, drag either end to change when it starts or ends, click it
// to pick it in the text panel, and double-click an empty spot to add another
// text there. Texts that overlap in time get lanes of their own, so every one
// has a strip. Times are seconds along the edit; the bar maps them to where
// the clips are drawn.
Item {
    id: root
    objectName: "textTrack"
    // The EditBar whose scale this follows.
    required property var bar
    // The picked text, or -1.
    property int current: -1
    // Pressed on a strip, to move or resize it: the text to select.
    signal picked(int index)
    // Pressed and let go without dragging: the text to edit.
    signal clicked(int index)
    signal addRequested(real seconds)

    // Time runs left to right in every language, as on the clips.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    readonly property var timeline: backend.timeline
    readonly property var texts: timeline.texts
    readonly property int laneHeight: 24
    readonly property int laneGap: 4
    readonly property int handleWidth: 9
    // The lanes as they were when a drag began: strips that cross while one
    // is dragged keep their lane, so none jumps from under the pointer.
    property var held: null

    // Each text's lane, by index, and how many lanes there are: a text takes
    // the first lane that is free when it starts.
    readonly property var arranged: {
        var order = [];
        for (var i = 0; i < texts.length; ++i)
            order.push(i);
        order.sort((a, b) => texts[a].start - texts[b].start || a - b);
        var ends = [], of = [];
        for (var k = 0; k < order.length; ++k) {
            var text = texts[order[k]], lane = 0;
            while (lane < ends.length && ends[lane] > text.start + 0.0005)
                ++lane;
            ends[lane] = text.end;
            of[order[k]] = lane;
        }
        return { of: of, count: Math.max(1, ends.length) };
    }
    readonly property var lanes: held || arranged

    implicitHeight: lanes.count * (laneHeight + laneGap) - laneGap
    clip: true

    function seqTime(x) { return bar.sequenceTime(bar.timeForX(x)); }
    function xOf(seconds) { return Math.round(bar.xForTime(bar.trackTime(seconds))); }

    // A strip's end, which sets when its text starts or ends.
    component Edge: MouseArea {
        id: edge
        property int textIndex: 0
        property bool leading: true
        // "start" or "end".
        property string field: "start"
        property bool lit: false
        x: leading ? 0 : parent.width - width
        width: Math.min(root.handleWidth, parent.width / 2)
        height: parent.height
        hoverEnabled: true
        cursorShape: Qt.SizeHorCursor
        onPressed: {
            root.held = root.arranged;
            root.timeline.beginGesture();
            root.picked(textIndex);
        }
        onPositionChanged: mouse => {
            if (pressed)
                root.timeline.setTextProperty(textIndex, field, root.seqTime(mapToItem(root, mouse.x, 0).x));
        }
        onReleased: finish()
        onCanceled: finish()
        function finish() {
            root.timeline.endGesture();
            root.held = null;
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 1
            radius: Math.min(3, theme.radius)
            color: root.bar.accent
            opacity: edge.lit || edge.pressed || edge.containsMouse ? 1 : 0.55
            Rectangle { anchors.centerIn: parent; width: 2; height: 10; radius: 1; color: "#1c1c1e" }
        }
    }

    // Empty track: where a double-click adds a text.
    MouseArea {
        anchors.fill: parent
        onDoubleClicked: mouse => root.addRequested(root.seqTime(mouse.x))
    }

    Repeater {
        model: root.texts.length
        delegate: Rectangle {
            id: strip
            objectName: "textStrip"
            required property int index
            // Briefly past the end while the count changes.
            readonly property var spec: root.texts[index] || { text: "", start: 0, end: 0 }
            readonly property bool active: index === root.current
            property bool dragging: false
            readonly property real startX: root.xOf(spec.start)
            // A text that ends at a cut ends on the clip before it, not after.
            readonly property real endX: root.xOf(Math.max(spec.start, spec.end - 0.001))
            x: startX
            width: Math.max(8, endX - startX)
            y: (root.lanes.of[index] || 0) * (root.laneHeight + root.laneGap)
            height: root.laneHeight
            radius: Math.min(4, theme.radius)
            color: active ? "#4a4a68" : body.containsMouse ? "#3c3c55" : "#32324a"
            border.width: active ? 2 : 1
            border.color: active ? root.bar.accent : "#4a4a60"
            z: dragging ? 1 : 0

            // The body moves the text; the ends, drawn over it, resize.
            MouseArea {
                id: body
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                property real pressSeconds: 0
                property real startWas: 0
                property bool moved: false
                onPressed: mouse => {
                    pressSeconds = root.seqTime(mapToItem(root, mouse.x, 0).x);
                    startWas = strip.spec.start;
                    moved = false;
                    root.held = root.arranged;
                    strip.dragging = true;
                    root.timeline.beginGesture();
                    root.picked(strip.index);
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return;
                    var now = root.seqTime(mapToItem(root, mouse.x, 0).x);
                    if (Math.abs(now - pressSeconds) * root.bar.pxPerSec > 3)
                        moved = true;
                    root.timeline.moveText(strip.index, startWas + now - pressSeconds);
                }
                onReleased: {
                    if (!moved)
                        root.clicked(strip.index);
                    finish();
                }
                onCanceled: finish()
                function finish() {
                    root.timeline.endGesture();
                    root.held = null;
                    strip.dragging = false;
                }
            }

            Text {
                anchors.fill: parent
                anchors.leftMargin: root.handleWidth + 4
                anchors.rightMargin: root.handleWidth + 4
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                color: "white"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                // While dragged, when it shows, as the ends are set.
                text: strip.dragging || edgeStart.pressed || edgeEnd.pressed
                    ? Format.fmt(strip.spec.start) + " – " + Format.fmt(strip.spec.end)
                    : (strip.spec.text.split("\n")[0] || "…")
            }

            Edge { id: edgeStart; textIndex: strip.index; leading: true; field: "start"; lit: strip.active }
            Edge { id: edgeEnd; textIndex: strip.index; leading: false; field: "end"; lit: strip.active }
        }
    }
}
