#include "backend.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>

#include <cstdio>
#include <functional>
#include <memory>
#include <utility>

#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"

namespace {
const QString kDefaultAccent = QStringLiteral("#FFD60A");

QString omarchyCurrentDir() {
    return QDir::homePath() + QStringLiteral("/.local/state/omarchy/current");
}

QString omarchyColorsPath() {
    return omarchyCurrentDir() + QStringLiteral("/theme/colors.toml");
}

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
}

Backend::Backend(ThumbProvider *provider, QObject *parent)
    : Backend(provider, new PortalFilePicker(), parent) {}

Backend::Backend(ThumbProvider *provider, FilePicker *filePicker, QObject *parent)
    : QObject(parent), m_provider(provider), m_filePicker(filePicker),
      m_themeAccent(kDefaultAccent) {
    if (!m_filePicker->parent())
        m_filePicker->setParent(this);
    wireFilePicker();

    // Follow omarchy theme switches live. The theme lives behind a symlink that
    // gets swapped, so the reload also re-arms the watch paths every time.
    const auto themeChanged = [this] {
        watchTheme();
        loadThemeAccent();
    };
    connect(&m_themeWatcher, &QFileSystemWatcher::directoryChanged, this, themeChanged);
    connect(&m_themeWatcher, &QFileSystemWatcher::fileChanged, this, themeChanged);
    watchTheme();
    loadThemeAccent();
}

Backend::~Backend() {
    // Quit mid-save: the ffmpeg writing it dies with us, so its file goes too.
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

QString Backend::accentFromColorsFile(const QString &path, const QString &fallback) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return fallback;

    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;

        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0 || line.left(equals).trimmed() != QStringLiteral("accent"))
            continue;

        QString value = line.mid(equals + 1).trimmed();

        // A value can carry a trailing comment. Inside quotes the '#' belongs
        // to the string; outside them it starts one -- except the leading '#'
        // of a bare hex colour, which is the value itself.
        if (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))) {
            const QChar quote = value.front();
            const int closing = value.indexOf(quote, 1);
            value = closing > 0 ? value.mid(1, closing - 1) : value.mid(1);
        } else {
            const int comment =
                value.indexOf(QLatin1Char('#'), value.startsWith(QLatin1Char('#')) ? 1 : 0);
            if (comment >= 0)
                value = value.left(comment).trimmed();
        }

        return QColor::fromString(value).isValid() ? value : fallback;
    }
    return fallback;
}

QString Backend::foregroundFor(const QString &color) {
    const QColor parsed = QColor::fromString(color);
    if (!parsed.isValid())
        return QStringLiteral("black");
    const double luminance = 0.299 * parsed.redF()
        + 0.587 * parsed.greenF() + 0.114 * parsed.blueF();
    return luminance < 0.5 ? QStringLiteral("white") : QStringLiteral("black");
}

QString Backend::themeAccentForeground() const {
    return foregroundFor(m_themeAccent);
}

void Backend::loadThemeAccent() {
    const QString accent = accentFromColorsFile(omarchyColorsPath(), kDefaultAccent);
    if (accent == m_themeAccent)
        return;
    m_themeAccent = accent;
    emit themeAccentChanged();
}

void Backend::watchTheme() {
    const QStringList watched = m_themeWatcher.files() + m_themeWatcher.directories();
    if (!watched.isEmpty())
        m_themeWatcher.removePaths(watched);

    const QString currentDir = omarchyCurrentDir();
    const QString themeDir = currentDir + QStringLiteral("/theme");
    if (QDir(currentDir).exists())
        m_themeWatcher.addPath(currentDir);
    if (QDir(themeDir).exists())
        m_themeWatcher.addPath(themeDir);
    if (QFileInfo::exists(omarchyColorsPath()))
        m_themeWatcher.addPath(omarchyColorsPath());
}

