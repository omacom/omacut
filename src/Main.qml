import QtQuick
import QtQml
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia
import "Format.js" as Format

ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 640
    minimumHeight: 460
    visible: true
    title: backend.source.toString() === "" ? "omacut" : "omacut — " + fileName(backend.source)
    readonly property bool hasVideo: backend.source.toString() !== ""
    readonly property color accent: backend.themeAccent
    readonly property color accentForeground: backend.themeAccentForeground
    readonly property bool audioOutputReady: audioOutput !== null
    property var audioOutput: null
    property string noticeText: ""
    property bool helpVisible: false
    property bool quitConfirmVisible: false
    property bool openConfirmVisible: false
    readonly property string statusText: noticeText !== "" ? noticeText : backend.status

    readonly property var timeline: backend.timeline
    // Quitting only warns about unexported edits. One clip spanning the whole
    // first video is never dirty — that's just the source.
    readonly property bool unexported: hasVideo && timeline.unexported
    // Editing shortcuts go quiet while a dialog or the help overlay is up,
    // a probe is about to replace the timeline, or a handle is dragged.
    readonly property bool editing: hasVideo && timeline.duration > 0 && !quitConfirmVisible && !openConfirmVisible && !helpVisible && !backend.probing && !editBar.interacting

    Material.theme: Material.Dark
    Material.accent: win.accent
    color: "#0e0e10"

    function fileName(url) {
        var s = url.toString();
        return s === "" ? "" : decodeURIComponent(s.substring(s.lastIndexOf('/') + 1));
    }
    function showNotice(text) {
        noticeText = text;
        noticeTimer.restart();
    }
    // Not while exporting: the export would mark the new edit as exported.
    // Replacing an unexported edit asks first, like quitting does.
    function openVideo() {
        if (backend.busy)
            return;
        if (unexported) {
            player.pause();
            openConfirmVisible = true;
            return;
        }
        backend.openVideoDialog();
    }
    function addVideo() {
        if (backend.busy)
            return;
        player.pause();
        backend.addVideoDialog(editBar.playheadSec);
    }
    // Dropped videos open into an empty window, and otherwise join the edit
    // after the clip under the playhead, in the order dropped, the way the +
    // button adds one. Adding is undoable, so a stray drop costs nothing.
    // Probing happens in the background, in drop order; a file that won't
    // open is skipped and the rest go on. When some made it, one notice
    // counts the skipped, since each error replaces the last.
    function dropVideos(urls) {
        return backend.addVideos(urls, editBar.playheadSec);
    }
    function exportVideo() {
        if (!win.hasVideo || timeline.duration <= 0 || backend.busy)
            return;
        player.pause();
        backend.exportDialog();
    }
    function ensureAudioOutput() {
        if (audioOutput === null && win.hasVideo)
            audioOutput = audioOutputComponent.createObject(win);
    }
    function releaseAudioOutput() {
        if (audioOutput === null)
            return;
        var oldAudioOutput = audioOutput;
        audioOutput = null;
        oldAudioOutput.destroy();
    }
    function playing() {
        return player.playbackState === MediaPlayer.PlayingState && !player.priming;
    }
    function togglePlay() {
        if (!win.hasVideo || timeline.duration <= 0)
            return;
        ensureAudioOutput();
        if (playing()) {
            player.pause();
            return;
        }
        // From the playhead, or from the top once at the end.
        var t = editBar.playheadSec >= timeline.duration - 0.03 ? 0 : editBar.playheadSec;
        var i = timeline.clipAt(t), clip = timeline.clips[i];
        editBar.playheadSec = t;
        player.show(i, clip.in + t - clip.start, true);
    }
    function seekTo(seconds) {
        if (!win.hasVideo || timeline.duration <= 0)
            return;
        var t = Math.max(0, Math.min(seconds, timeline.duration));
        var i = timeline.clipAt(t), clip = timeline.clips[i];
        editBar.playheadSec = t;
        player.show(i, clip.in + t - clip.start, playing());
    }
    // Moves the clip under the playhead one place earlier or later, keeping
    // the playhead on the same frame of it.
    function moveClipBy(direction) {
        var t = editBar.playheadSec, i = timeline.clipAt(t);
        var into = t - timeline.clipStart(i);
        if (i + direction < 0 || i + direction >= timeline.clips.length)
            return;
        timeline.moveClip(i, i + direction);
        seekTo(timeline.clipStart(i + direction) + into);
    }
    property bool quitting: false
    function requestQuit() {
        if (unexported) {
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause();
            quitConfirmVisible = true;
            return;
        }
        forceQuit();
    }
    // Closing the window is what reliably ends the app (quitOnLastWindowClosed);
    // Qt.quit() alone has proven ignorable in a live session, so it's only the
    // backstop. The quitting flag stops onClosing from re-asking on the way out.
    function forceQuit() {
        quitting = true;
        player.stop();
        releaseAudioOutput();
        win.close();
        Qt.quit();
    }

    Component.onCompleted: ensureAudioOutput()
    onHasVideoChanged: {
        if (hasVideo) {
            ensureAudioOutput();
        } else {
            player.stop();
            releaseAudioOutput();
        }
    }
    onClosing: (close) => {
        if (win.quitting)
            return;
        // A close with the open confirmation up just puts it away, rather
        // than stacking the quit confirmation on top of it.
        if (win.openConfirmVisible) {
            close.accepted = false;
            win.openConfirmVisible = false;
            return;
        }
        if (win.unexported) {
            close.accepted = false;
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause();
            win.quitConfirmVisible = true;
            return;
        }
        forceQuit();
    }

    // The playback and editing shortcuts go quiet while the quit confirmation is
    // up — a disabled Shortcut also stops swallowing its key, which lets the
    // dialog's own keyboard navigation receive the arrows, Space and Enter.
    // 0-9 jump to 0%-90% of the whole edit.
    Instantiator {
        model: 10
        delegate: Shortcut {
            required property int index
            sequence: index.toString()
            context: Qt.ApplicationShortcut
            enabled: win.editing
            // Held down, repeats would keep seeking to the same spot.
            autoRepeat: false
            onActivated: seekTo(timeline.duration * index / 10)
        }
    }

    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: win.hasVideo && !win.quitConfirmVisible && !win.openConfirmVisible && !win.helpVisible
        onActivated: togglePlay()
    }

    Shortcut {
        sequence: "Left"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: seekTo(editBar.playheadSec - 1)
    }

    Shortcut {
        sequence: "Right"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: seekTo(editBar.playheadSec + 1)
    }

    Shortcut {
        sequence: "Shift+Left"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: seekTo(editBar.playheadSec - 5)
    }

    Shortcut {
        sequence: "Shift+Right"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: seekTo(editBar.playheadSec + 5)
    }

    Shortcut {
        sequence: "Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: seekTo(editBar.playheadSec - 0.2)
    }

    Shortcut {
        sequence: "Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: seekTo(editBar.playheadSec + 0.2)
    }

    Shortcut {
        sequence: "["
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: {
            player.pause();
            seekTo(timeline.edgeFrom(editBar.playheadSec, -1));
        }
    }

    Shortcut {
        sequence: "]"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: {
            player.pause();
            seekTo(timeline.edgeFrom(editBar.playheadSec, 1));
        }
    }

    Shortcut {
        sequence: "Alt+["
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: moveClipBy(-1)
    }

    Shortcut {
        sequence: "Alt+]"
        context: Qt.ApplicationShortcut
        enabled: win.editing
        onActivated: moveClipBy(1)
    }

    Shortcut {
        sequence: ","
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: {
            const i = timeline.clipAt(editBar.playheadSec);
            const speed = timeline.clips[i].speed;
            timeline.setSpeed(i, speed <= 0.5 ? 0.5 : speed / 2);
        }
    }

    Shortcut {
        sequence: "."
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: {
            const i = timeline.clipAt(editBar.playheadSec);
            const speed = timeline.clips[i].speed;
            timeline.setSpeed(i, speed >= 2 ? 2 : speed * 2);
        }
    }

    Shortcut {
        sequence: "S"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: timeline.split(editBar.playheadSec)
    }

    Shortcut {
        sequences: ["X", "Delete", "Backspace"]
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: timeline.removeAt(editBar.playheadSec)
    }

    Shortcut {
        sequence: "Ctrl+Space"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: timeline.trimTo(editBar.playheadSec, true)
    }

    Shortcut {
        sequence: "Alt+Space"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: timeline.trimTo(editBar.playheadSec, false)
    }

    Shortcut {
        sequence: "Z"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing
        onActivated: editBar.toggleZoom()
    }

    Shortcut {
        sequence: "F"
        context: Qt.ApplicationShortcut
        autoRepeat: false
        enabled: win.editing && !backend.busy
        onActivated: backend.saveFrame(editBar.playheadSec)
    }

    Shortcut {
        sequence: "Ctrl+Z"
        context: Qt.ApplicationShortcut
        enabled: win.editing && timeline.canUndo
        onActivated: timeline.undo()
    }

    Shortcut {
        sequences: ["Ctrl+Shift+Z", "Ctrl+Y"]
        context: Qt.ApplicationShortcut
        enabled: win.editing && timeline.canRedo
        onActivated: timeline.redo()
    }

    Shortcut {
        sequence: "Ctrl+S"
        context: Qt.ApplicationShortcut
        enabled: win.hasVideo && timeline.duration > 0 && !backend.busy && !win.helpVisible && !win.openConfirmVisible
        onActivated: {
            win.quitConfirmVisible = false;
            exportVideo();
        }
    }

    Shortcut {
        sequence: "Ctrl+O"
        context: Qt.ApplicationShortcut
        enabled: !win.quitConfirmVisible && !win.openConfirmVisible && !win.helpVisible && !backend.busy
        onActivated: openVideo()
    }

    Shortcut {
        sequence: "Ctrl+Shift+O"
        context: Qt.ApplicationShortcut
        enabled: win.editing && !backend.busy
        onActivated: addVideo()
    }

    Shortcut {
        sequence: "Q"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (!win.quitConfirmVisible && !win.openConfirmVisible && !win.helpVisible)
                requestQuit();
        }
    }

    Shortcut {
        sequence: "?"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (!win.quitConfirmVisible && !win.openConfirmVisible)
                win.helpVisible = !win.helpVisible;
        }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.quitConfirmVisible)
                win.quitConfirmVisible = false;
            else if (win.openConfirmVisible)
                win.openConfirmVisible = false;
            else if (win.helpVisible)
                win.helpVisible = false;
        }
    }

    MediaPlayer {
        id: player
        videoOutput: videoOut
        audioOutput: win.audioOutput

        // The clip on screen, and where to go once a newly set source has
        // loaded (pendingMs is -1 when nothing is waiting).
        property int clip: -1
        property int pendingMs: -1
        property bool pendingPlay: false
        // Switching sources first reports the old video as loaded again; only
        // a load that follows the new one starting counts.
        property bool loadStarted: false
        // A freshly loaded source shows black until played; playing it muted
        // until the first frame arrives, then pausing, puts that frame up.
        property bool priming: false
        property int primeMs: 0

        // Shows sourceTime in clip index, and plays on from there if asked.
        function show(index, sourceTime, andPlay) {
            var url = backend.videos[timeline.clips[index].source].url;
            var ms = Math.round(sourceTime * 1000);
            clip = index;
            // Compared as text: url values are never identical as objects.
            if (source.toString() !== url.toString() || mediaStatus === MediaPlayer.LoadingMedia) {
                pendingMs = ms;
                pendingPlay = andPlay;
                loadStarted = mediaStatus === MediaPlayer.LoadingMedia && source.toString() === url.toString();
                source = url;
                return;
            }
            pendingMs = -1;
            finishPriming();
            position = ms;
            if (andPlay)
                play();
        }

        // Moves a load still in progress to sourceTime in clip index, keeping
        // whether it plays once loaded.
        function retarget(index, sourceTime) {
            var url = backend.videos[timeline.clips[index].source].url;
            if (source.toString() !== url.toString()) {
                show(index, sourceTime, pendingPlay);
                return;
            }
            clip = index;
            pendingMs = Math.round(sourceTime * 1000);
        }

        function startPriming(ms) {
            win.ensureAudioOutput();
            priming = true;
            primeMs = ms;
            position = ms;
            play();
            primeFallback.restart();
        }

        function finishPriming() {
            if (!priming)
                return;
            primeFallback.stop();
            pause();
            position = primeMs;
            priming = false;
        }

        onMediaStatusChanged: {
            // A video that won't load leaves nothing to wait for.
            if (mediaStatus === MediaPlayer.InvalidMedia)
                pendingMs = -1;
            if (mediaStatus === MediaPlayer.LoadingMedia)
                loadStarted = true;
            // Seeking or playing from inside this handler makes the player
            // load the video over again, so it waits for the handler to return.
            if (pendingMs >= 0 && loadStarted
                    && (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia))
                Qt.callLater(takePending);
        }
        function takePending() {
            if (pendingMs < 0)
                return;
            var ms = pendingMs;
            pendingMs = -1;
            if (pendingPlay) {
                position = ms;
                play();
            } else {
                startPriming(ms);
            }
        }
        onPositionChanged: {
            if (priming) {
                if (player.position > primeMs)
                    finishPriming();
                return;
            }
            if (pendingMs >= 0 || clip < 0 || clip >= timeline.clips.length)
                return;
            var c = timeline.clips[clip], t = player.position / 1000;
            // Play the clips in order: carry on into a clip that continues
            // this one, jump to any other, and stop after the last.
            if (playbackState === MediaPlayer.PlayingState && t >= c.out - 0.03) {
                if (clip + 1 >= timeline.clips.length) {
                    pause();
                    editBar.playheadSec = timeline.duration;
                    return;
                }
                var next = timeline.clips[clip + 1];
                if (next.source !== c.source || Math.abs(next.in - c.out) > 0.05) {
                    show(clip + 1, next.in, true);
                    return;
                }
                clip += 1;
                c = next;
            }
            if (!editBar.interacting)
                editBar.playheadSec = c.start + Math.max(0, Math.min(t, c.out) - c.in);
        }
    }

    // After an edit, the clip under the playhead may be another one now, or
    // gone, so put the playhead's frame back up.
    Connections {
        target: win.timeline
        function onChanged() {
            Qt.callLater(win.resync);
        }
    }
    function resync() {
        if (!hasVideo || timeline.duration <= 0 || editBar.interacting)
            return;
        var t = Math.min(editBar.playheadSec, timeline.duration);
        var i = timeline.clipAt(t), clip = timeline.clips[i];
        var sourceTime = clip.in + t - clip.start;
        // Edited while a video loads: aim the wait at the playhead's clip now,
        // not the one it was started for.
        if (player.pendingMs >= 0) {
            player.retarget(i, sourceTime);
            return;
        }
        // Still playing the right stretch, e.g. after a split: no need to seek.
        if (playing() && backend.videos[clip.source].url.toString() === player.source.toString()
                && Math.abs(sourceTime - player.position / 1000) < 0.1) {
            player.clip = i;
            return;
        }
        editBar.playheadSec = t;
        player.show(i, sourceTime, playing());
    }

    Component {
        id: audioOutputComponent
        AudioOutput {
            // Follows the system's default output as it changes, between
            // headphones, speakers, monitor audio, etc. Created with the
            // output, so nothing watches devices before a video is open.
            property MediaDevices devices: MediaDevices {}
            device: devices.defaultAudioOutput
            muted: player.priming
        }
    }

    Timer {
        id: primeFallback
        interval: 250
        repeat: false
        onTriggered: player.finishPriming()
    }

    Timer {
        id: noticeTimer
        interval: 5000
        repeat: false
        onTriggered: win.noticeText = ""
    }

    component DialogButton: Rectangle {
        id: dialogButton
        width: dialogButtonLabel.implicitWidth + 28
        height: 34
        radius: 8

        property string text: ""
        property bool primary: false
        signal clicked()

        color: primary ? win.accent : "#2c2c2f"
        border.color: activeFocus ? (primary ? win.accentForeground : win.accent) : "transparent"
        border.width: activeFocus ? 2 : 0

        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Keys.onSpacePressed: clicked()

        Label {
            id: dialogButtonLabel
            anchors.centerIn: parent
            text: dialogButton.text
            color: dialogButton.primary ? win.accentForeground : "white"
            font.pixelSize: 13
            font.weight: Font.DemiBold
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: dialogButton.clicked()
        }
    }

    component IconButton: Rectangle {
        id: iconButton
        implicitWidth: 44
        implicitHeight: 44
        radius: 22

        property string iconName: "play"
        property color iconColor: "white"
        property color buttonColor: "#2c2c2f"
        property string tipText: ""
        signal clicked()

        color: buttonColor
        opacity: enabled ? 1 : 0.45

        HoverHandler { id: iconHover }
        ToolTip.visible: iconHover.hovered && tipText !== ""
        ToolTip.text: tipText

        Canvas {
            id: iconCanvas
            anchors.centerIn: parent
            width: 24
            height: 24

            onPaint: {
                var ctx = getContext("2d");
                ctx.clearRect(0, 0, width, height);
                ctx.fillStyle = iconButton.iconColor;
                ctx.strokeStyle = iconButton.iconColor;
                ctx.lineWidth = 2.4;
                ctx.lineCap = "round";
                ctx.lineJoin = "round";

                if (iconButton.iconName === "pause") {
                    ctx.fillRect(7, 5, 4, 14);
                    ctx.fillRect(14, 5, 4, 14);
                } else if (iconButton.iconName === "play") {
                    ctx.beginPath();
                    ctx.moveTo(8, 5);
                    ctx.lineTo(8, 19);
                    ctx.lineTo(19, 12);
                    ctx.closePath();
                    ctx.fill();
                } else if (iconButton.iconName === "plus") {
                    ctx.beginPath();
                    ctx.moveTo(12, 5);
                    ctx.lineTo(12, 19);
                    ctx.moveTo(5, 12);
                    ctx.lineTo(19, 12);
                    ctx.stroke();
                } else if (iconButton.iconName === "download") {
                    ctx.beginPath();
                    ctx.moveTo(12, 4);
                    ctx.lineTo(12, 14);
                    ctx.stroke();

                    ctx.beginPath();
                    ctx.moveTo(7, 10);
                    ctx.lineTo(12, 15);
                    ctx.lineTo(17, 10);
                    ctx.stroke();

                    ctx.beginPath();
                    ctx.moveTo(6, 20);
                    ctx.lineTo(18, 20);
                    ctx.stroke();
                }
            }

            Connections {
                target: iconButton
                function onIconNameChanged() { iconCanvas.requestPaint(); }
                function onIconColorChanged() { iconCanvas.requestPaint(); }
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: iconButton.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: iconButton.clicked()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: win.hasVideo ? 16 : 0
        spacing: 14

        // --- video preview ---
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: win.hasVideo ? 12 : 0
            color: "black"
            clip: true

            VideoOutput {
                id: videoOut
                anchors.fill: parent
            }
            Connections {
                target: videoOut.videoSink
                function onVideoFrameChanged(frame) {
                    player.finishPriming();
                }
            }

            // Only the empty state opens on a preview click: with a video
            // loaded, a stray click must not reach the file picker at all.
            MouseArea {
                anchors.fill: parent
                enabled: !win.hasVideo
                cursorShape: Qt.PointingHandCursor
                onClicked: openVideo()
            }

            Button {
                id: openVideoButton
                anchors.centerIn: parent
                visible: !win.hasVideo
                text: "Open a video"
                highlighted: true
                focusPolicy: Qt.NoFocus
                font.pixelSize: 18
                Material.foreground: win.accentForeground
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
                contentItem: Label {
                    text: openVideoButton.text
                    font: openVideoButton.font
                    color: win.accentForeground
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: openVideo()
            }

            // With no video open there's no status line, so a file that
            // won't open says why here.
            Label {
                anchors.top: openVideoButton.bottom
                anchors.topMargin: 16
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, parent.width - 32)
                visible: !win.hasVideo && win.noticeText !== ""
                textFormat: Text.PlainText
                text: win.noticeText
                color: win.accent
                font.pixelSize: 13
                font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
            }
        }

        // --- timeline ---
        RowLayout {
            visible: win.hasVideo
            Layout.fillWidth: true
            spacing: 10

            IconButton {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                iconName: player.playbackState === MediaPlayer.PlayingState && !player.priming ? "pause" : "play"
                tipText: player.playbackState === MediaPlayer.PlayingState ? "Pause" : "Play"
                enabled: timeline.duration > 0
                onClicked: togglePlay()
            }

            EditBar {
                id: editBar
                objectName: "editBar"
                Layout.fillWidth: true
                // Frozen while a probe is in flight: it is about to replace
                // or extend the timeline the bar is showing.
                enabled: !backend.probing
                accent: win.accent
                accentForeground: win.accentForeground
                onScrub: (seconds) => {
                    player.pause();
                    seekTo(seconds);
                }
                onScrubClip: (index, sourceTime) => {
                    player.pause();
                    player.show(index, sourceTime, false);
                }
            }

            IconButton {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                iconName: "plus"
                tipText: "Add a video"
                enabled: timeline.duration > 0 && !backend.busy
                onClicked: addVideo()
            }

            IconButton {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                iconName: "download"
                tipText: "Export"
                enabled: timeline.duration > 0 && !backend.busy
                onClicked: exportVideo()
            }
        }

        // --- status line ---
        Item {
            visible: win.hasVideo
            Layout.fillWidth: true
            Layout.preferredHeight: 26

            Label {
                anchors.centerIn: parent
                width: parent.width
                visible: win.statusText !== ""
                // ffprobe's and ffmpeg's errors are shown as they are.
                textFormat: Text.PlainText
                text: win.statusText
                color: win.noticeText !== "" ? win.accent : "#b8b8bc"
                font.pixelSize: 13
                font.family: "monospace"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideMiddle
            }

            Label {
                anchors.centerIn: parent
                visible: win.statusText === "" && timeline.duration > 0 && !editBar.trimming
                textFormat: Text.StyledText
                text: Format.fmt(editBar.playheadSec) + " (" + Format.fmt(win.timeline.duration) + ")"
                    + (editBar.zoomed ? " · <font color=\"" + win.accent + "\">zoomed</font>" : "")
                color: "#d6d6da"
                font.pixelSize: 13
                font.family: "monospace"
            }
        }
    }

    DropArea {
        id: videoDropArea
        objectName: "videoDropArea"
        anchors.fill: parent
        z: 5
        enabled: !win.quitConfirmVisible && !win.openConfirmVisible && !win.helpVisible && !backend.busy
        // Only local files: "file:-report" or "file:http://..." would reach
        // ffprobe as an option or an address, and "file:////host/..." is a
        // network share.
        onEntered: (drag) => {
            drag.accepted = drag.hasUrls && drag.urls.length > 0
                && drag.urls.every((url) => /^file:\/\/\/(?!\/)/.test(url.toString()));
        }
        onDropped: (drop) => {
            player.pause();
            player.pendingPlay = false;
            if (win.dropVideos(drop.urls))
                drop.accept(Qt.CopyAction);
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 8
            visible: videoDropArea.containsDrag
            color: "#cc0e0e10"
            border.color: win.accent
            border.width: 2
            radius: 12
            Label {
                anchors.centerIn: parent
                text: win.hasVideo ? "Drop to add to the edit" : "Drop a video to open it"
                font.pixelSize: 20
                color: win.accent
            }
        }
    }

    // --- subtle help toggle in the corner ---
    Rectangle {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 10
        width: 24
        height: 24
        radius: 12
        color: helpHover.hovered ? "#2c2c2f" : "transparent"

        Text {
            anchors.centerIn: parent
            text: "?"
            color: helpHover.hovered ? "white" : "#7a7a80"
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }
        HoverHandler {
            id: helpHover
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            onTapped: win.helpVisible = !win.helpVisible
        }
    }

    // --- hotkey overlay ---
    Rectangle {
        visible: win.helpVisible
        anchors.fill: parent
        color: "#cc000000"

        MouseArea {
            anchors.fill: parent
            onClicked: win.helpVisible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: helpColumn.width + 56
            height: helpColumn.height + 48
            radius: 12
            color: "#1c1c1e"

            Column {
                id: helpColumn
                anchors.centerIn: parent
                spacing: 10

                Label {
                    text: "Keyboard shortcuts"
                    color: "white"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    bottomPadding: 8
                }

                Repeater {
                    model: [
                        { keys: "Space", action: "Play / pause" },
                        { keys: "0–9", action: "Jump to 0%–90% of the edit" },
                        { keys: "← / →", action: "Move playhead 1s" },
                        { keys: "Shift ← / →", action: "Move playhead 5s" },
                        { keys: "Alt ← / →", action: "Move playhead 0.2s" },
                        { keys: "[ / ]", action: "Previous / next clip edge" },
                        { keys: "S", action: "Split the clip at the playhead" },
                        { keys: "X", action: "Remove the clip" },
                        { keys: "Alt [ / ]", action: "Move the clip earlier / later" },
                        { keys: "Ctrl Space", action: "Clip start to playhead" },
                        { keys: "Alt Space", action: "Clip end to playhead" },
                        { keys: "Ctrl Z", action: "Undo (Ctrl Shift Z redo)" },
                        { keys: "Z", action: "Zoom to the clip" },
                        { keys: "F", action: "Save the playhead frame as a PNG" },
                        { keys: "Ctrl O", action: "Open a video" },
                        { keys: "Ctrl Shift O", action: "Add a video after this clip" },
                        { keys: "Ctrl S", action: "Export" },
                        { keys: "Q", action: "Quit" },
                        { keys: "?", action: "Show these shortcuts" }
                    ]
                    delegate: Row {
                        spacing: 18
                        Label {
                            width: 110
                            horizontalAlignment: Text.AlignRight
                            text: modelData.keys
                            color: win.accent
                            font.pixelSize: 13
                            font.family: "monospace"
                        }
                        Label {
                            text: modelData.action
                            color: "#d6d6da"
                            font.pixelSize: 13
                        }
                    }
                }
            }
        }
    }

    // --- open confirmation (replacing an unexported edit) ---
    Rectangle {
        visible: win.openConfirmVisible
        anchors.fill: parent
        color: "#cc000000"
        onVisibleChanged: {
            if (visible)
                openKeepButton.forceActiveFocus();
        }

        MouseArea {
            anchors.fill: parent
            onClicked: win.openConfirmVisible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: openColumn.width + 64
            height: openColumn.height + 48
            radius: 12
            color: "#1c1c1e"

            MouseArea {
                anchors.fill: parent
            }

            Column {
                id: openColumn
                anchors.centerIn: parent
                spacing: 8

                Label {
                    text: "Unexported edit"
                    color: "white"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    text: "Opening another video will discard this edit. Open it anyway?"
                    color: "#d6d6da"
                    font.pixelSize: 13
                    bottomPadding: 12
                }

                Row {
                    anchors.right: parent.right
                    spacing: 10

                    DialogButton {
                        id: openKeepButton
                        text: "Keep editing"
                        KeyNavigation.left: openVideoConfirmButton
                        KeyNavigation.right: openVideoConfirmButton
                        KeyNavigation.tab: openVideoConfirmButton
                        KeyNavigation.backtab: openVideoConfirmButton
                        onClicked: win.openConfirmVisible = false
                    }
                    DialogButton {
                        id: openVideoConfirmButton
                        text: "Open video"
                        primary: true
                        KeyNavigation.left: openKeepButton
                        KeyNavigation.right: openKeepButton
                        KeyNavigation.tab: openKeepButton
                        KeyNavigation.backtab: openKeepButton
                        onClicked: {
                            win.openConfirmVisible = false;
                            backend.openVideoDialog();
                        }
                    }
                }
            }
        }
    }

    // --- quit confirmation ---
    Rectangle {
        visible: win.quitConfirmVisible
        anchors.fill: parent
        color: "#cc000000"
        onVisibleChanged: {
            if (visible)
                quitExportButton.forceActiveFocus();
        }

        MouseArea {
            anchors.fill: parent
            onClicked: win.quitConfirmVisible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: quitColumn.width + 64
            height: quitColumn.height + 48
            radius: 12
            color: "#1c1c1e"

            MouseArea {
                anchors.fill: parent
            }

            Column {
                id: quitColumn
                anchors.centerIn: parent
                spacing: 8

                Label {
                    text: "Unexported edit"
                    color: "white"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    text: "Your edit hasn't been exported. Quit anyway?"
                    color: "#d6d6da"
                    font.pixelSize: 13
                    bottomPadding: 12
                }

                Row {
                    anchors.right: parent.right
                    spacing: 10

                    DialogButton {
                        id: quitCancelButton
                        text: "Cancel"
                        KeyNavigation.left: quitExportButton
                        KeyNavigation.right: quitQuitButton
                        KeyNavigation.tab: quitQuitButton
                        KeyNavigation.backtab: quitExportButton
                        onClicked: win.quitConfirmVisible = false
                    }
                    DialogButton {
                        id: quitQuitButton
                        text: "Quit"
                        KeyNavigation.left: quitCancelButton
                        KeyNavigation.right: quitExportButton
                        KeyNavigation.tab: quitExportButton
                        KeyNavigation.backtab: quitCancelButton
                        onClicked: forceQuit()
                    }
                    DialogButton {
                        id: quitExportButton
                        text: "Export"
                        primary: true
                        KeyNavigation.left: quitQuitButton
                        KeyNavigation.right: quitCancelButton
                        KeyNavigation.tab: quitCancelButton
                        KeyNavigation.backtab: quitQuitButton
                        onClicked: {
                            win.quitConfirmVisible = false;
                            exportVideo();
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: backend
        function onInfoChanged() {
            win.noticeText = "";
            noticeTimer.stop();
            // Drop any prime or load still pending for the last video.
            primeFallback.stop();
            player.priming = false;
            player.pendingMs = -1;
            // Load afresh, even the same path: it may be a new file now.
            player.source = "";
            editBar.zoomed = false;
            editBar.playheadSec = 0;
            if (win.hasVideo)
                player.show(0, 0, false);
        }
        function onExportDone(path) {
            win.showNotice("Saved " + path);
        }
        function onExportFailed(message) {
            win.showNotice("Export failed: " + message);
        }
        function onFrameSaved(path) {
            win.showNotice("Saved " + path);
        }
        function onFrameFailed(message) {
            win.showNotice("Could not save the frame: " + message);
        }
        function onLoadError(message) {
            // The export-reopen refusal is a complete sentence already.
            win.showNotice(message.startsWith("Saved") ? message : "Cannot open video: " + message);
        }
        function onSkippedVideos(count) {
            win.showNotice(count === 1 ? "Skipped a file that won't open." : "Skipped " + count + " files that won't open.");
        }
    }
}
