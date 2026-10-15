#include <QtTest>

#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

#include "backend.h"
#include "filepicker.h"
#include "portalfilepicker.h"
#include "thumbprovider.h"

// A fake org.freedesktop.portal.Request: only Close() matters here.
class FakePortalRequest : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Request")

public:
    int *closeCounter = nullptr;

public slots:
    void Close() {
        if (closeCounter)
            ++*closeCounter;
    }
};

// A fake org.freedesktop.portal.Desktop whose FileChooser hands back a
// request path and then never answers, like a portal that died mid-dialog.
class FakePortalService : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.FileChooser")

public:
    int openCalls = 0;
    int closeCalls = 0;
    QString currentPath;

    void respondWith(const QString &uri) {
        QVariantMap results;
        results.insert(QStringLiteral("uris"), QStringList{uri});
        QDBusMessage msg = QDBusMessage::createSignal(
            currentPath, QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"));
        msg << 0u << results;
        QDBusConnection::sessionBus().send(msg);
    }

public slots:
    QDBusObjectPath OpenFile(const QString &, const QString &, const QVariantMap &options) {
        ++openCalls;
        const QString token = options.value(QStringLiteral("handle_token")).toString();
        QString sender = message().service().mid(1);
        sender.replace(QLatin1Char('.'), QLatin1Char('_'));
        currentPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                          .arg(sender, token);
        auto *request = new FakePortalRequest;
        request->closeCounter = &closeCalls;
        request->setParent(this);
        QDBusConnection::sessionBus().registerObject(
            currentPath, request, QDBusConnection::ExportAllSlots);
        return QDBusObjectPath(currentPath);
    }
};

class FakeFilePicker : public FilePicker {
    Q_OBJECT

public:
    int openCount = 0;
    int exportCount = 0;
    QUrl lastSuggestedUrl;
    QList<int> lastScaleHeights;

    void openVideo() override { ++openCount; }

    void exportVideo(const QUrl &suggestedUrl, const QList<int> &scaleHeights) override {
        ++exportCount;
        lastSuggestedUrl = suggestedUrl;
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
    Q_PROPERTY(QVariantList videos READ videos NOTIFY videosChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool probing READ probing NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)
    Q_PROPERTY(QObject *timeline READ timelineObject CONSTANT)

public:
    explicit ShortcutBackend(QUrl source, double duration, QObject *parent = nullptr)
        : QObject(parent), m_source(std::move(source)), m_duration(duration) {
        timeline.reset(duration);
        if (!m_source.isEmpty())
            m_videos = {QVariantMap{{QStringLiteral("url"), m_source}, {QStringLiteral("thumbKey"), 1}}};
    }

    QUrl source() const { return m_source; }
    QVariantList videos() const { return m_videos; }
    bool busy() const { return false; }
    bool probing() const { return false; }
    QString status() const { return {}; }
    QString themeAccent() const { return QStringLiteral("#FFD60A"); }
    QString themeAccentForeground() const { return QStringLiteral("black"); }
    QObject *timelineObject() { return &timeline; }

    Q_INVOKABLE bool load(const QUrl &) { return false; }
    Q_INVOKABLE void openVideoDialog() { ++openCount; }
    Q_INVOKABLE void addVideoDialog(double t) {
        ++addCount;
        lastAddAt = t;
    }
    Q_INVOKABLE void exportDialog() {
        ++exportCount;
        exportedClips = timeline.clips();
    }
    Q_INVOKABLE QUrl suggestedExportUrl() const { return {}; }
    Q_INVOKABLE void saveFrame(double) {}
    Q_INVOKABLE bool addVideos(const QVariantList &, double) { return false; }

    void announceInfo() {
        timeline.reset(m_duration);
        emit infoChanged();
    }
    void announceExportDone() {
        timeline.markExported(exportedClips);
        emit exportDone(QStringLiteral("/tmp/exported.mp4"));
    }
    // What choosing a video in the add dialog does.
    void addVideo(double duration, double t) {
        m_videos.append(QVariantMap{{QStringLiteral("url"), m_source}, {QStringLiteral("thumbKey"), m_videos.size() + 1}});
        emit videosChanged();
        timeline.addSource(duration, t);
    }

    Timeline timeline;
    edit::Clips exportedClips;
    int openCount = 0;
    int addCount = 0;
    double lastAddAt = -1;
    int exportCount = 0;

signals:
    void infoChanged();
    void videosChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void frameSaved(const QString &path);
    void frameFailed(const QString &message);
    void loadError(const QString &message);
    void skippedVideos(int count);

private:
    QUrl m_source;
    double m_duration;
    QVariantList m_videos;
};

// All of the first video: what a freshly loaded one edits.
static edit::Clips wholeVideo(double duration) {
    return {{0, 0.0, duration}};
}

// Finds a DialogButton by its label ("primary" tells them apart from Labels).
static QQuickItem *dialogButton(QQuickWindow *window, const QString &text) {
    const auto items = window->findChildren<QQuickItem *>();
    for (QQuickItem *item : items) {
        if (item->property("primary").isValid() && item->property("text").toString() == text)
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
    explicit QmlHarness(QObject &backend) {
        m_engine.addImageProvider(QStringLiteral("thumbs"), new ThumbProvider);
        m_engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        m_engine.load(QUrl::fromLocalFile(mainQmlPath()));
        if (!m_engine.rootObjects().isEmpty())
            m_window = qobject_cast<QQuickWindow *>(m_engine.rootObjects().first());
    }

    QQuickWindow *window() const { return m_window; }
    QQmlApplicationEngine &engine() { return m_engine; }
    QQuickItem *editBar() const {
        return m_window ? m_window->findChild<QQuickItem *>(QStringLiteral("editBar")) : nullptr;
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
    void loadRefusesAnythingButAbsoluteLocalPaths();
    void addingAVideoInsertsItAfterThePlayheadsClip();
    void probeReportsTheDisplayedSize();
    void probeReadsStreamedWebmAndSkipsCoverArt();
    void probeRunsOffTheUiThread();
    void thumbProviderServesAndCachesFrames();
    void thumbProviderStopsBlockedJobs();
    void thumbProviderCancelsRunningJobs();
    void exportDialogDelegatesSuggestedUrl();
    void suggestedExportUrlAlwaysUsesMp4();
    void exportClipWritesMp4();
    void saveFrameWritesPngBesideTheSource();
    void saveFrameSquaresStretchedPixels();
    void shownFrameIsTheLastToStartByThen();
    void saveFrameTakesTheFrameOnScreen();
    void exportOddSizedWebmWritesEvenMp4();
    void exportKeepsOnlyTheClipsFromTheDialog();
    void exportJoinsDifferentVideos();
    void exportSkipsClipsWhoseVideoIsGone();
    void exportBlocksOpeningVideos();
    void exportClipCanReplaceSourceFile();
    void exportOverASourceStartsOverFromTheSavedFile();
    void exportZeroLengthClipFails();
    void exportRefusesRewrittenPathOverExistingFile();
    void exportStartFailureClearsBusy();
    void failedExportPreservesExistingFile();
    void qmlDoesNotCreateAudioOutputWithoutVideo();
    void qmlShortcutsTriggerBackendActions();
    void qmlArrowKeysMoveThePlayhead();
    void qmlNumberKeysJumpThroughTheEdit();
    void qmlSpaceChordsSetTheClipEdges();
    void qmlKeysSplitAndRemoveClips();
    void qmlBracketsJumpBetweenClipEdges();
    void qmlKeysMoveAndAddClips();
    void qmlZoomFocusesTheClip();
    void qmlPressingHandleScrubsItsFrame();
    void formatTimeShowsHoursFromSixtyMinutes();
    void qmlDropOpensThenAddsVideos();
    void qmlDropTakesOnlyLocalFiles();
    void qmlDropLandsAfterThePlayheadsClip();
    void qmlQuitConfirmsUnexportedEdit();
    void qmlOpenConfirmsUnexportedEdit();
    void qmlHelpOverlayBlocksEditingKeys();
    void timelineSplitsTrimsAndJoins();
    void timelineMutesAClip();
    void timelineUndoesAGestureAsOneStep();
    void timelineTracksUnexportedCuts();
    void timelineAddsAndMovesVideos();
    void timelineAnswersWhereTimesFall();
    void timelineEditsAtATime();
    void trimArgsReencodeForPreciseCuts();
    void trimArgsConcatenateTheRanges();
    void trimArgsFitDifferentVideosTogether();
    void trimArgsScaleTheShorterSide();
    void exportHeightsNeverUpscale();
    void themeAccentReadsOmarchyColors();
    void themeAccentForegroundKeepsContrast();
    void portalPickerReplacesAnUnansweredRequest();

private:
    QUrl videoUrl() const { return QUrl::fromLocalFile(m_videoPath); }
    QString formatName(const QString &path) const;
    // A test pattern, with a tone when audio is set, written into m_dir.
    QString makeVideo(const QString &name, double duration, bool audio,
                      const QString &size = QStringLiteral("32x32"));
    void waitForBackgroundWork(Backend &backend);
    // Probing finishes in the background now: wait until it has settled.
    void waitForProbe(Backend &backend);
    bool installBrokenFfmpeg(const QString &dirPath);

    QTemporaryDir m_dir;
    QString m_videoPath;
};

void BackendTests::initTestCase() {
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QVERIFY2(m_dir.isValid(), "temporary directory is valid");
    m_videoPath = m_dir.filePath(QStringLiteral("clip.mp4"));

    QVERIFY2(!QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty(), "ffmpeg is available");
    QVERIFY(!makeVideo(QStringLiteral("clip.mp4"), 1.0, false).isEmpty());
    QVERIFY(QFileInfo::exists(m_videoPath));
}

void BackendTests::waitForBackgroundWork(Backend &backend) {
    QTRY_VERIFY_WITH_TIMEOUT(backend.status().isEmpty(), 10000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void BackendTests::waitForProbe(Backend &backend) {
    QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 10000);
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

    QTRY_COMPARE_WITH_TIMEOUT(infoSpy.count(), 1, 10000);
    QCOMPARE(backend.source(), videoUrl());
    QVERIFY(backend.timeline()->duration() > 0);
    waitForBackgroundWork(backend);
}

void BackendTests::loadRefusesAnythingButAbsoluteLocalPaths() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy errorSpy(&backend, &Backend::loadError);

    // ffprobe would take these as an option and a network address.
    QVERIFY(!backend.load(QUrl(QStringLiteral("file:-report"))));
    QVERIFY(!backend.load(QUrl(QStringLiteral("file:http://example.com/a.mp4"))));
    QVERIFY(!backend.load(QUrl(QStringLiteral("https://example.com/a.mp4"))));
    QCOMPARE(errorSpy.count(), 3);
    QVERIFY(backend.source().isEmpty());
}

void BackendTests::addingAVideoInsertsItAfterThePlayheadsClip() {
    const QString second = makeVideo(QStringLiteral("second.mp4"), 2.0, false);
    QVERIFY(!second.isEmpty());

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy videosSpy(&backend, &Backend::videosChanged);

    // Before anything is open, adding a video just opens it.
    backend.addVideoDialog(0.0);
    emit picker->openSelected(videoUrl());
    QTRY_COMPARE_WITH_TIMEOUT(backend.source(), videoUrl(), 10000);
    QCOMPARE(backend.timeline()->clips(), wholeVideo(1.0));

    // After that, the add dialog's choice lands after the clip under the
    // playhead, and Ctrl+O's still starts over.
    backend.addVideoDialog(0.5);
    QCOMPARE(picker->openCount, 2);
    emit picker->openSelected(QUrl::fromLocalFile(second));
    QTRY_COMPARE_WITH_TIMEOUT(backend.videoList().size(), 2, 10000);
    QCOMPARE(backend.timeline()->clips(), (edit::Clips{{0, 0.0, 1.0}, {1, 0.0, 2.0}}));
    QVERIFY(backend.timeline()->unexported());
    QCOMPARE(videosSpy.count(), 2);

    backend.openVideoDialog();
    emit picker->openSelected(QUrl::fromLocalFile(second));
    QTRY_COMPARE_WITH_TIMEOUT(backend.source(), QUrl::fromLocalFile(second), 10000);
    QCOMPARE(backend.timeline()->clips(), wholeVideo(2.0));

    // A video that can't be read changes nothing: accepted, then refused
    // by the background probe.
    QSignalSpy errorSpy(&backend, &Backend::loadError);
    QVERIFY(backend.addVideo(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("missing.mp4"))), 0.0));
    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 10000);
    waitForProbe(backend);
    QCOMPARE(backend.timeline()->clips(), wholeVideo(2.0));
}

