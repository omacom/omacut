#pragma once

#include <QImage>
#include <QRect>
#include <QString>
#include <QStringList>

#include <atomic>

// Thin wrappers around the ffmpeg/ffprobe command-line tools.
namespace ffmpeg {

struct VideoInfo {
    QString path;
    double duration = 0.0;  // seconds
    int width = 0;
    int height = 0;
    bool ok = false;
    QString error;
};

// Probe a file for a usable video stream and duration (runs ffprobe).
VideoInfo probe(const QString &path);

// Grab a single frame at `time` seconds, scaled to `height` px.
// Returns a null QImage on failure. If `cancel` is set and flips to true while
// the ffmpeg child is running, the child is killed and a null QImage returned.
QImage thumbnail(const QString &path, double time, int height = 90,
                 const std::atomic<bool> *cancel = nullptr);

// Clamp crop to the frame and snap x/y/w/h to even pixels (libx264/yuv420).
// Returns an empty rect if the crop is missing or collapses.
QRect clampCrop(const QRect &crop, int frameWidth, int frameHeight);

// Build the ffmpeg argument list that writes [start, end] of src to dst.
// Cuts are frame-accurate and re-encoded with libx264/aac. A non-zero
// scaleHeight downscales so the shorter side becomes scaleHeight (1080p of a
// portrait video is 1080 wide), always preserving the aspect ratio. A valid
// crop is applied first (source pixels), then the optional scale.
QStringList trimArgs(const QString &src, const QString &dst, double start, double end,
                     int scaleHeight = 0, const QRect &crop = {});

// Locate a tool on PATH; returns empty string if missing.
QString toolPath(const QString &tool);

}  // namespace ffmpeg
