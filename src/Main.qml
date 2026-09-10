import QtQuick
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
    readonly property string statusText: noticeText !== "" ? noticeText : backend.status

    // What the last export wrote, so quitting only warns about unexported work.
    // A trim spanning the whole video is never dirty — that's just the source.
    property string exportedRanges: ""
    property string pendingExportRanges: ""
    property int playbackClip: 0
    readonly property bool canEdit: hasVideo && !backend.busy && !backend.dialogOpen
        && !quitConfirmVisible && !helpVisible
    readonly property bool trimDirty: hasVideo && backend.duration > 0
        && !(trimBar.ranges.length === 1 && trimBar.ranges[0].sourceStartSec === 0
             && trimBar.ranges[0].sourceEndSec === backend.duration)
        && JSON.stringify(trimBar.ranges) !== exportedRanges

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
    function openVideo() {
        backend.openVideoDialog();
    }
    function exportVideo() {
        if (!win.hasVideo || backend.clips.count === 0 || backend.busy || backend.dialogOpen)
            return;
        player.pause();
        pendingExportRanges = JSON.stringify(trimBar.ranges);
        backend.exportTimelineDialog();
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
    function togglePlay() {
        if (!win.canEdit || backend.clips.count === 0)
            return;
        ensureAudioOutput();
        if (player.priming)
            player.finishPriming();
        if (player.playbackState === MediaPlayer.PlayingState) {
            player.pause();
            return;
        }
        // The pause-at-end clamp rounds to whole milliseconds and the player
        // snaps seeks to frames, so a finished clip can rest a fraction of a
        // millisecond before endSec. Treat anything within 10 ms of the end
        // as "at the end" or play would instantly re-pause instead of
        // restarting from the trim start.
        var pos = trimBar.timelinePosition(trimBar.playheadSec);
        if (pos >= backend.clips.duration - 0.01)
            pos = 0;
        movePlayheadTo(backend.clips.sourceTime(pos));
        player.play();
    }
    function movePlayheadTo(seconds, selectClip = true) {
        if (player.priming)
            player.finishPriming();
        trimBar.playheadSec = seconds;
        playbackClip = backend.clips.clipAt(trimBar.timelinePosition(seconds));
        if (selectClip && playbackClip >= 0) backend.clips.select(playbackClip);
        player.position = Math.round(seconds * 1000);
    }
    function seekBy(seconds) {
        if (!win.hasVideo || backend.duration <= 0)
            return;
        if (backend.clips.count === 0) return;
        var position = Math.max(0, Math.min(trimBar.timelinePosition(trimBar.playheadSec) + seconds, backend.clips.duration));
        movePlayheadTo(backend.clips.sourceTime(position));
    }
    // Both edges park the playhead on themselves, so you see the frame you just
    // trimmed to — the same thing dragging a handle does. While zoomed, the
    // edges stop at the zoom window instead of the video bounds.
    function moveTrimStartTo(seconds) {
        if (!win.hasVideo || backend.duration <= 0)
            return;
        var minGap = Math.min(0.1, backend.duration);
        trimBar.startSec = Math.max(trimBar.windowStart, Math.min(seconds, trimBar.endSec - minGap));
        movePlayheadTo(trimBar.startSec, false);
    }
    function moveTrimEndTo(seconds) {
        if (!win.hasVideo || backend.duration <= 0)
            return;
        var minGap = Math.min(0.1, backend.duration);
        trimBar.endSec = Math.min(trimBar.windowEnd, Math.max(seconds, trimBar.startSec + minGap));
        movePlayheadTo(trimBar.endSec, false);
    }
    function splitClip() {
        if (!canEdit || backend.clips.count === 0) return;
        player.pause();
        if (backend.clips.split(trimBar.timelinePosition(trimBar.playheadSec))) {
            trimBar.zoomed = false;
            backend.requestThumbs(0, backend.duration);
            movePlayheadTo(trimBar.playheadSec);
        } else showNotice("Move the playhead away from the clip edges to split");
    }
    function deleteClip() {
        if (!canEdit || backend.clips.selectedIndex < 0) return;
        player.pause();
        if (backend.clips.removeSelected()) {
            trimBar.zoomed = false;
            if (backend.clips.count > 0) {
                backend.requestThumbs(0, backend.duration);
                movePlayheadTo(trimBar.startSec);
            } else {
                backend.clearVideo();
            }
        }
    }
    property bool quitting: false
    function requestQuit() {
        if (trimDirty) {
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
            videoOut.clearOutput();
            releaseAudioOutput();
        }
    }
    onClosing: (close) => {
        if (win.quitting)
            return;
        if (win.trimDirty) {
            close.accepted = false;
            if (player.playbackState === MediaPlayer.PlayingState)
                player.pause();
            win.quitConfirmVisible = true;
            return;
        }
        forceQuit();
    }

    // The playback and trim shortcuts go quiet while the quit confirmation is
    // up — a disabled Shortcut also stops swallowing its key, which lets the
    // dialog's own keyboard navigation receive the arrows, Space and Enter.
    Shortcut {
        sequence: "T"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: splitClip()
    }
    Shortcut {
        sequences: ["Delete", "Backspace"]
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.selectedIndex >= 0
        onActivated: deleteClip()
    }
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: togglePlay()
    }

    Shortcut {
        sequence: "Ctrl+Space"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.selectedIndex >= 0
        onActivated: moveTrimStartTo(trimBar.playheadSec)
    }

    Shortcut {
        sequence: "Alt+Space"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.selectedIndex >= 0
        onActivated: moveTrimEndTo(trimBar.playheadSec)
    }

    Shortcut {
        sequence: "Left"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: seekBy(-1)
    }

    Shortcut {
        sequence: "Right"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: seekBy(1)
    }

    Shortcut {
        sequence: "Shift+Left"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: seekBy(-5)
    }

    Shortcut {
        sequence: "Shift+Right"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: seekBy(5)
    }

    Shortcut {
        sequence: "Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: seekBy(-0.2)
    }

    Shortcut {
        sequence: "Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.count > 0
        onActivated: seekBy(0.2)
    }

    Shortcut {
        sequence: "Z"
        context: Qt.ApplicationShortcut
        enabled: win.canEdit && backend.clips.selectedIndex >= 0
        onActivated: {
            trimBar.toggleZoom();
            backend.requestThumbs(trimBar.windowStart, trimBar.windowEnd);
        }
    }

    Shortcut {
        sequence: "Ctrl+S"
        context: Qt.ApplicationShortcut
        enabled: win.hasVideo && backend.clips.count > 0 && !backend.busy && !backend.dialogOpen
        onActivated: {
            win.quitConfirmVisible = false;
            exportVideo();
        }
    }

    Shortcut {
        sequence: "Ctrl+O"
        context: Qt.ApplicationShortcut
        enabled: !win.quitConfirmVisible && !backend.busy && !backend.dialogOpen
        onActivated: openVideo()
    }

    Shortcut {
        sequence: "Q"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (!win.quitConfirmVisible)
                requestQuit();
        }
    }

    Shortcut {
        sequence: "?"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (!win.quitConfirmVisible)
                win.helpVisible = !win.helpVisible;
        }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.quitConfirmVisible)
                win.quitConfirmVisible = false;
            else if (win.helpVisible)
                win.helpVisible = false;
        }
    }

    MediaPlayer {
        id: player
        objectName: "player"
        source: backend.source
        videoOutput: videoOut
        audioOutput: win.audioOutput

        // Render the opening frame on load instead of showing black. Playback
        // starts muted and stops as soon as VideoOutput receives a frame.
        property bool primed: false
        property bool priming: false

        function startPriming() {
            if (primed || priming || backend.source.toString() === "")
                return;
            win.ensureAudioOutput();
            primed = true;
            priming = true;
            position = 0;
            play();
            primeFallback.restart();
        }

        function finishPriming() {
            if (!priming)
                return;
            primeFallback.stop();
            pause();
            position = 0;
            priming = false;
        }

        onMediaStatusChanged: {
            if (mediaStatus === MediaPlayer.LoadedMedia || mediaStatus === MediaPlayer.BufferedMedia)
                startPriming();
        }
        onPositionChanged: function(newPosition) {
            if (priming && newPosition > 0) {
                finishPriming();
                return;
            }
            // Jump over removed source material while previewing the timeline.
            var range = trimBar.ranges[win.playbackClip];
            if (playbackState === MediaPlayer.PlayingState && range && newPosition / 1000 >= range.sourceEndSec - 0.001) {
                if (win.playbackClip + 1 < trimBar.ranges.length) {
                    win.playbackClip++;
                    backend.clips.select(win.playbackClip);
                    player.position = Math.round(trimBar.ranges[win.playbackClip].sourceStartSec * 1000);
                } else {
                    pause();
                    player.position = Math.round(range.sourceEndSec * 1000);
                }
            }
            if (!trimBar.interacting)
                trimBar.playheadSec = player.position / 1000;
        }
    }

    Component {
        id: audioOutputComponent
        AudioOutput {
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
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: tipText
        Accessible.onPressAction: clicked()
        border.width: activeFocus ? 2 : 0
        border.color: win.accent
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()

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
                } else if (iconButton.iconName === "split") {
                    ctx.beginPath();
                    ctx.moveTo(5, 5); ctx.lineTo(19, 19);
                    ctx.moveTo(5, 19); ctx.lineTo(19, 5);
                    ctx.moveTo(12, 2); ctx.lineTo(12, 7);
                    ctx.stroke();
                } else if (iconButton.iconName === "delete") {
                    ctx.strokeRect(7, 7, 10, 13);
                    ctx.beginPath();
                    ctx.moveTo(5, 5); ctx.lineTo(19, 5);
                    ctx.moveTo(10, 2); ctx.lineTo(14, 2);
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

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: openVideo()
            }

            Button {
                id: openVideoButton
                objectName: "openVideoButton"
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
                tipText: player.playbackState === MediaPlayer.PlayingState ? "Pause (Space)" : "Play (Space)"
                enabled: win.canEdit && backend.clips.count > 0
                onClicked: togglePlay()
            }

            ClipTimeline {
                id: trimBar
                clips: backend.clips
                enabled: win.canEdit
                objectName: "trimBar"
                Layout.fillWidth: true
                accent: win.accent
                durationSec: backend.duration
                thumbCount: backend.thumbCount
                thumbReadyCount: backend.thumbReadyCount
                thumbRevision: backend.thumbRevision
                onScrub: (seconds) => { player.pause(); win.movePlayheadTo(seconds, false); }
            }

            IconButton {
                objectName: "splitButton"
                iconName: "split"
                tipText: "Split at playhead (T)"
                enabled: win.canEdit && backend.clips.count > 0
                onClicked: splitClip()
            }
            IconButton {
                objectName: "deleteButton"
                iconName: "delete"
                tipText: "Delete selected clip (Del / Backspace)"
                enabled: win.canEdit && backend.clips.selectedIndex >= 0
                onClicked: deleteClip()
            }
            IconButton {
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                iconName: "download"
                tipText: "Export timeline (Ctrl+S)"
                enabled: win.canEdit && backend.clips.count > 0
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
                visible: win.statusText === "" && backend.duration > 0 && !trimBar.trimmingRange
                textFormat: Text.StyledText
                text: Format.fmt(trimBar.timelinePosition(trimBar.playheadSec)) + " / " + Format.fmt(backend.clips.duration)
                    + " · " + backend.clips.count + " clip(s)"
                    + (trimBar.zoomed ? " · <font color=\"" + win.accent + "\">zoomed</font>" : "")
                color: "#d6d6da"
                font.pixelSize: 13
                font.family: "monospace"
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
        color: "#000000cc"

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
                        { keys: "T", action: "Split at playhead" },
                        { keys: "Del / Backspace", action: "Delete selected clip" },
                        { keys: "← / →", action: "Move playhead 1s" },
                        { keys: "Shift ← / →", action: "Move playhead 5s" },
                        { keys: "Alt ← / →", action: "Move playhead 0.2s" },
                        { keys: "Ctrl Space", action: "Trim start to playhead" },
                        { keys: "Alt Space", action: "Trim end to playhead" },
                        { keys: "Z", action: "Zoom the selection" },
                        { keys: "Ctrl O", action: "Open a video" },
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

    // --- quit confirmation ---
    Rectangle {
        visible: win.quitConfirmVisible
        anchors.fill: parent
        color: "#000000cc"
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
                    text: "Unexported trim"
                    color: "white"
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    text: "Your trim hasn't been exported. Quit anyway?"
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
            // Reset priming too, or a video opened mid-prime would stay black:
            // startPriming() bails while priming is still true.
            primeFallback.stop();
            player.priming = false;
            player.primed = false;
            trimBar.zoomed = false;
            trimBar.playheadSec = 0;
            win.exportedRanges = "";
            win.pendingExportRanges = "";
            win.playbackClip = win.hasVideo ? 0 : -1;
            trimBar.sync();
        }
        function onExportDone(path) {
            win.exportedRanges = win.pendingExportRanges;
            win.showNotice("Saved " + path);
        }
        function onExportFailed(message) {
            win.showNotice("Export failed: " + message);
        }
        function onLoadError(message) {
            win.showNotice("Cannot open video: " + message);
        }
    }
}