void BackendTests::probeReportsTheDisplayedSize() {
    const QString wide = makeVideo(QStringLiteral("wide.mp4"), 1.0, false, QStringLiteral("64x32"));
    QVERIFY(!wide.isEmpty());
    const QString turned = m_dir.filePath(QStringLiteral("turned.mp4"));
    QProcess proc;
    proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-display_rotation"), QStringLiteral("90"), QStringLiteral("-i"), wide,
        QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("-y"), turned,
    });
    QVERIFY(proc.waitForFinished(10000));
    QCOMPARE(proc.exitCode(), 0);

    const ffmpeg::VideoInfo info = ffmpeg::probe(turned);
    QVERIFY(info.ok);
    QCOMPARE(info.width, 32);
    QCOMPARE(info.height, 64);
    QCOMPARE(ffmpeg::probe(wide).width, 64);

    // PAL's 16:15 pixels show 720 x 576 as 768 x 576.
    const QString pal = m_dir.filePath(QStringLiteral("pal.mp4"));
    proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=720x576:rate=1:duration=1,setsar=16/15"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-y"), pal,
    });
    QVERIFY(proc.waitForFinished(10000));
    QCOMPARE(proc.exitCode(), 0);
    QCOMPARE(ffmpeg::probe(pal).width, 768);
    QCOMPARE(ffmpeg::probe(pal).height, 576);
}

void BackendTests::probeReadsStreamedWebmAndSkipsCoverArt() {
    const QString ffmpegBin = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY(!ffmpegBin.isEmpty());

    // Streamed to a pipe, a WebM's header carries no duration at all (the way
    // a browser's MediaRecorder writes one); probe falls back to the packets.
    const QString streamed = m_dir.filePath(QStringLiteral("streamed.webm"));
    {
        QProcess proc;
        proc.setStandardOutputFile(streamed);
        proc.start(ffmpegBin, {
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("testsrc=size=32x32:rate=10:duration=1.5"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"),
            QStringLiteral("-f"), QStringLiteral("webm"),
            QStringLiteral("pipe:1"),
        });
        QVERIFY(proc.waitForFinished(10000));
        QCOMPARE(proc.exitCode(), 0);
    }
    const ffmpeg::VideoInfo webmInfo = ffmpeg::probe(streamed);
    QVERIFY2(webmInfo.ok, qPrintable(webmInfo.error));
    QVERIFY2(qAbs(webmInfo.duration - 1.5) < 0.15, qPrintable(QString::number(webmInfo.duration)));

    // Timestamps that start an hour in still make a 1.5s video.
    const QString late = m_dir.filePath(QStringLiteral("streamed-late.webm"));
    {
        QProcess proc;
        proc.setStandardOutputFile(late);
        proc.start(ffmpegBin, {
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("testsrc=size=32x32:rate=10:duration=1.5"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"),
            QStringLiteral("-output_ts_offset"), QStringLiteral("3600"),
            QStringLiteral("-f"), QStringLiteral("webm"),
            QStringLiteral("pipe:1"),
        });
        QVERIFY(proc.waitForFinished(10000));
        QCOMPARE(proc.exitCode(), 0);
    }
    const ffmpeg::VideoInfo lateInfo = ffmpeg::probe(late);
    QVERIFY2(lateInfo.ok, qPrintable(lateInfo.error));
    QVERIFY2(qAbs(lateInfo.duration - 1.5) < 0.15, qPrintable(QString::number(lateInfo.duration)));

    // Cover art is listed as a video stream, but an audio file with art has
    // no video to edit.
    const QString cover = m_dir.filePath(QStringLiteral("cover.png"));
    const QString song = m_dir.filePath(QStringLiteral("song-with-art.m4a"));
    {
        QProcess proc;
        proc.start(ffmpegBin, {
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("color=c=red:size=16x16:d=1"),
            QStringLiteral("-frames:v"), QStringLiteral("1"),
            QStringLiteral("-y"), cover,
        });
        QVERIFY(proc.waitForFinished(10000));
        QCOMPARE(proc.exitCode(), 0);

        proc.start(ffmpegBin, {
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("sine=duration=1"),
            QStringLiteral("-i"), cover,
            QStringLiteral("-map"), QStringLiteral("0:a"),
            QStringLiteral("-map"), QStringLiteral("1:v"),
            QStringLiteral("-c:a"), QStringLiteral("aac"),
            QStringLiteral("-c:v"), QStringLiteral("png"),
            QStringLiteral("-disposition:v:0"), QStringLiteral("attached_pic"),
            QStringLiteral("-y"), song,
        });
        QVERIFY(proc.waitForFinished(10000));
        QCOMPARE(proc.exitCode(), 0);
    }
    const ffmpeg::VideoInfo songInfo = ffmpeg::probe(song);
    QVERIFY(!songInfo.ok);
    // Nor is a still image a video, though it has a frame and no duration.
    QVERIFY(!ffmpeg::probe(cover).ok);
}

void BackendTests::probeRunsOffTheUiThread() {
    // A slow ffprobe (a streamed file with no header duration is the real
    // case) must not freeze the caller: load() accepts, busy() reports the
    // probe in flight, and the refusal arrives later as a signal.
    const QString sleepBin = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    QVERIFY2(!sleepBin.isEmpty(), "sleep is available");
    QTemporaryDir pathDir;
    QVERIFY(pathDir.isValid());
    const QString fakeFfprobe = pathDir.filePath(QStringLiteral("ffprobe"));
    QFile fake(fakeFfprobe);
    QVERIFY(fake.open(QIODevice::WriteOnly | QIODevice::Truncate));
    fake.write(QStringLiteral("#!/bin/sh\nexec \"%1\" 2\n").arg(sleepBin).toUtf8());
    fake.close();
    QVERIFY(fake.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                | QFileDevice::ExeOwner));
    EnvVarGuard pathGuard("PATH");
    qputenv("PATH", QFile::encodeName(pathDir.path()));

    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QSignalSpy errorSpy(&backend, &Backend::loadError);

    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(backend.load(videoUrl()));
    QVERIFY2(elapsed.elapsed() < 1000, "load() blocked on ffprobe");
    QVERIFY(backend.busy());
    QVERIFY(backend.source().isEmpty());

    QTRY_COMPARE_WITH_TIMEOUT(errorSpy.count(), 1, 10000);
    waitForProbe(backend);
    QVERIFY(backend.source().isEmpty());
}

void BackendTests::thumbProviderServesAndCachesFrames() {
    ThumbProvider provider;
    provider.setVideo(7, m_videoPath);

    const auto frame = [&provider](const QString &id, int height) {
        std::unique_ptr<QQuickImageResponse> response(provider.requestImageResponse(id, QSize(0, height)));
        QSignalSpy finished(response.get(), &QQuickImageResponse::finished);
        if (!finished.wait(10000))
            return QImage();
        std::unique_ptr<QQuickTextureFactory> texture(response->textureFactory());
        return texture ? texture->image() : QImage();
    };

    const QImage image = frame(QStringLiteral("7/0"), 20);
    QCOMPARE(image.height(), 20);
    QVERIFY(!provider.cached(QStringLiteral("7/0@20")).isNull());
    // Served again from the cache.
    QCOMPARE(frame(QStringLiteral("7/0"), 20).size(), image.size());
    // An unknown video yields no frame rather than another video's.
    QVERIFY(frame(QStringLiteral("8/0"), 20).isNull());
}

void BackendTests::thumbProviderStopsBlockedJobs() {
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

    // Closing the app mustn't wait on a hung ffmpeg.
    auto provider = std::make_unique<ThumbProvider>();
    provider->setVideo(1, QStringLiteral("unused.mp4"));
    std::unique_ptr<QQuickImageResponse> response(
        provider->requestImageResponse(QStringLiteral("1/0"), QSize()));
    QTest::qWait(100);

    QElapsedTimer elapsed;
    elapsed.start();
    provider.reset();
    QVERIFY2(elapsed.elapsed() < 1500,
             qPrintable(QStringLiteral("stopping took %1 ms").arg(elapsed.elapsed())));
}

void BackendTests::thumbProviderCancelsRunningJobs() {
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

    // Cancelling a frame that scrolled off screen kills its ffmpeg instead
    // of leaving the worker busy until the extraction would have finished.
    ThumbProvider provider;
    provider.setVideo(1, QStringLiteral("unused.mp4"));
    std::unique_ptr<QQuickImageResponse> response(
        provider.requestImageResponse(QStringLiteral("1/0"), QSize()));
    QSignalSpy finishedSpy(response.get(), &QQuickImageResponse::finished);
    QTest::qWait(100);

    QElapsedTimer elapsed;
    elapsed.start();
    response->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);
    QVERIFY2(elapsed.elapsed() < 1500,
             qPrintable(QStringLiteral("cancelling took %1 ms").arg(elapsed.elapsed())));
}