bool Backend::isVideoPath(const QString &path) const {
    const QString target = QFileInfo(path).canonicalFilePath();
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
    // Only absolute local paths reach ffprobe: "file:-report" would be read
    // as an option, and "file:http://..." as a network address.
    *path = url.toLocalFile();
    if (!url.isLocalFile() || !QDir::isAbsolutePath(*path)) {
        emit loadError(QStringLiteral("Only local files can be opened."));
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
        if (pending.exportReload)
            emit exportDone(pending.path);
        emit loadError(pending.exportReload
            ? QStringLiteral("Saved, but couldn't reopen it, so the clips no longer match the file.")
            : info.error);
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
        emit frameFailed(QStringLiteral("ffmpeg is not installed."));
        return;
    }

    // In the background, as a 4K frame takes long enough to stall the
    // window: first find the frame on screen, then write it.
    m_savingFrame = true;
    runFrameStep(ffprobeBin, ffmpeg::shownFrameArgs(path, start + sourceTime),
                 [this, path, start, sourceTime, ffmpegBin](const QByteArray &packetTimes) {
        const ffmpeg::ShownFrame shown = ffmpeg::shownFrame(packetTimes, start + sourceTime);
        const double frameTime = shown.time - start;
        const double keyframeTime = shown.keyframe >= 0.0 ? shown.keyframe - start : -1.0;
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
        runFrameStep(ffmpegBin, ffmpeg::frameArgs(path, frameTime, keyframeTime, partPath),
                     [this, source, partPath](const QByteArray &) {
            m_savingFrame = false;
            m_framePart.clear();
            if (QFileInfo(partPath).size() <= 0) {
                QFile::remove(partPath);
                emit frameFailed(QStringLiteral("There's no frame there."));
                return;
            }
            const QString stem = source.absolutePath() + QLatin1Char('/') + source.completeBaseName();
            QString pngPath = stem + QStringLiteral("-frame.png");
            for (int n = 2; !QFile::rename(partPath, pngPath); ++n) {
                if (!QFileInfo::exists(pngPath) || n > 1000) {
                    QFile::remove(partPath);
                    emit frameFailed(QStringLiteral("Could not write %1.").arg(pngPath));
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

void Backend::runFrameStep(const QString &program, const QStringList &args,
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
            emit frameFailed(err.isEmpty() ? QStringLiteral("ffmpeg failed.") : err);
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
    for (const edit::Clip &clip : edit::merged(clips)) {
        if (clip.source < 0 || clip.source >= m_videos.size())
            continue;
        if (frameSource < 0)
            frameSource = clip.source;
        const ffmpeg::VideoInfo &info = m_videos[clip.source].info;
        segments.append({info.path, clip.in, clip.out, info.audio, clip.speed});
    }
    const double clipLen = edit::duration(clips);
    if (segments.isEmpty() || clipLen <= 0.0) {
        emit exportFailed("The selected clip has no length.");
        return;
    }

    // Forcing the .mp4 suffix can redirect the write to a file the save
    // dialog never asked the user about overwriting — refuse rather than
    // silently replace it.
    const QString selectedPath = dst.toLocalFile();
    const QString outPath = mp4PathFor(selectedPath);
    if (outPath != selectedPath && QFileInfo::exists(outPath)) {
        emit exportFailed(QStringLiteral("%1 already exists.")
                              .arg(QFileInfo(outPath).fileName()));
        return;
    }

    const QString ffmpegBin = ffmpeg::toolPath("ffmpeg");
    if (ffmpegBin.isEmpty()) {
        emit exportFailed("`ffmpeg` was not found on your PATH.");
        return;
    }

    setBusy(true);
    setStatus(QStringLiteral("Exporting 0%"));

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
                    setStatus(QStringLiteral("Exporting %1%").arg(percent));
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
                    failExport(tmpPath, err.isEmpty() ? QStringLiteral("ffmpeg trim failed.") : err);
                    return;
                }
                if (!replaceWithTemp(tmpPath, outPath)) {
                    failExport(tmpPath, QStringLiteral("Could not write the exported file."));
                    return;
                }
                setBusy(false);
                setStatus(QString());
                // Written over one of its own videos: the clips' times point
                // into a file that's gone, so start over from what was saved.
                // The reopen probes in the background; if it fails, the probe
                // result says the saved file couldn't be reopened.
                if (!isVideoPath(outPath)) {
                    m_timeline.markExported(clips);
                    emit exportDone(outPath);
                } else {
                    // exportDone waits for the reopen probe to settle.
                    PendingVideo pending;
                    pending.url = QUrl::fromLocalFile(outPath);
                    pending.exportReload = true;
                    if (checkedPath(pending.url, &pending.path))
                        enqueueVideo(pending);
                    else
                        emit exportDone(outPath);
                }
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, tmpPath, completed](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *completed)
                    return;
                *completed = true;
                const QString err = proc->errorString();
                proc->deleteLater();
                failExport(tmpPath, err.isEmpty() ? QStringLiteral("Could not start ffmpeg.") : err);
            });
    proc->start(ffmpegBin, args);
}

void Backend::failExport(const QString &tmpPath, const QString &message) {
    setBusy(false);
    setStatus(QString());
    QFile::remove(tmpPath);
    emit exportFailed(message);
}
