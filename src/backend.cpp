#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QTemporaryFile>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"

namespace {
QString mp4PathFor(const QString &path) {
    const QFileInfo file(path);
    if (file.suffix().compare(QStringLiteral("mp4"), Qt::CaseInsensitive) == 0)
        return path;

    const QString baseName = file.completeBaseName().isEmpty()
        ? file.fileName()
        : file.completeBaseName();
    return file.dir().filePath(baseName + QStringLiteral(".mp4"));
}

bool replaceWithTemp(const QString &tmpPath, const QString &outPath) {
    const QByteArray tmpName = QFile::encodeName(tmpPath);
    const QByteArray outName = QFile::encodeName(outPath);
    return std::rename(tmpName.constData(), outName.constData()) == 0;
}

// Rejects non-local schemes, relative paths (which ffprobe could read as
// options or URLs), and host-bearing or "//host/..." network paths.
bool isLocalAbsolutePath(const QUrl &url, QString *path) {
    *path = url.toLocalFile();
    return url.isLocalFile() && url.host().isEmpty()
        && QDir::isAbsolutePath(*path) && !path->startsWith(QLatin1String("//"));
}

constexpr double kFrameTolerance = 0.0005;

// Resolve the adjacent displayed frame timestamp within clip from a sorted
// frame-times table. Matches the player's millisecond resolution, including
// rounded timestamps at fractional frame rates.
double resolveStepTarget(const QList<double> &times, const edit::Clip &clip,
                         double sourceTime, bool atClipEnd, int direction) {
    if (direction > 0) {
        const auto it = std::upper_bound(times.cbegin(), times.cend(), sourceTime + kFrameTolerance);
        return it != times.cend() && *it < clip.out ? *it : clip.out;
    }
    const auto end = sourceTime + kFrameTolerance < clip.out
        ? std::upper_bound(times.cbegin(), times.cend(), sourceTime + kFrameTolerance)
        : std::lower_bound(times.cbegin(), times.cend(), clip.out);
    const auto first = std::lower_bound(times.cbegin(), end, clip.in);
    if (first == end)
        return clip.in;
    // Only at/past the actual end: treating a frame just before
    // it as the end would keep returning that same frame.
    if (atClipEnd)
        return *(end - 1);
    return end - first >= 2 ? *(end - 2) : clip.in;
}
}

Backend::Backend(ThumbProvider *provider, QObject *parent)
    : Backend(provider, new PortalFilePicker(), parent) {}

Backend::Backend(ThumbProvider *provider, FilePicker *filePicker, QObject *parent)
    : QObject(parent), m_provider(provider), m_filePicker(filePicker) {
    if (!m_filePicker->parent())
        m_filePicker->setParent(this);
    wireFilePicker();
}

Backend::~Backend() {
    // Stop any running export or frame-save children before removing m_framePart,
    // so ffmpeg can neither recreate it nor log destruction warnings.
    for (QProcess *proc : findChildren<QProcess *>(Qt::FindDirectChildrenOnly)) {
        proc->disconnect(this);
        if (proc->state() != QProcess::NotRunning) {
            proc->kill();
            proc->waitForFinished(1000);
        }
    }
    // Quit mid-save: the ffmpeg writing it is gone now, so its file goes too.
    if (!m_framePart.isEmpty())
        QFile::remove(m_framePart);
}

void Backend::wireFilePicker() {
    connect(m_filePicker, &FilePicker::openSelected, this, [this](const QUrl &url) {
        if (std::exchange(m_adding, false))
            addVideo(url, m_addAt);
        else
            load(url);
    });
    connect(m_filePicker, &FilePicker::exportSelected, this, [this](const QUrl &url, int scaleHeight) {
        exportClips(url, m_exportDialogClips, scaleHeight);
    });
    connect(m_filePicker, &FilePicker::failed, this, &Backend::loadError);
}