void BackendTests::exportDialogDelegatesSuggestedUrl() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    waitForBackgroundWork(backend);
    backend.exportDialog();

    QCOMPARE(picker->exportCount, 1);
    QCOMPARE(picker->lastSuggestedUrl,
             QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("clip_trimmed.mp4"))));
}

void BackendTests::suggestedExportUrlAlwaysUsesMp4() {
    const QString renamedSource = m_dir.filePath(QStringLiteral("renamed-source.webm"));
    QVERIFY(QFile::copy(m_videoPath, renamedSource));

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    QVERIFY(backend.load(QUrl::fromLocalFile(renamedSource)));
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    QCOMPARE(backend.suggestedExportUrl(),
             QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("renamed-source_trimmed.mp4"))));
}

void BackendTests::saveFrameWritesPngBesideTheSource() {
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QSignalSpy savedSpy(&backend, &Backend::frameSaved);
    QSignalSpy failedSpy(&backend, &Backend::frameFailed);
    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);

    // Written in the background, a lossless full-size PNG.
    backend.saveFrame(0.0);
    QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() + failedSpy.count() > 0, 10000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.value(0).value(0).toString()));
    const QString png = savedSpy.first().at(0).toString();
    QVERIFY(png.endsWith(QStringLiteral("clip-frame.png")));
    QCOMPARE(formatName(png), QStringLiteral("png_pipe"));
    QImage frame(png);
    QCOMPARE(frame.size(), QSize(32, 32));
    // The status line keeps showing the playhead.
    QVERIFY(backend.status().isEmpty());

    // A second save gets its own name, and a press while one is running is ignored.
    backend.saveFrame(0.0);
    backend.saveFrame(0.0);
    QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() + failedSpy.count() > 1, 10000);
    QTest::qWait(300);
    QCOMPARE(savedSpy.count(), 2);
    QVERIFY(savedSpy.at(1).at(0).toString().endsWith(QStringLiteral("clip-frame-2.png")));
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("clip-frame-3.png"))));

    // At the very end, past the last frame's start, it's still the frame on screen.
    backend.saveFrame(1.0);
    QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() + failedSpy.count() > 2, 10000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.value(0).value(0).toString()));
    QVERIFY(savedSpy.at(2).at(0).toString().endsWith(QStringLiteral("clip-frame-3.png")));

    // Nothing is left half-written under a hidden name.
    QVERIFY(QDir(m_dir.path()).entryList({QStringLiteral(".omacut-frame-*")},
                                          QDir::Files | QDir::Hidden).isEmpty());
}

void BackendTests::saveFrameTakesTheFrameOnScreen() {
    const QString ffmpegBin = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    // B-frames come after later frames in the file, and MPEG-TS timestamps
    // start over a second in; at 4.35s, 10 frames a second, frame 43 is up.
    // 4s in with a keyframe every 2s, so the frame decodes from one partway.
    for (const QString &name : {QStringLiteral("bframes.mp4"), QStringLiteral("bframes.ts")}) {
        const QString path = m_dir.filePath(name);
        const QString expected = m_dir.filePath(name + QStringLiteral("-expected.png"));
        QProcess proc;
        proc.start(ffmpegBin, {
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("testsrc=size=64x48:rate=10:duration=6"),
            QStringLiteral("-c:v"), QStringLiteral("libx264"), QStringLiteral("-bf"), QStringLiteral("3"),
            QStringLiteral("-g"), QStringLiteral("20"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-y"), path,
        });
        QVERIFY(proc.waitForFinished(10000) && proc.exitCode() == 0);
        proc.start(ffmpegBin, {
            QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-i"), path,
            QStringLiteral("-vf"), QStringLiteral("select=eq(n\\,43),scale=round(iw*sar):ih,setsar=1"),
            QStringLiteral("-frames:v"), QStringLiteral("1"), QStringLiteral("-y"), expected,
        });
        QVERIFY(proc.waitForFinished(10000) && proc.exitCode() == 0);

        ThumbProvider provider;
        Backend backend(&provider, new FakeFilePicker);
        QSignalSpy savedSpy(&backend, &Backend::frameSaved);
        QSignalSpy failedSpy(&backend, &Backend::frameFailed);
        QVERIFY(backend.load(QUrl::fromLocalFile(path)));
        waitForProbe(backend);
        backend.saveFrame(4.35);
        QTRY_VERIFY_WITH_TIMEOUT(savedSpy.count() + failedSpy.count() > 0, 10000);
        QVERIFY2(failedSpy.isEmpty(), qPrintable(name + ": " + failedSpy.value(0).value(0).toString()));
        QFile saved(savedSpy.first().at(0).toString()), reference(expected);
        QVERIFY(saved.open(QIODevice::ReadOnly) && reference.open(QIODevice::ReadOnly));
        QVERIFY2(saved.readAll() == reference.readAll(), qPrintable(name));
    }
}

void BackendTests::shownFrameIsTheLastToStartByThen() {
    // As ffprobe lists them: in file order, so a B-frame's after later ones.
    const QByteArray packets = "0.000000,K__,\n0.100000,___,\n0.300000,___\n0.200000,___,\n"
                               "0.400000,K__\n0.500000,___\n\n";
    QCOMPARE(ffmpeg::shownFrame(packets, 0.25).time, 0.2);
    QCOMPARE(ffmpeg::shownFrame(packets, 0.25).keyframe, 0.0);
    QCOMPARE(ffmpeg::shownFrame(packets, 0.4999).time, 0.5);       // printed rounded
    QCOMPARE(ffmpeg::shownFrame(packets, 0.4999).keyframe, 0.4);
    QCOMPARE(ffmpeg::shownFrame(packets, 9.0).time, 0.5);          // a held last frame
    QCOMPARE(ffmpeg::shownFrame(QByteArray(), 2.5).time, 2.5);
    QCOMPARE(ffmpeg::shownFrame(QByteArray(), 2.5).keyframe, -1.0);
}

void BackendTests::saveFrameSquaresStretchedPixels() {
    // 40x30 stored, shown 4:3 wider: the PNG is the shape it plays at.
    const QString path = m_dir.filePath(QStringLiteral("stretched.mp4"));
    QProcess proc;
    proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {
        QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=40x30:rate=1:duration=1"),
        QStringLiteral("-vf"), QStringLiteral("setsar=4/3"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), QStringLiteral("-y"), path,
    });
    QVERIFY(proc.waitForFinished(10000) && proc.exitCode() == 0);

    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QSignalSpy savedSpy(&backend, &Backend::frameSaved);
    QVERIFY(backend.load(QUrl::fromLocalFile(path)));
    waitForProbe(backend);
    backend.saveFrame(0.0);
    QTRY_COMPARE_WITH_TIMEOUT(savedSpy.count(), 1, 10000);
    QCOMPARE(QImage(savedSpy.first().at(0).toString()).size(), QSize(53, 30));
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
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    const QString selectedPath = m_dir.filePath(QStringLiteral("actual-export.webm"));
    const QString mp4Path = m_dir.filePath(QStringLiteral("actual-export.mp4"));
    backend.exportClips(QUrl::fromLocalFile(selectedPath), wholeVideo(1.0));

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

void BackendTests::exportOddSizedWebmWritesEvenMp4() {
    // Region and window captures can have odd dimensions (which VP9/WebM
    // allows and libx264 refuses unless rounded down to even).
    const QString oddWebm = m_dir.filePath(QStringLiteral("odd-region.webm"));
    {
        QProcess proc;
        proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {
            QStringLiteral("-loglevel"), QStringLiteral("error"),
            QStringLiteral("-f"), QStringLiteral("lavfi"),
            QStringLiteral("-i"), QStringLiteral("testsrc=size=33x35:rate=10:duration=1"),
            QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
            QStringLiteral("-c:v"), QStringLiteral("libvpx-vp9"),
            QStringLiteral("-y"), oddWebm,
        });
        QVERIFY(proc.waitForFinished(10000));
        QCOMPARE(proc.exitCode(), 0);
    }

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(oddWebm)));
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    const QString outPath = m_dir.filePath(QStringLiteral("odd-region-exported.mp4"));
    backend.exportClips(QUrl::fromLocalFile(outPath), backend.timeline()->clips());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.value(0).value(0).toString()));

    const ffmpeg::VideoInfo info = ffmpeg::probe(outPath);
    QVERIFY(info.ok);
    QCOMPARE(info.width, 32);
    QCOMPARE(info.height, 34);
}

