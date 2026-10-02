#include "backend.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTextStream>
#include <QTemporaryFile>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"
#include "thumbworker.h"

namespace {
constexpr int kThumbCount = 12;
constexpr int kThumbRevealMs = 70;
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

QString fileIdentity(const QString &path) {
    const QFileInfo file(path);
    const QString canonical = file.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    // A new destination has no canonical file path yet, but its parent can
    // resolve symlinks so aliases still reserve the same pending output.
    const QString directory = file.dir().canonicalPath();
    return directory.isEmpty() ? QDir::cleanPath(file.absoluteFilePath())
                               : QDir(directory).filePath(file.fileName());
}

bool replaceWithTemp(const QString &tmpPath, const QString &outPath, bool overwriteAllowed) {
    // QFile::rename refuses an existing destination. Only replace a file that
    // was already present at the exact path selected in the save dialog.
    if (!overwriteAllowed)
        return QFile::rename(tmpPath, outPath);
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
    m_thumbRevealTimer.setInterval(kThumbRevealMs);
    connect(&m_thumbRevealTimer, &QTimer::timeout, this, &Backend::revealNextThumb);

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
    m_shuttingDown = true;
    if (m_exportProcess) {
        m_exportProcess->disconnect(this);
        if (m_exportProcess->state() == QProcess::Starting)
            m_exportProcess->waitForStarted(3000);
        m_exportProcess->kill();
        m_exportProcess->waitForFinished(5000);
    }
    for (const Job &job : m_exportJobs) {
        if (!job->tmpPath.isEmpty())
            QFile::remove(job->tmpPath);
    }
    stopThumbs();
}

void Backend::wireFilePicker() {
    connect(m_filePicker, &FilePicker::openSelected, this, &Backend::load);
    connect(m_filePicker, &FilePicker::exportSelected, this, [this](const QUrl &url, int scaleHeight) {
        enqueueExport(url, m_exportDialogRequest, scaleHeight);
    });
    connect(m_filePicker, &FilePicker::failed, this, &Backend::loadError);
}

void Backend::setBusy(bool busy) {
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
    emit statusChanged();
}

void Backend::setStatus(const QString &status) {
    if (m_status == status)
        return;
    m_status = status;
    emit statusChanged();
}

void Backend::setExportProgress(int percent) {
    if (m_exportProgress == percent)
        return;
    m_exportProgress = percent;
    emit exportProgressChanged();
    emit statusChanged();
}

QString Backend::status() const {
    return m_busy ? QStringLiteral("Exporting %1%").arg(qMax(0, m_exportProgress)) : m_status;
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
        if (value.size() >= 2
                && ((value.front() == QLatin1Char('"') && value.back() == QLatin1Char('"'))
                    || (value.front() == QLatin1Char('\'') && value.back() == QLatin1Char('\''))))
            value = value.mid(1, value.size() - 2);

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

bool Backend::load(const QUrl &url) {
    const QString path = url.toLocalFile();
    const ffmpeg::VideoInfo info = ffmpeg::probe(path);
    if (!info.ok) {
        emit loadError(info.error);
        return false;
    }

    m_info = info;
    m_path = path;
    m_source = url;
    ++m_sourceRevision;
    m_timeline.reset(m_info.duration);

    // New video: drop the old filmstrip and bump the revision so QML reloads.
    stopThumbs();
    m_thumbStart = 0.0;
    m_thumbLen = m_info.duration;
    m_fullThumbs = QVector<QImage>(kThumbCount);
    m_fullThumbsComplete = false;
    m_thumbCount = kThumbCount;
    m_thumbAvailableCount = 0;
    m_thumbReadyCount = 0;
    m_thumbWorkerDone = false;
    ++m_thumbRevision;
    m_provider->setImages(QVector<QImage>(kThumbCount));
    emit thumbsChanged();

    emit infoChanged();

    setStatus(QStringLiteral("Loading..."));
    startThumbs();
    return true;
}

void Backend::openVideoDialog() {
    m_filePicker->openVideo();
}

void Backend::exportDialog() {
    if (m_path.isEmpty() || !m_info.ok || m_filePicker->isPending())
        return;

    m_exportDialogRequest = currentExportRequest(m_timeline.clips());
    m_filePicker->exportVideo(suggestedExportUrl(), exportHeights(m_info.width, m_info.height));
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

void Backend::startThumbs() {
    auto *worker = new ThumbWorker(m_path, m_thumbStart, m_thumbLen, kThumbCount);
    m_thumbWorker = worker;
    // Pair the pointer check with the revision: a recycled worker address could
    // otherwise let a stale queued callback write into the new filmstrip.
    const int revision = m_thumbRevision;

    connect(worker, &ThumbWorker::thumbReady, this, [this, worker, revision](int index, const QImage &image) {
        if (worker != m_thumbWorker || revision != m_thumbRevision)
            return;
        m_provider->setImage(index, image);
        // Thumbs arrive in order, so the strip is fully cached at the last one.
        if (m_thumbStart <= 0.0 && m_thumbLen >= m_info.duration) {
            m_fullThumbs[index] = image;
            if (index == kThumbCount - 1)
                m_fullThumbsComplete = true;
        }
        m_thumbAvailableCount = qMax(m_thumbAvailableCount, index + 1);
        if (m_thumbReadyCount == 0)
            revealNextThumb();
        if (!m_thumbRevealTimer.isActive())
            m_thumbRevealTimer.start();
    });
    connect(worker, &ThumbWorker::finished, this, [this, worker, revision] {
        if (worker == m_thumbWorker && revision == m_thumbRevision) {
            m_thumbWorker = nullptr;
            m_thumbWorkerDone = true;
            if (m_thumbReadyCount >= m_thumbCount)
                setStatus(QString());
            else if (!m_thumbRevealTimer.isActive())
                m_thumbRevealTimer.start();
        }
        worker->deleteLater();
    });
    worker->start();
}

void Backend::revealNextThumb() {
    if (m_thumbReadyCount < m_thumbAvailableCount) {
        ++m_thumbReadyCount;
        emit thumbsChanged();
    }

    if (m_thumbReadyCount < m_thumbAvailableCount)
        return;

    m_thumbRevealTimer.stop();
    if (m_thumbWorkerDone && m_thumbReadyCount >= m_thumbCount)
        setStatus(QString());
}

void Backend::stopThumbs() {
    m_thumbRevealTimer.stop();
    if (!m_thumbWorker)
        return;

    ThumbWorker *worker = m_thumbWorker;
    m_thumbWorker = nullptr;
    worker->disconnect(this);
    worker->requestStop();
    worker->wait();
    delete worker;
}

void Backend::requestThumbs(double start, double end) {
    if (m_path.isEmpty() || !m_info.ok)
        return;
    start = qBound(0.0, start, m_info.duration);
    end = qBound(start, end, m_info.duration);
    if (end - start <= 0.0 || (start == m_thumbStart && end - start == m_thumbLen))
        return;

    stopThumbs();
    m_thumbStart = start;
    m_thumbLen = end - start;
    ++m_thumbRevision;

    // Zooming back out: restore the cached full-length strip instantly.
    if (start <= 0.0 && end >= m_info.duration && m_fullThumbsComplete) {
        m_provider->setImages(m_fullThumbs);
        m_thumbAvailableCount = kThumbCount;
        m_thumbReadyCount = kThumbCount;
        m_thumbWorkerDone = true;
        emit thumbsChanged();
        return;
    }

    m_thumbAvailableCount = 0;
    m_thumbReadyCount = 0;
    m_thumbWorkerDone = false;
    m_provider->setImages(QVector<QImage>(kThumbCount));
    emit thumbsChanged();
    startThumbs();
}

QUrl Backend::suggestedExportUrl() const {
    if (m_path.isEmpty())
        return {};
    const QFileInfo src(m_path);
    const QString target = src.dir().filePath(src.completeBaseName() + "_trimmed.mp4");
    return QUrl::fromLocalFile(target);
}

Backend::ExportRequest Backend::currentExportRequest(const edit::Clips &clips) const {
    return {m_path, m_info, clips, m_sourceRevision};
}

QVariantList Backend::exportJobs() const {
    QVariantList result;
    for (const Job &job : m_exportJobs) {
        result.append(QVariantMap{{"id", job->id}, {"path", job->outPath},
                                 {"state", job->state}, {"progress", job->progress},
                                 {"error", job->error}});
    }
    return result;
}

void Backend::exportClips(const QUrl &dst, const edit::Clips &clips, int scaleHeight) {
    enqueueExport(dst, currentExportRequest(clips), scaleHeight);
}

void Backend::enqueueExport(const QUrl &dst, const ExportRequest &request, int scaleHeight) {
    if (m_shuttingDown || request.sourcePath.isEmpty() || !request.info.ok)
        return;
    if (edit::keptDuration(request.clips) <= 0.0) {
        emit exportFailed(QStringLiteral("The selected clip has no length."));
        return;
    }
    if (!dst.isLocalFile() || dst.toLocalFile().isEmpty()) {
        emit exportFailed(QStringLiteral("Choose a local export destination."));
        return;
    }

    // Do not silently overwrite a different path after forcing the MP4 suffix.
    const QString selectedPath = QFileInfo(dst.toLocalFile()).absoluteFilePath();
    const QString outPath = mp4PathFor(selectedPath);
    if (outPath != selectedPath && QFileInfo::exists(outPath)) {
        emit exportFailed(QStringLiteral("%1 already exists.").arg(QFileInfo(outPath).fileName()));
        return;
    }
    const auto sameFile = [](const QString &a, const QString &b) {
        return fileIdentity(a) == fileIdentity(b);
    };
    for (const Job &job : m_exportJobs) {
        if (job->state != "queued" && job->state != "running" && job->state != "cancelling")
            continue;
        // A job must not overwrite a file another pending job is reading.
        if (sameFile(outPath, job->outPath)
                || sameFile(outPath, job->request.sourcePath)
                || sameFile(request.sourcePath, job->outPath)) {
            emit exportFailed(QStringLiteral("This file is already used by a pending export. Choose another destination or wait for it to finish."));
            return;
        }
    }
    if (ffmpeg::toolPath("ffmpeg").isEmpty()) {
        emit exportFailed(QStringLiteral("`ffmpeg` was not found on your PATH."));
        return;
    }

    auto job = std::make_shared<ExportJob>();
    job->id = m_nextExportId++;
    job->request = request;
    job->outPath = outPath;
    job->overwriteAllowed = outPath == selectedPath && QFileInfo::exists(outPath);
    job->scaleHeight = scaleHeight;
    m_exportJobs.append(job);
    refreshExportState();
    startNextExport();
}

void Backend::refreshExportState() {
    bool pending = false;
    for (const Job &job : m_exportJobs)
        pending |= job->state == "queued" || job->state == "running" || job->state == "cancelling";
    setExportProgress(m_activeExport ? m_activeExport->progress : -1);
    setBusy(pending);
    emit exportJobsChanged();
}

void Backend::startNextExport() {
    if (m_shuttingDown || m_activeExport)
        return;
    Job job;
    for (const Job &candidate : m_exportJobs) {
        if (candidate->state == "queued") {
            job = candidate;
            break;
        }
    }
    if (!job) {
        refreshExportState();
        return;
    }
    m_activeExport = job;
    job->state = QStringLiteral("running");
    refreshExportState();

    // Unique sibling temp files allow separate Omacut windows to export safely.
    QTemporaryFile temp(job->outPath + QStringLiteral(".omacut-XXXXXX.mp4"));
    if (!temp.open()) {
        finishExport(job, QStringLiteral("failed"), QStringLiteral("Could not create the export file: ") + temp.errorString());
        return;
    }
    temp.setAutoRemove(false);
    job->tmpPath = temp.fileName();
    temp.close();

    auto *proc = new QProcess(this);
    m_exportProcess = proc;
    connect(proc, &QProcess::readyReadStandardError, this, [proc, job] {
        job->errorBuffer.append(proc->readAllStandardError());
        // Keep useful diagnostics without buffering an unbounded ffmpeg log.
        job->errorBuffer = job->errorBuffer.right(16384);
    });
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, job] {
        job->progressBuffer.append(proc->readAllStandardOutput());
        int newline;
        while ((newline = job->progressBuffer.indexOf('\n')) >= 0) {
            const QByteArray line = job->progressBuffer.left(newline).trimmed();
            job->progressBuffer.remove(0, newline + 1);
            if (job != m_activeExport || job->state != "running" || !line.startsWith("out_time_us="))
                continue;
            bool ok = false;
            const double seconds = line.mid(12).toLongLong(&ok) / 1e6;
            if (!ok)
                continue;
            job->progress = qMax(job->progress, qBound(0, qRound(seconds / edit::keptDuration(job->request.clips) * 100.0), 100));
            refreshExportState();
        }
    });
    connect(proc, &QProcess::finished, this, [this, proc, job](int code, QProcess::ExitStatus exitStatus) {
        if (job != m_activeExport)
            return;
        job->errorBuffer.append(proc->readAllStandardError());
        if (job->state == "cancelling") {
            finishExport(job, QStringLiteral("cancelled"));
        } else if (exitStatus != QProcess::NormalExit || code != 0) {
            const QString error = QString::fromUtf8(job->errorBuffer).trimmed();
            finishExport(job, QStringLiteral("failed"), error.isEmpty() ? QStringLiteral("ffmpeg trim failed.") : error);
        } else if (!replaceWithTemp(job->tmpPath, job->outPath, job->overwriteAllowed)) {
            finishExport(job, QStringLiteral("failed"),
                         !job->overwriteAllowed && QFileInfo::exists(job->outPath)
                         ? QStringLiteral("The destination was created after this export was queued. Choose another destination.")
                         : QStringLiteral("Could not write the exported file."));
        } else {
            finishExport(job, QStringLiteral("done"));
        }
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, job](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && job == m_activeExport)
            finishExport(job, job->state == "cancelling" ? QStringLiteral("cancelled") : QStringLiteral("failed"), proc->errorString());
    });
    connect(proc, &QProcess::started, this, [proc, job] {
        if (job->state == "cancelling")
            proc->kill();
    });
    proc->start(ffmpeg::toolPath("ffmpeg"), ffmpeg::trimArgs(job->request.sourcePath, job->tmpPath,
                edit::kept(job->request.clips), job->request.info.audio, job->scaleHeight));
}