void Backend::setBusy(bool busy) {
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

bool Backend::isVideoPath(const QString &path) const {
    const QString target = QFileInfo(path).canonicalFilePath();
    if (target.isEmpty())
        return false;
    for (const Video &video : m_videos) {
        if (QFileInfo(video.info.path).canonicalFilePath() == target)
            return true;
    }
    return false;
}

QVariantList Backend::videoList() const {
    QVariantList result;
    for (const Video &video : m_videos)
        result.append(QVariantMap{{QStringLiteral("url"), video.url},
                                  {QStringLiteral("thumbKey"), video.thumbKey}});
    return result;
}

bool Backend::checkedPath(const QUrl &url, QString *path) {
    if (!isLocalAbsolutePath(url, path)) {
        emit loadError(tr("Only local files can be opened."));
        return false;
    }
    return true;
}

void Backend::enqueueVideo(const PendingVideo &pending) {
    m_probeQueue.append(pending);
    startNextProbe();
}

void Backend::startNextProbe() {
    if (m_probing || m_probeQueue.isEmpty())
        return;
    const PendingVideo pending = m_probeQueue.takeFirst();
    m_probing = true;
    emit busyChanged();

    // ffprobe can take seconds on a streamed file with no header duration,
    // so it runs on a worker thread; only applying the result needs the UI
    // thread. The guard is only read back on the UI thread, and drops the
    // result if this backend is already gone by then.
    const QPointer<Backend> guard(this);
    QThreadPool::globalInstance()->start([guard, pending] {
        const ffmpeg::VideoInfo info = ffmpeg::probe(pending.path);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [guard, pending, info] {
            if (guard)
                guard->applyProbe(pending, info);
        }, Qt::QueuedConnection);
    });
}

void Backend::applyProbe(const PendingVideo &pending, const ffmpeg::VideoInfo &info) {
    if (!info.ok) {
        // An export that rewrote its own source reports done either way;
        // the reopen failing is the part the user needs explained.
        if (pending.exportReload) {
            emit exportDone(pending.path);
            emit exportReopenFailed();
        } else {
            emit loadError(info.error);
        }
        if (pending.batch)
            ++m_batchSkipped;
    } else {
        Video video;
        video.info = info;
        video.url = pending.url;
        video.thumbKey = ++m_nextThumbKey;
        m_provider->setVideo(video.thumbKey, video.info.path);
        if (pending.batch)
            ++m_batchAdded;

        if (!pending.adding || m_videos.isEmpty()) {
            m_videos = {video};
            m_timeline.reset(video.info.duration);
            emit videosChanged();
            emit infoChanged();
        } else {
            // A chained batch add lands after the clip the previous batch
            // member landed on, so retarget the rest of the batch at it.
            const int inserted = m_timeline.clipAt(pending.addAt) + 1;
            m_videos.append(video);
            emit videosChanged();
            m_timeline.addSource(video.info.duration, pending.addAt);
            if (pending.chain) {
                const double start = m_timeline.clipStart(inserted);
                for (PendingVideo &queued : m_probeQueue) {
                    if (queued.batch == pending.batch)
                        queued.addAt = start;
                }
            }
        }
        // Only now, with the saved file reopened, is the export fully
        // done: reporting earlier let the reload erase the "Saved" notice
        // and reset the timeline under edits made in between.
        if (pending.exportReload)
            emit exportDone(pending.path);
    }

    m_probing = false;
    if (!m_probeQueue.isEmpty()) {
        startNextProbe();
        return;
    }
    emit busyChanged();
    if (m_batchAdded > 0 && m_batchSkipped > 0)
        emit skippedVideos(m_batchSkipped);
    m_batchAdded = 0;
    m_batchSkipped = 0;
}

bool Backend::load(const QUrl &url) {
    // A running export marks its clips exported when it finishes, which
    // must not land on a different set of videos.
    if (busy())
        return false;

    PendingVideo pending;
    pending.url = url;
    pending.adding = false;
    if (!checkedPath(url, &pending.path))
        return false;
    enqueueVideo(pending);
    return true;
}

bool Backend::addVideo(const QUrl &url, double t) {
    if (busy())
        return false;
    if (m_videos.isEmpty())
        return load(url);

    PendingVideo pending;
    pending.url = url;
    pending.adding = true;
    pending.addAt = t;
    if (!checkedPath(url, &pending.path))
        return false;
    enqueueVideo(pending);
    return true;
}

bool Backend::addVideos(const QVariantList &urls, double t) {
    if (busy() || urls.isEmpty())
        return false;

    const int batch = ++m_nextBatch;
    int queued = 0;
    for (const QVariant &value : urls) {
        PendingVideo pending;
        pending.url = value.toUrl();
        pending.adding = true;
        pending.chain = true;
        pending.addAt = t;
        pending.batch = batch;
        if (!checkedPath(pending.url, &pending.path)) {
            ++m_batchSkipped;
            continue;
        }
        m_probeQueue.append(pending);
        ++queued;
    }
    if (queued == 0) {
        // Nothing probed, so nothing loaded: the per-file errors already
        // said why, and the batch counters start clean for the next batch.
        m_batchAdded = 0;
        m_batchSkipped = 0;
        return false;
    }
    startNextProbe();
    return true;
}

