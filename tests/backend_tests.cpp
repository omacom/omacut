#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <unistd.h>

#include "backend.h"
#include "filepicker.h"
#include "thumbprovider.h"
#include "thumbworker.h"

class FakeFilePicker : public FilePicker {
    Q_OBJECT

public:
    int openCount = 0;
    int exportCount = 0;
    QUrl lastSuggestedUrl;
    double lastStart = 0;
    double lastEnd = 0;
    QList<int> lastScaleHeights;

    void openVideo() override { ++openCount; }

    void exportVideo(const QUrl &suggestedUrl, double start, double end,
                     const QList<int> &scaleHeights) override {
        ++exportCount;
        lastSuggestedUrl = suggestedUrl;
        lastStart = start;
        lastEnd = end;
        lastScaleHeights = scaleHeights;
    }
};

class EnvVarGuard {
public:
    explicit EnvVarGuard(const char *name)
        : m_name(name), m_oldValue(qgetenv(name)), m_hadValue(qEnvironmentVariableIsSet(name)) {}

    ~EnvVarGuard() {
        if (m_hadValue)
            qputenv(m_name.constData(), m_oldValue);
        else
            qunsetenv(m_name.constData());
    }

private:
    QByteArray m_name;
    QByteArray m_oldValue;
    bool m_hadValue;
};

class ShortcutBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    Q_PROPERTY(double duration READ duration NOTIFY infoChanged)
    Q_PROPERTY(int thumbCount READ thumbCount NOTIFY thumbsChanged)
    Q_PROPERTY(int thumbReadyCount READ thumbReadyCount NOTIFY thumbsChanged)
    Q_PROPERTY(int thumbRevision READ thumbRevision NOTIFY thumbsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)

public:
    explicit ShortcutBackend(QUrl source, double duration, QObject *parent = nullptr)
        : QObject(parent), m_source(std::move(source)), m_duration(duration) {}

    QUrl source() const { return m_source; }
    double duration() const { return m_duration; }
    int thumbCount() const { return 0; }
    int thumbReadyCount() const { return 0; }
    int thumbRevision() const { return 0; }
    bool busy() const { return m_busy; }
    QString status() const { return {}; }
    QString themeAccent() const { return QStringLiteral("#FFD60A"); }
    QString themeAccentForeground() const { return QStringLiteral("black"); }

    Q_INVOKABLE bool load(const QUrl &) { return false; }
    Q_INVOKABLE void openVideoDialog() { ++openCount; }
    Q_INVOKABLE void exportDialog(double start, double end) {
        ++exportCount;
        lastStart = start;
        lastEnd = end;
    }
    Q_INVOKABLE QUrl suggestedExportUrl() const { return {}; }
    Q_INVOKABLE void exportClip(const QUrl &, double, double) {}
    Q_INVOKABLE void requestThumbs(double start, double end) {
        ++thumbRequestCount;
        lastThumbStart = start;
        lastThumbEnd = end;
    }
    Q_INVOKABLE void overwriteOriginal(double start, double end) {
        ++overwriteCount;
        lastOverwriteStart = start;
        lastOverwriteEnd = end;
    }
    Q_INVOKABLE QStringList overwriteDrops() const { return drops; }
    Q_INVOKABLE QString overwriteTargetName() const {
        const QFileInfo file(m_source.toLocalFile());
        if (sourceIsMp4())
            return file.fileName();
        // Mirror the sibling resolution: non-MP4 sources name the .mp4 that
        // would be written next to them.
        const QString base = file.completeBaseName().isEmpty()
            ? file.fileName()
            : file.completeBaseName();
        return base + QStringLiteral(".mp4");
    }
    Q_INVOKABLE bool sourceIsMp4() const {
        return m_source.toLocalFile().endsWith(QStringLiteral(".mp4"),
                                               Qt::CaseInsensitive);
    }
    Q_INVOKABLE bool sourceChangedOnDisk() const { return changedOnDisk; }

    void announceInfo() { emit infoChanged(); }
    void announceExportDone() { emit exportDone(QStringLiteral("/tmp/exported.mp4")); }
    void announceOverwriteDone() {
        emit overwriteDone(QStringLiteral("/tmp/overwritten.mp4"), QString());
    }
    void setSource(const QUrl &url) { m_source = url; }
    void setBusy(bool busy) {
        if (m_busy == busy)
            return;
        m_busy = busy;
        emit busyChanged();
    }

    int openCount = 0;
    int exportCount = 0;
    double lastStart = 0;
    double lastEnd = 0;
    int thumbRequestCount = 0;
    double lastThumbStart = 0;
    double lastThumbEnd = 0;
    int overwriteCount = 0;
    double lastOverwriteStart = 0;
    double lastOverwriteEnd = 0;
    QStringList drops;
    bool changedOnDisk = false;

signals:
    void infoChanged();
    void thumbsChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);
    void overwriteDone(const QString &path, const QString &trashError);

private:
    QUrl m_source;
    double m_duration;
    bool m_busy = false;
};

// Finds a DialogButton by its label ("primary" tells them apart from Labels).
// Must be visible — several overlays carry a "Cancel" button, and an
// invisible one would click through to whatever is underneath.
static QQuickItem *dialogButton(QQuickWindow *window, const QString &text) {
    const auto items = window->findChildren<QQuickItem *>();
    for (QQuickItem *item : items) {
        if (item->isVisible() && item->property("primary").isValid()
                && item->property("text").toString() == text)
            return item;
    }
    return nullptr;
}

static QPoint itemCenter(QQuickItem *item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

static QString mainQmlPath() {
    return QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath(
        QStringLiteral("../src/Main.qml"));
}

// Loads Main.qml against a stub backend and keeps the engine alive for as long
// as the window is in use, so each shortcut test is just the key presses.
class QmlHarness {
public:
    explicit QmlHarness(ShortcutBackend &backend) {
        m_engine.addImageProvider(QStringLiteral("thumbs"), new ThumbProvider);
        m_engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        m_engine.load(QUrl::fromLocalFile(mainQmlPath()));
        if (!m_engine.rootObjects().isEmpty())
            m_window = qobject_cast<QQuickWindow *>(m_engine.rootObjects().first());
    }

    QQuickWindow *window() const { return m_window; }
    QQmlApplicationEngine &engine() { return m_engine; }
    QQuickItem *trimBar() const {
        return m_window ? m_window->findChild<QQuickItem *>(QStringLiteral("trimBar")) : nullptr;
    }

private:
    QQmlApplicationEngine m_engine;
    QQuickWindow *m_window = nullptr;
};

class BackendTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void openDialogDelegatesToFilePicker();
    void pickerSelectionLoadsVideo();
    void thumbnailSlotsAreExposedImmediately();
    void thumbProviderUsesRevisionPrefixedIds();
    void thumbProviderScalesHeightOnlyRequests();
    void thumbnailWorkerStopsBlockedJobs();
    void exportDialogDelegatesSuggestedUrlAndRange();
    void suggestedExportUrlAlwaysUsesMp4();
    void exportClipWritesMp4();
    void exportClipCanReplaceSourceFile();
    void exportZeroLengthClipFails();
    void exportRefusesRewrittenPathOverExistingFile();
    void exportStartFailureClearsBusy();
    void failedExportPreservesExistingFile();
    void qmlDoesNotCreateAudioOutputWithoutVideo();
    void qmlShortcutsTriggerBackendActions();
    void qmlArrowKeysMoveThePlayhead();
    void qmlSpaceChordsSetTheTrimEdges();
    void qmlZoomFocusesTheSelection();
    void qmlQuitConfirmsUnexportedTrim();
    void overwriteArgsMapsAllStreamsByOutputIndex();
    void overwriteOriginalReplacesMp4InPlace();
    void qmlSavePromptOffersOverwriteOrExport();
    void overwriteDropsNamesUnmappableStreams();
    void qmlDropWarnMustBeConfirmed();
    void overwritePreservesStreamsEndToEnd();
    void nextFreeMp4SiblingAutoNumbers();
    void sourceChangedOnDiskDetectsModification();
    void overwriteNonMp4WritesSiblingAndTrashes();
    void secondOverwriteAfterNonMp4IsInPlace();
    void overwriteWarnsOnChangedSource();
    void qmlNonMp4SavePromptOffersTrashWrite();
    void trashFailureStillLoadsSibling();
    void busyBlocksLoadAndOpenDuringEncode();
    void qmlBusyBlocksOpenAndQuit();
    void qmlBusySaveShowsNotice();
    void overwriteReloadFailureKeepsErrorVisible();
    void failedOverwriteLeavesSourceUntouched();
    void overwriteKeepsSourcePermissions();
    void tempPathForNamesAreUniqueAndSibling();
    void overwriteZeroLengthRefused();
    void trimArgsReencodeForPreciseCuts();
    void trimArgsScaleTheShorterSide();
    void exportHeightsNeverUpscale();
    void themeAccentReadsOmarchyColors();
    void themeAccentForegroundKeepsContrast();

private:
    QUrl videoUrl() const { return QUrl::fromLocalFile(m_videoPath); }
    QString formatName(const QString &path) const;
    QString formatTag(const QString &path, const QString &key) const;
    QList<QPair<double, double>> chapterRanges(const QString &path) const;
    void waitForBackgroundWork(Backend &backend);
    bool installBrokenFfmpeg(const QString &dirPath);

    QTemporaryDir m_dir;
    QString m_videoPath;
    QString m_longPath;
    QString m_multiPath;
    QString m_multiMkvPath;
};