QString BackendTests::makeVideo(const QString &name, double duration, bool audio, const QString &size) {
    const QString path = m_dir.filePath(name);
    QStringList args = {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=%1:rate=1:duration=%2").arg(size).arg(duration),
    };
    if (audio)
        args << QStringLiteral("-f") << QStringLiteral("lavfi")
             << QStringLiteral("-i") << QStringLiteral("sine=duration=%1").arg(duration)
             << QStringLiteral("-c:a") << QStringLiteral("aac");
    args << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p") << QStringLiteral("-y") << path;

    QProcess proc;
    proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), args);
    if (!proc.waitForFinished(10000) || proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    return path;
}

void BackendTests::exportSkipsClipsWhoseVideoIsGone() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    // Clips from a dialog opened on a project with more videos than this one.
    const QString outPath = m_dir.filePath(QStringLiteral("gone-first.mp4"));
    backend.exportClips(QUrl::fromLocalFile(outPath), {{3, 0.0, 1.0}, {0, 0.0, 1.0}});
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(ffmpeg::probe(outPath).ok);
}

void BackendTests::exportBlocksOpeningVideos() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    waitForBackgroundWork(backend);
    backend.exportClips(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("blocking.mp4"))), wholeVideo(1.0));
    QVERIFY(backend.busy());

    // The finished export marks its clips exported, so the videos can't change under it.
    QVERIFY(!backend.load(videoUrl()));
    QVERIFY(!backend.addVideo(videoUrl(), 0.0));
    emit picker->openSelected(videoUrl());
    QCOMPARE(backend.videoList().size(), 1);

    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);
    QCOMPARE(failedSpy.count(), 0);
    QVERIFY(backend.addVideo(videoUrl(), 0.0));
    waitForProbe(backend);
}

void BackendTests::exportKeepsOnlyTheClipsFromTheDialog() {
    const QString sourcePath = makeVideo(QStringLiteral("spliced-source.mp4"), 3.0, true);
    QVERIFY(!sourcePath.isEmpty());

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    // Cut the middle second out: 0..1 and 2..3 survive.
    Timeline *timeline = backend.timeline();
    timeline->split(1.0);
    timeline->split(2.0);
    timeline->removeClip(1);
    QCOMPARE(timeline->clips(), (edit::Clips{{0, 0.0, 1.0}, {0, 2.0, 3.0}}));
    QVERIFY(timeline->unexported());

    // Edits made while the dialog is open don't change what it exports.
    backend.exportDialog();
    const edit::Clips exported = timeline->clips();
    timeline->removeClip(0);

    const QString outPath = m_dir.filePath(QStringLiteral("spliced.mp4"));
    emit picker->exportSelected(QUrl::fromLocalFile(outPath), 0);
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);
    QCOMPARE(failedSpy.count(), 0);

    const ffmpeg::VideoInfo info = ffmpeg::probe(outPath);
    QVERIFY(info.ok);
    QVERIFY(info.audio);
    QVERIFY2(qAbs(info.duration - 2.0) < 0.15, qPrintable(QString::number(info.duration)));

    // Only the exported clips count as exported.
    QVERIFY(timeline->unexported());
    timeline->undo();
    QCOMPARE(timeline->clips(), exported);
    QVERIFY(!timeline->unexported());
}

void BackendTests::exportJoinsDifferentVideos() {
    const QString first = makeVideo(QStringLiteral("joined-first.mp4"), 2.0, true, QStringLiteral("64x36"));
    const QString second = makeVideo(QStringLiteral("joined-second.mp4"), 2.0, false, QStringLiteral("32x64"));
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(first)));
    waitForProbe(backend);
    QVERIFY(backend.addVideo(QUrl::fromLocalFile(second), 0.0));
    waitForProbe(backend);
    // The silent portrait video first, then a second of the first video.
    Timeline *timeline = backend.timeline();
    timeline->moveClip(1, 0);
    timeline->setClip(1, 0.0, 1.0);
    QCOMPARE(timeline->clips(), (edit::Clips{{1, 0.0, 2.0}, {0, 0.0, 1.0}}));

    const QString outPath = m_dir.filePath(QStringLiteral("joined.mp4"));
    backend.exportClips(QUrl::fromLocalFile(outPath), timeline->clips());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);
    QVERIFY2(failedSpy.isEmpty(), qPrintable(failedSpy.value(0).value(0).toString()));

    // Framed like the first clip's video, with audio the whole way through.
    const ffmpeg::VideoInfo info = ffmpeg::probe(outPath);
    QVERIFY(info.ok);
    QVERIFY(info.audio);
    QCOMPARE(info.width, 32);
    QCOMPARE(info.height, 64);
    QVERIFY2(qAbs(info.duration - 3.0) < 0.2, qPrintable(QString::number(info.duration)));
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
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    backend.exportClips(QUrl::fromLocalFile(sourcePath), wholeVideo(1.0));

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

void BackendTests::exportOverASourceStartsOverFromTheSavedFile() {
    const QString sourcePath = makeVideo(QStringLiteral("overwritten.mp4"), 3.0, true);
    QVERIFY(!sourcePath.isEmpty());

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(QUrl::fromLocalFile(sourcePath)));
    waitForProbe(backend);
    waitForBackgroundWork(backend);
    Timeline *timeline = backend.timeline();
    timeline->split(1.0);
    timeline->split(2.0);
    timeline->removeClip(2);
    timeline->removeClip(0);

    backend.exportClips(QUrl::fromLocalFile(sourcePath), timeline->clips());
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);
    QCOMPARE(failedSpy.count(), 0);

    // The old 1..2 clip is gone with the old file: the project is the saved
    // second now, whole and already exported.
    waitForProbe(backend);
    QCOMPARE(timeline->clips().size(), 1);
    QCOMPARE(timeline->clips().first().in, 0.0);
    QVERIFY2(qAbs(timeline->duration() - 1.0) < 0.15, qPrintable(QString::number(timeline->duration())));
    QVERIFY(!timeline->unexported());
    QCOMPARE(backend.videoList().size(), 1);
}