void Backend::openVideoDialog() {
    m_adding = false;
    m_filePicker->openVideo();
}

void Backend::addVideoDialog(double t) {
    m_adding = !m_videos.isEmpty();
    m_addAt = t;
    m_filePicker->openVideo();
}

void Backend::exportDialog() {
    if (m_videos.isEmpty())
        return;

    m_exportDialogClips = m_timeline.clips();
    const ffmpeg::VideoInfo &frame = m_videos.value(m_exportDialogClips.value(0).source).info;
    m_filePicker->exportVideo(suggestedExportUrl(), exportHeights(frame.width, frame.height));
}

void Backend::stepFrame(double t, int direction) {
    if (m_steppingFrame || busy() || m_timeline.duration() <= 0 || !direction)
        return;
    t = qBound(0.0, t, m_timeline.duration());
    const edit::Clips clips = m_timeline.clips();
    int index = m_timeline.clipAt(t);
    // An edge belongs to the following clip, but stepping left from it
    // should show the previous clip's last frame. The player can round
    // that edge up by half a millisecond when reporting its position.
    if (direction < 0 && index > 0 && t - m_timeline.clipStart(index) <= kFrameTolerance)
        --index;
    const edit::Clip clip = clips.at(index);
    const double clipStart = m_timeline.clipStart(index);
    const bool atClipEnd = t >= m_timeline.clipStart(index + 1);
    const Video &video = m_videos.at(clip.source);
    if (!video.frameTimes.isEmpty()) {
        const double sourceTime = atClipEnd ? clip.out : clip.in + t - clipStart;
        const double target = resolveStepTarget(video.frameTimes, clip, sourceTime, atClipEnd, direction);
        emit frameStepped(t, clipStart + target - clip.in);
        return;
    }

    m_steppingFrame = true;
    const QPointer<Backend> guard(this);
    const int source = clip.source;
    const QString path = video.info.path;
    const double start = video.info.start;
    const int thumbKey = video.thumbKey;
    QThreadPool::globalInstance()->start([guard, source, path, start, thumbKey, clips, t, direction] {
        const auto times = ffmpeg::frameTimes(path, start);
        QMetaObject::invokeMethod(QCoreApplication::instance(),
            [guard, source, thumbKey, clips, t, direction, times] {
                if (!guard)
                    return;
                guard->m_steppingFrame = false;
                if (guard->m_videos.value(source).thumbKey != thumbKey)
                    return;
                if (!times.isEmpty())
                    guard->m_videos[source].frameTimes = times;
                if (guard->m_timeline.clips() != clips)
                    return;
                if (times.isEmpty()) {
                    emit guard->frameStepFailed(tr("Could not read video frame times."));
                    return;
                }
                guard->stepFrame(t, direction);
            }, Qt::QueuedConnection);
    });
}

