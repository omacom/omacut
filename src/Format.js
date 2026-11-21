.pragma library

// [hh:]mm:ss.cc, shared by Main.qml and EditBar.qml. Rounds to centiseconds first so
// boundary values render correctly (e.g. 59.999 -> "01:00.00", not "00:60.00").
function fmt(sec) {
    if (sec < 0 || isNaN(sec))
        sec = 0;
    var cs = Math.round(sec * 100);
    var totalMinutes = Math.floor(cs / 6000);
    var h = Math.floor(totalMinutes / 60);
    var m = totalMinutes % 60;
    var s = (cs - totalMinutes * 6000) / 100;
    var mmss = (m < 10 ? "0" : "") + m + ":" + (s < 10 ? "0" : "") + s.toFixed(2);
    if (h > 0)
        return (h < 10 ? "0" : "") + h + ":" + mmss;
    return mmss;
}

// Seconds from what a person types for a time: "12", "12.5", "1:02.5" or
// "1:01:02". NaN for anything else.
function parseTime(text) {
    var parts = String(text).trim().split(":");
    if (parts.length > 3)
        return NaN;
    var seconds = 0;
    for (var i = 0; i < parts.length; ++i) {
        if (!/^\d+(\.\d*)?$|^\.\d+$/.test(parts[i].trim()))
            return NaN;
        seconds = seconds * 60 + parseFloat(parts[i]);
    }
    return seconds;
}
