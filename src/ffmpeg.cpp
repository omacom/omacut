#include "ffmpeg.h"

#include <QJsonArray>
#include <QtMath>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

namespace ffmpeg {

namespace {
// Upper bound on a synchronous probe so a hung/unresponsive file (e.g. a stalled
// network mount) can't freeze the caller — load() runs probe() on the UI thread.
constexpr int kProbeTimeoutMs = 15000;
// Poll granularity while waiting on a thumbnail child, so cancellation is prompt.
constexpr int kThumbPollMs = 50;

// Run ffmpeg with `args`, expecting a single image of `format` on stdout.
// Returns a null QImage if the process fails, is cancelled, or writes nothing.
QImage runImageCommand(const QStringList &args, const char *format,
                       const std::atomic<bool> *cancel) {
    const QString ffmpeg = toolPath("ffmpeg");
    if (ffmpeg.isEmpty())
        return {};

    QProcess proc;
    proc.start(ffmpeg, args);

    // Poll instead of waitForFinished(-1) so a cancel request can kill the child
    // promptly — otherwise the std::future destructor in ThumbWorker would block
    // the UI thread until ffmpeg finishes on its own.
    while (!proc.waitForFinished(kThumbPollMs)) {
        if (proc.state() == QProcess::NotRunning)
            break;  // failed to start, or exited between polls
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            proc.kill();
            proc.waitForFinished(-1);
            return {};
        }
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};

    const QByteArray data = proc.readAllStandardOutput();
    QImage img;
    img.loadFromData(data, format);
    return img;
}

QString seconds(double value) {
    return QString::number(qMax(value, 0.0), 'f', 3);
}
}

QString toolPath(const QString &tool) {
    return QStandardPaths::findExecutable(tool);
}

VideoInfo probe(const QString &path) {
    VideoInfo info;
    info.path = path;

    const QString ffprobe = toolPath("ffprobe");
    if (ffprobe.isEmpty()) {
        info.error = "`ffprobe` was not found on your PATH. Install ffmpeg.";
        return info;
    }

    QProcess proc;
    proc.start(ffprobe, {
        "-v", "error",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        path,
    });
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(-1);
        info.error = "ffprobe timed out reading this file.";
        return info;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        info.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (info.error.isEmpty())
            info.error = "ffprobe failed.";
        return info;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
    const QJsonObject root = doc.object();
    const QJsonArray streams = root.value("streams").toArray();

    // The first real video stream wins; failing that, the first audio stream.
    // Cover art never counts: it is a video stream flagged attached_pic, or in
    // containers that lack the flag, a single-frame video track next to audio.
    QJsonObject video;
    QJsonObject audio;
    bool videoSingleFrame = false;
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        const QString type = stream.value("codec_type").toString();
        if (type == "video" && video.isEmpty()) {
            const bool attachedPic =
                stream.value("disposition").toObject().value("attached_pic").toInt() == 1;
            if (!attachedPic) {
                video = stream;
                videoSingleFrame = stream.value("nb_frames").toString() == "1";
            }
        } else if (type == "audio" && audio.isEmpty()) {
            audio = stream;
        }
    }
    if (videoSingleFrame && !audio.isEmpty())
        video = QJsonObject();

    info.hasVideo = !video.isEmpty();
    info.hasAudio = !audio.isEmpty();
    if (!info.hasVideo && !info.hasAudio) {
        info.error = "No video or audio stream found in this file.";
        return info;
    }

    const QJsonObject stream = info.hasVideo ? video : audio;
    if (info.hasVideo) {
        info.width = stream.value("width").toInt();
        info.height = stream.value("height").toInt();
    }

    // Duration can live on the stream or on the container.
    QString durationStr = stream.value("duration").toString();
    if (durationStr.isEmpty())
        durationStr = root.value("format").toObject().value("duration").toString();
    if (durationStr.isEmpty()) {
        info.error = "Could not determine the media duration.";
        return info;
    }

    info.duration = durationStr.toDouble();

    info.ok = info.duration > 0.0;
    if (!info.ok)
        info.error = "File has a zero or invalid duration.";
    return info;
}