void Backend::saveFrame(double t) {
    if (m_savingFrame || m_videos.isEmpty() || m_timeline.duration() <= 0)
        return;
    t = qBound(0.0, t, m_timeline.duration());
    const int index = m_timeline.clipAt(t);
    const edit::Clips clips = m_timeline.clips();
    if (index < 0 || index >= clips.size())
        return;
    const edit::Clip clip = clips.at(index);
    if (clip.source < 0 || clip.source >= m_videos.size())
        return;
    const double sourceTime = clip.in + (t - m_timeline.clipStart(index));
    const QString path = QFileInfo(m_videos.at(clip.source).info.path).absoluteFilePath();
    const double start = m_videos.at(clip.source).info.start;
    const QString ffprobeBin = ffmpeg::toolPath("ffprobe");
    const QString ffmpegBin = ffmpeg::toolPath("ffmpeg");
    if (ffprobeBin.isEmpty() || ffmpegBin.isEmpty()) {
        emit frameFailed(tr("ffmpeg is not installed."));
        return;
    }

    // In the background, as a 4K frame takes long enough to stall the
    // window: first find the frame on screen, then write it.
    m_savingFrame = true;
    runSaveFrameStep(ffprobeBin, ffmpeg::shownFrameArgs(path, start + sourceTime),
                 [this, path, start, sourceTime, ffmpegBin](const QByteArray &packetTimes) {
        const ffmpeg::ShownFrame shown = ffmpeg::shownFrame(packetTimes, start + sourceTime);
        const double frameTime = shown.time - start;
        const double keyframeTime = shown.hasKeyframe ? shown.keyframe - start : -1.0;
        const QFileInfo source(path);
        // Written under a hidden name, then given the first free one, so a
        // failed or interrupted save leaves no half PNG and never replaces one.
        QTemporaryFile part(source.absolutePath() + QStringLiteral("/.omacut-frame-XXXXXX.png"));
        part.setAutoRemove(false);
        if (!part.open()) {
            m_savingFrame = false;
            emit frameFailed(part.errorString());
            return;
        }
        const QString partPath = part.fileName();
        part.close();
        m_framePart = partPath;
        runSaveFrameStep(ffmpegBin, ffmpeg::frameArgs(path, frameTime, keyframeTime, partPath),
                     [this, source, partPath](const QByteArray &) {
            m_savingFrame = false;
            m_framePart.clear();
            if (QFileInfo(partPath).size() <= 0) {
                QFile::remove(partPath);
                emit frameFailed(tr("There's no frame there."));
                return;
            }
            const QString stem = source.absolutePath() + QLatin1Char('/') + source.completeBaseName();
            QString pngPath = stem + QStringLiteral("-frame.png");
            for (int n = 2; !QFile::rename(partPath, pngPath); ++n) {
                if (!QFileInfo::exists(pngPath) || n > 1000) {
                    QFile::remove(partPath);
                    emit frameFailed(tr("Could not write %1.").arg(pngPath));
                    return;
                }
                pngPath = stem + QStringLiteral("-frame-%1.png").arg(n);
            }
            // The temporary file was private; the frame is as shareable as any.
            QFile::setPermissions(pngPath, QFile::ReadOwner | QFile::WriteOwner
                                               | QFile::ReadGroup | QFile::ReadOther);
            emit frameSaved(pngPath);
        }, partPath);
    });
}

void Backend::runSaveFrameStep(const QString &program, const QStringList &args,
                           std::function<void(const QByteArray &)> done, const QString &partPath) {
    auto *proc = new QProcess(this);
    // A stuck file (say, on a hung network mount) mustn't block F for good.
    QTimer::singleShot(30000, proc, [proc] { proc->kill(); });
    connect(proc, &QProcess::finished, this,
            [this, proc, done, partPath](int code, QProcess::ExitStatus status) {
        const QByteArray out = proc->readAllStandardOutput();
        const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
        proc->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            m_savingFrame = false;
            m_framePart.clear();
            if (!partPath.isEmpty())
                QFile::remove(partPath);
            emit frameFailed(err.isEmpty() ? tr("ffmpeg failed.") : err);
            return;
        }
        done(out);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, partPath](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        m_savingFrame = false;
        m_framePart.clear();
        if (!partPath.isEmpty())
            QFile::remove(partPath);
        proc->deleteLater();
        emit frameFailed(proc->errorString());
    });
    proc->start(program, args);
}

QList<int> Backend::exportHeights(int width, int height) {
    const int shortSide = qMin(width, height);
    QList<int> heights;
    for (const int candidate : {1080, 720}) {
        if (shortSide > candidate)
            heights << candidate;
    }
    return heights;
}

QUrl Backend::suggestedExportUrl() const {
    if (m_videos.isEmpty())
        return {};
    const QFileInfo src(m_videos.first().info.path);
    const QString target = src.dir().filePath(src.completeBaseName() + "_trimmed.mp4");
    return QUrl::fromLocalFile(target);
}

