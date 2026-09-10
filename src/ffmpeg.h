#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <atomic>

// Thin wrappers around the ffmpeg/ffprobe command-line tools.
namespace ffmpeg {

// What probe() learned about a media file. Despite the name it describes
// audio-only files too: those have hasVideo == false and no width/height.
struct VideoInfo {
    QString path;
    double duration = 0.0;  // seconds
    int width = 0;
    int height = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool ok = false;
    QString error;
};

// Probe a file for a usable video or audio stream and duration (runs ffprobe).
// Cover art embedded in audio files shows up as a video stream too; it is
// ignored, so an MP3 with a picture still counts as audio.
VideoInfo probe(const QString &path);

// Grab a single frame at `time` seconds, scaled to `height` px.
// Returns a null QImage on failure. If `cancel` is set and flips to true while
// the ffmpeg child is running, the child is killed and a null QImage returned.
QImage thumbnail(const QString &path, double time, int height = 90,
                 const std::atomic<bool> *cancel = nullptr);

// Render the waveform of [start, start + len] seconds as a width x height
// image with a transparent background. Amplitude is absolute (not normalised
// per slice), so neighbouring slices line up. Same failure/cancel behaviour
// as thumbnail().
QImage waveform(const QString &path, double start, double len, int width, int height,
                const std::atomic<bool> *cancel = nullptr);

// Build the ffmpeg argument list that writes [start, end] of src to dst.
// Cuts are frame-accurate and re-encoded with libx264/aac. A non-zero
// scaleHeight downscales so the shorter side becomes scaleHeight (1080p of a
// portrait video is 1080 wide), always preserving the aspect ratio.
QStringList trimArgs(const QString &src, const QString &dst, double start, double end,
                     int scaleHeight = 0);

// Build the ffmpeg argument list that writes [start, end] of an audio source
// to dst as an MP3 (libmp3lame VBR). Any embedded cover art is dropped.
QStringList audioTrimArgs(const QString &src, const QString &dst, double start, double end);

// Locate a tool on PATH; returns empty string if missing.
QString toolPath(const QString &tool);

}  // namespace ffmpeg