void Backend::finishExport(const Job &job, const QString &state, const QString &error) {
    if (job != m_activeExport)
        return;
    if (m_exportProcess) {
        m_exportProcess->disconnect(this);
        m_exportProcess->deleteLater();
        m_exportProcess = nullptr;
    }
    if (!job->tmpPath.isEmpty())
        QFile::remove(job->tmpPath);
    job->tmpPath.clear();
    job->state = state;
    job->error = error;
    if (state == "done") {
        job->progress = 100;
        setExportProgress(100);
        // Loading even the same source again creates a different edit session.
        if (job->request.sourceRevision == m_sourceRevision)
            m_timeline.markExported(job->request.clips);
    }
    m_activeExport.reset();
    refreshExportState();
    if (state == "done")
        emit exportDone(job->outPath);
    else if (state == "failed")
        emit exportFailed(QFileInfo(job->outPath).fileName() + QStringLiteral(": ") + error);
    QTimer::singleShot(0, this, &Backend::startNextExport);
}

void Backend::cancelExport(int id) {
    for (const Job &job : m_exportJobs) {
        if (job->id != id)
            continue;
        if (job->state == "queued") {
            job->state = QStringLiteral("cancelled");
            refreshExportState();
        } else if (job == m_activeExport && job->state == "running") {
            job->state = QStringLiteral("cancelling");
            // QProcess::kill is asynchronous; keep ownership until it exits.
            if (m_exportProcess)
                m_exportProcess->kill();
            refreshExportState();
        }
        return;
    }
}

void Backend::clearFinishedExports() {
    m_exportJobs.erase(std::remove_if(m_exportJobs.begin(), m_exportJobs.end(), [](const Job &job) {
        return job->state == "done" || job->state == "failed" || job->state == "cancelled";
    }), m_exportJobs.end());
    emit exportJobsChanged();
}
