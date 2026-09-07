.pragma library

// [hh:]mm:ss.cc, shared by Main.qml and TrimBar.qml. Rounds to centiseconds first so
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
