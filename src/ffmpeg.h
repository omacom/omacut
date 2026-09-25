#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>

// Thin wrappers around the ffmpeg/ffprobe command-line tools.
namespace ffmpeg {

// One stream from the probed file. `index` is the absolute ffprobe stream
// index the overwrite path passes to `-map 0:N`.
struct StreamInfo {
    int index = -1;
    QString codecType;
    QString codecName;
    QString codecTag;
    QString fileName;
    bool attachedPic = false;
};

struct VideoInfo {
    QString path;
    double duration = 0.0;  // seconds
    int width = 0;
    int height = 0;
    bool ok = false;
    QString error;
    QVector<StreamInfo> streams;
};

// Probe a file for a usable video stream and duration (runs ffprobe).
VideoInfo probe(const QString &path);

// Grab a single frame at `time` seconds, scaled to `height` px.
// Returns a null QImage on failure. If `cancel` is set and flips to true while
// the ffmpeg child is running, the child is killed and a null QImage returned.
QImage thumbnail(const QString &path, double time, int height = 90,
                 const std::atomic<bool> *cancel = nullptr);

// Build the ffmpeg argument list that writes [start, end] of src to dst.
// Cuts are frame-accurate and re-encoded with libx264/aac. A non-zero
// scaleHeight downscales so the shorter side becomes scaleHeight (1080p of a
// portrait video is 1080 wide), always preserving the aspect ratio.
QStringList trimArgs(const QString &src, const QString &dst, double start, double end,
                     int scaleHeight = 0);

// Build the ffmpeg argument list that rewrites [start, end] of the probed file
// to dst, keeping every stream MP4 can carry: the primary video re-encodes to
// libx264, cover art and whitelisted audio copy bit-exact, other audio
// re-encodes to aac, and text subtitles convert to mov_text. Always Original
// quality — there is no scale option on the overwrite path.
QStringList overwriteArgs(const VideoInfo &info, const QString &dst, double start, double end);

// Human-readable names for the streams overwriteArgs would drop — the same
// classifier drives both, so the pre-flight warning and the -map list can
// never disagree.
QStringList overwriteDrops(const VideoInfo &info);

// Locate a tool on PATH; returns empty string if missing.
QString toolPath(const QString &tool);

}  // namespace ffmpeg