void BackendTests::initTestCase() {
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QVERIFY2(m_dir.isValid(), "temporary directory is valid");
    m_videoPath = m_dir.filePath(QStringLiteral("clip.mp4"));

    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY2(!ffmpeg.isEmpty(), "ffmpeg is available");

    QProcess proc;
    proc.start(ffmpeg, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-f"),
        QStringLiteral("lavfi"),
        QStringLiteral("-i"),
        QStringLiteral("testsrc=size=32x32:rate=1:duration=1"),
        QStringLiteral("-pix_fmt"),
        QStringLiteral("yuv420p"),
        QStringLiteral("-y"),
        m_videoPath,
    });
    QVERIFY2(proc.waitForFinished(10000), qPrintable(QString::fromUtf8(proc.readAll())));
    QCOMPARE(proc.exitStatus(), QProcess::NormalExit);
    QCOMPARE(proc.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_videoPath));

    // A multi-frame clip for overwrite tests — trimming a 1-frame fixture
    // can't change its duration, so nothing would prove the reload worked.
    m_longPath = m_dir.filePath(QStringLiteral("clip4s.mp4"));
    QProcess longProc;
    longProc.start(ffmpeg, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-f"),
        QStringLiteral("lavfi"),
        QStringLiteral("-i"),
        QStringLiteral("testsrc=size=32x32:rate=10:duration=4"),
        QStringLiteral("-pix_fmt"),
        QStringLiteral("yuv420p"),
        QStringLiteral("-y"), m_longPath,
    });
    QVERIFY2(longProc.waitForFinished(10000),
             qPrintable(QString::fromUtf8(longProc.readAll())));
    QCOMPARE(longProc.exitStatus(), QProcess::NormalExit);
    QCOMPARE(longProc.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_longPath));

    // A stream-rich MP4: h264 video + two aac audio + mov_text subtitle +
    // chapters + a format title — exercises every overwrite codec policy.
    m_multiPath = m_dir.filePath(QStringLiteral("multi.mp4"));
    const QString metaPath = m_dir.filePath(QStringLiteral("multi.ffmetadata"));
    const QString subPath = m_dir.filePath(QStringLiteral("multi.srt"));
    {
        QFile meta(metaPath);
        QVERIFY(meta.open(QIODevice::WriteOnly | QIODevice::Truncate));
        meta.write(";FFMETADATA1\n"
                   "title=omacut fixture\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2000\ntitle=first\n"
                   "[CHAPTER]\nTIMEBASE=1/1000\nSTART=2000\nEND=4000\ntitle=second\n");
        meta.close();
        QFile sub(subPath);
        QVERIFY(sub.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sub.write("1\n00:00:00,500 --> 00:00:03,000\nfixture subtitle\n");
        sub.close();
    }
    QProcess multiProc;
    multiProc.start(ffmpeg, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=32x32:rate=10:duration=4"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=4"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("sine=frequency=880:duration=4"),
        QStringLiteral("-i"), subPath,
        QStringLiteral("-f"), QStringLiteral("ffmetadata"),
        QStringLiteral("-i"), metaPath,
        QStringLiteral("-map"), QStringLiteral("0:v"),
        QStringLiteral("-map"), QStringLiteral("1:a"),
        QStringLiteral("-map"), QStringLiteral("2:a"),
        QStringLiteral("-map"), QStringLiteral("3:s"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-c:s"), QStringLiteral("mov_text"),
        QStringLiteral("-map_metadata"), QStringLiteral("4"),
        QStringLiteral("-map_chapters"), QStringLiteral("4"),
        QStringLiteral("-y"), m_multiPath,
    });
    QVERIFY2(multiProc.waitForFinished(15000),
             qPrintable(QString::fromUtf8(multiProc.readAll())));
    QCOMPARE(multiProc.exitStatus(), QProcess::NormalExit);
    QCOMPARE(multiProc.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_multiPath));

    // The same stream set in a Matroska container — the non-MP4 overwrite arm
    // exercises sibling-write + trash on it. Matroska muxes subrip directly,
    // so the text subtitle gets -c:s copy instead of the mov_text conversion.
    m_multiMkvPath = m_dir.filePath(QStringLiteral("multi.mkv"));
    QProcess mkvProc;
    mkvProc.start(ffmpeg, {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=32x32:rate=10:duration=4"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("sine=frequency=440:duration=4"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("sine=frequency=880:duration=4"),
        QStringLiteral("-i"), subPath,
        QStringLiteral("-f"), QStringLiteral("ffmetadata"),
        QStringLiteral("-i"), metaPath,
        QStringLiteral("-map"), QStringLiteral("0:v"),
        QStringLiteral("-map"), QStringLiteral("1:a"),
        QStringLiteral("-map"), QStringLiteral("2:a"),
        QStringLiteral("-map"), QStringLiteral("3:s"),
        QStringLiteral("-c:v"), QStringLiteral("libx264"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-c:a"), QStringLiteral("aac"),
        QStringLiteral("-c:s"), QStringLiteral("copy"),
        QStringLiteral("-map_metadata"), QStringLiteral("4"),
        QStringLiteral("-map_chapters"), QStringLiteral("4"),
        QStringLiteral("-y"), m_multiMkvPath,
    });
    QVERIFY2(mkvProc.waitForFinished(15000),
             qPrintable(QString::fromUtf8(mkvProc.readAll())));
    QCOMPARE(mkvProc.exitStatus(), QProcess::NormalExit);
    QCOMPARE(mkvProc.exitCode(), 0);
    QVERIFY(QFileInfo::exists(m_multiMkvPath));
}

void BackendTests::waitForBackgroundWork(Backend &backend) {
    QTRY_VERIFY_WITH_TIMEOUT(backend.status().isEmpty(), 10000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QString BackendTests::formatName(const QString &path) const {
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty())
        return {};

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"),
        QStringLiteral("error"),
        QStringLiteral("-show_entries"),
        QStringLiteral("format=format_name"),
        QStringLiteral("-of"),
        QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        path,
    });
    if (!proc.waitForFinished(10000))
        return {};
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

QString BackendTests::formatTag(const QString &path, const QString &key) const {
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty())
        return {};

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"),
        QStringLiteral("error"),
        QStringLiteral("-show_entries"),
        QStringLiteral("format_tags=") + key,
        QStringLiteral("-of"),
        QStringLiteral("default=noprint_wrappers=1:nokey=1"),
        path,
    });
    if (!proc.waitForFinished(10000))
        return {};
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

// [start, end] seconds for every chapter in the file — empty list for a
// chapterless file or a probe failure.
QList<QPair<double, double>> BackendTests::chapterRanges(const QString &path) const {
    QList<QPair<double, double>> ranges;
    const QString ffprobe = QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
    if (ffprobe.isEmpty())
        return ranges;

    QProcess proc;
    proc.start(ffprobe, {
        QStringLiteral("-v"),
        QStringLiteral("error"),
        QStringLiteral("-print_format"),
        QStringLiteral("json"),
        QStringLiteral("-show_chapters"),
        path,
    });
    if (!proc.waitForFinished(10000))
        return ranges;
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return ranges;

    const QJsonArray chapters = QJsonDocument::fromJson(proc.readAllStandardOutput())
                                    .object()
                                    .value(QStringLiteral("chapters"))
                                    .toArray();
    for (const QJsonValue &value : chapters) {
        const QJsonObject chapter = value.toObject();
        ranges.append({chapter.value(QStringLiteral("start_time")).toString().toDouble(),
                       chapter.value(QStringLiteral("end_time")).toString().toDouble()});
    }
    return ranges;
}