void Backend::exportClips(const QUrl &dst, const edit::Clips &clips, int scaleHeight) {
    if (m_videos.isEmpty() || busy())
        return;

    QList<ffmpeg::Segment> segments;
    // The first clip that still has its video sets the frame.
    int frameSource = -1;
    double clipLen = 0.0;
    for (const edit::Clip &clip : edit::merged(clips)) {
        if (clip.source < 0 || clip.source >= m_videos.size() || clip.out <= clip.in)
            continue;
        if (frameSource < 0)
            frameSource = clip.source;
        const ffmpeg::VideoInfo &info = m_videos[clip.source].info;
        segments.append({info.path, clip.in, clip.out, info.audio});
        clipLen += clip.length();
    }
    if (segments.isEmpty() || clipLen <= 0.0) {
        emit exportFailed(tr("The selected clip has no length."));
        return;
    }

    // Forcing the .mp4 suffix can redirect the write to a file the save
    // dialog never asked the user about overwriting — refuse rather than
    // silently replace it.
    QString selectedPath;
    if (!isLocalAbsolutePath(dst, &selectedPath)) {
        emit exportFailed(QStringLiteral("Only local files can be exported to."));
        return;
    }
    const QString outPath = mp4PathFor(selectedPath);
    if (outPath != selectedPath && QFileInfo::exists(outPath)) {
        emit exportFailed(tr("%1 already exists.")
                              .arg(QFileInfo(outPath).fileName()));
        return;
    }

    const QString ffmpegBin = ffmpeg::toolPath("ffmpeg");
    if (ffmpegBin.isEmpty()) {
        emit exportFailed(tr("`ffmpeg` was not found on your PATH."));
        return;
    }

    setBusy(true);
    setStatus(tr("Exporting %1%").arg(0));

    // Encode to a sibling temp file and atomically replace the target only after
    // success, so failed/cancelled exports preserve any existing file.
    const QString tmpPath = outPath + QStringLiteral(".omacut-part.mp4");
    QFile::remove(tmpPath);
    const ffmpeg::VideoInfo &frame = m_videos[frameSource].info;
    const QStringList args = ffmpeg::trimArgs(segments, tmpPath, frame.width, frame.height, scaleHeight);

    auto *proc = new QProcess(this);
    auto completed = std::make_shared<bool>(false);

    // ffmpeg -progress writes key=value blocks to stdout as it encodes;
    // out_time_us against the kept length gives the percentage.
    auto progressBuf = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, proc, progressBuf, clipLen, completed] {
                progressBuf->append(proc->readAllStandardOutput());
                int newline;
                while ((newline = progressBuf->indexOf('\n')) >= 0) {
                    const QByteArray line = progressBuf->left(newline).trimmed();
                    progressBuf->remove(0, newline + 1);
                    if (*completed || !line.startsWith("out_time_us="))
                        continue;
                    bool ok = false;
                    const double outSecs = line.mid(line.indexOf('=') + 1).toLongLong(&ok) / 1e6;
                    if (!ok)
                        continue;
                    const int percent = qBound(0, qRound(outSecs / clipLen * 100.0), 100);
                    setStatus(tr("Exporting %1%").arg(percent));
                }
            });

    connect(proc, &QProcess::finished, this,
            [this, proc, outPath, tmpPath, completed, clips](int code, QProcess::ExitStatus exitStatus) {
                if (*completed)
                    return;
                *completed = true;
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                proc->deleteLater();
                if (exitStatus != QProcess::NormalExit || code != 0) {
                    failExport(tmpPath, err.isEmpty() ? tr("ffmpeg trim failed.") : err);
                    return;
                }
                if (!replaceWithTemp(tmpPath, outPath)) {
                    failExport(tmpPath, tr("Could not write the exported file."));
                    return;
                }
                setStatus(QString());
                // Written over one of its own videos: the clips' times point
                // into a file that's gone, so start over from what was saved.
                // The reopen probes in the background; if it fails, the probe
                // result says the saved file couldn't be reopened.
                if (!isVideoPath(outPath)) {
                    setBusy(false);
                    m_timeline.markExported(clips);
                    emit exportDone(outPath);
                } else {
                    // exportDone waits for the reopen probe to settle. Hand off
                    // directly from exporting to probing so busy() never drops
                    // to false in between.
                    PendingVideo pending;
                    pending.url = QUrl::fromLocalFile(outPath);
                    pending.exportReload = true;
                    if (checkedPath(pending.url, &pending.path)) {
                        // Safe because exportClips() refuses while probing;
                        // enqueueVideo() immediately starts the probe and emits busyChanged().
                        m_busy = false;
                        enqueueVideo(pending);
                    } else {
                        setBusy(false);
                        emit exportDone(outPath);
                    }
                }
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, tmpPath, completed](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *completed)
                    return;
                *completed = true;
                const QString err = proc->errorString();
                proc->deleteLater();
                failExport(tmpPath, err.isEmpty() ? tr("Could not start ffmpeg.") : err);
            });
    proc->start(ffmpegBin, args);
}

void Backend::failExport(const QString &tmpPath, const QString &message) {
    setBusy(false);
    setStatus(QString());
    QFile::remove(tmpPath);
    emit exportFailed(message);
}
