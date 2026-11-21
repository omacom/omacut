#include "ffmpeg.h"

#include <algorithm>
#include <cmath>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
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
    double first = 0.0, end = 0.0;
    int packets = 0;
    for (const QByteArray &raw : proc.readAllStandardOutput().split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty())
            continue;
        const QList<QByteArray> fields = line.split(',');
        bool ptsOk = false;
        const double pts = fields.value(0).toDouble(&ptsOk);
        if (!ptsOk)
            continue;
        bool durOk = false;
        const double pktDur = fields.value(1).toDouble(&durOk);
        first = packets == 0 ? pts : qMin(first, pts);
        end = qMax(end, pts + (durOk && pktDur > 0.0 ? pktDur : 0.0));
        ++packets;
    }
    return packets > 1 ? end - first : 0.0;
}
}

QString toolPath(const QString &tool) {
    return QStandardPaths::findExecutable(tool);
}

namespace {

// "13250/50000" as 0.265.
double rational(const QJsonValue &value) {
    const QStringList parts = value.toString().split('/');
    const double numerator = parts.value(0).toDouble();
    const double denominator = parts.size() > 1 ? parts[1].toDouble() : 1.0;
    return denominator != 0.0 ? numerator / denominator : 0.0;
}

// The bits per channel of ffprobe's pixel format name: yuv420p10le, gbrp12be,
// p010le (8 when it says nothing).
int bitDepthOf(const QJsonObject &stream) {
    const int raw = stream.value("bits_per_raw_sample").toString().toInt();
    if (raw > 0)
        return raw;
    const QString format = stream.value("pix_fmt").toString();
    static const QRegularExpression suffix("(\\d+)(le|be)$");
    const QRegularExpressionMatch match = suffix.match(format);
    if (match.hasMatch())
        return match.captured(1).toInt();
    static const QRegularExpression semiPlanar("^p0(\\d\\d)");
    const QRegularExpressionMatch semi = semiPlanar.match(format);
    if (semi.hasMatch())
        return semi.captured(1).toInt();
    return 8;
}

// Reads HDR10's static metadata out of a list of side data, whether it came
// from the stream or from a frame.
void readStaticMetadata(const QJsonArray &sideData, Color &color) {
    for (const QJsonValue &value : sideData) {
        const QJsonObject data = value.toObject();
        const QString type = data.value("side_data_type").toString();
        if (type == "Mastering display metadata") {
            // x265 counts chromaticity in 0.00002 and luminance in 0.0001 cd/m².
            const auto chroma = [&](const char *key) { return qRound(rational(data.value(key)) * 50000); };
            const auto luma = [&](const char *key) { return qRound(rational(data.value(key)) * 10000); };
            color.masterDisplay = QString("G(%1,%2)B(%3,%4)R(%5,%6)WP(%7,%8)L(%9,%10)")
                .arg(chroma("green_x")).arg(chroma("green_y"))
                .arg(chroma("blue_x")).arg(chroma("blue_y"))
                .arg(chroma("red_x")).arg(chroma("red_y"))
                .arg(chroma("white_point_x")).arg(chroma("white_point_y"))
                .arg(luma("max_luminance")).arg(luma("min_luminance"));
        } else if (type == "Content light level metadata") {
            color.maxCll = QString("%1,%2").arg(data.value("max_content").toInt())
                                           .arg(data.value("max_average").toInt());
        }
    }
}

Color colorOf(const QJsonObject &stream) {
    Color color;
    color.bitDepth = bitDepthOf(stream);
    const auto tag = [&](const char *key) {
        const QString value = stream.value(key).toString();
        return value == "unknown" ? QString() : value;
    };
    color.primaries = tag("color_primaries");
    color.transfer = tag("color_transfer");
    color.space = tag("color_space");
    color.range = tag("color_range");
    readStaticMetadata(stream.value("side_data_list").toArray(), color);
    return color;
}

// HEVC carries the static metadata in SEI messages, which ffprobe lists on
// frames rather than on the stream: read the first one.
void readFrameMetadata(const QString &ffprobe, const QString &path, int streamIndex, Color &color) {
    QProcess proc;
    proc.start(ffprobe, {"-v", "error", "-print_format", "json", "-select_streams", QString::number(streamIndex),
                         "-read_intervals", "%+#1", "-show_frames", path});
    if (!proc.waitForFinished(kProbeTimeoutMs) || proc.exitCode() != 0) {
        proc.kill();
        proc.waitForFinished(-1);
        return;
    }
    const QJsonArray frames = QJsonDocument::fromJson(proc.readAllStandardOutput()).object().value("frames").toArray();
    if (!frames.isEmpty())
        readStaticMetadata(frames.first().toObject().value("side_data_list").toArray(), color);
}

}  // namespace

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

    info.color = colorOf(stream);
    if (info.color.transfer == "smpte2084" && info.color.masterDisplay.isEmpty())
        readFrameMetadata(ffprobe, path, stream.value("index").toInt(), info.color);
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

    info.start = root.value("format").toObject().value("start_time").toString().toDouble();

    // Duration can live on the stream, on the container, or (for streamed
    // WebM captures) only on the last video packet.
    QString durationStr = stream.value("duration").toString();
    if (durationStr.isEmpty())
        durationStr = root.value("format").toObject().value("duration").toString();
    if (!durationStr.isEmpty()) {
        info.duration = durationStr.toDouble();
    } else {
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
    QProcess proc;
    proc.start(toolPath("ffprobe"), {"-v", "error", "-select_streams", "V:0",
               "-show_entries", "packet=pts_time", "-of", "csv=p=0", path});
    if (!proc.waitForFinished(kProbeTimeoutMs)) {
        proc.kill();
        proc.waitForFinished();
        return {};
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    QList<double> times;
    for (const QByteArray &line : proc.readAllStandardOutput().split('\n')) {
        bool ok = false;
        const double time = line.split(',').value(0).toDouble(&ok);
        if (ok && std::isfinite(time))
            times.append(time - start);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    return times;
}

QStringList shownFrameArgs(const QString &path, double pts) {
    // Packets only, nothing decoded, so reaching back 30s is cheap. That
    // covers a recording's still last frame, held long past its start. The
    // 10s past it covers B-frames, whose packets come after later frames',
    // by a few frames: seconds, at a frame a second.
    return {
        "-v", "error",
        "-select_streams", "V:0",
        "-read_intervals", QString("%1%%2").arg(qMax(pts - 30.0, 0.0), 0, 'f', 3)
                               .arg(pts + 10.0, 0, 'f', 3),
        "-show_entries", "packet=pts_time,flags",
        "-of", "csv=p=0",
        path,
    };
}

ShownFrame shownFrame(const QByteArray &packets, double pts) {
    QList<std::pair<double, bool>> listed;  // time, keyframe
    double shown = -1.0;
    for (const QByteArray &line : packets.split('\n')) {
        // "time,flags", with empty fields after it in some containers.
        const QList<QByteArray> fields = line.trimmed().split(',');
        bool ok = false;
        const double time = fields.value(0).toDouble(&ok);
        if (!ok)
            continue;
        listed.append({time, fields.value(1).startsWith('K')});
        if (time <= pts + 0.0005)
            shown = qMax(shown, time);
    }
    ShownFrame frame;
    frame.time = shown >= 0.0 ? shown : pts;
    for (const auto &[time, key] : listed) {
        if (key && time <= frame.time)
            frame.keyframe = qMax(frame.keyframe, time);
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
                     int width, int height, int scaleHeight) {
    bool audio = false;
    bool mixed = false;
    // The output is as deep as the deepest segment, and takes the color tags
    // of the first HDR one (or else the first segment).
    Color color = segments.first().color;
    int depth = 8;
    for (const Segment &segment : segments) {
        audio = audio || segment.audio;
        mixed = mixed || segment.path != segments.first().path;
        depth = qMax(depth, segment.color.bitDepth);
        if (!color.hdr() && segment.color.hdr())
            color = segment.color;
    }
    // Encoders take 8, 10 or 12 bits: 9 becomes 10, and 14 or 16 become 12.
    const bool deep = depth > 8;
    const QString pixFmt = deep ? QString("yuv420p%1le").arg(depth > 10 ? 12 : 10) : QString("yuv420p");

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
        if (!mixed) {
            inputs += QString("[%1:V:0]").arg(i) + (audio ? QString("[%1:a:0]").arg(i) : QString());
            continue;
        }
        // Concat needs one frame size and one audio format throughout. Pixels
        // are squared first, so non-square video keeps its shape; -1 centres
        // the padding; libx264 needs even sides.
        graph += QString("[%1:V:0]scale='trunc(iw*sar/2)*2':ih,setsar=1,"
                         "scale=%2:%3:force_original_aspect_ratio=decrease,"
                         "pad=%2:%3:-1:-1,setsar=1,format=%4[v%1];")
                     .arg(i).arg(width / 2 * 2).arg(height / 2 * 2).arg(pixFmt);
        if (segment.audio)
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
    args << "-filter_complex" << graph << "-map" << "[v]";
    if (audio)
        args << "-map" << "[a]" << "-c:a" << "aac";
    // +faststart puts the moov atom up front so shared clips start playing
    // before they finish downloading. The pixel format keeps RGB and 4:4:4
    // sources playable everywhere; 10 and 12 bit sources stay that deep, which
    // libx264 can't do everywhere, so they go to HEVC.
    if (deep)
        args << "-c:v" << "libx265" << "-tag:v" << "hvc1";
    else
        args << "-c:v" << "libx264";
    args << "-pix_fmt" << pixFmt;
    // Say how to read the pixels, or players guess BT.709 and an HDR video
    // comes out washed out.
    if (deep || color.hdr()) {
        const QList<QPair<QString, QString>> tags = {
            {"-color_primaries", color.primaries}, {"-color_trc", color.transfer},
            {"-colorspace", color.space}, {"-color_range", color.range}};
        for (const auto &[option, value] : tags) {
            if (!value.isEmpty())
                args << option << value;
        }
    }
    if (deep) {
        // HDR10's static metadata only reaches x265 through its own options.
        QStringList x265;
        if (color.transfer == "smpte2084") {
            if (!color.masterDisplay.isEmpty())
                x265 << "master-display=" + color.masterDisplay;
            if (!color.maxCll.isEmpty())
                x265 << "max-cll=" + color.maxCll;
        }
        if (color.hdr())
            x265 << "repeat-headers=1";
        if (!x265.isEmpty())
            args << "-x265-params" << x265.join(':');
        args << "-preset" << "veryfast" << "-crf" << "20";
    } else {
        args << "-preset" << "veryfast" << "-crf" << "18";
    }
    args << "-movflags" << "+faststart" << dst;
    return args;
}

}  // namespace ffmpeg