// Drop an "ffmpeg" into dirPath that always fails to start (its shebang points
// nowhere), for tests that prepend dirPath to PATH.
bool BackendTests::installBrokenFfmpeg(const QString &dirPath) {
    QFile fake(QDir(dirPath).filePath(QStringLiteral("ffmpeg")));
    if (!fake.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    fake.write("#!/definitely/missing/omacut-ffmpeg\n");
    fake.close();
    return fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                               | QFileDevice::ExeOwner);
}

// A successful moveToTrash leaves the fixture sitting in the FreeDesktop
// trash — sweep it (and its .trashinfo metadata) back out so repeated test
// runs don't pile test media up in the user's trash.
static void removeFromTrash(const QString &fileName) {
    const int dot = fileName.lastIndexOf(QLatin1Char('.'));
    const QString base = dot < 0 ? fileName : fileName.left(dot);
    const QStringList trashDirs = {
        QDir::homePath() + QStringLiteral("/.local/share/Trash/files"),
        QStringLiteral("/tmp/.Trash-") + QString::number(::getuid())
            + QStringLiteral("/files"),
    };
    for (const QString &dirPath : trashDirs) {
        const QDir files(dirPath);
        const auto entries = files.entryList({base + QStringLiteral("*")}, QDir::Files);
        for (const QString &entry : entries)
            QFile::remove(files.filePath(entry));
        const QString infoPath = dirPath.left(dirPath.lastIndexOf(QLatin1Char('/')))
            + QStringLiteral("/info");
        const QDir info(infoPath);
        const auto infos = info.entryList({base + QStringLiteral("*")}, QDir::Files);
        for (const QString &entry : infos)
            QFile::remove(info.filePath(entry));
    }
}

void BackendTests::openDialogDelegatesToFilePicker() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    backend.openVideoDialog();
    backend.openVideoDialog();

    QCOMPARE(picker->openCount, 2);
}

void BackendTests::pickerSelectionLoadsVideo() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy infoSpy(&backend, &Backend::infoChanged);

    emit picker->openSelected(videoUrl());

    QCOMPARE(infoSpy.count(), 1);
    QCOMPARE(backend.source(), videoUrl());
    QVERIFY(backend.duration() > 0);
    waitForBackgroundWork(backend);
}

void BackendTests::thumbnailSlotsAreExposedImmediately() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy thumbsSpy(&backend, &Backend::thumbsChanged);

    QVERIFY(backend.load(videoUrl()));

    QVERIFY(backend.thumbCount() > 0);
    QCOMPARE(backend.thumbReadyCount(), 0);
    waitForBackgroundWork(backend);
    QCOMPARE(backend.thumbReadyCount(), backend.thumbCount());
    QVERIFY(thumbsSpy.count() > 2);
}

void BackendTests::thumbProviderUsesRevisionPrefixedIds() {
    ThumbProvider provider;
    provider.setImages(QVector<QImage>(2));

    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    provider.setImage(1, image);

    QSize size;
    QVERIFY(provider.requestImage(QStringLiteral("4/0"), &size, QSize()).isNull());
    QVERIFY(!provider.requestImage(QStringLiteral("4/1"), &size, QSize()).isNull());
    QCOMPARE(size, image.size());
}

void BackendTests::thumbProviderScalesHeightOnlyRequests() {
    ThumbProvider provider;
    provider.setImages(QVector<QImage>(1));

    QImage image(200, 100, QImage::Format_RGB32);
    image.fill(Qt::red);
    provider.setImage(0, image);

    QSize originalSize;
    const QImage scaled = provider.requestImage(QStringLiteral("1/0"), &originalSize, QSize(0, 50));

    QCOMPARE(originalSize, image.size());
    QCOMPARE(scaled.size(), QSize(100, 50));
}

void BackendTests::thumbnailWorkerStopsBlockedJobs() {
    const QString sleepBin = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    QVERIFY2(!sleepBin.isEmpty(), "sleep is available");

    QTemporaryDir pathDir;
    QVERIFY(pathDir.isValid());
    const QString fakeFfmpeg = pathDir.filePath(QStringLiteral("ffmpeg"));
    QFile fake(fakeFfmpeg);
    QVERIFY(fake.open(QIODevice::WriteOnly | QIODevice::Truncate));
    fake.write(QStringLiteral("#!/bin/sh\nexec \"%1\" 30\n").arg(sleepBin).toUtf8());
    fake.close();
    QVERIFY(fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ExeOwner));

    EnvVarGuard pathGuard("PATH");
    qputenv("PATH", QFile::encodeName(pathDir.path()));

    ThumbWorker worker(QStringLiteral("unused.mp4"), 0.0, 60.0, 4);
    worker.start();
    QTest::qWait(100);

    QElapsedTimer elapsed;
    elapsed.start();
    worker.requestStop();
    const bool stopped = worker.wait(2000);
    const qint64 elapsedMs = elapsed.elapsed();
    if (!stopped) {
        worker.terminate();
        worker.wait(2000);
    }

    QVERIFY2(stopped, qPrintable(QStringLiteral("worker did not stop within 2000 ms")));
    QVERIFY2(elapsedMs < 1500,
             qPrintable(QStringLiteral("worker stop took %1 ms").arg(elapsedMs)));
}

void BackendTests::exportDialogDelegatesSuggestedUrlAndRange() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);
    backend.exportDialog(0.25, 0.75);

    QCOMPARE(picker->exportCount, 1);
    QCOMPARE(picker->lastSuggestedUrl,
             QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("clip_trimmed.mp4"))));
    QCOMPARE(picker->lastStart, 0.25);
    QCOMPARE(picker->lastEnd, 0.75);
}

void BackendTests::suggestedExportUrlAlwaysUsesMp4() {
    const QString renamedSource = m_dir.filePath(QStringLiteral("renamed-source.webm"));
    QVERIFY(QFile::copy(m_videoPath, renamedSource));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    QVERIFY(backend.load(QUrl::fromLocalFile(renamedSource)));
    waitForBackgroundWork(backend);

    QCOMPARE(backend.suggestedExportUrl(),
             QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("renamed-source_trimmed.mp4"))));
}

void BackendTests::exportClipWritesMp4() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);
    QStringList statuses;
    connect(&backend, &Backend::statusChanged, [&backend, &statuses] {
        statuses << backend.status();
    });

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);

    const QString selectedPath = m_dir.filePath(QStringLiteral("actual-export.webm"));
    const QString mp4Path = m_dir.filePath(QStringLiteral("actual-export.mp4"));
    backend.exportClip(QUrl::fromLocalFile(selectedPath), 0.0, 1.0);

    QVERIFY(backend.busy());
    QCOMPARE(backend.status(), QStringLiteral("Exporting 0%"));
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);

    QCOMPARE(failedSpy.count(), 0);

    // ffmpeg's -progress stream drove the status to a completed percentage.
    QVERIFY2(statuses.contains(QStringLiteral("Exporting 100%")),
             qPrintable(statuses.join(QStringLiteral(" | "))));
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.first().at(0).toString(), mp4Path);
    QVERIFY(!backend.busy());
    QVERIFY(QFileInfo::exists(mp4Path));
    QVERIFY(!QFileInfo::exists(selectedPath));
    QVERIFY(ffmpeg::probe(mp4Path).ok);
    QVERIFY2(formatName(mp4Path).contains(QStringLiteral("mp4")),
             qPrintable(formatName(mp4Path)));
}

void BackendTests::exportClipCanReplaceSourceFile() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("replace-source.mp4"));
    QVERIFY(QFile::copy(m_videoPath, sourcePath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.exportClip(QUrl::fromLocalFile(sourcePath), 0.0, 1.0);

    QVERIFY(backend.busy());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);

    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.first().at(0).toString(), sourcePath);
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(ffmpeg::probe(sourcePath).ok);
    QVERIFY2(formatName(sourcePath).contains(QStringLiteral("mp4")),
             qPrintable(formatName(sourcePath)));
    QVERIFY(!QFileInfo::exists(sourcePath + QStringLiteral(".omacut-part.mp4")));
}

void BackendTests::exportZeroLengthClipFails() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);

    const QString outPath = m_dir.filePath(QStringLiteral("empty-range.mp4"));
    backend.exportClip(QUrl::fromLocalFile(outPath), 0.5, 0.5);

    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(!backend.busy());
    QVERIFY(!QFileInfo::exists(outPath));
}