void BackendTests::exportZeroLengthClipFails() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    const QString outPath = m_dir.filePath(QStringLiteral("empty-range.mp4"));
    backend.exportClips(QUrl::fromLocalFile(outPath), {});

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
    waitForProbe(backend);
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

    backend.exportClips(QUrl::fromLocalFile(selectedPath), wholeVideo(1.0));

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
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    QTemporaryDir pathDir;
    QVERIFY(pathDir.isValid());
    QVERIFY(installBrokenFfmpeg(pathDir.path()));

    EnvVarGuard pathGuard("PATH");
    qputenv("PATH", QFile::encodeName(pathDir.path()) + ':' + qgetenv("PATH"));

    backend.exportClips(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("failed.mp4"))),
                        wholeVideo(1.0));

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
    waitForProbe(backend);
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

    backend.exportClips(QUrl::fromLocalFile(outPath), wholeVideo(1.0));
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

    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);

    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.openCount, 1, 3000);

    // ? toggles the hotkey overlay, and Escape closes it again.
    QTest::keyClick(window, Qt::Key_Question);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("helpVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("helpVisible").toBool(), false, 3000);
    QCOMPARE(backend.openCount, 1);
}

// Brings Main.qml up against the stub backend, loaded and focused, ready for keys.
static QQuickWindow *showEditor(QmlHarness &harness, ShortcutBackend &backend) {
    QQuickWindow *window = harness.window();
    if (!window || !QTest::qWaitFor([window] { return window->property("audioOutputReady").toBool(); }, 3000))
        return nullptr;
    backend.announceInfo();
    window->show();
    window->requestActivate();
    QTest::qWait(100);
    return window;
}

static double playhead(QmlHarness &harness) {
    return harness.editBar()->property("playheadSec").toDouble();
}

static QUrl placeholderUrl(const QTemporaryDir &dir) {
    return QUrl::fromLocalFile(dir.filePath(QStringLiteral("shortcut-placeholder.mp4")));
}

void BackendTests::qmlNumberKeysJumpThroughTheEdit() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QVERIFY(harness.editBar());

    QTest::keyClick(window, Qt::Key_5);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 10.0, 3000);
    QTest::keyClick(window, Qt::Key_9);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 18.0, 3000);
    QTest::keyClick(window, Qt::Key_0);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.0, 3000);

    // Across the whole edit, not just the clip under the playhead.
    backend.timeline.split(10.0);
    QTest::keyClick(window, Qt::Key_7);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 14.0, 3000);

    // Quiet while the quit confirmation is up.
    window->setProperty("quitConfirmVisible", true);
    QTest::keyClick(window, Qt::Key_2);
    QTest::qWait(50);
    QCOMPARE(playhead(harness), 14.0);
}

void BackendTests::qmlArrowKeysMoveThePlayhead() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QVERIFY(harness.editBar());

    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 1.0, 3000);

    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 6.0, 3000);

    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 6.2, 3000);

    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 6.0, 3000);

    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 1.0, 3000);

    // Seeking never leaves the video, so this stops at the start instead of -4.
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.0, 3000);

    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 5.0, 20.0}}), 3000);
}

void BackendTests::qmlSpaceChordsSetTheClipEdges() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    // Park the playhead at 15 s and pull the end in to it.
    for (int i = 0; i < 3; ++i)
        QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 15.0, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 0.0, 15.0}}), 3000);

    // Same for the start, at 5 s. The clip now starts there, so the
    // playhead's 5 s along the edit is 10 s into the video.
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 5.0, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 5.0, 15.0}}), 3000);

    // The edges never cross: pulling the end onto the start leaves the clip be.
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.0, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTest::qWait(50);
    QCOMPARE(backend.timeline.clips(), (edit::Clips{{0, 5.0, 15.0}}));
}

void BackendTests::qmlKeysSplitAndRemoveClips() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    // S splits the clip under the playhead, at 5 s and then at 10 s.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_S);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 20.0}}), 3000);
    QCOMPARE(window->property("unexported").toBool(), false);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_S);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 10.0}, {0, 10.0, 20.0}}), 3000);

    // X removes the clip under the playhead, closing up behind it.
    QTest::keyClick(window, Qt::Key_Left);
    QTest::keyClick(window, Qt::Key_X);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 10.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

    // Delete too; the last clip stays.
    QTest::keyClick(window, Qt::Key_Delete);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 0.0, 5.0}}), 3000);
    QTest::keyClick(window, Qt::Key_X);
    QTest::qWait(50);
    QCOMPARE(backend.timeline.clips(), (edit::Clips{{0, 0.0, 5.0}}));

    // Undo steps back through each edit; redo steps forward again.
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 10.0, 20.0}}), 3000);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 10.0}, {0, 10.0, 20.0}}), 3000);
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 10.0, 20.0}}), 3000);
}

void BackendTests::qmlBracketsJumpBetweenClipEdges() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    backend.timeline.split(5.0);
    backend.timeline.split(12.0);
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 1.0, 3000);

    QTest::keyClick(window, Qt::Key_BracketRight);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 5.0, 3000);
    QTest::keyClick(window, Qt::Key_BracketRight);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 12.0, 3000);
    QTest::keyClick(window, Qt::Key_BracketRight);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 20.0, 3000);
    QTest::keyClick(window, Qt::Key_BracketLeft);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 12.0, 3000);
    QTest::keyClick(window, Qt::Key_BracketLeft);
    QTest::keyClick(window, Qt::Key_BracketLeft);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.0, 3000);
}

void BackendTests::qmlKeysMoveAndAddClips() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    // Ctrl+Shift+O asks for a video to add after the clip under the playhead.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_S);
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.2, 3000);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.addCount, 1, 3000);
    QCOMPARE(backend.lastAddAt, 0.2);
    backend.addVideo(3.0, backend.lastAddAt);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {1, 0.0, 3.0}, {0, 5.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

    // Alt+] moves the clip under the playhead later, and the playhead moves
    // with it, still 0.2 s in.
    QTest::keyClick(window, Qt::Key_BracketRight, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{1, 0.0, 3.0}, {0, 0.0, 5.0}, {0, 5.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 3.2, 3000);

    // At the end already, Alt+] does nothing; Alt+[ moves it back.
    QTest::keyClick(window, Qt::Key_BracketRight, Qt::AltModifier);
    QTest::keyClick(window, Qt::Key_BracketRight, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{1, 0.0, 3.0}, {0, 5.0, 20.0}, {0, 0.0, 5.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 18.2, 3000);
    QTest::keyClick(window, Qt::Key_BracketLeft, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{1, 0.0, 3.0}, {0, 0.0, 5.0}, {0, 5.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 3.2, 3000);
}

void BackendTests::qmlZoomFocusesTheClip() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();

    // Zoom from inside the 5..15 clip: it fills 80% of the track, so the
    // window stretches an extra eighth of the clip on each side.
    backend.timeline.split(5.0);
    backend.timeline.split(15.0);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 10.0, 3000);

    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), true, 3000);
    QCOMPARE(editBar->property("viewStartSec").toDouble(), 3.75);
    QCOMPARE(editBar->property("viewEndSec").toDouble(), 16.25);

    // Tighten the clip while zoomed: end to the playhead at 10 s.
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 10.0}, {0, 15.0, 20.0}}), 3000);
    QTest::keyClick(window, Qt::Key_Left);

    // The clip changed since the zoom, so Z zooms again instead of out.
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("viewStartSec").toDouble(), 4.375, 3000);
    QCOMPARE(editBar->property("viewEndSec").toDouble(), 10.625);
    QCOMPARE(editBar->property("zoomed").toBool(), true);

    // Untouched since the last zoom, so Z now zooms back out to the whole edit.
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), false, 3000);

    // If edits shrink the sequence past viewStartSec, zoom resets so the
    // remaining clips don't sit entirely off-screen to the left.
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), true, 3000);
    QCOMPARE(editBar->property("viewStartSec").toDouble(), 4.375);
    backend.timeline.removeClip(2);
    backend.timeline.removeClip(1);
    backend.timeline.setClip(0, 0.0, 4.0);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), false, 3000);
}

void BackendTests::qmlPressingHandleScrubsItsFrame() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();
    QVERIFY(editBar);

    backend.timeline.setClip(0, 3.0, 15.0);
    QTest::qWait(50);

    QSignalSpy scrubClipSpy(editBar, SIGNAL(scrubClip(int,double)));
    QVERIFY(scrubClipSpy.isValid());

    // Pressing the leading handle immediately previews the clip's in-point.
    const QPoint leftHandle = editBar->mapToScene(QPointF(5.0, editBar->height() / 2.0)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, leftHandle);
    QCOMPARE(scrubClipSpy.count(), 1);
    QCOMPARE(scrubClipSpy.first().at(0).toInt(), 0);
    QCOMPARE(scrubClipSpy.first().at(1).toDouble(), 3.0);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, leftHandle);

    // Pressing the trailing handle immediately previews the clip's out-point.
    const QPoint rightHandle = editBar->mapToScene(QPointF(editBar->width() - 5.0, editBar->height() / 2.0)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, rightHandle);
    QCOMPARE(scrubClipSpy.count(), 2);
    QCOMPARE(scrubClipSpy.last().at(0).toInt(), 0);
    QCOMPARE(scrubClipSpy.last().at(1).toDouble(), 14.999);

    // Dragging it keeps showing the clip's last frame, not the moment past it.
    QTest::mouseMove(window, rightHandle - QPoint(40, 0));
    QTRY_VERIFY_WITH_TIMEOUT(scrubClipSpy.count() > 2, 3000);
    const double out = backend.timeline.clips().first().out;
    QVERIFY(out < 15.0);
    QCOMPARE(scrubClipSpy.last().at(1).toDouble(), out - 0.001);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, rightHandle - QPoint(40, 0));
}

