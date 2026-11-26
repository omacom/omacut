#include "ffmpeg.h"

#include <algorithm>
#include <cmath>

#include <QByteArrayView>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

namespace ffmpeg {

namespace {
// Upper bound on a probe so a hung/unresponsive file (e.g. a stalled
// network mount) can't stall the worker thread running it forever. Callers
// keep probe() off the UI thread; this bounds even that wait.
constexpr int kProbeTimeoutMs = 15000;
// Poll granularity while waiting on a thumbnail child, so cancellation is prompt.
constexpr int kThumbPollMs = 50;
// A thumbnail child that never finishes (a stalled file) must not occupy a
// thumbnail worker forever; single frames take nowhere near this long.
constexpr int kThumbTimeoutMs = 30000;

// Scan trimmed non-empty lines without per-line QByteArray allocations.
template <typename Func>
void forEachLine(QByteArrayView data, Func &&fn) {
    qsizetype pos = 0;
    while (pos < data.size()) {
        const qsizetype nl = data.indexOf('\n', pos);
        const qsizetype end = nl < 0 ? data.size() : nl;
        const QByteArrayView line = data.sliced(pos, end - pos).trimmed();
        if (!line.isEmpty())
            fn(line);
        pos = nl < 0 ? data.size() : nl + 1;
    }
}

// Streamed WebM/MKV captures (e.g. from a browser's MediaRecorder) have no
// duration in the header; scan the video stream's packet timestamps. That
// reads the whole file, so it's only the last resort: a couple of hours of
// cached video takes under a second, a slow disk up to the probe timeout.
double probePacketDuration(const QString &ffprobe, const QString &path, int streamIndex) {
    QProcess proc;
    proc.start(ffprobe, {
        "-v", "error",
        "-select_streams", QString::number(streamIndex),
        "-show_entries", "packet=pts_time,duration_time",
        "-of", "csv=p=0",
        path,
    });
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(-1);
        return 0.0;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return 0.0;

    // From the first timestamp, which a cut or a capture can start well past 0.
    // A single packet is a still image, not a video.
    const QByteArray out = proc.readAllStandardOutput();
    double first = 0.0, end = 0.0;
    int packets = 0;
    forEachLine(out, [&](QByteArrayView line) {
        const qsizetype comma = line.indexOf(',');
        const QByteArrayView ptsField = comma < 0 ? line : line.first(comma);
        bool ptsOk = false;
        const double pts = ptsField.toDouble(&ptsOk);
        if (!ptsOk || !std::isfinite(pts))
            return;
        double pktDur = 0.0;
        if (comma >= 0) {
            const qsizetype nextComma = line.indexOf(',', comma + 1);
            const QByteArrayView durField = nextComma < 0
                ? line.sliced(comma + 1)
                : line.sliced(comma + 1, nextComma - comma - 1);
            bool durOk = false;
            const double parsed = durField.toDouble(&durOk);
            if (durOk && std::isfinite(parsed) && parsed > 0.0)
                pktDur = parsed;
        }
        first = packets == 0 ? pts : qMin(first, pts);
        end = qMax(end, pts + pktDur);
        ++packets;
    });
    return packets > 1 ? end - first : 0.0;
}

bool parseFiniteDouble(const QJsonValue &value, double *out) {
    bool ok = false;
    const double parsed = value.toString().toDouble(&ok);
    if (!ok || !std::isfinite(parsed))
        return false;
    *out = parsed;
    return true;
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
        info.error = QCoreApplication::translate("ffmpeg", "`ffprobe` was not found on your PATH. Install ffmpeg.");
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
        info.error = QCoreApplication::translate("ffmpeg", "ffprobe timed out reading this file.");
        return info;
    }

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        info.error = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        if (info.error.isEmpty())
            info.error = QCoreApplication::translate("ffmpeg", "ffprobe failed.");
        return info;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(proc.readAllStandardOutput());
    const QJsonObject root = doc.object();
    QJsonObject stream;
    for (const QJsonValue &value : root.value("streams").toArray()) {
        const QJsonObject candidate = value.toObject();
        const QString type = candidate.value("codec_type").toString();
        // Skip embedded cover art, which ffprobe lists as a single-frame video stream.
        if (type == "video" && stream.isEmpty()
                && candidate.value("disposition").toObject().value("attached_pic").toInt() != 1)
            stream = candidate;
        else if (type == "audio")
            info.audio = true;
    }
    if (stream.isEmpty()) {
        info.error = QCoreApplication::translate("ffmpeg", "No video stream found in this file.");
        return info;
    }