void BackendTests::exportRefusesRewrittenPathOverExistingFile() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);

    // The dialog confirmed "rewrite-target.webm"; forcing the .mp4 suffix
    // would land on this existing file the user was never asked about.
    const QString selectedPath = m_dir.filePath(QStringLiteral("rewrite-target.webm"));
    const QString mp4Path = m_dir.filePath(QStringLiteral("rewrite-target.mp4"));
    const QByteArray original("original contents");
    {
        QFile existing(mp4Path);
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
        existing.write(original);
        existing.close();
    }

    backend.exportClip(QUrl::fromLocalFile(selectedPath), 0.0, 1.0);

    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(!backend.busy());

    QFile check(mp4Path);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), original);
}

void BackendTests::exportStartFailureClearsBusy() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);

    QTemporaryDir pathDir;
    QVERIFY(pathDir.isValid());
    QVERIFY(installBrokenFfmpeg(pathDir.path()));

    EnvVarGuard pathGuard("PATH");
    qputenv("PATH", QFile::encodeName(pathDir.path()) + ':' + qgetenv("PATH"));

    backend.exportClip(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("failed.mp4"))),
                       0.0, 1.0);

    QVERIFY(backend.busy());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);
    QVERIFY(!backend.busy());
    QVERIFY(backend.status().isEmpty());
    QVERIFY(!failedSpy.first().at(0).toString().isEmpty());
}

void BackendTests::failedExportPreservesExistingFile() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);

    // A pre-existing destination file that a failed export must not clobber.
    const QString outPath = m_dir.filePath(QStringLiteral("keep-me.mp4"));
    const QByteArray original("original contents");
    {
        QFile existing(outPath);
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
        existing.write(original);
        existing.close();
    }

    // Force ffmpeg to fail to start, the same way exportStartFailureClearsBusy does.
    QTemporaryDir pathDir;
    QVERIFY(pathDir.isValid());
    QVERIFY(installBrokenFfmpeg(pathDir.path()));

    EnvVarGuard pathGuard("PATH");
    qputenv("PATH", QFile::encodeName(pathDir.path()) + ':' + qgetenv("PATH"));

    backend.exportClip(QUrl::fromLocalFile(outPath), 0.0, 1.0);
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);

    // The original file survives untouched, and no temp part file is left behind.
    QFile check(outPath);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), original);
    QVERIFY(!QFileInfo::exists(outPath + QStringLiteral(".omacut-part.mp4")));
}

void BackendTests::qmlDoesNotCreateAudioOutputWithoutVideo() {
    ShortcutBackend backend(QUrl(), 0.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QVERIFY(harness.window()->property("audioOutputReady").isValid());
    QCOMPARE(harness.window()->property("audioOutputReady").toBool(), false);
}

void BackendTests::qmlShortcutsTriggerBackendActions() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            1.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Ctrl+S opens the save prompt; Enter presses the focused "Export as
    // new…", which delegates to the portal export path.
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              false, 3000);

    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.openCount, 1, 3000);

    // ? toggles the hotkey overlay, and Escape closes it again.
    QTest::keyClick(window, Qt::Key_Question);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("helpVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("helpVisible").toBool(), false, 3000);
    QCOMPARE(backend.openCount, 1);
}

void BackendTests::qmlArrowKeysMoveThePlayhead() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    // The trim only spans the video once the backend reports what it loaded.
    backend.announceInfo();
    QQuickItem *trimBar = harness.trimBar();
    QVERIFY(trimBar);
    QCOMPARE(trimBar->property("endSec").toDouble(), 20.0);

    window->show();
    window->requestActivate();
    QTest::qWait(100);

    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 1.0, 3000);

    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 6.0, 3000);

    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 6.2, 3000);

    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 6.0, 3000);

    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 1.0, 3000);

    // Seeking never leaves the trim, so this stops at the start instead of -4.
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 0.0, 3000);

    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("startSec").toDouble(), 5.0, 3000);
    QCOMPARE(trimBar->property("endSec").toDouble(), 20.0);
}

void BackendTests::qmlSpaceChordsSetTheTrimEdges() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    QQuickItem *trimBar = harness.trimBar();
    QVERIFY(trimBar);

    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Park the playhead at 15 s and pull the end in to it.
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 15.0, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("endSec").toDouble(), 15.0, 3000);

    // Same for the start, at 5 s.
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("playheadSec").toDouble(), 5.0, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("startSec").toDouble(), 5.0, 3000);

    // The edges never cross: pulling the end onto the start stops 0.1 s past it.
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("endSec").toDouble(), 5.1, 3000);
    QCOMPARE(trimBar->property("startSec").toDouble(), 5.0);
}

void BackendTests::qmlZoomFocusesTheSelection() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    QQuickItem *trimBar = harness.trimBar();
    QVERIFY(trimBar);

    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Trim to 5..15, then zoom: the selection fills 80% of the track, so the
    // window stretches an extra eighth of the selection on each side.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("startSec").toDouble(), 5.0, 3000);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("endSec").toDouble(), 15.0, 3000);

    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("zoomed").toBool(), true, 3000);
    QCOMPARE(trimBar->property("viewStartSec").toDouble(), 3.75);
    QCOMPARE(trimBar->property("viewEndSec").toDouble(), 16.25);

    // The filmstrip regenerates for the window, so the thumbs match the zoom.
    QCOMPARE(backend.thumbRequestCount, 1);
    QCOMPARE(backend.lastThumbStart, 3.75);
    QCOMPARE(backend.lastThumbEnd, 16.25);

    // Tighten the trim while zoomed: end to the playhead at 10 s.
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("endSec").toDouble(), 10.0, 3000);

    // The selection changed since the zoom, so Z zooms again instead of out.
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("viewStartSec").toDouble(), 4.375, 3000);
    QCOMPARE(trimBar->property("viewEndSec").toDouble(), 10.625);
    QCOMPARE(trimBar->property("zoomed").toBool(), true);
    QCOMPARE(backend.thumbRequestCount, 2);

    // Untouched since the last zoom, so Z now zooms back out to the whole video.
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("zoomed").toBool(), false, 3000);
    QCOMPARE(backend.thumbRequestCount, 3);
    QCOMPARE(backend.lastThumbStart, 0.0);
    QCOMPARE(backend.lastThumbEnd, 20.0);
}

void BackendTests::qmlQuitConfirmsUnexportedTrim() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    QQuickItem *trimBar = harness.trimBar();
    QVERIFY(trimBar);

    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Trim the video, making the work unexported: Q now asks instead of quitting.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("startSec").toDouble(), 5.0, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("trimDirty").toBool(), true, 3000);

    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);

    // Escape backs out of the confirmation.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);

    // The dialog is keyboard-driven: arrows move between the buttons instead
    // of seeking, and Enter presses the focused one. Right from the default
    // Export focus wraps around to Cancel, which closes without exporting.
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);
    QCOMPARE(backend.exportCount, 0);
    QCOMPARE(trimBar->property("playheadSec").toDouble(), 5.0);

    // Enter on the default Export focus routes into the save prompt, where
    // Enter again presses the focused "Export as new…".
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);

    // The buttons work with the mouse too: Cancel dismisses, Export opens the
    // save prompt, and "Export as new…" exports.
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QQuickItem *cancelButton = dialogButton(window, QStringLiteral("Cancel"));
    QVERIFY(cancelButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(cancelButton));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);
    QCOMPARE(backend.exportCount, 1);

    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QQuickItem *exportButton = dialogButton(window, QStringLiteral("Export"));
    QVERIFY(exportButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(exportButton));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(), true, 3000);
    QQuickItem *exportNewButton = dialogButton(window, QStringLiteral("Export as new…"));
    QVERIFY(exportNewButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(exportNewButton));
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 2, 3000);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);

    // A completed export cleans the trim; changing it again re-dirties.
    backend.announceExportDone();
    QTRY_COMPARE_WITH_TIMEOUT(window->property("trimDirty").toBool(), false, 3000);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("endSec").toDouble(), 10.0, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("trimDirty").toBool(), true, 3000);

    // Confirming the quit really ends the app: the window closes instead of
    // being re-intercepted by onClosing, and Qt.quit() is requested too.
    QSignalSpy quitSpy(&harness.engine(), &QQmlApplicationEngine::quit);
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 3000);
    QCOMPARE(quitSpy.count(), 1);
}

