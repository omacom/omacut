import QtQml
import QtMultimedia

// A deterministic clock for Main.qml's playback transition tests.
QtObject {
    objectName: "playbackTestPlayer"
    property var videoOutput: null
    property var audioOutput: null
    property url source: ""
    property int position: 0
    property int mediaStatus: MediaPlayer.LoadedMedia
    property int playbackState: MediaPlayer.StoppedState

    function play() { playbackState = MediaPlayer.PlayingState; }
    function pause() { playbackState = MediaPlayer.PausedState; }
    function stop() { playbackState = MediaPlayer.StoppedState; }
}