QImage thumbnail(const QString &path, double time, int height,
                 const std::atomic<bool> *cancel) {
    return runImageCommand({
        "-loglevel", "error",
        "-ss", seconds(time),
        "-i", path,
        "-frames:v", "1",
        "-vf", QString("scale=-1:%1").arg(height),
        "-f", "image2pipe",
        "-vcodec", "mjpeg",
        "pipe:1",
    }, "JPEG", cancel);
}

QImage waveform(const QString &path, double start, double len, int width, int height,
                const std::atomic<bool> *cancel) {
    // Downmix to mono so one solid wave is drawn (stereo overlays each channel
    // at half alpha). Resample so the window always holds at least two samples
    // per column (showwavespic refuses fewer) while bounding how much audio it
    // buffers on hours-long files. Pad with silence to the full window: the
    // filter stretches whatever decodes across the whole width, so a window
    // running past the decodable end would otherwise skew the time axis. peak
    // keeps transients honest where the default average shrinks them, and sqrt
    // scaling keeps quiet speech visible. showwavespic draws on a transparent
    // background, so PNG keeps the strip colour showing through.
    const int rate = qMax(16000, qCeil(2.0 * width / qMax(len, 0.001)));
    const QString filter = QString(
        "aformat=channel_layouts=mono,aresample=%3,apad=whole_dur=%4,"
        "showwavespic=s=%1x%2:colors=#d6d6da:filter=peak:scale=sqrt")
        .arg(width).arg(height).arg(rate).arg(seconds(len));
    return runImageCommand({
        "-loglevel", "error",
        "-ss", seconds(start),
        "-t", seconds(len),
        "-i", path,
        "-filter_complex", filter,
        "-frames:v", "1",
        "-f", "image2pipe",
        "-vcodec", "png",
        "pipe:1",
    }, "PNG", cancel);
}

QStringList trimArgs(const QString &src, const QString &dst, double start, double end,
                     int scaleHeight) {
    // Machine-readable progress on stdout (errors stay on stderr), so the UI
    // can show how far along the encode is.
    QStringList args = {"-y", "-loglevel", "error", "-progress", "pipe:1"};
    // -ss before -i seeks fast; -t gives the output duration. +faststart puts
    // the moov atom up front so shared clips start playing before they finish
    // downloading.
    args << "-ss" << QString::number(start, 'f', 3)
         << "-i" << src
         << "-t" << QString::number(qMax(end - start, 0.0), 'f', 3);
    // Cap the shorter side, judged on the decoded (rotation-applied) frame, so
    // portrait and landscape both keep their aspect ratio. -2 keeps the other
    // side divisible by two, which libx264 requires.
    if (scaleHeight > 0)
        args << "-vf"
             << QString("scale='if(gt(iw,ih),-2,%1)':'if(gt(iw,ih),%1,-2)'").arg(scaleHeight);
    args << "-c:v" << "libx264" << "-preset" << "veryfast"
         << "-crf" << "18" << "-c:a" << "aac"
         << "-movflags" << "+faststart"
         << dst;
    return args;
}

QStringList audioTrimArgs(const QString &src, const QString &dst, double start, double end) {
    QStringList args = {"-y", "-loglevel", "error", "-progress", "pipe:1"};
    args << "-ss" << QString::number(start, 'f', 3)
         << "-i" << src
         << "-t" << QString::number(qMax(end - start, 0.0), 'f', 3);
    // -vn drops embedded cover art (tags still carry over). -q:a 2 is lame's
    // ~190 kbps VBR preset. -f mp3 is explicit so the muxer never depends on
    // the temp file's name.
    args << "-vn" << "-c:a" << "libmp3lame" << "-q:a" << "2"
         << "-f" << "mp3"
         << dst;
    return args;
}

}  // namespace ffmpeg