    info.width = stream.value("width").toInt();
    info.height = stream.value("height").toInt();
    // Phones record sideways and say how to turn it upright, in a display
    // matrix or the older rotate tag.
    int rotation = stream.value("tags").toObject().value("rotate").toString().toInt();
    for (const QJsonValue &sideData : stream.value("side_data_list").toArray()) {
        if (sideData.toObject().contains("rotation"))
            rotation = sideData.toObject().value("rotation").toInt();
    }
    // Sized as displayed: widened for non-square pixels, like PAL's 16:15.
    const QStringList sar = stream.value("sample_aspect_ratio").toString().split(':');
    if (sar.size() == 2 && sar[0].toInt() > 0 && sar[1].toInt() > 0)
        info.width = qRound(double(info.width) * sar[0].toInt() / sar[1].toInt());
    if (qAbs(rotation) % 180 == 90)
        std::swap(info.width, info.height);

    parseFiniteDouble(root.value("format").toObject().value("start_time"), &info.start);

    // Duration can live on the stream, on the container, or (for streamed
    // WebM captures) only on the last video packet. Missing or "N/A" values
    // fall through to the next source.
    if (!parseFiniteDouble(stream.value("duration"), &info.duration)
        && !parseFiniteDouble(root.value("format").toObject().value("duration"), &info.duration)) {
        info.duration = probePacketDuration(ffprobe, path, stream.value("index").toInt());
        if (info.duration <= 0.0) {
            info.error = QCoreApplication::translate("ffmpeg", "Could not determine the video duration.");
            return info;
        }
    }