void BackendTests::overwriteArgsMapsAllStreamsByOutputIndex() {
    ffmpeg::VideoInfo info;
    info.path = QStringLiteral("multi.mkv");
    info.ok = true;

    ffmpeg::StreamInfo video;
    video.index = 0;
    video.codecType = QStringLiteral("video");
    video.codecName = QStringLiteral("h264");
    ffmpeg::StreamInfo aacAudio;
    aacAudio.index = 1;
    aacAudio.codecType = QStringLiteral("audio");
    aacAudio.codecName = QStringLiteral("aac");
    ffmpeg::StreamInfo vorbisAudio;
    vorbisAudio.index = 2;
    vorbisAudio.codecType = QStringLiteral("audio");
    vorbisAudio.codecName = QStringLiteral("vorbis");
    ffmpeg::StreamInfo subrip;
    subrip.index = 3;
    subrip.codecType = QStringLiteral("subtitle");
    subrip.codecName = QStringLiteral("subrip");
    info.streams = {video, aacAudio, vorbisAudio, subrip};

    const QStringList args = ffmpeg::overwriteArgs(info, QStringLiteral("out.mp4"),
                                                   1.0, 4.0);

    // Every MP4-safe stream is mapped by absolute input index, in order.
    const int map0 = args.indexOf(QStringLiteral("0:0"));
    const int map1 = args.indexOf(QStringLiteral("0:1"));
    const int map2 = args.indexOf(QStringLiteral("0:2"));
    const int map3 = args.indexOf(QStringLiteral("0:3"));
    QVERIFY(map0 >= 0);
    QVERIFY(map1 > map0 && map2 > map1 && map3 > map2);
    QCOMPARE(args.value(map0 - 1), QStringLiteral("-map"));

    // -c:<N> addresses the output position, not the input index: the primary
    // video re-encodes, whitelisted audio copies, vorbis re-encodes to aac,
    // and the text subtitle converts to mov_text.
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-c:0")) + 1),
             QStringLiteral("libx264"));
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-c:1")) + 1),
             QStringLiteral("copy"));
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-c:2")) + 1),
             QStringLiteral("aac"));
    QCOMPARE(args.value(args.indexOf(QStringLiteral("-c:3")) + 1),
             QStringLiteral("mov_text"));

    // Chapters and global metadata ride along; the overwrite never scales.
    QVERIFY(args.contains(QStringLiteral("-map_metadata")));
    QVERIFY(args.contains(QStringLiteral("-map_chapters")));
    QVERIFY(!args.contains(QStringLiteral("-vf")));
    QCOMPARE(args.last(), QStringLiteral("out.mp4"));

    // Cover art maps and copies bit-exact — it can't be re-encoded to h264.
    ffmpeg::VideoInfo withPic;
    withPic.path = QStringLiteral("cover.mkv");
    withPic.ok = true;
    ffmpeg::StreamInfo picAudio = aacAudio;
    picAudio.index = 2;
    ffmpeg::StreamInfo art;
    art.index = 1;
    art.codecType = QStringLiteral("video");
    art.codecName = QStringLiteral("mjpeg");
    art.attachedPic = true;
    withPic.streams = {video, art, picAudio};

    const QStringList picArgs = ffmpeg::overwriteArgs(withPic,
                                                      QStringLiteral("out.mp4"), 0.0, 2.0);
    QCOMPARE(picArgs.count(QStringLiteral("-map")), 3);
    QCOMPARE(picArgs.value(picArgs.indexOf(QStringLiteral("-c:0")) + 1),
             QStringLiteral("libx264"));
    QCOMPARE(picArgs.value(picArgs.indexOf(QStringLiteral("-c:1")) + 1),
             QStringLiteral("copy"));
    QCOMPARE(picArgs.value(picArgs.indexOf(QStringLiteral("-c:2")) + 1),
             QStringLiteral("copy"));
}

void BackendTests::overwriteOriginalReplacesMp4InPlace() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("replace-in-place.mp4"));
    QVERIFY(QFile::copy(m_longPath, sourcePath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.0, 0.5);

    QVERIFY(backend.busy());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);

    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.first().at(0).toString(), sourcePath);
    QCOMPARE(doneSpy.first().at(1).toString(), QString());

    // The trimmed file replaced the original and re-probes cleanly.
    const ffmpeg::VideoInfo reprobe = ffmpeg::probe(sourcePath);
    QVERIFY2(reprobe.ok, qPrintable(reprobe.error));
    QVERIFY2(reprobe.duration > 0.2 && reprobe.duration < 0.8,
             qPrintable(QString::number(reprobe.duration)));
    QVERIFY2(formatName(sourcePath).contains(QStringLiteral("mp4")),
             qPrintable(formatName(sourcePath)));

    // The reload already happened before overwriteDone fired — duration()
    // describes the new file, not the unlinked pre-trim inode.
    QCOMPARE(backend.duration(), reprobe.duration);
    QVERIFY(qAbs(backend.duration() - 0.5) < 0.3);

    // The UUID temp file is renamed away — no litter next to the target.
    QVERIFY(QDir(m_dir.path())
                .entryList({QStringLiteral("*.omacut-*")}, QDir::Files)
                .isEmpty());
}

void BackendTests::qmlSavePromptOffersOverwriteOrExport() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    QQuickItem *trimBar = harness.trimBar();
    QVERIFY(trimBar);

    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Ctrl+S opens the save prompt instead of going straight to the portal.
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);
    QCOMPARE(backend.exportCount, 0);
    QCOMPARE(backend.overwriteCount, 0);

    // Enter presses the focused "Export as new…" — the unchanged portal path.
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QCOMPARE(backend.lastStart, 0.0);
    QCOMPARE(backend.lastEnd, 20.0);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              false, 3000);

    // Re-open and arrow left off the safe default onto "Overwrite original".
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.overwriteCount, 1, 3000);
    QCOMPARE(backend.lastOverwriteStart, 0.0);
    QCOMPARE(backend.lastOverwriteEnd, 20.0);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);

    // Escape dismisses the prompt without touching either path.
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              false, 3000);
    QCOMPARE(backend.exportCount, 1);
    QCOMPARE(backend.overwriteCount, 1);
}

void BackendTests::overwriteDropsNamesUnmappableStreams() {
    ffmpeg::VideoInfo info;
    info.path = QStringLiteral("messy.mkv");
    info.ok = true;

    ffmpeg::StreamInfo video;
    video.index = 0;
    video.codecType = QStringLiteral("video");
    video.codecName = QStringLiteral("h264");
    ffmpeg::StreamInfo pgs;
    pgs.index = 1;
    pgs.codecType = QStringLiteral("subtitle");
    pgs.codecName = QStringLiteral("hdmv_pgs_subtitle");
    ffmpeg::StreamInfo cover;
    cover.index = 2;
    cover.codecType = QStringLiteral("attachment");
    cover.fileName = QStringLiteral("cover.png");
    // The MP4 chapter track: codec_type "data" + codec_tag_string "text".
    // Chapters ride -map_chapters, so this must NOT produce a warning.
    ffmpeg::StreamInfo chapterTrack;
    chapterTrack.index = 3;
    chapterTrack.codecType = QStringLiteral("data");
    chapterTrack.codecName = QStringLiteral("bin_data");
    chapterTrack.codecTag = QStringLiteral("text");
    ffmpeg::StreamInfo extraVideo;
    extraVideo.index = 4;
    extraVideo.codecType = QStringLiteral("video");
    extraVideo.codecName = QStringLiteral("h264");
    info.streams = {video, pgs, cover, chapterTrack, extraVideo};

    const QStringList drops = ffmpeg::overwriteDrops(info);
    const QString text = drops.join(QLatin1Char('\n'));
    QCOMPARE(drops.size(), 3);
    QVERIFY2(text.contains(QStringLiteral("hdmv_pgs_subtitle")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("cover.png")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("video")), qPrintable(text));
    QVERIFY2(!text.contains(QStringLiteral("bin_data"))
                 && !text.contains(QStringLiteral("data stream")),
             qPrintable(text));

    // The same classifier drives the map: only the primary video survives.
    const QStringList args = ffmpeg::overwriteArgs(info, QStringLiteral("out.mp4"),
                                                   0.0, 1.0);
    QCOMPARE(args.count(QStringLiteral("-map")), 1);
}