void BackendTests::formatTimeShowsHoursFromSixtyMinutes() {
    QJSEngine engine;
    const QString formatPath = QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath(
        QStringLiteral("../src/Format.js"));
    QFile file(formatPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QString script = QString::fromUtf8(file.readAll());
    script.remove(QStringLiteral(".pragma library"));
    QVERIFY(!engine.evaluate(script).isError());
    QJSValue fmt = engine.globalObject().property(QStringLiteral("fmt"));
    QVERIFY(fmt.isCallable());

    const auto callFmt = [&fmt](double s) {
        return fmt.call({QJSValue(s)}).toString();
    };
    QCOMPARE(callFmt(0.0), QStringLiteral("00:00.00"));
    QCOMPARE(callFmt(59.999), QStringLiteral("01:00.00"));
    QCOMPARE(callFmt(3599.99), QStringLiteral("59:59.99"));
    QCOMPARE(callFmt(3599.999), QStringLiteral("01:00:00.00"));
    QCOMPARE(callFmt(3661.25), QStringLiteral("01:01:01.25"));
}

void BackendTests::qmlOpenConfirmsUnexportedEdit() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    // Trim the video, making the work unexported.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 5.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

    // Ctrl+O asks before replacing the edit instead of opening the picker.
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), true, 3000);
    QCOMPARE(backend.openCount, 0);

    // Keeping the edit leaves everything as it was.
    QQuickItem *keep = dialogButton(window, QStringLiteral("Keep editing"));
    QVERIFY(keep);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(keep));
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), false, 3000);
    QCOMPARE(backend.openCount, 0);
    QCOMPARE(backend.timeline.clips(), (edit::Clips{{0, 5.0, 20.0}}));

    // Clicking the preview with a video loaded does not open anything.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier,
                      QPoint(window->width() / 2, window->height() / 3));
    QTest::qWait(50);
    QCOMPARE(backend.openCount, 0);
    QCOMPARE(window->property("openConfirmVisible").toBool(), false);

    // Asking again and confirming opens the picker.
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), true, 3000);
    QQuickItem *open = dialogButton(window, QStringLiteral("Open video"));
    QVERIFY(open);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, itemCenter(open));
    QTRY_COMPARE_WITH_TIMEOUT(backend.openCount, 1, 3000);
    QCOMPARE(window->property("openConfirmVisible").toBool(), false);
}

void BackendTests::qmlHelpOverlayBlocksEditingKeys() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    QTest::keyClick(window, Qt::Key_Question);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("helpVisible").toBool(), true, 3000);

    // Split, delete, seek, export, play, and open all stay quiet behind
    // the overlay.
    QTest::keyClick(window, Qt::Key_S);
    QTest::keyClick(window, Qt::Key_X);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_Space);
    QTest::qWait(50);
    QCOMPARE(backend.timeline.clips(), wholeVideo(20.0));
    QCOMPARE(playhead(harness), 0.0);
    QCOMPARE(backend.openCount, 0);
    QCOMPARE(backend.exportCount, 0);

    // Escape closes the overlay and the keys work again.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("helpVisible").toBool(), false, 3000);
    QTest::keyClick(window, Qt::Key_S, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QTest::keyClick(window, Qt::Key_Right);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 1.0, 3000);
    QTest::keyClick(window, Qt::Key_S);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 1.0}, {0, 1.0, 20.0}}), 3000);
}

void BackendTests::qmlQuitConfirmsUnexportedEdit() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));

    // Trim the video, making the work unexported: Q now asks instead of quitting.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 5.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

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
    QCOMPARE(playhead(harness), 5.0);

    // Enter on the default Export focus exports, as does Ctrl+S.
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QCOMPARE(window->property("quitConfirmVisible").toBool(), false);

    // The buttons work with the mouse too: Cancel dismisses, Export exports.
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
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 2, 3000);
    QCOMPARE(window->property("quitConfirmVisible").toBool(), false);

    // A completed export cleans the edit; changing it again re-dirties.
    backend.announceExportDone();
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), false, 3000);
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 5.0, 15.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

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

void BackendTests::timelineMutesAClip() {
    Timeline timeline;
    timeline.reset(10.0);
    timeline.split(4.0);
    timeline.toggleAudio(0);
    QCOMPARE(timeline.clips().at(0).audio, false);
    QCOMPARE(timeline.clips().at(1).audio, true);
    QVERIFY(!timeline.canJoin(0));
    timeline.toggleAudio(0);
    QVERIFY(timeline.canJoin(0));
    timeline.undo();
    QCOMPARE(timeline.clips().at(0).audio, false);
}

void BackendTests::timelineSplitsTrimsAndJoins() {
    Timeline timeline;
    timeline.reset(10.0);
    QCOMPARE(timeline.clips(), wholeVideo(10.0));

    timeline.split(4.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 4.0}, {0, 4.0, 10.0}}));
    QVERIFY(timeline.canJoin(0));
    // Too close to an edge to leave a clip worth keeping: no split.
    timeline.split(4.05);
    QCOMPARE(timeline.clips().size(), 2);

    // Trimming a clip closes up behind it; it can reach back into its source,
    // but not past either end.
    timeline.setClip(1, 6.0, 10.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 4.0}, {0, 6.0, 10.0}}));
    QCOMPARE(timeline.duration(), 8.0);
    QVERIFY(!timeline.canJoin(0));
    timeline.setClip(1, 2.0, 12.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 4.0}, {0, 2.0, 10.0}}));

    // Only a clip that carries on where the last left off can join it.
    timeline.joinClips(0);
    QCOMPARE(timeline.clips().size(), 2);
    timeline.setClip(1, 4.0, 10.0);
    timeline.joinClips(0);
    QCOMPARE(timeline.clips(), wholeVideo(10.0));
    // The last clip can't be removed.
    timeline.removeClip(0);
    QCOMPARE(timeline.clips(), wholeVideo(10.0));

    // Touching pieces of the same stretch export as one.
    timeline.split(5.0);
    QCOMPARE(edit::merged(timeline.clips()), wholeVideo(10.0));
}

void BackendTests::timelineAddsAndMovesVideos() {
    Timeline timeline;
    timeline.reset(10.0);
    timeline.split(5.0);

    // Added after the clip under t, as one undoable edit.
    QCOMPARE(timeline.addSource(3.0, 2.0), 1);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 5.0}, {1, 0.0, 3.0}, {0, 5.0, 10.0}}));
    QCOMPARE(timeline.duration(), 13.0);
    QVERIFY(timeline.unexported());
    QVERIFY(!timeline.canJoin(0));
    QVERIFY(!timeline.canJoin(1));

    // Moving puts the split pieces back together, which then export as one.
    timeline.moveClip(1, 2);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 10.0}, {1, 0.0, 3.0}}));
    QCOMPARE(edit::merged(timeline.clips()), (edit::Clips{{0, 0.0, 10.0}, {1, 0.0, 3.0}}));
    timeline.moveClip(2, 5);
    QCOMPARE(timeline.clips().size(), 3);

    // Each clip is bound by its own video.
    timeline.setClip(2, 0.0, 99.0);
    QCOMPARE(timeline.clips().last(), (edit::Clip{1, 0.0, 3.0}));

    // Past the last clip, it goes at the end.
    QCOMPARE(timeline.addSource(2.0, 99.0), 2);
    QCOMPARE(timeline.clips().last(), (edit::Clip{2, 0.0, 2.0}));

    timeline.undo();
    timeline.undo();
    timeline.undo();
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 10.0}}));
}

void BackendTests::timelineUndoesAGestureAsOneStep() {
    Timeline timeline;
    timeline.reset(10.0);
    QVERIFY(!timeline.canUndo());

    timeline.beginGesture();
    timeline.moveEdge(0, true, 1.0);
    timeline.moveEdge(0, true, 2.0);
    timeline.moveEdge(0, true, 3.0);
    timeline.endGesture();
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 3.0, 10.0}}));

    timeline.undo();
    QCOMPARE(timeline.clips(), wholeVideo(10.0));
    QVERIFY(!timeline.canUndo());
    timeline.redo();
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 3.0, 10.0}}));

    // A new edit drops the redo history, and a reset drops it all.
    timeline.undo();
    timeline.split(5.0);
    QVERIFY(!timeline.canRedo());
    timeline.reset(10.0);
    QVERIFY(!timeline.canUndo());
}

