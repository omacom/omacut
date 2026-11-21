import QtQuick
import "Format.js" as Format

// The logos' own track, above the clips and on the same time scale: each logo
// is a strip from where it starts showing to where it stops, with its fades
// shaded at the ends. Drag a strip to move the logo in time, drag either end
// to change when it starts or ends, click it to pick it. Logos that overlap in
// time get lanes of their own, so every one has a strip. Times are seconds
// along the edit; the bar maps them to where the clips are drawn.
Item {
    id: root
    objectName: "logoTrack"
    // The EditBar whose scale this follows.
    required property var bar
    // Pressed and let go without dragging: show the logo's controls.
    signal clicked(int index)

    // Time runs left to right in every language, as on the clips.
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true

    readonly property var timeline: backend.timeline
    readonly property var logo: backend.logo
    readonly property var items: logo.items
    readonly property int laneHeight: 24
    readonly property int laneGap: 4
    readonly property int handleWidth: 9
    // The lanes as they were when a drag began: strips that cross while one
    // is dragged keep their lane, so none jumps from under the pointer.
    property var held: null

    function endOf(item) {
        return item.end < 0 || item.end > timeline.duration ? timeline.duration : item.end
    }

    // Each logo's lane, by index, and how many lanes there are: a logo takes
    // the first lane that is free when it starts.
    readonly property var arranged: {
        var order = [];
        for (var i = 0; i < items.length; ++i)
            order.push(i);
        order.sort((a, b) => items[a].start - items[b].start || a - b);
        var ends = [], of = [];
        for (var k = 0; k < order.length; ++k) {
            var item = items[order[k]], lane = 0;
            while (lane < ends.length && ends[lane] > item.start + 0.0005)
                ++lane;
            ends[lane] = endOf(item);
            of[order[k]] = lane;
        }
        return { of: of, count: Math.max(1, ends.length) };
    }
    readonly property var lanes: held || arranged

    implicitHeight: lanes.count * (laneHeight + laneGap) - laneGap
    clip: true

    function seqTime(x) { return bar.sequenceTime(bar.timeForX(x)); }
    function xOf(seconds) { return Math.round(bar.xForTime(bar.trackTime(seconds))); }

    // A strip's end, which sets when its logo starts or ends.
    component Edge: MouseArea {
        id: edge
        property int logoIndex: 0
        property bool leading: true
        property bool lit: false
        x: leading ? 0 : parent.width - width
        width: Math.min(root.handleWidth, parent.width / 2)
        height: parent.height
        hoverEnabled: true
        cursorShape: Qt.SizeHorCursor
        onPressed: {
            
            root.held = root.arranged;
            root.logo.select(logoIndex);
            root.logo.beginGesture();
        }
        onPositionChanged: mouse => {
            if (!pressed)
                return;
            var t = root.seqTime(mapToItem(root, mouse.x, 0).x);
            
            if (leading)
                root.logo.setStart(t);
            else
                root.logo.setEnd(t);
        }
        onReleased: finish()
        onCanceled: finish()
        function finish() {
            root.logo.endGesture();
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

    Repeater {
        model: root.items.length
        delegate: Rectangle {
            id: strip
            objectName: "logoStrip"
            required property int index
            // Briefly past the end while the count changes.
            readonly property var spec: root.items[index] || { source: "", start: 0, end: 0, fadeIn: 0, fadeOut: 0 }
            readonly property real endSec: root.endOf(spec)
            readonly property bool active: index === root.logo.current
            property bool dragging: false
            readonly property real startX: root.xOf(spec.start)
            // A logo that ends at a cut ends on the clip before it, not after.
            readonly property real endX: root.xOf(Math.max(spec.start, endSec - 0.001))
            x: startX
            width: Math.max(8, endX - startX)
            y: (root.lanes.of[index] || 0) * (root.laneHeight + root.laneGap)
            height: root.laneHeight
            radius: Math.min(4, theme.radius)
            color: active ? "#3f5a4a" : body.containsMouse ? "#365041" : "#2e4438"
            border.width: active ? 2 : 1
            border.color: active ? root.bar.accent : "#456455"
            z: dragging ? 1 : 0
            clip: true

            // The fades, shaded where the logo is not yet or no longer fully there.
            Rectangle {
                visible: strip.spec.fadeIn > 0
                x: 0
                width: Math.min(strip.width, Math.max(0, root.xOf(strip.spec.start + strip.spec.fadeIn) - strip.startX))
                height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#aa000000" }
                    GradientStop { position: 1; color: "#00000000" }
                }
            }
            Rectangle {
                visible: strip.spec.fadeOut > 0
                readonly property real w: Math.min(strip.width, Math.max(0, strip.endX - root.xOf(strip.endSec - strip.spec.fadeOut)))
                x: strip.width - w
                width: w
                height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#00000000" }
                    GradientStop { position: 1; color: "#aa000000" }
                }
            }

            // The body moves the logo in time; the ends, drawn over it, resize.
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
                    root.logo.select(strip.index);
                    root.logo.beginGesture();
                }
                onPositionChanged: mouse => {
                    if (!pressed)
                        return;
                    var now = root.seqTime(mapToItem(root, mouse.x, 0).x);
                    if (Math.abs(now - pressSeconds) * root.bar.pxPerSec > 3)
                        moved = true;
                    root.logo.moveTo(startWas + now - pressSeconds);
                }
                onReleased: {
                    if (!moved)
                        root.clicked(strip.index);
                    finish();
                }
                onCanceled: finish()
                function finish() {
                    root.logo.endGesture();
                    root.held = null;
                    strip.dragging = false;
                }
            }

            Image {
                id: thumb
                x: root.handleWidth + 3
                anchors.verticalCenter: parent.verticalCenter
                height: parent.height - 8
                width: height * 1.5
                source: strip.spec.source
                fillMode: Image.PreserveAspectFit
                sourceSize.height: 48
                asynchronous: true
                visible: strip.width > 90
            }
            Text {
                anchors.fill: parent
                anchors.leftMargin: root.handleWidth + 4 + (thumb.visible ? thumb.width + 4 : 0)
                anchors.rightMargin: root.handleWidth + 4
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                textFormat: Text.PlainText
                color: "white"
                font.pixelSize: 12
                font.weight: Font.DemiBold
                // While dragged, when it shows, as the ends are set.
                text: strip.dragging || edgeStart.pressed || edgeEnd.pressed
                    ? Format.fmt(strip.spec.start) + " – " + Format.fmt(strip.endSec)
                    : decodeURIComponent(String(strip.spec.source).split("/").pop())
            }

            Edge { id: edgeStart; logoIndex: strip.index; leading: true; lit: strip.active }
            Edge { id: edgeEnd; logoIndex: strip.index; leading: false; lit: strip.active }
        }
    }
}