void BackendTests::qmlDropWarnMustBeConfirmed() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    backend.drops = {QStringLiteral("subtitle stream (hdmv_pgs_subtitle)")};
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Drops open the warning instead of calling overwriteOriginal; the save
    // prompt stays open underneath.
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("dropWarnVisible").toBool(),
                              true, 3000);
    QCOMPARE(backend.overwriteCount, 0);
    QCOMPARE(window->property("savePromptVisible").toBool(), true);

    // Escape peels only the warning.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("dropWarnVisible").toBool(),
                              false, 3000);
    QCOMPARE(window->property("savePromptVisible").toBool(), true);
    QCOMPARE(backend.overwriteCount, 0);

    // Re-open the warning and Cancel back to the save prompt — the warning
    // opens focused on Cancel.
    QQuickItem *overwriteButton =
        dialogButton(window, QStringLiteral("Overwrite original"));
    QVERIFY(overwriteButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      itemCenter(overwriteButton));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("dropWarnVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("dropWarnVisible").toBool(),
                              false, 3000);
    QCOMPARE(window->property("savePromptVisible").toBool(), true);
    QCOMPARE(backend.overwriteCount, 0);

    // "Overwrite anyway" finally encodes and closes both overlays.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      itemCenter(overwriteButton));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("dropWarnVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.overwriteCount, 1, 3000);
    QCOMPARE(window->property("dropWarnVisible").toBool(), false);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);
}

void BackendTests::overwritePreservesStreamsEndToEnd() {
    const QString targetPath = m_dir.filePath(QStringLiteral("multi-overwrite.mp4"));
    QVERIFY(QFile::copy(m_multiPath, targetPath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(targetPath)));
    waitForBackgroundWork(backend);

    // Every stream in the fixture is MP4-carryable — no drop warning.
    QVERIFY(backend.overwriteDrops().isEmpty());

    backend.overwriteOriginal(1.0, 3.0);

    QVERIFY(backend.busy());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 30000);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);

    const ffmpeg::VideoInfo reprobe = ffmpeg::probe(targetPath);
    QVERIFY2(reprobe.ok, qPrintable(reprobe.error));

    // One re-encoded h264 video, two bit-exact aac audio copies, and one
    // mov_text subtitle — nothing MP4-safe was lost.
    int videos = 0;
    int audios = 0;
    int subtitles = 0;
    for (const ffmpeg::StreamInfo &stream : reprobe.streams) {
        if (stream.codecType == QStringLiteral("video")) {
            ++videos;
            QCOMPARE(stream.codecName, QStringLiteral("h264"));
        } else if (stream.codecType == QStringLiteral("audio")) {
            ++audios;
            QCOMPARE(stream.codecName, QStringLiteral("aac"));
        } else if (stream.codecType == QStringLiteral("subtitle")) {
            ++subtitles;
            QCOMPARE(stream.codecName, QStringLiteral("mov_text"));
        }
    }
    QCOMPARE(videos, 1);
    QCOMPARE(audios, 2);
    QCOMPARE(subtitles, 1);

    // Chapters were copied and re-based into the trim range: none may start
    // before 0 or end past the new duration.
    const QList<QPair<double, double>> chapters = chapterRanges(targetPath);
    QVERIFY2(!chapters.isEmpty(), "chapters were preserved");
    for (const QPair<double, double> &range : chapters)
        QVERIFY2(range.first >= -0.001 && range.second <= reprobe.duration + 0.05,
                 qPrintable(QStringLiteral("chapter %1-%2 outside %3")
                                .arg(range.first)
                                .arg(range.second)
                                .arg(reprobe.duration)));

    // Format-level metadata carried through the overwrite.
    QCOMPARE(formatTag(targetPath, QStringLiteral("title")),
             QStringLiteral("omacut fixture"));
}

void BackendTests::nextFreeMp4SiblingAutoNumbers() {
    const QString mkvPath = m_dir.filePath(QStringLiteral("numbered.mkv"));
    const QString mp4Path = m_dir.filePath(QStringLiteral("numbered.mp4"));

    QCOMPARE(Backend::nextFreeMp4Sibling(mkvPath), mp4Path);

    // Each collision moves on to the next number — never refuses, and never
    // returns a path that already exists (D-06).
    {
        QFile existing(mp4Path);
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
    }
    const QString first = m_dir.filePath(QStringLiteral("numbered-1.mp4"));
    QCOMPARE(Backend::nextFreeMp4Sibling(mkvPath), first);
    {
        QFile existing(first);
        QVERIFY(existing.open(QIODevice::WriteOnly | QIODevice::Truncate));
    }
    QCOMPARE(Backend::nextFreeMp4Sibling(mkvPath),
             m_dir.filePath(QStringLiteral("numbered-2.mp4")));
    QVERIFY(!QFileInfo::exists(Backend::nextFreeMp4Sibling(mkvPath)));
}

void BackendTests::sourceChangedOnDiskDetectsModification() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("stale-source.mp4"));
    QVERIFY(QFile::copy(m_videoPath, sourcePath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    QVERIFY(!backend.sourceChangedOnDisk());

    // Any write after load() moves mtime+size — the save prompt warns instead
    // of silently destroying the newer file (warn-and-allow).
    QFile file(sourcePath);
    QVERIFY(file.open(QIODevice::Append));
    file.write("x");
    file.close();
    QVERIFY(backend.sourceChangedOnDisk());
}

void BackendTests::overwriteNonMp4WritesSiblingAndTrashes() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("nonmp4-source.mkv"));
    QVERIFY(QFile::copy(m_multiMkvPath, sourcePath));
    const QString siblingPath = m_dir.filePath(QStringLiteral("nonmp4-source.mp4"));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    QVERIFY(!backend.sourceIsMp4());
    QCOMPARE(backend.overwriteTargetName(),
             QStringLiteral("nonmp4-source.mp4"));
    // Every stream in the fixture can carry into MP4 — no drop warning.
    QVERIFY2(backend.overwriteDrops().isEmpty(),
             qPrintable(backend.overwriteDrops().join(QLatin1Char(','))));

    backend.overwriteOriginal(0.0, 0.5);

    QVERIFY(backend.busy());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 30000);

    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.first().at(0).toString(), siblingPath);
    const QString trashError = doneSpy.first().at(1).toString();

    // The sibling is a real MP4 carrying the fixture's streams: one
    // re-encoded h264 video, two bit-exact aac copies, one mov_text subtitle.
    const ffmpeg::VideoInfo reprobe = ffmpeg::probe(siblingPath);
    QVERIFY2(reprobe.ok, qPrintable(reprobe.error));
    int videos = 0;
    int audios = 0;
    int subtitles = 0;
    for (const ffmpeg::StreamInfo &stream : reprobe.streams) {
        if (stream.codecType == QStringLiteral("video")) {
            ++videos;
            QCOMPARE(stream.codecName, QStringLiteral("h264"));
        } else if (stream.codecType == QStringLiteral("audio")) {
            ++audios;
            QCOMPARE(stream.codecName, QStringLiteral("aac"));
        } else if (stream.codecType == QStringLiteral("subtitle")) {
            ++subtitles;
            QCOMPARE(stream.codecName, QStringLiteral("mov_text"));
        }
    }
    QCOMPARE(videos, 1);
    QCOMPARE(audios, 2);
    QCOMPARE(subtitles, 1);

    // Chapters were copied and re-based into the trim range.
    const QList<QPair<double, double>> chapters = chapterRanges(siblingPath);
    QVERIFY2(!chapters.isEmpty(), "chapters were preserved");
    for (const QPair<double, double> &range : chapters)
        QVERIFY2(range.first >= -0.001 && range.second <= reprobe.duration + 0.05,
                 qPrintable(QStringLiteral("chapter %1-%2 outside %3")
                                .arg(range.first)
                                .arg(range.second)
                                .arg(reprobe.duration)));

    // The new sibling auto-loads regardless of the trash outcome (NMP4-03).
    QCOMPARE(backend.source(), QUrl::fromLocalFile(siblingPath));
    QVERIFY(backend.sourceIsMp4());
    QVERIFY(qAbs(backend.duration() - 0.5) < 0.3);

    // A clean trash removes the original; a failed move keeps it and reports
    // the error on overwriteDone (D-08) — the save counts as done either way.
    if (trashError.isEmpty())
        QVERIFY(!QFileInfo::exists(sourcePath));
    else
        QVERIFY(QFileInfo::exists(sourcePath));
    removeFromTrash(QFileInfo(sourcePath).fileName());
}