void BackendTests::timelineTracksUnexportedCuts() {
    Timeline timeline;
    timeline.reset(10.0);
    // Splits alone cut nothing, so there's nothing to lose.
    timeline.split(5.0);
    QVERIFY(!timeline.unexported());
    // Swapping the halves is an edit, though.
    timeline.moveClip(0, 1);
    QVERIFY(timeline.unexported());
    timeline.undo();

    timeline.removeClip(1);
    QVERIFY(timeline.unexported());
    timeline.markExported(timeline.clips());
    QVERIFY(!timeline.unexported());
    timeline.undo();
    timeline.redo();
    QVERIFY(!timeline.unexported());
}

void BackendTests::timelineAnswersWhereTimesFall() {
    Timeline timeline;
    timeline.reset(20.0);
    timeline.split(5.0);
    timeline.split(10.0);
    timeline.removeClip(1);
    // Video 0..5 then 10..20, back to back: 15 s along the edit.
    QCOMPARE(timeline.clipAt(4.9), 0);
    QCOMPARE(timeline.clipAt(5.0), 1);
    // The very end belongs to the last clip.
    QCOMPARE(timeline.clipAt(15.0), 1);
    QCOMPARE(timeline.clipStart(1), 5.0);
    QCOMPARE(timeline.clipList().value(1).toMap().value(QStringLiteral("end")).toDouble(), 15.0);

    QCOMPARE(timeline.edgeFrom(1.0, 1), 5.0);
    QCOMPARE(timeline.edgeFrom(5.0, 1), 15.0);
    QCOMPARE(timeline.edgeFrom(15.0, 1), 15.0);
    QCOMPARE(timeline.edgeFrom(15.0, -1), 5.0);
    QCOMPARE(timeline.edgeFrom(0.0, -1), 0.0);
}

void BackendTests::timelineEditsAtATime() {
    Timeline timeline;
    timeline.reset(20.0);
    timeline.split(5.0);
    timeline.split(10.0);

    timeline.removeAt(7.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 5.0}, {0, 10.0, 20.0}}));

    // Times along the edit map into the clip's own video.
    timeline.trimTo(2.0, true);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 2.0, 5.0}, {0, 10.0, 20.0}}));
    timeline.trimTo(12.0, false);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 2.0, 5.0}, {0, 10.0, 19.0}}));
    // 4 s along is 1 s into the second clip, 11 s into the video.
    timeline.split(4.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 2.0, 5.0}, {0, 10.0, 11.0}, {0, 11.0, 19.0}}));

    // Dragging an edge past the other stops a minimum clip short of it.
    timeline.moveEdge(0, false, -3.0);
    QCOMPARE(timeline.clips().first(), (edit::Clip{0, 2.0, 2.0 + edit::minimumClip}));
    timeline.moveEdge(0, true, 30.0);
    QCOMPARE(timeline.clips().first(), (edit::Clip{0, 2.0, 2.0 + edit::minimumClip}));
}

void BackendTests::trimArgsReencodeForPreciseCuts() {
    const QStringList args = ffmpeg::trimArgs({{QStringLiteral("in.mp4"), 0.25, 0.75, true}},
                                              QStringLiteral("out.mp4"), 1920, 1080);

    QVERIFY(args.contains(QStringLiteral("libx264")));
    QVERIFY(args.contains(QStringLiteral("yuv420p")));
    QVERIFY(args.contains(QStringLiteral("aac")));
    QVERIFY(args.contains(QStringLiteral("+faststart")));
    QVERIFY(!args.contains(QStringLiteral("copy")));

    // The segment is a fast input seek bounded by its length.
    const int seekAt = args.indexOf(QStringLiteral("-ss"));
    QCOMPARE(args.mid(seekAt, 6), (QStringList{"-ss", "0.250", "-t", "0.500", "-i", "in.mp4"}));

    // Progress reporting goes to stdout so the UI can show a percentage.
    const int progressAt = args.indexOf(QStringLiteral("-progress"));
    QVERIFY(progressAt >= 0);
    QCOMPARE(args.value(progressAt + 1), QStringLiteral("pipe:1"));
}

static QString filterGraph(const QStringList &args) {
    return args.value(args.indexOf(QStringLiteral("-filter_complex")) + 1);
}

void BackendTests::trimArgsConcatenateTheRanges() {
    const QList<ffmpeg::Segment> segments = {{QStringLiteral("in.mp4"), 0.0, 1.0, true},
                                             {QStringLiteral("in.mp4"), 2.0, 3.5, true}};
    const QStringList args = ffmpeg::trimArgs(segments, QStringLiteral("out.mp4"), 1920, 1080);
    QCOMPARE(args.count(QStringLiteral("-i")), 2);
    // One video needs no fitting together, only even sides.
    QCOMPARE(filterGraph(args),
             QStringLiteral("[0:V:0][0:a:0][1:V:0][1:a:0]concat=n=2:v=1:a=1[joined][a];"
                            "[joined]crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0[v]"));

    // Without audio, only the video is joined and mapped.
    const QStringList silent = ffmpeg::trimArgs({{QStringLiteral("in.mp4"), 0.0, 1.0, false},
                                                 {QStringLiteral("in.mp4"), 2.0, 3.5, false}},
                                                QStringLiteral("out.mp4"), 1920, 1080);
    QCOMPARE(filterGraph(silent), QStringLiteral("[0:V:0][1:V:0]concat=n=2:v=1:a=0[joined];"
                                                  "[joined]crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0[v]"));
    QVERIFY(!silent.contains(QStringLiteral("[a]")));
    QVERIFY(!silent.contains(QStringLiteral("aac")));
}

void BackendTests::trimArgsFitDifferentVideosTogether() {
    const QStringList args = ffmpeg::trimArgs({{QStringLiteral("a.mp4"), 0.0, 1.0, true},
                                               {QStringLiteral("b.mp4"), 3.0, 5.0, false}},
                                              QStringLiteral("out.mp4"), 1921, 1080);
    // Each has its pixels squared, then is letterboxed into the first's frame
    // (sides kept even), and the silent one gets silence to match.
    QCOMPARE(filterGraph(args),
             QStringLiteral("[0:V:0]scale='trunc(iw*sar/2)*2':ih,setsar=1,"
                            "scale=1920:1080:force_original_aspect_ratio=decrease,"
                            "pad=1920:1080:-1:-1,setsar=1,format=yuv420p[v0];"
                            "[0:a:0]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo[a0];"
                            "[1:V:0]scale='trunc(iw*sar/2)*2':ih,setsar=1,"
                            "scale=1920:1080:force_original_aspect_ratio=decrease,"
                            "pad=1920:1080:-1:-1,setsar=1,format=yuv420p[v1];"
                            "anullsrc=r=48000:cl=stereo,atrim=duration=2.000,aformat=sample_fmts=fltp[a1];"
                            "[v0][a0][v1][a1]concat=n=2:v=1:a=1[v][a]"));
}

void BackendTests::trimArgsScaleTheShorterSide() {
    const QList<ffmpeg::Segment> segment = {{QStringLiteral("in.mp4"), 0.0, 1.0, true}};

    // No scale request, no scale filter.
    QVERIFY(!filterGraph(ffmpeg::trimArgs(segment, QStringLiteral("out.mp4"), 1920, 1080))
                 .contains(QStringLiteral("scale")));

    // Sides are rounded down to even so libx264 can open the encoder.
    QCOMPARE(filterGraph(ffmpeg::trimArgs(segment, QStringLiteral("out.mp4"), 1921, 1080)),
             QStringLiteral("[0:V:0][0:a:0]concat=n=1:v=1:a=1[joined][a];"
                            "[joined]crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0[v]"));

    // The filter caps whichever side is shorter, keeping the aspect ratio for
    // portrait and landscape alike.
    QCOMPARE(filterGraph(ffmpeg::trimArgs(segment, QStringLiteral("out.mp4"), 1920, 1080, 1080)),
             QStringLiteral("[0:V:0][0:a:0]concat=n=1:v=1:a=1[joined][a];"
                            "[joined]scale='if(gt(iw,ih),-2,1080)':'if(gt(iw,ih),1080,-2)'[v]"));
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

    // Themes annotate their palette, and the note is not part of the colour.
    writeColors("accent = \"#4ECDC4\"          # secondary-400 -- the paired accent\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), QStringLiteral("#4ECDC4"));

    writeColors("accent = '#aabbcc'  # single quoted, with a note\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), QStringLiteral("#aabbcc"));

    // Unquoted, the colour's own '#' opens it and a later one opens a comment.
    writeColors("accent = #4ECDC4 # secondary-400\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), QStringLiteral("#4ECDC4"));

    // A named colour keeps working with a note after it.
    writeColors("accent = teal # not a hex value at all\n");
    QCOMPARE(Backend::accentFromColorsFile(colorsPath, fallback), QStringLiteral("teal"));

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

static bool dropUrls(QQuickWindow *window, const QList<QUrl> &urls) {
    QMimeData mime;
    mime.setUrls(urls);
    const QPoint point(window->width() / 2, window->height() / 3);
    QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &enter);
    if (!enter.isAccepted()) {
        QDragLeaveEvent leave;
        QCoreApplication::sendEvent(window, &leave);
        return false;
    }
    QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window, &drop);
    const bool accepted = drop.isAccepted();
    QDragLeaveEvent leave;
    QCoreApplication::sendEvent(window, &leave);
    return accepted;
}

void BackendTests::qmlDropOpensThenAddsVideos() {
    const QString second = makeVideo(QStringLiteral("dropped #2 with spaces.mp4"), 2.0, false);
    const QString third = makeVideo(QStringLiteral("dropped-3.mp4"), 3.0, false);
    QVERIFY(!second.isEmpty() && !third.isEmpty());
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QmlHarness harness(backend);
    auto *window = harness.window();
    QVERIFY(window);
    QTest::qWait(100);
    Timeline *timeline = backend.timeline();

    // Into an empty window, a drop opens the video, saying what it skipped.
    QVERIFY(dropUrls(window, {QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("missing-first.mp4"))), videoUrl()}));
    waitForProbe(backend);
    QCOMPARE(window->property("noticeText").toString(), QStringLiteral("Skipped a file that won't open."));
    QCOMPARE(backend.source(), videoUrl());
    QCOMPARE(timeline->clips(), wholeVideo(1.0));

    // After that, drops join the edit after the clip under the playhead, in
    // order, and one undo takes each back out.
    QVERIFY(dropUrls(window, {QUrl::fromLocalFile(second), QUrl::fromLocalFile(third)}));
    waitForProbe(backend);
    QCOMPARE(backend.source(), videoUrl());
    QCOMPARE(timeline->clips(), (edit::Clips{{0, 0.0, 1.0}, {1, 0.0, 2.0}, {2, 0.0, 3.0}}));
    QVERIFY(timeline->unexported());
    timeline->undo();
    QCOMPARE(timeline->clips(), (edit::Clips{{0, 0.0, 1.0}, {1, 0.0, 2.0}}));

    // A file that won't open adds nothing, and says so. The drop is taken
    // before the probe knows; the refusal arrives in the background.
    QSignalSpy errors(&backend, &Backend::loadError);
    QVERIFY(dropUrls(window, {QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("missing.mp4")))}));
    QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 10000);
    QCOMPARE(timeline->clips().size(), 2);
}

