#include "backend.h"

#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>
#include <memory>

#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"
#include "thumbworker.h"
#include "ytdlp.h"

namespace {
constexpr int kThumbCount = 12;
constexpr int kThumbRevealMs = 70;
// Trimming is a length edit, not a mastering job, so a link is fetched at a
// sane size rather than at whatever 4K the source happens to offer.
constexpr int kMaxDownloadHeight = 1080;
// How long a killed download gets to die before the app stops waiting on it.
constexpr int kDownloadStopMs = 2000;
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

// Downloads are re-fetchable, so they belong in the cache rather than in the
// user's own folders.
QString downloadDir() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        return {};
    const QString dir = base + QStringLiteral("/downloads");
    return QDir().mkpath(dir) ? dir : QString();
}

// Somewhere the user will actually look for a finished clip.
QString videosDir() {
    for (const QStandardPaths::StandardLocation location : {QStandardPaths::MoviesLocation,
                                                            QStandardPaths::DownloadLocation,
                                                            QStandardPaths::HomeLocation}) {
        const QString dir = QStandardPaths::writableLocation(location);
        if (!dir.isEmpty() && QFileInfo(dir).isWritable())
            return dir;
    }
    return QDir::homePath();
}

QString pathFromFile(const QString &pathFile) {
    QFile file(pathFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QString last;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty())
            last = line;
    }
    return last;
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
    stopDownload();
    stopThumbs();
}

void Backend::wireFilePicker() {
    connect(m_filePicker, &FilePicker::openSelected, this, &Backend::load);
    connect(m_filePicker, &FilePicker::exportSelected, this, &Backend::exportClip);
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
    return loadPath(url.toLocalFile(), false);
}

bool Backend::loadPath(const QString &path, bool fromLink) {
    // Opening a file by hand abandons whatever a link was still fetching.
    if (!fromLink)
        stopDownload();

    const ffmpeg::VideoInfo info = ffmpeg::probe(path);
    if (!info.ok) {
        emit loadError(info.error);
        return false;
    }

    m_info = info;
    m_path = path;
    m_source = QUrl::fromLocalFile(path);
    m_sourceIsDownload = fromLink;

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

void Backend::openClipboardLink() {
    const QString text = QGuiApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) {
        emit loadError(QStringLiteral("The clipboard is empty. Copy a video link first."));
        return;
    }
    openLink(text);
}

void Backend::openLink(const QString &text) {
    const QString link = text.trimmed();
    if (!ytdlp::isLink(link)) {
        emit loadError(QStringLiteral("That isn't a video link."));
        return;
    }
    // An export in flight owns the status line and the ffmpeg it's driving.
    if (m_busy)
        return;

    startDownload(link);
}

void Backend::openVideoDialog() {
    m_filePicker->openVideo();
}

void Backend::exportDialog(double start, double end) {
    if (m_path.isEmpty() || !m_info.ok)
        return;

    m_filePicker->exportVideo(suggestedExportUrl(), start, end,
                              exportHeights(m_info.width, m_info.height));
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

void Backend::startDownload(const QString &url) {
    const QString tool = ytdlp::toolPath();
    if (tool.isEmpty()) {
        emit loadError(QStringLiteral("`yt-dlp` was not found on your PATH. "
                                      "Install yt-dlp to open links."));
        return;
    }

    const QString dir = downloadDir();
    if (dir.isEmpty()) {
        emit loadError(QStringLiteral("Could not create a folder to download into."));
        return;
    }

    stopDownload();

    // yt-dlp writes the finished file's path here. A leftover from an earlier
    // fetch would otherwise be read back as this download's result.
    const QString pathFile = dir + QStringLiteral("/.omacut-filepath");
    QFile::remove(pathFile);

    setBusy(true);
    setStatus(QStringLiteral("Fetching..."));

    auto *proc = new QProcess(this);
    m_download = proc;
    auto completed = std::make_shared<bool>(false);
    auto progressBuf = std::make_shared<QByteArray>();

    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, progressBuf] {
        progressBuf->append(proc->readAllStandardOutput());
        int newline;
        while ((newline = progressBuf->indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(progressBuf->left(newline)).trimmed();
            progressBuf->remove(0, newline + 1);

            const double percent = ytdlp::percentFromProgress(line);
            if (percent >= 0.0)
                setStatus(QStringLiteral("Downloading %1%").arg(qRound(percent)));
            else if (line.startsWith(QLatin1String("[Merger]")))
                setStatus(QStringLiteral("Merging..."));
        }
    });

    connect(proc, &QProcess::finished, this,
            [this, proc, pathFile, completed](int code, QProcess::ExitStatus exitStatus) {
                if (*completed)
                    return;
                *completed = true;
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
                m_download = nullptr;
                proc->deleteLater();
                setBusy(false);
                setStatus(QString());

                if (exitStatus != QProcess::NormalExit || code != 0) {
                    emit loadError(ytdlp::errorFrom(err));
                    return;
                }

                const QString path = pathFromFile(pathFile);
                if (path.isEmpty()) {
                    emit loadError(QStringLiteral("yt-dlp did not report a downloaded file."));
                    return;
                }
                loadPath(path, true);
            });

    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, completed](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || *completed)
                    return;
                *completed = true;
                const QString err = proc->errorString();
                m_download = nullptr;
                proc->deleteLater();
                setBusy(false);
                setStatus(QString());
                emit loadError(err.isEmpty() ? QStringLiteral("Could not start yt-dlp.") : err);
            });

    proc->start(tool, ytdlp::downloadArgs(url, dir, pathFile, kMaxDownloadHeight));
}

void Backend::stopDownload() {
    if (!m_download)
        return;

    QProcess *proc = m_download;
    m_download = nullptr;
    // Drop the handlers first: killing the child fires finished(), which would
    // otherwise report the abandoned download as a failure.
    proc->disconnect(this);
    proc->kill();
    proc->waitForFinished(kDownloadStopMs);
    proc->deleteLater();
    setBusy(false);
    setStatus(QString());
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
    return suggestedExportUrlFor(m_path, m_sourceIsDownload, videosDir());
}

QUrl Backend::suggestedExportUrlFor(const QString &sourcePath, bool sourceIsDownload,
                                    const QString &downloadsTarget) {
    if (sourcePath.isEmpty())
        return {};
    const QFileInfo src(sourcePath);
    const QDir dir = sourceIsDownload ? QDir(downloadsTarget) : src.dir();
    return QUrl::fromLocalFile(dir.filePath(src.completeBaseName() + "_trimmed.mp4"));
}

void Backend::exportClip(const QUrl &dst, double start, double end, int scaleHeight) {
    if (m_path.isEmpty() || !m_info.ok || m_busy)
        return;

    if (end - start <= 0.0) {
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
    const QStringList args = ffmpeg::trimArgs(m_path, tmpPath, start, end, scaleHeight);

    auto *proc = new QProcess(this);
    auto completed = std::make_shared<bool>(false);

    // ffmpeg -progress writes key=value blocks to stdout as it encodes;
    // out_time_us against the clip length gives the percentage.
    const double clipLen = end - start;
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
            [this, proc, outPath, tmpPath, completed](int code, QProcess::ExitStatus exitStatus) {
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
                emit exportDone(outPath);
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