void BackendTests::secondOverwriteAfterNonMp4IsInPlace() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("second-overwrite.mkv"));
    QVERIFY(QFile::copy(m_multiMkvPath, sourcePath));
    const QString siblingPath = m_dir.filePath(QStringLiteral("second-overwrite.mp4"));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.0, 0.5);
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 30000);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.first().at(0).toString(), siblingPath);

    // The auto-loaded sibling is an MP4 now — a second overwrite replaces it
    // in place instead of numbering up to a fresh sibling (NMP4-03).
    backend.overwriteOriginal(0.0, 0.2);
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 1, 30000);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 2);
    QCOMPARE(doneSpy.at(1).at(0).toString(), siblingPath);
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("second-overwrite-1.mp4"))));
    QVERIFY(qAbs(backend.duration() - 0.2) < 0.3);
    removeFromTrash(QFileInfo(sourcePath).fileName());
}

void BackendTests::overwriteWarnsOnChangedSource() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    backend.changedOnDisk = true;
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // warn-and-allow: a file changed since load() opens the staleness warning
    // instead of overwriting; the save prompt stays open underneath.
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("staleWarnVisible").toBool(),
                              true, 3000);
    QCOMPARE(backend.overwriteCount, 0);
    QCOMPARE(window->property("savePromptVisible").toBool(), true);

    // The warning opens focused on Cancel — Enter backs out to the prompt.
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("staleWarnVisible").toBool(),
                              false, 3000);
    QCOMPARE(backend.overwriteCount, 0);
    QCOMPARE(window->property("savePromptVisible").toBool(), true);

    // Confirming is the only way through: Overwrite original → the warning
    // again → "Overwrite anyway" finally calls the backend.
    QQuickItem *overwriteButton =
        dialogButton(window, QStringLiteral("Overwrite original"));
    QVERIFY(overwriteButton);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      itemCenter(overwriteButton));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("staleWarnVisible").toBool(),
                              true, 3000);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.overwriteCount, 1, 3000);
    QCOMPARE(window->property("staleWarnVisible").toBool(), false);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);
}

void BackendTests::qmlNonMp4SavePromptOffersTrashWrite() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mkv"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    window->show();
    window->requestActivate();
    QTest::qWait(100);

    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("savePromptVisible").toBool(),
                              true, 3000);

    // D-07: the destructive button bundles the trash action — it must say
    // exactly what it does, and the MP4-only label must not be offered.
    QQuickItem *writeButton =
        dialogButton(window, QStringLiteral("Write MP4 + move original to trash"));
    QVERIFY(writeButton);
    QVERIFY(!dialogButton(window, QStringLiteral("Overwrite original")));

    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      itemCenter(writeButton));
    QTRY_COMPARE_WITH_TIMEOUT(backend.overwriteCount, 1, 3000);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);
}

void BackendTests::trashFailureStillLoadsSibling() {
    // For files under $HOME the FreeDesktop trash is $XDG_DATA_HOME/Trash —
    // pointing it at an unwritable directory forces the moveToTrash failure
    // path (D-08). If the environment trashes anyway there is nothing to test.
    QTemporaryDir homeDir(QDir::homePath() + QStringLiteral("/omacut-test-XXXXXX"));
    QVERIFY2(homeDir.isValid(), "could not create a test dir under $HOME");
    QTemporaryDir blockedTrash;
    QVERIFY(blockedTrash.isValid());
    QVERIFY(QFile::setPermissions(blockedTrash.path(), QFileDevice::Permissions()));

    EnvVarGuard xdgGuard("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(blockedTrash.path()));

    const QString sourcePath = homeDir.filePath(QStringLiteral("trash-fail.mkv"));
    QVERIFY(QFile::copy(m_multiMkvPath, sourcePath));
    const QString siblingPath = homeDir.filePath(QStringLiteral("trash-fail.mp4"));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.0, 0.5);
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 30000);

    // Restore before any terminal check so the temp dir can clean itself up.
    QVERIFY(QFile::setPermissions(blockedTrash.path(),
                                QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                    | QFileDevice::ExeOwner));

    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);
    QCOMPARE(doneSpy.first().at(0).toString(), siblingPath);
    const QString trashError = doneSpy.first().at(1).toString();
    if (trashError.isEmpty()) {
        removeFromTrash(QFileInfo(sourcePath).fileName());
        QSKIP("environment moved the original to the trash despite the blocked XDG_DATA_HOME");
    }

    // The original stays put and the trash failure is reported, but the save
    // still counts as done and the sibling auto-loads (D-08/NMP4-03).
    QVERIFY2(trashError.contains(QStringLiteral("trash")), qPrintable(trashError));
    QVERIFY(QFileInfo::exists(sourcePath));
    QVERIFY(QFileInfo::exists(siblingPath));
    QCOMPARE(backend.source(), QUrl::fromLocalFile(siblingPath));
}

void BackendTests::busyBlocksLoadAndOpenDuringEncode() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("busy-source.mkv"));
    QVERIFY(QFile::copy(m_multiMkvPath, sourcePath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);
    QSignalSpy loadErrSpy(&backend, &Backend::loadError);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.0, 3.0);
    QVERIFY(backend.busy());

    // A blocked open is refused — but never silently (SAVE-05).
    QVERIFY(!backend.load(videoUrl()));
    QCOMPARE(loadErrSpy.count(), 1);
    QCOMPARE(loadErrSpy.first().at(0).toString(),
             QStringLiteral("An export is still running."));

    backend.openVideoDialog();
    QCOMPARE(picker->openCount, 0);
    QCOMPARE(loadErrSpy.count(), 2);

    // Export-path refusals are never silent either: exportDialog must not
    // reach the picker, and every refused export emits exportFailed.
    backend.exportDialog(0.0, 1.0);
    QCOMPARE(picker->exportCount, 0);
    QCOMPARE(failedSpy.count(), 1);

    backend.exportClip(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("busy-out.mp4"))),
                       0.0, 1.0, 0);
    QCOMPARE(failedSpy.count(), 2);

    backend.overwriteOriginal(0.0, 1.0);
    QCOMPARE(failedSpy.count(), 3);
    for (int i = 0; i < failedSpy.count(); ++i)
        QCOMPARE(failedSpy.at(i).at(0).toString(),
                 QStringLiteral("An export is still running."));
    failedSpy.clear();

    // Drain the encode before the fixture dir goes away.
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 30000);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(!backend.busy());
    removeFromTrash(QFileInfo(sourcePath).fileName());
}

void BackendTests::qmlBusyBlocksOpenAndQuit() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    backend.setBusy(true);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Ctrl+O can't open mid-encode — the refusal shows a notice.
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        window->property("noticeText").toString()
            .contains(QStringLiteral("still running")),
        3000);
    QCOMPARE(backend.openCount, 0);

    // Q and window-close are refused the same way — no quit prompt, no close.
    window->setProperty("noticeText", QString());
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_VERIFY_WITH_TIMEOUT(
        window->property("noticeText").toString()
            .contains(QStringLiteral("still running")),
        3000);
    QCOMPARE(window->property("quitConfirmVisible").toBool(), false);
    QVERIFY(window->isVisible());

    window->setProperty("noticeText", QString());
    window->close();
    QTest::qWait(200);
    QVERIFY(window->isVisible());
    QVERIFY2(window->property("noticeText").toString()
                 .contains(QStringLiteral("still running")),
             qPrintable(window->property("noticeText").toString()));
}

void BackendTests::qmlBusySaveShowsNotice() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    backend.setBusy(true);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QQuickWindow *window = harness.window();
    QTRY_VERIFY_WITH_TIMEOUT(window->property("audioOutputReady").toBool(), 3000);

    backend.announceInfo();
    window->show();
    window->requestActivate();
    QTest::qWait(100);

    // Ctrl+S mid-encode is refused — but never silently (SAVE-05): the
    // enabled gate must not swallow the key, exportVideo() shows the notice.
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(
        window->property("noticeText").toString()
            .contains(QStringLiteral("still running")),
        3000);
    QCOMPARE(window->property("savePromptVisible").toBool(), false);
    QCOMPARE(backend.overwriteCount, 0);
    QCOMPARE(backend.exportCount, 0);
}