    info.ok = info.duration > 0.0;
    if (!info.ok)
        info.error = QCoreApplication::translate("ffmpeg", "Video has a zero or invalid duration.");
    return info;
}

namespace {

// The frame at `time`, or with `before`, the last frame in the `before`
// seconds up to it: decoded in order, then reversed to take the last.
QImage frameAt(const QString &path, double time, int height, double before,
               const QElapsedTimer &deadline, const std::function<bool()> &stopRequested) {
    const QString ffmpeg = toolPath("ffmpeg");
    if (ffmpeg.isEmpty())
        return {};

    QStringList args = {"-loglevel", "error", "-ss", QString::number(qMax(time - before, 0.0), 'f', 3)};
    if (before > 0.0)
        args << "-t" << QString::number(qMin(before, time), 'f', 3);
    args << "-i" << path << "-frames:v" << "1"
         << "-vf" << QString("scale=-1:%1%2").arg(height).arg(before > 0.0 ? ",reverse" : "")
         << "-f" << "image2pipe" << "-vcodec" << "mjpeg" << "pipe:1";
    QProcess proc;
    proc.start(ffmpeg, args);

    // Poll instead of waitForFinished(-1) so a stop request can kill the child
    // promptly — otherwise a worker thread would block until ffmpeg finishes
    // on its own. The deadline bounds the wait even if nobody asks to stop.
    while (!proc.waitForFinished(kThumbPollMs)) {
        if (proc.state() == QProcess::NotRunning)
            break;  // failed to start, or exited between polls
        if ((stopRequested && stopRequested())
            || deadline.elapsed() > kThumbTimeoutMs) {
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

}  // namespace

QImage thumbnail(const QString &path, double time, int height,
                 const std::function<bool()> &stopRequested) {
    QElapsedTimer deadline;  // one for every attempt, so a stalled file costs one timeout
    deadline.start();
    QImage image = frameAt(path, time, height, 0.0, deadline, stopRequested);
    // Past the last frame, which a video stream can end before the file does
    // (or a slow frame rate leaves a while before the end), ffmpeg gives
    // nothing: show the last frame before then rather than a blank. A second
    // back covers any ordinary frame rate; 30 s, a recording's still last
    // frame, held long past its start.
    for (double before : {1.0, 30.0}) {
        if (!image.isNull() || time <= 0.0 || deadline.elapsed() > kThumbTimeoutMs
            || (stopRequested && stopRequested()))
            break;
        image = frameAt(path, time, height, before, deadline, stopRequested);
    }
    return image;
}

QList<double> frameTimes(const QString &path, double start) {
    const QString ffprobe = toolPath("ffprobe");
    if (ffprobe.isEmpty())
        return {};
    QProcess proc;
    proc.start(ffprobe, {"-v", "error", "-select_streams", "V:0",
               "-show_entries", "packet=pts_time", "-of", "csv=p=0", path});
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        return {};
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    const QByteArray out = proc.readAllStandardOutput();
    QList<double> times;
    times.reserve(out.count('\n'));
    forEachLine(out, [&](QByteArrayView line) {
        const qsizetype comma = line.indexOf(',');
        const QByteArrayView field = comma < 0 ? line : line.first(comma);
        bool ok = false;
        const double time = field.toDouble(&ok);
        if (ok && std::isfinite(time))
            times.append(time - start);
    });
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

QStringList shownFrameArgs(const QString &path, double pts) {
    // Packets only, nothing decoded, so reaching back 30s is cheap. That
    // covers a recording's still last frame, held long past its start. The
    // 10s past it covers B-frames, whose packets come after later frames',
    // by a few frames: seconds, at a frame a second. Leaving the start empty
    // near zero keeps negative-PTS packets at the beginning of the stream.
    const double from = pts - 30.0;
    const QString interval = from > 0.0
        ? QString("%1%%2").arg(from, 0, 'f', 3).arg(pts + 10.0, 0, 'f', 3)
        : QString("%%1").arg(pts + 10.0, 0, 'f', 3);
    return {
        "-v", "error",
        "-select_streams", "V:0",
        "-read_intervals", interval,
        "-show_entries", "packet=pts_time,flags",
        "-of", "csv=p=0",
        path,
    };
}

ShownFrame shownFrame(const QByteArray &packets, double pts) {
    QList<std::pair<double, bool>> listed;  // time, keyframe
    bool hasShown = false;
    double shown = 0.0;
    for (const QByteArray &line : packets.split('\n')) {
        // "time,flags", with empty fields after it in some containers.
        const QList<QByteArray> fields = line.trimmed().split(',');
        bool ok = false;
        const double time = fields.value(0).toDouble(&ok);
        if (!ok || !std::isfinite(time))
            continue;
        listed.append({time, fields.value(1).startsWith('K')});
        if (time <= pts + 0.0005) {
            shown = hasShown ? qMax(shown, time) : time;
            hasShown = true;
        }
    }
    ShownFrame frame;
    frame.time = hasShown ? shown : pts;
    for (const auto &[time, key] : listed) {
        if (key && time <= frame.time) {
            frame.keyframe = frame.hasKeyframe ? qMax(frame.keyframe, time) : time;
            frame.hasKeyframe = true;
        }
    }
    return frame;
}

QStringList frameArgs(const QString &path, double frameTime, double keyframeTime,
                      const QString &pngPath) {
    // Jump to the keyframe, then decode up to a hair before the frame's start
    // (which ffprobe printed rounded). Jumping anywhere else can come up
    // empty: MPEG-TS has no index, so it lands mid-stream with nothing to
    // decode from.
    const double target = qMax(frameTime - 0.0005, 0.0);
    const double jump = qBound(0.0, keyframeTime, target);
    return {
        "-loglevel", "error",
        "-y",
        "-ss", QString::number(jump, 'f', 6),
        "-i", path,
        "-ss", QString::number(target - jump, 'f', 6),
        "-frames:v", "1",
        "-vf", "scale=round(iw*sar):ih,setsar=1",
        pngPath,
    };
}

QStringList trimArgs(const QList<Segment> &segments, const QString &dst,
                     int width, int height, int scaleHeight, bool gif) {
    bool audio = false;
    bool mixed = false;
    for (const Segment &segment : segments) {
        audio = audio || (!gif && segment.audio);
        mixed = mixed || segment.path != segments.first().path;
    }

    // Machine-readable progress on stdout (errors stay on stderr), so the UI
    // can show how far along the encode is.
    QStringList args = {"-y", "-loglevel", "error", "-progress", "pipe:1"};
    // One input per segment: -ss before -i seeks fast and -t bounds it, so
    // only what survives the cut is decoded. The concat filter joins them.
    // V:0 is the first video stream that isn't cover art, as probe picks.
    QString graph;
    QString inputs;
    for (int i = 0; i < segments.size(); ++i) {
        const Segment &segment = segments[i];
        const QString length = QString::number(qMax(segment.end - segment.start, 0.0), 'f', 3);
        args << "-ss" << QString::number(segment.start, 'f', 3) << "-t" << length
             << "-i" << segment.path;
        QString video = QString("[%1:V:0]").arg(i);
        if (gif) {
            // A slow source frame can extend past the cut. Bound each segment
            // on the GIF's time base before joining it to the next one.
            graph += video + QString("fps=10:round=up,trim=duration=%1[g%2];").arg(length).arg(i);
            video = QString("[g%1]").arg(i);
        }
        if (!mixed) {
            inputs += video + (audio ? QString("[%1:a:0]").arg(i) : QString());
            continue;
        }
        // Concat needs one frame size and one audio format throughout. Pixels
        // are squared first, so non-square video keeps its shape; -1 centres
        // the padding; libx264 needs even sides.
        graph += video + QString("scale='trunc(iw*sar/2)*2':ih,setsar=1,"
                         "scale=%2:%3:force_original_aspect_ratio=decrease,"
                         "pad=%2:%3:-1:-1,setsar=1,format=yuv420p[v%1];")
                     .arg(i).arg(width / 2 * 2).arg(height / 2 * 2);
        if (audio && segment.audio)
            graph += QString("[%1:a:0]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[a%1];").arg(i);
        else if (audio)
            graph += QString("anullsrc=r=48000:cl=stereo,atrim=duration=%2,aformat=sample_fmts=fltp[a%1];")
                         .arg(i).arg(length);
        inputs += QString("[v%1]").arg(i) + (audio ? QString("[a%1]").arg(i) : QString());
    }
    graph += inputs + QString("concat=n=%1:v=1:a=%2").arg(segments.size()).arg(audio ? 1 : 0);
    // Cap the shorter side, judged on the decoded (rotation-applied) frame, so
    // portrait and landscape both keep their aspect ratio. -2 keeps the other
    // side divisible by two, which libx264 requires.
    if (scaleHeight > 0)
        graph += QString("[joined]%1;[joined]scale='if(gt(iw,ih),-2,%2)':'if(gt(iw,ih),%2,-2)'[v]")
                     .arg(audio ? "[a]" : "").arg(scaleHeight);
    // libx264 needs even sides. Judged on the decoded frame, since the probed
    // size is the displayed one; cropping the odd pixel keeps pixels square.
    else if (!mixed)
        graph += QString("[joined]%1;[joined]crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0[v]")
                     .arg(audio ? "[a]" : "");
    else
        graph += QString("[v]") + (audio ? "[a]" : "");
    if (gif) {
        // Per-frame palettes keep the encoder streaming even for long recordings.
        graph += ";[v]split[pixels][colors];"
                 "[colors]palettegen=stats_mode=single[palette];"
                 "[pixels][palette]paletteuse=new=1:dither=bayer[gif]";
        args << "-filter_complex" << graph << "-map" << "[gif]" << "-loop" << "0" << dst;
        return args;
    }
    args << "-filter_complex" << graph << "-map" << "[v]";
    if (audio)
        args << "-map" << "[a]" << "-c:a" << "aac";
    // +faststart puts the moov atom up front so shared clips start playing
    // before they finish downloading; yuv420p keeps RGB/4:4:4/10-bit sources
    // playable everywhere.
    args << "-c:v" << "libx264" << "-pix_fmt" << "yuv420p"
         << "-preset" << "veryfast" << "-crf" << "18"
         << "-movflags" << "+faststart"
         << dst;
    return args;
}

}  // namespace ffmpeg
