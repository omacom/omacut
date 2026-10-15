#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <functional>


// Thin wrappers around the ffmpeg/ffprobe command-line tools.
namespace ffmpeg {

struct VideoInfo {
    QString path;
    double duration = 0.0;  // seconds
    // Where the timestamps start. Packet times include it; seeks and the
    // player's positions count from it as 0.
    double start = 0.0;
    // As displayed: swapped for video rotated a quarter turn.
    int width = 0;
    int height = 0;
    bool audio = false;
    bool ok = false;
    QString error;
};

// Probe a file for a usable video stream and duration (runs ffprobe).
VideoInfo probe(const QString &path);

// Grab a single frame at `time` seconds, scaled to `height` px.
// Returns a null QImage on failure. If `stopRequested` is set and starts
// returning true while the ffmpeg child is running, the child is killed and
// a null QImage returned; a child that never finishes is killed regardless
// after a generous timeout.
QImage thumbnail(const QString &path, double time, int height = 90,
                 const std::function<bool()> &stopRequested = nullptr);

// ffprobe arguments listing the video packet times around `pts` (a packet
// time, not a seek time), from far enough back to find the frame on screen.
QStringList shownFrameArgs(const QString &path, double pts);
// The frame on screen at `pts`, the latest listed packet time not after it
// (`pts` itself when none is listed), and the keyframe it decodes from (-1
// when none is listed).
struct ShownFrame {
    double time = 0.0;
    double keyframe = -1.0;
};
ShownFrame shownFrame(const QByteArray &packets, double pts);
// ffmpeg arguments that write the frame starting at `frameTime` seconds to
// `pngPath`, full size, upright and with square pixels, decoding from the
// keyframe at `keyframeTime` (or from the start, when negative).
QStringList frameArgs(const QString &path, double frameTime, double keyframeTime,
                      const QString &pngPath);

// A stretch [start, end] seconds of one video, as exported.
struct Segment {
    QString path;
    double start = 0.0;
    double end = 0.0;
    bool audio = false;
    double speed = 1.0;
};

// Build the ffmpeg argument list that writes the segments, back to back, to
// dst. Cuts are frame-accurate and re-encoded with libx264/aac. Segments from
// different videos are fitted into width x height, letterboxed, with silence
// for any without audio. A non-zero scaleHeight downscales so the shorter
// side becomes scaleHeight (1080p of a portrait video is 1080 wide), always
// preserving the aspect ratio.
QStringList trimArgs(const QList<Segment> &segments, const QString &dst,
                     int width, int height, int scaleHeight = 0);

// Locate a tool on PATH; returns empty string if missing.
QString toolPath(const QString &tool);

}  // namespace ffmpeg
