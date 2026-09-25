#include "ffmpeg.h"

#include <QJsonArray>
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

// Audio codecs MP4 muxes bit-exact (verified on ffmpeg 9). Other codecs mux
// under a generic mp4a tag that players assume is AAC — re-encode instead.
bool mp4SafeAudio(const QString &codec) {
    static const QStringList safe = {
        "aac", "mp3", "mp2", "alac", "ac3", "eac3", "flac", "opus",
        "pcm_s16le", "pcm_s24le",
    };
    return safe.contains(codec);
}

// Text subtitle codecs that convert cleanly to mov_text.
bool mp4TextSubtitle(const QString &codec) {
    static const QStringList safe = {
        "subrip", "ass", "ssa", "mov_text", "webvtt", "text",
    };
    return safe.contains(codec);
}

// Human-readable name for a stream the overwrite can't carry into MP4 —
// shown in the pre-flight warning, so it must name what will be lost.
QString describeDrop(const StreamInfo &stream) {
    if (stream.codecType == "attachment")
        return stream.fileName.isEmpty()
            ? QStringLiteral("attachment")
            : QStringLiteral("attachment \"%1\"").arg(stream.fileName);
    if (stream.codecName.isEmpty())
        return QStringLiteral("%1 stream").arg(stream.codecType);
    return QStringLiteral("%1 stream (%2)").arg(stream.codecType, stream.codecName);
}

// What an overwrite does with one stream: map it with a -c policy, skip it
// quietly, or drop it with a warning line. Shared by overwriteArgs and
// overwriteDrops so the map and the warning can never disagree.
struct OverwritePlan {
    bool map = false;
    QString codec;
    QString drop;
};

OverwritePlan planOverwriteStream(const StreamInfo &stream, bool &primaryVideoTaken) {
    if (stream.codecType == "video") {
        // Attached pics (cover art) can't be re-encoded to h264 — copy them;
        // the mp4 muxer carries the attached_pic disposition automatically.
        if (stream.attachedPic)
            return {true, "copy", {}};
        if (!primaryVideoTaken) {
            primaryVideoTaken = true;
            return {true, "libx264", {}};
        }
        return {false, {}, describeDrop(stream)};
    }
    if (stream.codecType == "audio")
        return {true, mp4SafeAudio(stream.codecName) ? QString("copy") : QString("aac"), {}};
    if (stream.codecType == "subtitle")
        return mp4TextSubtitle(stream.codecName)
            ? OverwritePlan{true, "mov_text", {}}
            : OverwritePlan{false, {}, describeDrop(stream)};
    // codec_tag_string "text" marks the MP4 chapter data track — it carries no
    // frames and the muxer regenerates it from -map_chapters, so it is neither
    // mapped nor warned about.
    if (stream.codecType == "data" && stream.codecTag == "text")
        return {};
    return {false, {}, describeDrop(stream)};
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
    if (streams.isEmpty()) {
        info.error = "No video stream found in this file.";
        return info;
    }

    // Inventory every stream — the overwrite path maps them selectively
    // instead of letting ffmpeg pick one video and one audio stream.
    QJsonObject primary;
    for (const QJsonValue &value : streams) {
        const QJsonObject stream = value.toObject();
        StreamInfo s;
        s.index = stream.value("index").toInt(-1);
        s.codecType = stream.value("codec_type").toString();
        s.codecName = stream.value("codec_name").toString();
        s.codecTag = stream.value("codec_tag_string").toString();
        s.attachedPic = stream.value("disposition").toObject()
                            .value("attached_pic").toInt() == 1;
        s.fileName = stream.value("tags").toObject()
                         .value("filename").toString();
        info.streams.append(s);
        // An attached pic reports codec_type "video" but carries cover-art
        // dimensions — never let it supply the frame size or duration.
        if (primary.isEmpty() && s.codecType == "video" && !s.attachedPic)
            primary = stream;
    }
    if (primary.isEmpty()) {
        info.error = "No video stream found in this file.";
        return info;
    }

    info.width = primary.value("width").toInt();
    info.height = primary.value("height").toInt();

    // Duration can live on the stream or on the container.
    QString durationStr = primary.value("duration").toString();
    if (durationStr.isEmpty())
        durationStr = root.value("format").toObject().value("duration").toString();
    if (durationStr.isEmpty()) {
        info.error = "Could not determine the video duration.";
        return info;
    }

    info.duration = durationStr.toDouble();

    info.ok = info.duration > 0.0;
    if (!info.ok)
        info.error = "Video has a zero or invalid duration.";
    return info;
}

QImage thumbnail(const QString &path, double time, int height,
                 const std::atomic<bool> *cancel) {
    const QString ffmpeg = toolPath("ffmpeg");
    if (ffmpeg.isEmpty())
        return {};

    QProcess proc;
    proc.start(ffmpeg, {
        "-loglevel", "error",
        "-ss", QString::number(qMax(time, 0.0), 'f', 3),
        "-i", path,
        "-frames:v", "1",
        "-vf", QString("scale=-1:%1").arg(height),
        "-f", "image2pipe",
        "-vcodec", "mjpeg",
        "pipe:1",
    });

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
    img.loadFromData(data, "JPEG");
    return img;
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

QStringList overwriteArgs(const VideoInfo &info, const QString &dst, double start, double end) {
    // Machine-readable progress on stdout (errors stay on stderr), so the UI
    // can show how far along the encode is.
    QStringList args = {"-y", "-loglevel", "error", "-progress", "pipe:1"};
    // -ss before -i seeks fast; -t gives the output duration. +faststart puts
    // the moov atom up front so shared clips start playing before they finish
    // downloading.
    args << "-ss" << QString::number(start, 'f', 3)
         << "-i" << info.path
         << "-t" << QString::number(qMax(end - start, 0.0), 'f', 3);

    // Map every stream MP4 can carry by absolute input index, and emit codec
    // policy in the same loop: -c:N addresses the OUTPUT position, which only
    // counts mapped streams.
    bool primaryVideoTaken = false;
    int out = 0;
    for (const StreamInfo &stream : info.streams) {
        const OverwritePlan plan = planOverwriteStream(stream, primaryVideoTaken);
        if (!plan.map)
            continue;
        args << "-map" << QString("0:%1").arg(stream.index);
        args << QString("-c:%1").arg(out) << plan.codec;
        if (plan.codec == "libx264")
            args << "-preset" << "veryfast" << "-crf" << "18";
        ++out;
    }
    // Chapters are copied and re-based to the trim range by the muxer; global
    // metadata carries over unchanged.
    args << "-map_metadata" << "0" << "-map_chapters" << "0"
         << "-movflags" << "+faststart"
         << dst;
    return args;
}

QStringList overwriteDrops(const VideoInfo &info) {
    QStringList drops;
    bool primaryVideoTaken = false;
    for (const StreamInfo &stream : info.streams) {
        const OverwritePlan plan = planOverwriteStream(stream, primaryVideoTaken);
        if (!plan.drop.isEmpty())
            drops << plan.drop;
    }
    return drops;
}

}  // namespace ffmpeg