void BackendTests::overwriteReloadFailureKeepsErrorVisible() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("reload-fail.mp4"));
    QVERIFY(QFile::copy(m_longPath, sourcePath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy loadErrSpy(&backend, &Backend::loadError);

    QStringList order;
    connect(&backend, &Backend::overwriteDone, this,
            [&] { order << QStringLiteral("done"); });
    connect(&backend, &Backend::loadError, this,
            [&] { order << QStringLiteral("loadError"); });

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.0, 0.5);
    QVERIFY(backend.busy());

    // Break PATH before the finished handler runs so the post-rename
    // reload's ffprobe lookup fails. Deterministic: QProcess::finished is
    // delivered on the event loop, and PATH is only read inside load().
    EnvVarGuard pathGuard("PATH");
    QTemporaryDir emptyPath;
    QVERIFY(emptyPath.isValid());
    qputenv("PATH", QFile::encodeName(emptyPath.path()));

    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() == 1, 30000);
    QVERIFY(!backend.busy());
    // load() emits loadError internally, overwriteDone follows, and the
    // re-signal must land last so the Saved notice never masks the failure.
    QCOMPARE(order.last(), QStringLiteral("loadError"));
    QCOMPARE(loadErrSpy.count(), 2);
}

void BackendTests::failedOverwriteLeavesSourceUntouched() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("failed-overwrite.mkv"));
    QVERIFY(QFile::copy(m_multiMkvPath, sourcePath));
    QByteArray original;
    {
        QFile file(sourcePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        original = file.readAll();
    }

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    // Force ffmpeg to fail to start, the same way exportStartFailureClearsBusy does.
    QTemporaryDir pathDir;
    QVERIFY(pathDir.isValid());
    QVERIFY(installBrokenFfmpeg(pathDir.path()));

    EnvVarGuard pathGuard("PATH");
    qputenv("PATH", QFile::encodeName(pathDir.path()) + ':' + qgetenv("PATH"));

    backend.overwriteOriginal(0.0, 0.5);
    QVERIFY(backend.busy());
    QTRY_COMPARE_WITH_TIMEOUT(failedSpy.count(), 1, 5000);

    // A failed overwrite must not write, move, or trash anything: the source
    // is byte-identical and no temp litter is left behind.
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(!backend.busy());
    QVERIFY(backend.status().isEmpty());
    QFile check(sourcePath);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), original);
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("failed-overwrite.mp4"))));
    QVERIFY(QDir(m_dir.path())
                .entryList({QStringLiteral("*.omacut-*")}, QDir::Files)
                .isEmpty());
}

void BackendTests::overwriteKeepsSourcePermissions() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("mode-0600.mp4"));
    QVERIFY(QFile::copy(m_longPath, sourcePath));
    // QFile::permissions returns the octal 0600 expanded to both the Owner
    // and legacy User bits (0x6600).
    const QFileDevice::Permissions wanted =
        QFileDevice::ReadOwner | QFileDevice::WriteOwner
        | QFileDevice::ReadUser | QFileDevice::WriteUser;
    QVERIFY(QFile::setPermissions(sourcePath, wanted));
    QCOMPARE(QFile::permissions(sourcePath), wanted);

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.0, 0.5);
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 30000);
    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);

    // The replaced file keeps the owner's mode bits — the encode temp's umask
    // defaults must not loosen them.
    QCOMPARE(QFile::permissions(sourcePath), wanted);
}

void BackendTests::tempPathForNamesAreUniqueAndSibling() {
    const QString outPath = m_dir.filePath(QStringLiteral("temp-name.mp4"));

    const QString first = Backend::tempPathFor(outPath);
    const QString second = Backend::tempPathFor(outPath);
    QVERIFY(first != second);

    // Temps sit next to the target (same filesystem keeps rename atomic), end
    // in .mp4, and the generated suffix is pure ASCII — overwrite behaves
    // identically under any locale or filename encoding.
    QCOMPARE(QFileInfo(first).dir().absolutePath(),
             QFileInfo(outPath).dir().absolutePath());
    QVERIFY(first.endsWith(QStringLiteral(".mp4")));
    QVERIFY(first.startsWith(outPath + QStringLiteral(".omacut-")));
    for (const QChar c : first.sliced(outPath.size()))
        QVERIFY2(c.unicode() < 128,
                 qPrintable(QStringLiteral("non-ASCII char in %1").arg(first)));
}

void BackendTests::overwriteZeroLengthRefused() {
    const QString sourcePath = m_dir.filePath(QStringLiteral("zero-length.mp4"));
    QVERIFY(QFile::copy(m_videoPath, sourcePath));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::overwriteDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForBackgroundWork(backend);

    backend.overwriteOriginal(0.5, 0.5);

    // Refused before any temp file is created or busy flag is raised.
    QCOMPARE(failedSpy.count(), 1);
    QCOMPARE(failedSpy.first().at(0).toString(),
             QStringLiteral("The selected clip has no length."));
    QCOMPARE(doneSpy.count(), 0);
    QVERIFY(!backend.busy());
    QVERIFY(QDir(m_dir.path())
                .entryList({QStringLiteral("*.omacut-*")}, QDir::Files)
                .isEmpty());
}

void BackendTests::trimArgsReencodeForPreciseCuts() {
    const QStringList args = ffmpeg::trimArgs(QStringLiteral("in.mp4"),
                                              QStringLiteral("out.mp4"),
                                              0.25, 0.75);

    QVERIFY(args.contains(QStringLiteral("libx264")));
    QVERIFY(args.contains(QStringLiteral("aac")));
    QVERIFY(args.contains(QStringLiteral("+faststart")));
    QVERIFY(!args.contains(QStringLiteral("copy")));

    // Progress reporting goes to stdout so the UI can show a percentage.
    const int progressAt = args.indexOf(QStringLiteral("-progress"));
    QVERIFY(progressAt >= 0);
    QCOMPARE(args.value(progressAt + 1), QStringLiteral("pipe:1"));
}

void BackendTests::trimArgsScaleTheShorterSide() {
    // No scale request, no scale filter.
    QVERIFY(!ffmpeg::trimArgs(QStringLiteral("in.mp4"), QStringLiteral("out.mp4"), 0.0, 1.0)
                 .contains(QStringLiteral("-vf")));

    // The filter caps whichever side is shorter, keeping the aspect ratio for
    // portrait and landscape alike.
    const QStringList args = ffmpeg::trimArgs(QStringLiteral("in.mp4"), QStringLiteral("out.mp4"),
                                              0.0, 1.0, 1080);
    const int vfAt = args.indexOf(QStringLiteral("-vf"));
    QVERIFY(vfAt >= 0);
    QCOMPARE(args.value(vfAt + 1),
             QStringLiteral("scale='if(gt(iw,ih),-2,1080)':'if(gt(iw,ih),1080,-2)'"));
}

void BackendTests::exportHeightsNeverUpscale() {
    QCOMPARE(Backend::exportHeights(3840, 2160), (QList<int>{1080, 720}));
    // Portrait sources are judged by their shorter side too.
    QCOMPARE(Backend::exportHeights(2160, 3840), (QList<int>{1080, 720}));
    QCOMPARE(Backend::exportHeights(1920, 1080), (QList<int>{720}));
    // At or below a target there's nothing to gain, so it isn't offered.
    QCOMPARE(Backend::exportHeights(1280, 720), QList<int>{});
    QCOMPARE(Backend::exportHeights(0, 0), QList<int>{});
}

void BackendTests::themeAccentReadsOmarchyColors() {
    const QString fallback = QStringLiteral("#FFD60A");
    const QString colorsPath = m_dir.filePath(QStringLiteral("colors.toml"));

    // No file at all — the non-omarchy case — keeps the fallback.
    QCOMPARE(Backend::accentFromColorsFile(m_dir.filePath(QStringLiteral("missing.toml")), fallback),
             fallback);

    const auto writeColors = [&colorsPath](const char *contents) {
        QFile file(colorsPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(contents);
    };

    writeColors("# omarchy theme\n"
                "mode = \"dark\"\n"
                "background = \"#121212\"\n"
                "accent = \"#33ccff\"\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), QStringLiteral("#33ccff"));

    writeColors("accent = '#aabbcc'\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), QStringLiteral("#aabbcc"));

    // A value that isn't a color keeps the fallback rather than breaking bindings.
    writeColors("accent = \"not-a-color\"\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), fallback);

    // As does a theme without an accent at all.
    writeColors("background = \"#121212\"\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), fallback);
}

void BackendTests::themeAccentForegroundKeepsContrast() {
    QCOMPARE(Backend::foregroundFor(QStringLiteral("#FFD60A")), QStringLiteral("black"));
    QCOMPARE(Backend::foregroundFor(QStringLiteral("#222266")), QStringLiteral("white"));
    QCOMPARE(Backend::foregroundFor(QStringLiteral("garbage")), QStringLiteral("black"));
}

QTEST_MAIN(BackendTests)
#include "backend_tests.moc"