void BackendTests::qmlDropLandsAfterThePlayheadsClip() {
    const QString second = makeVideo(QStringLiteral("drop-mid-2.mp4"), 2.0, false);
    const QString third = makeVideo(QStringLiteral("drop-mid-3.mp4"), 3.0, false);
    QVERIFY(!second.isEmpty() && !third.isEmpty());
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    Timeline *timeline = backend.timeline();
    timeline->split(0.5);
    QmlHarness harness(backend);
    auto *window = harness.window();
    QVERIFY(window);
    QTest::qWait(100);

    // In the first of two clips: the drop goes between them, in order, past
    // a file that won't open.
    harness.editBar()->setProperty("playheadSec", 0.25);
    QSignalSpy errors(&backend, &Backend::loadError);
    QVERIFY(dropUrls(window, {QUrl::fromLocalFile(second),
                              QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("missing.mp4"))),
                              QUrl::fromLocalFile(third)}));
    waitForProbe(backend);
    QCOMPARE(errors.count(), 1);
    QCOMPARE(timeline->clips(), (edit::Clips{{0, 0.0, 0.5}, {1, 0.0, 2.0}, {2, 0.0, 3.0}, {0, 0.5, 1.0}}));
    QCOMPARE(window->property("noticeText").toString(), QStringLiteral("Skipped a file that won't open."));
    QVERIFY(!dropUrls(window, {QUrl(QStringLiteral("file:////host/share/a.mp4"))}));
}

void BackendTests::qmlDropTakesOnlyLocalFiles() {
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QmlHarness harness(backend);
    auto *window = harness.window();
    QVERIFY(window);
    QTest::qWait(100);

    QVERIFY(!dropUrls(window, {QUrl(QStringLiteral("https://example.com/video.mp4"))}));
    QVERIFY(!dropUrls(window, {QUrl(QStringLiteral("file:-report"))}));
    QVERIFY(!dropUrls(window, {videoUrl(), QUrl(QStringLiteral("file:http://example.com/a.mp4"))}));
    QVERIFY(backend.source().isEmpty());

    // Not while exporting: the export would mark the new edit as exported.
    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    backend.exportClips(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("drop-busy.mp4"))), wholeVideo(1.0));
    QVERIFY(backend.busy());
    QVERIFY(!dropUrls(window, {videoUrl()}));
    QCOMPARE(backend.timeline()->clips(), wholeVideo(1.0));
    QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 20000);
}

void BackendTests::portalPickerReplacesAnUnansweredRequest() {
    // The picker talks to whatever DBUS_SESSION_BUS_ADDRESS named when this
    // process first touched the session bus, and by the time a test runs
    // that is the real bus, where a real portal owns the name. So the
    // parent invocation starts a private daemon with no service
    // activation (nothing real can claim the name there) and re-runs just
    // this test in a child born on that bus.
    if (!qEnvironmentVariableIsSet("OMACUT_PORTAL_TEST_CHILD")) {
        const QString dbusDaemon = QStandardPaths::findExecutable(QStringLiteral("dbus-daemon"));
        if (dbusDaemon.isEmpty())
            QSKIP("dbus-daemon is not available");

        QTemporaryDir busDir;
        QVERIFY(busDir.isValid());
        const QString socketPath = busDir.filePath(QStringLiteral("bus.sock"));
        {
            QFile conf(busDir.filePath(QStringLiteral("bus.conf")));
            QVERIFY(conf.open(QIODevice::WriteOnly | QIODevice::Truncate));
            conf.write(QStringLiteral(
                "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\" "
                "\"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                "<busconfig><type>session</type>"
                "<listen>unix:path=%1</listen><auth>EXTERNAL</auth>"
                "<policy context=\"default\"><allow user=\"*\"/><allow own=\"*\"/>"
                "<allow send_destination=\"*\" eavesdrop=\"true\"/>"
                "<allow receive_sender=\"*\" eavesdrop=\"true\"/>"
                "<allow send_requested_reply=\"true\" send_type=\"method_return\"/>"
                "<allow send_requested_reply=\"true\" send_type=\"error\"/>"
                "<allow receive_requested_reply=\"true\"/></policy></busconfig>\n")
                           .arg(socketPath)
                           .toUtf8());
        }
        QProcess daemon;
        daemon.start(dbusDaemon, {QStringLiteral("--config-file"),
                                  busDir.filePath(QStringLiteral("bus.conf")),
                                  QStringLiteral("--fork"), QStringLiteral("--print-pid=1")});
        QVERIFY(daemon.waitForFinished(10000));
        const int daemonPid = QString::fromUtf8(daemon.readAllStandardOutput()).trimmed().toInt();
        QVERIFY(daemonPid > 0);

        QProcess child;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
                   QStringLiteral("unix:path=") + socketPath);
        env.insert(QStringLiteral("OMACUT_PORTAL_TEST_CHILD"), QStringLiteral("1"));
        child.setProcessEnvironment(env);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("portalPickerReplacesAnUnansweredRequest")});
        const bool finished = child.waitForFinished(60000);
        const QByteArray childOut = child.readAll();
        QProcess::execute(QStringLiteral("kill"), {QString::number(daemonPid)});
        QVERIFY2(finished, qPrintable(QString::fromUtf8(childOut)));
        QVERIFY2(child.exitCode() == 0, qPrintable(QString::fromUtf8(childOut)));
        return;
    }

    QVERIFY(QDBusConnection::sessionBus().isConnected());
    FakePortalService portal;
    QVERIFY(QDBusConnection::sessionBus().registerService(
        QStringLiteral("org.freedesktop.portal.Desktop")));
    QDBusConnection::sessionBus().registerObject(
        QStringLiteral("/org/freedesktop/portal/desktop"), &portal,
        QDBusConnection::ExportAllSlots);

    PortalFilePicker picker;
    QSignalSpy openSpy(&picker, &FilePicker::openSelected);
    QSignalSpy failedSpy(&picker, &FilePicker::failed);

    // The first request is never answered, as if the portal died with the
    // dialog up. Asking again must close it and open a fresh dialog
    // instead of dropping the request on the floor.
    picker.openVideo();
    QTRY_COMPARE_WITH_TIMEOUT(portal.openCalls, 1, 5000);
    picker.openVideo();
    QTRY_COMPARE_WITH_TIMEOUT(portal.openCalls, 2, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(portal.closeCalls, 1, 5000);
    QCOMPARE(failedSpy.count(), 0);

    // The replacement dialog's answer is picked up normally.
    portal.respondWith(videoUrl().toString());
    QTRY_COMPARE_WITH_TIMEOUT(openSpy.count(), 1, 5000);
    QCOMPARE(openSpy.first().at(0).toUrl(), videoUrl());

    QDBusConnection::sessionBus().unregisterObject(
        QStringLiteral("/org/freedesktop/portal/desktop"));
    QDBusConnection::sessionBus().unregisterService(
        QStringLiteral("org.freedesktop.portal.Desktop"));
}

QTEST_MAIN(BackendTests)
#include "backend_tests.moc"
