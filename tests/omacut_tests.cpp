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
#include <QMediaPlayer>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QStyleHints>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

#include "backend.h"
#include "filepicker.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "thumbprovider.h"
#include "translator.h"

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

// Like the real backend's, video keys are never reused: QML caches frames by
// URL for the whole process, so a key reused by a later test would be shown
// the earlier test's frames, or blanks.
static int nextThumbKey() {
    static int key = 0;
    return ++key;
}

class ShortcutBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    Q_PROPERTY(QVariantList videos READ videos NOTIFY videosChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool probing READ probing NOTIFY busyChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QObject *timeline READ timelineObject CONSTANT)

public:
    explicit ShortcutBackend(QUrl source, double duration, QObject *parent = nullptr)
        : QObject(parent), m_source(std::move(source)), m_duration(duration) {
        timeline.reset(duration);
        if (!m_source.isEmpty())
            m_videos = {QVariantMap{{QStringLiteral("url"), m_source}, {QStringLiteral("thumbKey"), nextThumbKey()}}};
    }

    QUrl source() const { return m_source; }
    QVariantList videos() const { return m_videos; }
    bool busy() const { return m_exporting; }
    bool probing() const { return false; }
    bool exporting() const { return m_exporting; }
    void setExporting(bool exporting) {
        m_exporting = exporting;
        emit busyChanged();
    }
    QString status() const { return {}; }
    QObject *timelineObject() { return &timeline; }

    Q_INVOKABLE void openVideoDialog() { ++openCount; }
    Q_INVOKABLE void addVideoDialog(double t) {
        ++addCount;
        lastAddAt = t;
    }
    Q_INVOKABLE void exportDialog() {
        ++exportCount;
        exportedClips = timeline.clips();
    }
    Q_INVOKABLE void saveFrame(double) {}
    Q_INVOKABLE void stepFrame(double t, int direction) { emit frameStepped(t, t + direction * 0.04); }
    Q_INVOKABLE bool addVideos(const QVariantList &, double) { return false; }

    void announceInfo() {
        timeline.reset(m_duration);
        emit infoChanged();
    }
    void announceExportDone() {
        timeline.markExported(exportedClips);
        emit exportDone(QStringLiteral("/tmp/exported.mp4"));
    }
    // What choosing a video of this length in the add dialog does.
    void addVideoOfLength(double duration, double t) {
        m_videos.append(QVariantMap{{QStringLiteral("url"), m_source}, {QStringLiteral("thumbKey"), nextThumbKey()}});
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
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void frameStepped(double from, double to);
    void frameStepFailed(const QString &message);
    void frameSaved(const QString &path);
    void frameFailed(const QString &message);
    void loadError(const QString &message);
    void exportReopenFailed();
    void skippedVideos(int count);

private:
    QUrl m_source;
    double m_duration;
    QVariantList m_videos;
    bool m_exporting = false;
};

// All of the first video: what a freshly loaded one edits.
static edit::Clips wholeVideo(double duration) {
    return {{0, 0.0, duration}};
}

// Finds a DialogButton by its label ("primary" tells them apart from Labels).
// A visible dialog button by its text. Walks the visual tree, since buttons
// made by a Repeater aren't QObject children of the window.
static QQuickItem *dialogButton(QQuickItem *item, const QString &text) {
    for (QQuickItem *child : item->childItems()) {
        if (child->isVisible() && child->property("primary").isValid() && child->property("text").toString() == text)
            return child;
        if (QQuickItem *found = dialogButton(child, text))
            return found;
    }
    return nullptr;
}

static QQuickItem *dialogButton(QQuickWindow *window, const QString &text) {
    return dialogButton(window->contentItem(), text);
}

static QPoint itemCenter(QQuickItem *item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

static QString mainQmlPath() {
    return QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath(
        QStringLiteral("../src/Main.qml"));
}

// Calls back when a string-connected signal fires.
class Relay : public QObject {
    Q_OBJECT
public:
    std::function<void()> callback;
public slots:
    void trigger() { if (callback) callback(); }
};

// Loads Main.qml against a stub backend and keeps the engine alive for as long
// as the window is in use, so each shortcut test is just the key presses.
class QmlHarness {
public:
    // framesFrom, a real video, serves the filmstrip for every video the
    // backend has, now and later; without it the filmstrip stays blank.
    explicit QmlHarness(QObject &backend, const QByteArray &source = {}, const QString &framesFrom = {})
        : m_backend(backend), m_framesFrom(framesFrom) {
        m_thumbs = new ThumbProvider;
        // Before loading: the filmstrip asks for frames as soon as it's built.
        serveFrames();
        m_relay.callback = [this] { serveFrames(); };
        if (backend.metaObject()->indexOfSignal("videosChanged()") >= 0)
            QObject::connect(&backend, SIGNAL(videosChanged()), &m_relay, SLOT(trigger()));
        m_engine.addImageProvider(QStringLiteral("thumbs"), m_thumbs);
        m_engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
        m_engine.rootContext()->setContextProperty(QStringLiteral("theme"), &m_theme);
        // English, from the catalog in src/, so counted strings get their forms.
        QCoreApplication::installTranslator(&m_translator);
        m_engine.rootContext()->setContextProperty(QStringLiteral("translator"), &m_translator);
        const QUrl url = QUrl::fromLocalFile(mainQmlPath());
        if (source.isEmpty())
            m_engine.load(url);
        else
            m_engine.loadData(source, url);
        if (!m_engine.rootObjects().isEmpty())
            m_window = qobject_cast<QQuickWindow *>(m_engine.rootObjects().first());
    }

    ~QmlHarness() { QCoreApplication::removeTranslator(&m_translator); }

    QQuickWindow *window() const { return m_window; }
    QQmlApplicationEngine &engine() { return m_engine; }
    QQuickItem *editBar() const {
        return m_window ? m_window->findChild<QQuickItem *>(QStringLiteral("editBar")) : nullptr;
    }

private:
    void serveFrames() {
        if (m_framesFrom.isEmpty())
            return;
        for (const QVariant &video : m_backend.property("videos").toList())
            m_thumbs->setVideo(video.toMap().value(QStringLiteral("thumbKey")).toInt(), m_framesFrom);
    }

    QObject &m_backend;
    QString m_framesFrom;
    // Declared before the engine, so it outlives the bindings that read it.
    QTemporaryDir m_themeDir;
    Theme m_theme{m_themeDir.path()};
    Translator m_translator{m_themeDir.filePath(QStringLiteral("language")),
                            QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath(QStringLiteral("../src/i18n"))};
    Relay m_relay;
    QQmlApplicationEngine m_engine;
    ThumbProvider *m_thumbs = nullptr;
    QQuickWindow *m_window = nullptr;
};

class BackendTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
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
    void frameStepsFollowPresentationTimes();
    void frameStepFailuresHaveTheirOwnNotice();
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
    void exportRefusesAnythingButAbsoluteLocalPaths();
    void exportStartFailureClearsBusy();
    void failedExportPreservesExistingFile();
    void qmlDoesNotCreateAudioOutputWithoutVideo();
    void qmlShortcutsTriggerBackendActions();
    void qmlArrowKeysMoveThePlayhead();
    void qmlFrameStepsSurviveRoundedPlayerFeedback();
    void qmlNumberKeysJumpThroughTheEdit();
    void qmlSpaceChordsSetTheClipEdges();
    void qmlKeysSplitAndRemoveClips();
    void qmlBracketsJumpBetweenClipEdges();
    void qmlKeysMoveAndAddClips();
    void qmlZoomFocusesTheClip();
    void qmlTrimKeepsTheTimelineScale();
    void qmlZoomingOutRestoresTheFilmstrip();
    void qmlTimelineViewDependsOnlyOnTheEdit_data();
    void qmlTimelineViewDependsOnlyOnTheEdit();
    void qmlTrackRefitsAfterRemovingClips();
    void qmlSplitPiecesTrimApart();
    void qmlMovingAPieceKeepsTheCut();
    void qmlFilmstripHoldsStillUnderADrag();
    void qmlPressingHandleScrubsItsFrame();
    void formatTimeShowsHoursFromSixtyMinutes();
    void qmlDropOpensThenAddsVideos();
    void qmlDropTakesOnlyLocalFiles();
    void qmlDropLandsAfterThePlayheadsClip();
    void qmlQuitConfirmsUnexportedEdit();
    void qmlQuitAsksWhileExporting();
    void qmlOpenConfirmsUnexportedEdit();
    void qmlHelpOverlayBlocksEditingKeys();
    void timelineSplitsTrimsAndJoins();
    void timelineUndoesAGestureAsOneStep();
    void timelineTracksUnexportedCuts();
    void timelineAddsAndMovesVideos();
    void timelineSeeksAcrossARealCut();
    void playbackSeeksAcrossARealCut_data();
    void playbackSeeksAcrossARealCut();
    void timelineAnswersWhereTimesFall();
    void timelineEditsAtATime();
    void trimArgsReencodeForPreciseCuts();
    void trimArgsConcatenateTheRanges();
    void trimArgsFitDifferentVideosTogether();
    void trimArgsScaleTheShorterSide();
    void exportHeightsNeverUpscale();
    void thumbnailPastTheLastFrameShowsTheLastOne();
    void themeAccentReadsOmarchyColors();
    void themeAccentForegroundKeepsContrast();
    void themeFollowsOmarchyInstalledLater();
    void themeFollowsASymlinkSwitch();
    void themeFollowsHyprlandRounding();
    void translatorFallsBackToEnglish();
    void translatorFollowsTheLanguageFile();
    void translatorPicksPluralForms();
    void catalogsMatchTheCode();
    void qmlDialogsAnswerFromTheKeyboard();
    void qmlDialogsAnswerToABareLetter();
    void stubBackendMatchesTheRealOne();
    void portalPickerReplacesAnUnansweredRequest();

private:
    QUrl videoUrl() const { return QUrl::fromLocalFile(m_videoPath); }
    QString formatName(const QString &path) const;
    // A test pattern, with a tone when audio is set, written into m_dir.
    // Its frames differ; `rate` is frames a second, as ffmpeg takes it ("1/5").
    QString makeVideo(const QString &name, double duration, bool audio,
                      const QString &size = QStringLiteral("32x32"),
                      const QString &rate = QStringLiteral("1"));
    void waitForBackgroundWork(Backend &backend);
    // Probing finishes in the background now: wait until it has settled.
    void waitForProbe(Backend &backend);
    bool installBrokenFfmpeg(const QString &dirPath);

    QTemporaryDir m_dir;
    QString m_videoPath;
};

// A QML warning, such as a TypeError from a binding or a call the backend
// doesn't have, fails the test that caused it; otherwise QML only logs it and
// the test passes with the UI broken.
void BackendTests::init() {
    QTest::failOnWarning(QRegularExpression(QStringLiteral("\\.qml:\\d+")));
}

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

void BackendTests::frameStepsFollowPresentationTimes() {
    const QString path = m_dir.filePath(QStringLiteral("step-vfr.mkv"));
    QProcess proc;
    // Irregular presentation times, with B-frame reordering in the file.
    proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {
        "-v", "error", "-f", "lavfi", "-i", "testsrc=size=32x32:rate=10:duration=1",
        "-vf", "select='eq(n,0)+eq(n,1)+eq(n,4)+eq(n,8)'",
        "-fps_mode", "vfr", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y", path
    });
    QVERIFY(proc.waitForFinished(10000));
    QCOMPARE(proc.exitCode(), 0);
    const auto times = ffmpeg::frameTimes(path, 0);
    QCOMPARE(times, (QList<double>{0.0, 0.1, 0.4, 0.8}));
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QVERIFY(backend.load(QUrl::fromLocalFile(path)));
    waitForProbe(backend);
    QSignalSpy stepped(&backend, &Backend::frameStepped);
    const auto step = [&](double from, int direction, double expected) {
        stepped.clear();
        backend.stepFrame(from, direction);
        QTRY_COMPARE_WITH_TIMEOUT(stepped.count(), 1, 10000);
        QVERIFY(qAbs(stepped.first().at(1).toDouble() - expected) < 0.000001);
    };
    step(0, 1, 0.1);
    step(0.1, 1, 0.4);
    step(0.4, -1, 0.1);
    step(0.6, -1, 0.1);
    step(0, -1, 0);
    step(backend.timeline()->duration(), 1, backend.timeline()->duration());
    step(backend.timeline()->duration(), -1, 0.8);
    backend.timeline()->split(0.4);
    step(0.4, -1, 0.1);
    step(0.1, 1, 0.4);
    // Trimmed clips still use their source's timestamps.
    backend.timeline()->setClip(1, 0.5, 0.9);
    step(0.4, 1, 0.7);
    step(0.7, -1, 0.4);

    // A seek to this cut rounds up to 0.401s in the player. It must
    // still step into the previous clip rather than returning to the cut.
    backend.timeline()->reset(0.9);
    backend.timeline()->split(0.4006);
    step(0.401, -1, 0.4);
    step(0.4, -1, 0.1);

    // The last frame can start less than half a millisecond before a cut.
    // After reaching it, the next press must leave that frame behind.
    backend.timeline()->reset(0.9);
    backend.timeline()->split(0.4004);
    step(0.4004, -1, 0.4);
    step(0.4, -1, 0.1);
}

void BackendTests::frameStepFailuresHaveTheirOwnNotice() {
    const QString path = m_dir.filePath(QStringLiteral("step-removed.mp4"));
    QVERIFY(QFile::copy(m_videoPath, path));
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QVERIFY(backend.load(QUrl::fromLocalFile(path)));
    waitForProbe(backend);
    QVERIFY(QFile::remove(path));
    QSignalSpy stepFailed(&backend, &Backend::frameStepFailed);
    QSignalSpy saveFailed(&backend, &Backend::frameFailed);
    backend.stepFrame(0, 1);
    QTRY_COMPARE_WITH_TIMEOUT(stepFailed.count() + saveFailed.count(), 1, 10000);
    QCOMPARE(stepFailed.count(), 1);
    QCOMPARE(saveFailed.count(), 0);

    ShortcutBackend display(QUrl(), 0);
    QmlHarness harness(display);
    QVERIFY(harness.window());
    const QString message = stepFailed.first().first().toString();
    emit display.frameStepFailed(message);
    QCOMPARE(harness.window()->property("noticeText").toString(),
             QStringLiteral("Could not step to the frame: ") + message);
    emit display.frameFailed(QStringLiteral("save error"));
    QCOMPARE(harness.window()->property("noticeText").toString(),
             QStringLiteral("Could not save the frame: save error"));
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

QString BackendTests::makeVideo(const QString &name, double duration, bool audio, const QString &size,
                                const QString &rate) {
    const QString path = m_dir.filePath(name);
    QStringList args = {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=%1:rate=%2:duration=%3").arg(size, rate).arg(duration),
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

void BackendTests::exportRefusesAnythingButAbsoluteLocalPaths() {
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);
    QVERIFY(backend.load(videoUrl()));
    waitForProbe(backend);
    waitForBackgroundWork(backend);

    // Neither may become ".mp4" or "out.mp4" in the working directory.
    const QString cwd = QDir::currentPath();
    QVERIFY(QDir::setCurrent(m_dir.path()));
    backend.exportClips(QUrl(QStringLiteral("smb://server/share/out.mp4")), wholeVideo(1.0));
    backend.exportClips(QUrl(QStringLiteral("file:out.mp4")), wholeVideo(1.0));
    QVERIFY(QDir::setCurrent(cwd));
    // Nor a file URL naming a host, which would become "//server/share/out.mp4".
    backend.exportClips(QUrl(QStringLiteral("file://server/share/out.mp4")), wholeVideo(1.0));
    QCOMPARE(failedSpy.count(), 3);
    QVERIFY(!backend.busy());
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral(".mp4"))));
    QVERIFY(!QFileInfo::exists(m_dir.filePath(QStringLiteral("out.mp4"))));
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

    // A tap of Alt+arrow steps one frame (0.04s in this backend).
    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 6.04) < 0.000001, 3000);
    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 6.0) < 0.000001, 3000);

    // Holding it repeats faster than any tap: the first press steps one
    // frame, and each repeat after it moves 0.2s. After a pause, the next
    // press is a single frame again.
    QTest::qWait(150);
    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 6.44) < 0.000001, 3000);
    QTest::qWait(150);
    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 6.0) < 0.000001, 3000);

    // A step cancels a pending play on a source that's still loading, so
    // playback can't start under the stepped frame.
    auto *player = window->findChild<QMediaPlayer *>();
    QVERIFY(player);
    QVERIFY(player->setProperty("pendingPlay", true));
    QTest::qWait(150);
    QTest::keyClick(window, Qt::Key_Left, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 5.96) < 0.000001, 3000);
    QCOMPARE(player->property("pendingPlay").toBool(), false);
    QTest::qWait(150);
    QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 6.0) < 0.000001, 3000);

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

void BackendTests::qmlFrameStepsSurviveRoundedPlayerFeedback() {
    // MP4 retains sub-millisecond timestamps; Matroska's millisecond
    // time base would round away the very condition being tested.
    const QString path = m_dir.filePath(QStringLiteral("step-24fps.mp4"));
    QProcess proc;
    proc.start(QStandardPaths::findExecutable(QStringLiteral("ffmpeg")), {
        "-v", "error", "-f", "lavfi", "-i", "testsrc=size=32x32:rate=24:duration=1",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-y", path
    });
    QVERIFY(proc.waitForFinished(10000));
    QCOMPARE(proc.exitCode(), 0);
    ThumbProvider provider;
    Backend backend(&provider, new FakeFilePicker);
    QVERIFY(backend.load(QUrl::fromLocalFile(path)));
    waitForProbe(backend);
    backend.timeline()->setClip(0, 0, 0.1668);
    // Silent video tests need no connection to the desktop audio server.
    const QString sourceDir = QUrl::fromLocalFile(QFileInfo(mainQmlPath()).absolutePath()).toString();
    QmlHarness harness(backend, QString("import \"%1\" as Editor\n"
                                        "Editor.Main { function ensureAudioOutput() {} }")
                                    .arg(sourceDir).toUtf8());
    QVERIFY(harness.window());
    auto *player = harness.window()->findChild<QMediaPlayer *>();
    QVERIFY(player);
    emit backend.infoChanged();
    QTRY_COMPARE_WITH_TIMEOUT(player->playbackState(), QMediaPlayer::PausedState, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!player->property("priming").toBool(), 10000);
    QVERIFY(QMetaObject::invokeMethod(harness.window(), "seekTo", Q_ARG(QVariant, 0.1668)));
    QSignalSpy stepped(&backend, &Backend::frameStepped);
    // Each step starts with the playhead produced by the real QML handler,
    // after feeding back the player's rounded source position.
    for (double expected : {0.166667, 0.125, 0.083333, 0.041667, 0.0}) {
        stepped.clear();
        backend.stepFrame(playhead(harness), -1);
        QTRY_COMPARE_WITH_TIMEOUT(stepped.count(), 1, 10000);
        const double resolved = stepped.first().at(1).toDouble();
        QVERIFY(qAbs(resolved - expected) < 0.000001);
        const qint64 rounded = qRound64(resolved * 1000);
        player->setPosition(rounded);
        // Emit even when the rounded position has not changed (167ms
        // for both the clip end and its final frame).
        emit player->positionChanged(rounded);
        QVERIFY(qAbs(playhead(harness) - expected) < 0.000001);
    }
    // A subsequent ordinary seek must replace the remembered frame time.
    QVERIFY(QMetaObject::invokeMethod(harness.window(), "seekTo", Q_ARG(QVariant, 0.1668)));
    emit player->positionChanged(167);
    QVERIFY(qAbs(playhead(harness) - 0.1668) < 0.000001);
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
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 0.0, 15.0}}), 3000);
    // The playhead stays at the cut, on the clip's last frame, as after
    // dragging the end handle there, and pressing again trims nothing more.
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 14.999, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(50);
    QCOMPARE(backend.timeline.clips(), (edit::Clips{{0, 0.0, 15.0}}));

    // Same for the start, at 5 s. The playhead stays on its frame, which is
    // now the clip's first: the start of the edit, and still over the clip's
    // left edge on the track, which keeps its place.
    QQuickItem *editBar = harness.editBar();
    editBar->setProperty("playheadSec", 5.0);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 5.0, 15.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.0, 3000);
    QCOMPARE(editBar->property("positions").toList().value(0).toMap().value(QStringLiteral("start")).toDouble(), 5.0);
    QVariant playheadOnTrack;
    QVERIFY(QMetaObject::invokeMethod(editBar, "trackTime", Q_RETURN_ARG(QVariant, playheadOnTrack), Q_ARG(QVariant, playhead(harness))));
    QCOMPARE(playheadOnTrack.toDouble(), 5.0);
    // So pressing it again trims nothing more, instead of another 5 s each time.
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTest::qWait(50);
    QCOMPARE(backend.timeline.clips(), (edit::Clips{{0, 5.0, 15.0}}));

    // The edges never cross: pulling the end onto the start leaves the clip be.
    QTest::keyClick(window, Qt::Key_Left, Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), 0.0, 3000);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTest::qWait(50);
    QCOMPARE(backend.timeline.clips(), (edit::Clips{{0, 5.0, 15.0}}));

    // Trimming while the video is still loading to play leaves it to play
    // once loaded, from the playhead's frame.
    auto *player = window->findChild<QMediaPlayer *>();
    QVERIFY(player);
    editBar->setProperty("playheadSec", 2.0);
    QVERIFY(player->setProperty("pendingMs", 1));
    QVERIFY(player->setProperty("pendingPlay", true));
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 7.0, 15.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(player->property("pendingMs").toInt(), 7000, 3000);
    QCOMPARE(player->property("pendingPlay").toBool(), true);
    editBar->setProperty("playheadSec", 3.0);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 7.0, 10.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(player->property("pendingMs").toInt(), 9999, 3000);
    QCOMPARE(player->property("pendingPlay").toBool(), true);
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
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 0.04) < 0.000001, 3000);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.addCount, 1, 3000);
    QCOMPARE(backend.lastAddAt, 0.04);
    backend.addVideoOfLength(3.0, backend.lastAddAt);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{0, 0.0, 5.0}, {1, 0.0, 3.0}, {0, 5.0, 20.0}}), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

    // Alt+] moves the clip under the playhead later, and the playhead moves
    // with it, still one frame (0.04 s) in.
    QTest::keyClick(window, Qt::Key_BracketRight, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{1, 0.0, 3.0}, {0, 0.0, 5.0}, {0, 5.0, 20.0}}), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 3.04) < 0.000001, 3000);

    // At the end already, Alt+] does nothing; Alt+[ moves it back.
    QTest::keyClick(window, Qt::Key_BracketRight, Qt::AltModifier);
    QTest::keyClick(window, Qt::Key_BracketRight, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{1, 0.0, 3.0}, {0, 5.0, 20.0}, {0, 0.0, 5.0}}), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 18.04) < 0.000001, 3000);
    QTest::keyClick(window, Qt::Key_BracketLeft, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(),
                              (edit::Clips{{1, 0.0, 3.0}, {0, 0.0, 5.0}, {0, 5.0, 20.0}}), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(qAbs(playhead(harness) - 3.04) < 0.000001, 3000);
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
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
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
    // Zoomed out, the track spans the whole video: the clip and what's trimmed off its end.
    QCOMPARE(editBar->property("windowEnd").toDouble(), 20.0);
}

// Where clip index draws on the track, in track seconds.
static QPair<double, double> clipPlace(QQuickItem *editBar, int index) {
    const QVariantMap at = editBar->property("positions").toList().value(index).toMap();
    return {at.value(QStringLiteral("start")).toDouble(), at.value(QStringLiteral("end")).toDouble()};
}

void BackendTests::qmlTrimKeepsTheTimelineScale() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("windowEnd").toDouble(), 20.0, 3000);

    // Trimming either end leaves the track at the video's length, with the
    // cut-off footage dimmed, rather than stretching the clip to fill it.
    backend.timeline.setClip(0, 3.0, 15.0);
    QTest::qWait(50);
    QCOMPARE(editBar->property("windowEnd").toDouble(), 20.0);
    QCOMPARE(clipPlace(editBar, 0), qMakePair(3.0, 15.0));

    // So do split pieces trimmed apart.
    backend.timeline.setClip(0, 0.0, 20.0);
    backend.timeline.split(10.0);
    backend.timeline.setClip(0, 0.0, 6.0);
    backend.timeline.setClip(1, 14.0, 18.0);
    QTest::qWait(50);
    QCOMPARE(editBar->property("windowEnd").toDouble(), 20.0);

    // A clip that goes back over footage the one before used starts the
    // video over after it; the track fits both.
    backend.timeline.setClip(1, 5.0, 20.0);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("windowEnd").toDouble(), 40.0, 3000);

    // Another video follows the whole of the one before, so trimming the end
    // of a clip from that one leaves it where it was.
    backend.timeline.restore({20.0}, {{0, 0.0, 20.0}});
    backend.addVideoOfLength(10.0, 19.0);
    QTRY_COMPARE_WITH_TIMEOUT(clipPlace(editBar, 1), qMakePair(20.0, 30.0), 3000);
    backend.timeline.setClip(0, 0.0, 12.0);
    QTest::qWait(50);
    QCOMPARE(clipPlace(editBar, 1), qMakePair(20.0, 30.0));
    QCOMPARE(editBar->property("windowEnd").toDouble(), 30.0);
}

void BackendTests::qmlTrackRefitsAfterRemovingClips() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();

    // Out of order, the pieces lay out past the video's length...
    backend.timeline.split(10.0);
    backend.timeline.moveClip(1, 0);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("windowEnd").toDouble(), 40.0, 3000);

    // ...and once a piece is removed, the track fits what's left, with no
    // empty stretch past the footage.
    backend.timeline.removeClip(0);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("windowEnd").toDouble(), 20.0, 3000);
    QCOMPARE(clipPlace(editBar, 0), qMakePair(0.0, 10.0));

    // Zoomed on a clip, removing it zooms back out to what's left, wherever
    // that is: here the clip from 15 s, right of the zoomed view.
    backend.timeline.restore({20.0}, {{0, 0.0, 5.0}, {0, 15.0, 20.0}});
    editBar->setProperty("playheadSec", 1.0);
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_VERIFY_WITH_TIMEOUT(editBar->property("zoomed").toBool(), 3000);
    QVERIFY(editBar->property("windowEnd").toDouble() < 15.0);
    backend.timeline.removeClip(0);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), false, 3000);
}

static QList<QQuickItem *> itemsNamed(QQuickItem *item, const QString &name) {
    QList<QQuickItem *> found;
    for (QQuickItem *child : item->childItems()) {
        if (child->objectName() == name) found << child;
        found << itemsNamed(child, name);
    }
    return found;
}

// Each filmstrip frame on the track: the item, its source, and where it
// sits. A frame that is rebuilt, reloads, or moves shows up as a difference.
struct Frame {
    QQuickItem *item;
    QString source;
    double x;
    bool operator==(const Frame &o) const { return item == o.item && source == o.source && x == o.x; }
};
static QList<Frame> filmstripFrames(QQuickItem *item) {
    QList<Frame> frames;
    const QList<QQuickItem *> children = item->childItems();
    for (QQuickItem *child : children) {
        if (child->inherits("QQuickImage") && child->isVisible())
            frames.append({child, child->property("source").toUrl().toString(), child->mapToScene(QPointF()).x()});
        frames.append(filmstripFrames(child));
    }
    return frames;
}

void BackendTests::qmlFilmstripHoldsStillUnderADrag() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();
    QVERIFY(editBar);
    backend.timeline.setClip(0, 0.0, 12.0);
    QTest::qWait(50);

    // The footage trimmed off the end stays on the track, dimmed.
    const QList<Frame> before = filmstripFrames(editBar);
    const double clipEnd = editBar->mapToScene(QPointF(editBar->width() * 12.0 / 20.0, 0)).x();
    QVERIFY(std::any_of(before.begin(), before.end(), [&](const Frame &frame) {
        return frame.x + frame.item->width() > clipEnd;
    }));
    const QList<QQuickItem *> cuts = itemsNamed(editBar, QStringLiteral("cut"));
    QVERIFY(std::any_of(cuts.begin(), cuts.end(), [&](QQuickItem *cut) {
        const double x = cut->mapToScene(QPointF()).x();
        return cut->isVisible() && qAbs(x - clipEnd) <= 1 && qAbs(x + cut->width() - editBar->mapToScene(QPointF(editBar->width(), 0)).x()) <= 1;
    }));

    // Dragging the end handle in and back out moves only the frame around
    // the clip: no filmstrip frame reloads, is rebuilt, or shifts.
    const double y = editBar->height() / 2.0;
    auto at = [&](double x) { return editBar->mapToScene(QPointF(x, y)).toPoint(); };
    const double handle = editBar->width() * 12.0 / 20.0 - 5.0;
    QTest::mouseMove(window, at(handle));
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at(handle));
    QVERIFY(filmstripFrames(editBar) == before);
    for (const double dx : {-20.0, -60.0, -100.0, -40.0, 30.0, 80.0}) {
        QTest::mouseMove(window, at(handle + dx));
        QVERIFY2(filmstripFrames(editBar) == before, qPrintable(QStringLiteral("dragged by %1").arg(dx)));
    }
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at(handle + 80.0));
    QVERIFY(backend.timeline.clips().first().out > 12.0);
}

void BackendTests::qmlZoomingOutRestoresTheFilmstrip() {
    const QString video = makeVideo(QStringLiteral("zoom.mp4"), 20.0, false, QStringLiteral("64x36"));
    QVERIFY(!video.isEmpty());
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend, {}, video);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();
    backend.timeline.split(5.0);
    backend.timeline.split(15.0);
    auto frames = [&] {
        QStringList list;
        for (const Frame &frame : filmstripFrames(editBar))
            list << frame.source + QStringLiteral(" at ") + QString::number(frame.x)
                    + QStringLiteral(" status ") + QString::number(frame.item->property("status").toInt());
        return list;
    };
    auto allLoaded = [&] {
        const QList<Frame> list = filmstripFrames(editBar);
        return !list.isEmpty() && std::all_of(list.begin(), list.end(), [](const Frame &frame) {
            return frame.item->property("status").toInt() == 1 && frame.item->implicitWidth() > 0;
        });
    };
    QTRY_VERIFY_WITH_TIMEOUT(allLoaded(), 10000);
    const QStringList before = frames();

    // Zoom in, and straight back out before the closer frames have loaded.
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), true, 3000);
    QTest::qWait(20);
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), false, 3000);
    QTRY_VERIFY2_WITH_TIMEOUT(allLoaded(), qPrintable(frames().join(QStringLiteral("\n"))), 10000);
    QCOMPARE(frames(), before);

    // And again, letting the zoomed-in frames load first.
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), true, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(allLoaded(), 10000);
    QTest::keyClick(window, Qt::Key_Z);
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("zoomed").toBool(), false, 3000);
    QTRY_VERIFY2_WITH_TIMEOUT(allLoaded(), qPrintable(frames().join(QStringLiteral("\n"))), 10000);
    QCOMPARE(frames(), before);
}

void BackendTests::qmlSplitPiecesTrimApart() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();
    QVERIFY(editBar);
    backend.timeline.split(10.0);
    QTest::qWait(50);
    const double y = editBar->height() / 2.0;
    auto at = [&](double x) { return editBar->mapToScene(QPointF(x, y)).toPoint(); };
    auto drag = [&](double from, double by) {
        QTest::mouseMove(window, at(from));
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at(from));
        for (int step = 1; step <= 6; ++step) QTest::mouseMove(window, at(from + by * step / 6));
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at(from + by));
        QTest::qWait(400);  // Past the double-click interval, so the next press is a fresh one.
    };
    const double perSec = editBar->width() / 20.0;

    // Pull the second piece's start away from the split: the footage between
    // is left out, and the piece stays over its own footage, not closed up
    // against the first.
    drag(10.0 * perSec + 6, 60);
    edit::Clips clips = backend.timeline.clips();
    QCOMPARE(clips.size(), 2);
    QCOMPARE(clips[0].out, 10.0);
    QVERIFY(clips[1].in > 10.0);
    const double secondIn = clips[1].in;
    QCOMPARE(clipPlace(editBar, 0), qMakePair(0.0, 10.0));
    QCOMPARE(clipPlace(editBar, 1), qMakePair(secondIn, 20.0));

    // Then the first piece's end: the gap widens, and both stay put.
    drag(10.0 * perSec - 6, -60);
    clips = backend.timeline.clips();
    QCOMPARE(clips.size(), 2);
    QVERIFY(clips[0].out < 10.0);
    QCOMPARE(clips[1].in, secondIn);
    QCOMPARE(clipPlace(editBar, 0), qMakePair(0.0, clips[0].out));
    QCOMPARE(clipPlace(editBar, 1), qMakePair(secondIn, 20.0));

    // Clicking in the gap puts the playhead on the second piece's start, and
    // the sequence still plays the pieces back to back.
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, at((clips[0].out + secondIn) / 2.0 * perSec));
    QTRY_COMPARE_WITH_TIMEOUT(playhead(harness), clips[0].length(), 3000);

    // Dragging the second piece's start back restores the cut footage.
    drag(secondIn * perSec + 6, -(secondIn - 10.0) * perSec - 40);
    clips = backend.timeline.clips();
    QVERIFY(clips[1].in < secondIn);
    QCOMPARE(clipPlace(editBar, 1).first, clips[1].in);
}

void BackendTests::qmlMovingAPieceKeepsTheCut() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QQuickItem *editBar = harness.editBar();
    QVERIFY(editBar);
    backend.timeline.split(10.0);
    backend.timeline.setClip(1, 12.0, 20.0);
    QTest::qWait(50);
    QCOMPARE(clipPlace(editBar, 1), qMakePair(12.0, 20.0));

    // Hold the second piece and drop it before the first.
    const double y = editBar->height() / 2.0, perSec = editBar->width() / 20.0;
    auto at = [&](double x) { return editBar->mapToScene(QPointF(x, y)).toPoint(); };
    QTest::mouseMove(window, at(16.0 * perSec));
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, at(16.0 * perSec));
    QTest::qWait(450);
    for (int step = 1; step <= 8; ++step) QTest::mouseMove(window, at((16.0 - step * 2.0) * perSec));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, at(0.0));
    QTRY_COMPARE_WITH_TIMEOUT(backend.timeline.clips(), (edit::Clips{{0, 12.0, 20.0}, {0, 0.0, 10.0}}), 3000);

    // Out of order, nothing is cut between them, so they sit back to back,
    // the first over its own footage and the track grown to fit.
    QCOMPARE(clipPlace(editBar, 0), qMakePair(12.0, 20.0));
    QCOMPARE(clipPlace(editBar, 1), qMakePair(20.0, 30.0));
    QTRY_COMPARE_WITH_TIMEOUT(editBar->property("windowEnd").toDouble(), 40.0, 3000);
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
    // The clip stays over its footage, 3 s along the 20 s track.
    const QPoint leftHandle = editBar->mapToScene(QPointF(editBar->width() * 3.0 / 20.0 + 5.0, editBar->height() / 2.0)).toPoint();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, leftHandle);
    QCOMPARE(scrubClipSpy.count(), 1);
    QCOMPARE(scrubClipSpy.first().at(0).toInt(), 0);
    QCOMPARE(scrubClipSpy.first().at(1).toDouble(), 3.0);
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, leftHandle);

    // Pressing the trailing handle immediately previews the clip's out-point.
    // The trim keeps the track's 20 s scale, and the clip ends at 15 s.
    const QPoint rightHandle = editBar->mapToScene(QPointF(editBar->width() * 15.0 / 20.0 - 5.0, editBar->height() / 2.0)).toPoint();
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

void BackendTests::qmlQuitAsksWhileExporting() {
    // An untouched video has nothing unexported, but exporting it (say, down
    // to 720p) is work a quit would throw away.
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QCOMPARE(window->property("unexported").toBool(), false);
    backend.setExporting(true);

    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QVERIFY(window->isVisible());
    // There's no Export to start: only Cancel and Quit.
    QVERIFY(dialogButton(window, QStringLiteral("Cancel")));
    QVERIFY(!dialogButton(window, QStringLiteral("Export")));

    // Arrows cycle Cancel and Quit only, starting from Cancel.
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Right);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);
    QVERIFY(window->isVisible());

    // Closing the window asks the same, and the question goes away with the export.
    window->close();
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QVERIFY(window->isVisible());
    backend.setExporting(false);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);

    QSignalSpy quitSpy(&harness.engine(), &QQmlApplicationEngine::quit);
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 3000);
    QCOMPARE(quitSpy.count(), 1);
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
    // Right didn't seek: the playhead is still on the trimmed clip's first frame.
    QCOMPARE(playhead(harness), 0.0);

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
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier | Qt::ShiftModifier);
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
    // A hair off, as a dragged handle leaves it, counts as carrying on, the
    // same as playback and export see it.
    timeline.setClip(1, 4.0004, 10.0);
    QVERIFY(timeline.canJoin(0));
    timeline.joinClips(0);
    QCOMPARE(timeline.clips(), wholeVideo(10.0));
    // The last clip can't be removed.
    timeline.removeClip(0);
    QCOMPARE(timeline.clips(), wholeVideo(10.0));

    // Touching pieces of the same stretch export as one.
    timeline.split(5.0);
    QCOMPARE(edit::merged(timeline.clips()), wholeVideo(10.0));

    // A dragged edge stops at the piece that picks up after it in the same
    // video, so a drag never reuses footage the next piece shows...
    timeline.moveEdge(0, false, 8.0);
    timeline.moveEdge(1, true, 2.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 5.0}, {0, 5.0, 10.0}}));
    // ...but crosses a cut up to it.
    timeline.setClip(0, 0.0, 3.0);
    timeline.setClip(1, 7.0, 10.0);
    timeline.moveEdge(0, false, 9.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 7.0}, {0, 7.0, 10.0}}));
    timeline.setClip(0, 0.0, 3.0);
    timeline.moveEdge(1, true, 1.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 0.0, 3.0}, {0, 3.0, 10.0}}));
    // A piece that goes back in the video doesn't stop it.
    timeline.restore({10.0}, {{0, 5.0, 8.0}, {0, 0.0, 3.0}});
    timeline.moveEdge(0, false, 9.5);
    timeline.moveEdge(1, true, 0.0);
    QCOMPARE(timeline.clips(), (edit::Clips{{0, 5.0, 9.5}, {0, 0.0, 3.0}}));
}

void BackendTests::timelineSeeksAcrossARealCut() {
    const edit::Clip left{0, 0.0, 5.0};
    QVERIFY(edit::continues(left, {0, 5.0, 8.0}));
    QVERIFY(edit::continues(left, {0, 5.0004, 8.0}));
    QVERIFY(!edit::continues(left, {0, 5.04, 8.0}));
    QVERIFY(!edit::continues(left, {1, 5.0, 8.0}));

    Timeline timeline;
    timeline.reset(10.0);
    timeline.split(5.0);
    QVERIFY(timeline.continues(0));
    timeline.setClip(1, 5.04, 10.0);
    QVERIFY(!timeline.continues(0));
    QCOMPARE(edit::merged(timeline.clips()).size(), 2);
}

void BackendTests::playbackSeeksAcrossARealCut_data() {
    QTest::addColumn<double>("gap");
    QTest::addColumn<int>("expectedPosition");
    QTest::newRow("exact split plays through") << 0.0 << 4980;
    QTest::newRow("split float noise plays through") << 0.0004 << 4980;
    QTest::newRow("40ms cut seeks forward") << 0.04 << 5040;
    QTest::newRow("40ms overlap seeks backward") << -0.04 << 4960;
}

void BackendTests::playbackSeeksAcrossARealCut() {
    QFETCH(double, gap);
    QFETCH(int, expectedPosition);
    ShortcutBackend backend(QUrl::fromLocalFile(m_videoPath), 10.0);
    backend.timeline.split(5.0);
    backend.timeline.setClip(1, 5.0 + gap, 10.0);

    // Run the actual Main.qml position handler and show() method. Only the
    // media clock/codec is replaced, so a restored 50ms check fails this test.
    QFile main(mainQmlPath());
    QVERIFY(main.open(QIODevice::ReadOnly));
    QByteArray source = main.readAll();
    QCOMPARE(source.count("    MediaPlayer {"), 1);
    source.replace("    MediaPlayer {", "    PlaybackTest.FakeMediaPlayer {");
    source.prepend("import \"../tests\" as PlaybackTest\n");
    QmlHarness harness(backend, source);
    QVERIFY(harness.window());
    QObject *player = harness.window()->findChild<QObject *>(QStringLiteral("playbackTestPlayer"));
    QVERIFY(player);
    QVERIFY(player->setProperty("source", backend.source()));
    QVERIFY(QMetaObject::invokeMethod(player, "show", Q_ARG(QVariant, 0),
                                     Q_ARG(QVariant, 0.0), Q_ARG(QVariant, true)));
    QCOMPARE(player->property("clip").toInt(), 0);

    // A playback tick reaches the transition window. A split advances the
    // clip without writing position; a cut must seek to the next in-point.
    QVERIFY(player->setProperty("position", 4980));
    QCOMPARE(player->property("clip").toInt(), 1);
    QCOMPARE(player->property("position").toInt(), expectedPosition);
    QCOMPARE(player->property("playbackState").toInt(), int(QMediaPlayer::PlayingState));
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

void BackendTests::thumbnailPastTheLastFrameShowsTheLastOne() {
    // Past a video's last frame ffmpeg finds nothing, which left the
    // filmstrip blank at the end. At a frame a second the last of 3 s is at
    // 2 s; at one every 5 s, the last of 10 s is at 5 s, too far back for a
    // fixed step back to find.
    const QString perSecond = makeVideo(QStringLiteral("slow.mp4"), 3.0, false);
    const QString perFive = makeVideo(QStringLiteral("slower.mp4"), 10.0, false,
                                      QStringLiteral("32x32"), QStringLiteral("1/5"));
    QVERIFY(!perSecond.isEmpty() && !perFive.isEmpty());
    const QImage last = ffmpeg::thumbnail(perSecond, 2.0, 36);
    QVERIFY(!last.isNull());
    QVERIFY(ffmpeg::thumbnail(perSecond, 1.0, 36) != last);
    QCOMPARE(ffmpeg::thumbnail(perSecond, 2.9, 36), last);
    QCOMPARE(ffmpeg::thumbnail(perFive, 9.9, 36), ffmpeg::thumbnail(perFive, 5.0, 36));
    QVERIFY(ffmpeg::thumbnail(perFive, 9.9, 36) != ffmpeg::thumbnail(perFive, 0.0, 36));
}

void BackendTests::themeAccentReadsOmarchyColors() {
    const QString fallback = QStringLiteral("#FFD60A");
    const QString colorsPath = m_dir.filePath(QStringLiteral("colors.toml"));

    // No file at all — the non-omarchy case — keeps the fallback.
    QCOMPARE(Theme::readAccent(m_dir.filePath(QStringLiteral("missing.toml"))), fallback);

    const auto writeColors = [&colorsPath](const char *contents) {
        QFile file(colorsPath);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(contents);
    };

    writeColors("# omarchy theme\n"
                "mode = \"dark\"\n"
                "background = \"#121212\"\n"
                "accent = \"#33ccff\"\n");
    QCOMPARE(Theme::readAccent(colorsPath), QStringLiteral("#33ccff"));

    writeColors("accent = '#aabbcc'\n");
    QCOMPARE(Theme::readAccent(colorsPath), QStringLiteral("#aabbcc"));

    // Themes annotate their palette, and the note is not part of the colour.
    writeColors("accent = \"#4ECDC4\"          # secondary-400 -- the paired accent\n");
    QCOMPARE(Theme::readAccent(colorsPath), QStringLiteral("#4ECDC4"));

    writeColors("accent = '#aabbcc'  # single quoted, with a note\n");
    QCOMPARE(Theme::readAccent(colorsPath), QStringLiteral("#aabbcc"));

    // Unquoted, the colour's own '#' opens it and a later one opens a comment.
    writeColors("accent = #4ECDC4 # secondary-400\n");
    QCOMPARE(Theme::readAccent(colorsPath), QStringLiteral("#4ECDC4"));

    // A named colour keeps working with a note after it.
    writeColors("accent = teal # not a hex value at all\n");
    QCOMPARE(Theme::readAccent(colorsPath), QStringLiteral("teal"));

    // A value that isn't a color keeps the fallback rather than breaking bindings.
    writeColors("accent = \"not-a-color\"\n");
    QCOMPARE(Theme::readAccent(colorsPath), fallback);

    // As does a theme without an accent at all.
    writeColors("background = \"#121212\"\n");
    QCOMPARE(Theme::readAccent(colorsPath), fallback);
}

static bool writeFile(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

void BackendTests::themeAccentForegroundKeepsContrast() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("theme")));
    const QString colors = dir.filePath(QStringLiteral("theme/colors.toml"));

    Theme fallback(dir.path());
    QCOMPARE(fallback.accent(), QStringLiteral("#FFD60A"));
    QCOMPARE(fallback.accentForeground(), QStringLiteral("black"));

    QVERIFY(writeFile(colors, "accent = \"#222266\"\n"));
    QCOMPARE(Theme(dir.path()).accentForeground(), QStringLiteral("white"));

    // Named colours are accents too, and get a foreground like any other.
    QVERIFY(writeFile(colors, "accent = teal\n"));
    QCOMPARE(Theme(dir.path()).accentForeground(), QStringLiteral("white"));

    // Saturated red and green read better in black, by relative luminance,
    // than the white a plain brightness cut-off would pick.
    QVERIFY(writeFile(colors, "accent = \"#ff0000\"\n"));
    QCOMPARE(Theme(dir.path()).accentForeground(), QStringLiteral("black"));
    QVERIFY(writeFile(colors, "accent = \"#00a000\"\n"));
    QCOMPARE(Theme(dir.path()).accentForeground(), QStringLiteral("black"));
}

void BackendTests::themeFollowsOmarchyInstalledLater() {
    // The app starts before Omarchy's state directory exists, and picks up
    // the accent once a theme lands there, without a restart.
    QTemporaryDir dir;
    const QString current = dir.filePath(QStringLiteral("state/omarchy/current"));
    Theme theme(current);
    QCOMPARE(theme.accent(), QStringLiteral("#FFD60A"));

    QVERIFY(QDir().mkpath(current + QStringLiteral("/theme")));
    QVERIFY(writeFile(current + QStringLiteral("/theme/colors.toml"), "accent = \"#112233\"\n"));
    QTRY_COMPARE(theme.accent(), QStringLiteral("#112233"));

    // A theme switch replaces the file, and the watch follows it.
    QVERIFY(QFile::remove(current + QStringLiteral("/theme/colors.toml")));
    QVERIFY(writeFile(current + QStringLiteral("/theme/colors.toml"), "accent = \"#445566\"\n"));
    QTRY_COMPARE(theme.accent(), QStringLiteral("#445566"));

    // So does replacing the whole current directory, as a symlink swap does.
    QVERIFY(QDir(current).removeRecursively());
    QTRY_COMPARE(theme.accent(), QStringLiteral("#FFD60A"));
    QVERIFY(QDir().mkpath(current + QStringLiteral("/theme")));
    QVERIFY(writeFile(current + QStringLiteral("/theme/colors.toml"), "accent = \"#778899\"\n"));
    QTRY_COMPARE(theme.accent(), QStringLiteral("#778899"));
}

void BackendTests::themeFollowsASymlinkSwitch() {
    // A theme switch that swaps current/theme as a symlink between two theme
    // directories, then an edit to the newly linked theme.
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("themes/a")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("themes/b")));
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("current")));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("themes/a/colors.toml")), "accent = \"#111111\"\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("themes/b/colors.toml")), "accent = \"#222222\"\n"));
    const QString link = dir.filePath(QStringLiteral("current/theme"));
    QVERIFY(QFile::link(dir.filePath(QStringLiteral("themes/a")), link));

    Theme theme(dir.filePath(QStringLiteral("current")));
    QCOMPARE(theme.accent(), QStringLiteral("#111111"));

    QVERIFY(QFile::link(dir.filePath(QStringLiteral("themes/b")), link + QStringLiteral(".next")));
    QVERIFY(std::rename(QFile::encodeName(link + QStringLiteral(".next")).constData(),
                        QFile::encodeName(link).constData()) == 0);
    QTRY_COMPARE(theme.accent(), QStringLiteral("#222222"));

    QVERIFY(writeFile(dir.filePath(QStringLiteral("themes/b/colors.toml")), "accent = \"#333333\"\n"));
    QTRY_COMPARE(theme.accent(), QStringLiteral("#333333"));
}

void BackendTests::themeFollowsHyprlandRounding() {
    QTemporaryDir dir;
    const QString response = dir.filePath(QStringLiteral("response.json"));
    const QString command = dir.filePath(QStringLiteral("hyprctl"));
    QVERIFY(writeFile(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n"));
    QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(writeFile(response, "{\"int\":0}"));

    Theme theme(dir.path(), nullptr, command);
    QCOMPARE(theme.radius(), 0);
    QVERIFY(writeFile(response, "{\"int\":6}"));
    QTRY_COMPARE(theme.radius(), 6);
}

void BackendTests::translatorFallsBackToEnglish() {
    QTemporaryDir dir;
    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.language(), QStringLiteral("en"));
    QVERIFY(translator.translate("Main", "Export").isNull());
}

void BackendTests::translatorFollowsTheLanguageFile() {
    QTemporaryDir dir;
    QVERIFY(writeFile(dir.filePath(QStringLiteral("language")), "pt-BR\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("pt.json")), R"({"Export": "Exportar", "Open": "Abrir"})"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("pt-BR.json")), R"({"Open": "Abrir vídeo"})"));

    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.language(), QStringLiteral("pt-BR"));
    QCOMPARE(translator.translate("Main", "Open"), QStringLiteral("Abrir vídeo"));
    QCOMPARE(translator.translate("Main", "Export"), QStringLiteral("Exportar"));
    QVERIFY(translator.translate("Main", "Quit").isNull());

    // A language switch replaces the file, and the app follows it.
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("language"))));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("language")), "en\n"));
    QTRY_COMPARE(translator.language(), QStringLiteral("en"));
    QVERIFY(translator.translate("Main", "Export").isNull());

    // A first pick can create Omarchy's config directory, too.
    const QString later = dir.filePath(QStringLiteral("config/omarchy/language"));
    Translator fresh(later, dir.path());
    QCOMPARE(fresh.language(), QStringLiteral("en"));
    QVERIFY(QDir().mkpath(QFileInfo(later).absolutePath()));
    QVERIFY(writeFile(later, "pt\n"));
    QTRY_COMPARE(fresh.language(), QStringLiteral("pt"));
    QCOMPARE(fresh.translate("Main", "Export"), QStringLiteral("Exportar"));
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

void BackendTests::translatorPicksPluralForms() {
    QTemporaryDir dir;
    QVERIFY(writeFile(dir.filePath(QStringLiteral("language")), "ru\n"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("ru.json")),
                      R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

    Translator translator(dir.filePath(QStringLiteral("language")), dir.path());
    QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QStringLiteral("%n клип"));
    QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QStringLiteral("%n клипа"));
    QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QStringLiteral("%n клипов"));
    QCOMPARE(Translator::pluralCategory(QStringLiteral("ar"), 11), QStringLiteral("many"));
    QCOMPARE(Translator::pluralCategory(QStringLiteral("ja"), 1), QStringLiteral("other"));

    // A counted string Russian lacks falls back to English, with English
    // rules: 21 is "one" in Russian but plural in English.
    QVERIFY(writeFile(dir.filePath(QStringLiteral("en.json")), R"({"%n files": {"one": "%n file", "other": "%n files"}})"));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("language")), "uk\n"));
    QTRY_COMPARE(translator.language(), QStringLiteral("uk"));
    QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QStringLiteral("%n files"));
    QCOMPARE(translator.translate("Main", "%n files", nullptr, 1), QStringLiteral("%n file"));

    // The translator set the default locale; later tests expect English.
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
}

static QString sourceDir() {
    return QFileInfo(QString::fromUtf8(__FILE__)).dir().absoluteFilePath(QStringLiteral("../src"));
}

static QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

static QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder(QStringLiteral("%L?[1-9n]"));
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

void BackendTests::catalogsMatchTheCode() {
    // Every string the code hands to qsTr(), tr() or translate() is in
    // en.json, counted ones as plural forms, and nothing else is.
    static const QRegularExpression call(
        QStringLiteral(R"re(\b(?:qsTr|tr)\(\s*"((?:\\.|[^"\\])*)"(\s*,\s*"[^"]*"\s*,)?|translate\(\s*"[^"]*"\s*,\s*"((?:\\.|[^"\\])*)")re"));
    QMap<QString, bool> used;
    const QDir src(sourceDir());
    for (const QFileInfo &info : src.entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QStringList code;
        for (const QString &line : QString::fromUtf8(file.readAll()).split('\n'))
            if (!line.trimmed().startsWith(QStringLiteral("//")) && !line.trimmed().startsWith('*'))
                code << line;
        for (auto it = call.globalMatch(code.join('\n')); it.hasNext();) {
            const auto match = it.next();
            QString text = match.captured(1).isNull() ? match.captured(3) : match.captured(1);
            text.replace(QStringLiteral("\\\""), QStringLiteral("\"")).replace(QStringLiteral("\\'"), QStringLiteral("'"));
            used[text] = used.value(text) || !match.captured(2).isEmpty();
        }
    }
    const QJsonObject english = readCatalog(src.filePath(QStringLiteral("i18n/en.json")));
    QVERIFY(!english.isEmpty());
    for (auto it = used.cbegin(); it != used.cend(); ++it) {
        QVERIFY2(english.contains(it.key()), qPrintable(it.key() + " is missing from en.json"));
        QVERIFY2(english.value(it.key()).isObject() == it.value(),
                 qPrintable(it.key() + (it.value() ? " needs plural forms" : " is not counted")));
    }
    for (const QString &key : english.keys())
        QVERIFY2(used.contains(key), qPrintable(key + " in en.json is no longer used"));

    // Each catalog is compiled in, and its translations fit the English:
    // the same placeholders, and the plural forms its language uses.
    QFile qrc(src.filePath(QStringLiteral("resources.qrc")));
    QVERIFY(qrc.open(QIODevice::ReadOnly));
    const QString resources = QString::fromUtf8(qrc.readAll());
    for (const QFileInfo &info : QDir(src.filePath(QStringLiteral("i18n"))).entryInfoList({"*.json"}, QDir::Files)) {
        const QString language = info.completeBaseName();
        QVERIFY2(resources.contains(QStringLiteral("<file alias=\"i18n/%1\">i18n/%1</file>").arg(info.fileName())),
                 qPrintable(info.fileName() + " is not in resources.qrc"));
        if (language == QStringLiteral("en"))
            continue;
        // Each plural category the language uses, and how many counts reach it.
        QHash<QString, int> reach{{QStringLiteral("other"), 2}};
        for (long long n = 0; n <= 1000; ++n)
            ++reach[Translator::pluralCategory(language, n)];
        ++reach[Translator::pluralCategory(language, 1000000)];
        const QList<QString> used = reach.keys();
        const QSet<QString> categories(used.begin(), used.end());
        const QJsonObject catalog = readCatalog(info.filePath());
        QVERIFY2(!catalog.isEmpty(), qPrintable(info.fileName() + " is empty or not JSON"));
        for (auto it = catalog.begin(); it != catalog.end(); ++it) {
            const QString where = info.fileName() + ": " + it.key();
            QVERIFY2(english.contains(it.key()), qPrintable(where + " is not in en.json"));
            if (english.value(it.key()).isString()) {
                QVERIFY2(it.value().isString() && !it.value().toString().isEmpty(), qPrintable(where));
                QCOMPARE(placeholders(it.value().toString()), placeholders(it.key()));
                continue;
            }
            const QJsonObject forms = it.value().toObject();
            const QStringList given = forms.keys();
            QVERIFY2(QSet<QString>(given.begin(), given.end()) == categories, qPrintable(where + " has the wrong plural forms"));
            // Every form keeps the other placeholders, and only a form for a
            // single count ("a file", "two files") may leave the number out.
            QStringList wanted = placeholders(it.key());
            wanted.removeAll(QStringLiteral("%n"));
            for (const QString &form : given) {
                const QString text = forms.value(form).toString();
                QVERIFY2(!text.isEmpty(), qPrintable(where + " " + form + " needs a translation"));
                QStringList found = placeholders(text);
                const bool counted = found.removeAll(QStringLiteral("%n")) > 0;
                QVERIFY2(found == wanted, qPrintable(where + " " + form + " has the wrong placeholders"));
                QVERIFY2(counted || reach.value(form) == 1, qPrintable(where + " " + form + " leaves out the count"));
            }
        }
    }
}

void BackendTests::qmlDialogsAnswerFromTheKeyboard() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);
    auto focused = [&] {
        QQuickItem *item = window->activeFocusItem();
        return item ? item->property("text").toString() : QString();
    };

    // Focus starts on Export; Tab and Shift+Tab walk the buttons, wrapping.
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(focused(), QStringLiteral("Export"), 3000);
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(focused(), QStringLiteral("Cancel"));
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(focused(), QStringLiteral("Quit"));
    QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(focused(), QStringLiteral("Cancel"));
    QTest::keyClick(window, Qt::Key_Left);
    QCOMPARE(focused(), QStringLiteral("Export"));

    // On Omarchy's default the corners are sharp, like the windows.
    QQuickItem *cancel = dialogButton(window, QStringLiteral("Cancel"));
    QVERIFY(cancel);
    QCOMPARE(cancel->property("background").value<QQuickItem *>()->property("radius").toDouble(), 0.0);

    // Alt and a button's underlined letter presses it, wherever focus is.
    QTest::keyClick(window, Qt::Key_C, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);
    QCOMPARE(backend.exportCount, 0);
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_E, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QCOMPARE(window->property("quitConfirmVisible").toBool(), false);

    // The same in the open confirmation: focus on keeping the edit, Alt+O opens.
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), true, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(focused(), QStringLiteral("Keep editing"), 3000);
    QTest::keyClick(window, Qt::Key_O, Qt::AltModifier);
    QTRY_COMPARE_WITH_TIMEOUT(backend.openCount, 1, 3000);
    QCOMPARE(window->property("openConfirmVisible").toBool(), false);
}

// Neither dialog has anything to type into, so a button's letter presses it
// without Alt too (omacom/plain#10). The quit dialog's Q is the app's Q, which
// has to step aside for it: two live shortcuts on one key fire neither.
void BackendTests::qmlDialogsAnswerToABareLetter() {
    ShortcutBackend backend(placeholderUrl(m_dir), 20.0);
    QmlHarness harness(backend);
    QQuickWindow *window = showEditor(harness, backend);
    QVERIFY2(window, qPrintable(mainQmlPath()));
    QTest::keyClick(window, Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(window, Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("unexported").toBool(), true, 3000);

    // The open confirmation: K keeps the edit, O opens another video.
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_K);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), false, 3000);
    QCOMPARE(backend.openCount, 0);
    // With the dialog gone its letters mean nothing.
    QTest::keyClick(window, Qt::Key_O);
    QTest::qWait(50);
    QCOMPARE(backend.openCount, 0);
    QTest::keyClick(window, Qt::Key_O, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("openConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_O);
    QTRY_COMPARE_WITH_TIMEOUT(backend.openCount, 1, 3000);
    QCOMPARE(window->property("openConfirmVisible").toBool(), false);

    // The quit confirmation: C cancels, E exports.
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_C);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), false, 3000);
    QCOMPARE(backend.exportCount, 0);
    QTest::keyClick(window, Qt::Key_E);
    QTest::qWait(50);
    QCOMPARE(backend.exportCount, 0);
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    QTest::keyClick(window, Qt::Key_E);
    QTRY_COMPARE_WITH_TIMEOUT(backend.exportCount, 1, 3000);
    QCOMPARE(window->property("quitConfirmVisible").toBool(), false);

    // Holding Q to ask must not answer: its repeats are not a second press.
    QSignalSpy quitSpy(&harness.engine(), &QQmlApplicationEngine::quit);
    QTest::keyPress(window, Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(window->property("quitConfirmVisible").toBool(), true, 3000);
    qt_handleKeyEvent(window, QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"), true);
    qt_handleKeyEvent(window, QEvent::KeyPress, Qt::Key_Q, Qt::NoModifier, QStringLiteral("q"), true);
    QTest::keyRelease(window, Qt::Key_Q);
    QTest::qWait(50);
    QVERIFY(window->isVisible());
    QCOMPARE(window->property("quitConfirmVisible").toBool(), true);
    QCOMPARE(quitSpy.count(), 0);

    // A second Q, pressed on purpose, quits.
    QTest::keyClick(window, Qt::Key_Q);
    QTRY_VERIFY_WITH_TIMEOUT(!window->isVisible(), 3000);
    QCOMPARE(quitSpy.count(), 1);
}

// What QML can reach on an object: its properties with their types, change
// signals and writability, and its signals and invokable methods with their
// full signatures, return type included.
static QStringList qmlSurface(const QMetaObject &meta) {
    QStringList surface;
    for (int i = meta.propertyOffset(); i < meta.propertyCount(); ++i) {
        const QMetaProperty property = meta.property(i);
        surface << QStringLiteral("property %1 %2 notify %3%4%5").arg(
            QString::fromLatin1(property.typeName()), QString::fromLatin1(property.name()),
            QString::fromLatin1(property.notifySignal().methodSignature()),
            property.isWritable() ? QStringLiteral(" writable") : QString(),
            property.isConstant() ? QStringLiteral(" constant") : QString());
    }
    for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
        const QMetaMethod method = meta.method(i);
        if (method.access() != QMetaMethod::Public || method.methodType() == QMetaMethod::Slot)
            continue;
        surface << QStringLiteral("%1 %2 %3").arg(
            method.methodType() == QMetaMethod::Signal ? QStringLiteral("signal") : QStringLiteral("method"),
            QString::fromLatin1(method.typeName()), QString::fromLatin1(method.methodSignature()));
    }
    surface.sort();
    return surface;
}

void BackendTests::stubBackendMatchesTheRealOne() {
    // The QML tests run Main.qml against ShortcutBackend, so it must offer
    // QML everything the real Backend does, the same way; anything missing
    // would leave those tests passing while the app breaks.
    const QStringList stub = qmlSurface(ShortcutBackend::staticMetaObject);
    const QStringList real = qmlSurface(Backend::staticMetaObject);
    QStringList missing, extra;
    for (const QString &entry : real) if (!stub.contains(entry)) missing << entry;
    for (const QString &entry : stub) if (!real.contains(entry)) extra << entry;
    QVERIFY2(missing.isEmpty() && extra.isEmpty(),
             qPrintable(QStringLiteral("stub lacks: %1\nstub adds: %2").arg(missing.join(QStringLiteral("; ")), extra.join(QStringLiteral("; ")))));
}

static bool dropUrls(QQuickWindow *window, const QList<QUrl> &urls) {
    QMimeData mime;
    mime.setUrls(urls);
    const QPointF point(window->width() / 2, window->height() / 3);
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


// ---------------------------------------------------------------------------
// The timeline view as a function of the edit.
//
// The track is a map of the footage: clips are windows onto it, edits move the
// windows, and only zooming changes the scale. So what the track shows must
// depend only on the clips, the zoom and the playhead, never on how they got
// there. The test below runs random edits, the way a person might, and after
// each one compares the track against a freshly opened one showing the same
// edit, down to the pixel. Along the way it checks the rules a person sees:
// the zoomed-out track is covered end to end, a zoomed view shows a clip,
// trimming a clip shorter moves no other clip, and nothing in the filmstrip
// reloads or moves under a drag.

namespace {

struct BarView {
    QVariantList positions;
    double windowStart = 0, windowEnd = 0;
    QStringList frames;
    QImage picture;
    bool loaded = false;
};

// Each filmstrip frame with where it sits, keyed so a frame that reloads,
// moves, or is rebuilt shows up as a difference.
static void collectFrames(QQuickItem *item, QQuickItem *container, QHash<QQuickItem *, QString> &frames) {
    const QList<QQuickItem *> children = item->childItems();
    for (QQuickItem *child : children) {
        const QString name = child->objectName();
        QQuickItem *next = name == QLatin1String("clip") || name == QLatin1String("run") ? child : container;
        if (child->inherits("QQuickImage") && child->isVisible())
            frames.insert(child, QStringLiteral("%1 %2 %3+%4 in %5+%6")
                                     .arg(quintptr(container), 0, 16)
                                     .arg(child->property("source").toUrl().toString())
                                     .arg(child->mapToScene(QPointF()).x()).arg(child->width())
                                     .arg(child->parentItem()->mapToScene(QPointF()).x()).arg(child->parentItem()->width()));
        collectFrames(child, next, frames);
    }
}

static QHash<QQuickItem *, QString> framesOf(QQuickItem *editBar) {
    QHash<QQuickItem *, QString> frames;
    collectFrames(editBar, nullptr, frames);
    return frames;
}

static bool framesLoaded(QQuickItem *editBar) {
    const auto frames = framesOf(editBar);
    for (auto it = frames.begin(); it != frames.end(); ++it)
        // Ready with a picture: a frame that failed to extract is Ready but empty.
        if (it.key()->property("status").toInt() != 1 || it.key()->implicitWidth() <= 0)
            return false;
    return true;
}

// Where the clips and the videos' filmstrips draw, as [from, to) in bar pixels.
static QList<QPair<double, double>> coveredSpans(QQuickItem *editBar) {
    QList<QPair<double, double>> spans;
    std::function<void(QQuickItem *)> walk = [&](QQuickItem *item) {
        for (QQuickItem *child : item->childItems()) {
            const QString name = child->objectName();
            if ((name == QLatin1String("clip") || name == QLatin1String("run")) && child->isVisible() && child->width() > 0) {
                const double x = child->mapToItem(editBar, QPointF()).x();
                spans.append({x, x + child->width()});
            }
            walk(child);
        }
    };
    walk(editBar);
    std::sort(spans.begin(), spans.end());
    return spans;
}

}  // namespace

// Owns one editor window showing an edit, for the test below.
class EditorUnderTest {
public:
    EditorUnderTest(const QUrl &placeholder, const QString &video, const QList<double> &durations)
        : backend(placeholder, durations.value(0)), harness(backend, {}, video) {
        window = showEditor(harness, backend);
        if (!window)
            return;
        bar = harness.editBar();
        // The offscreen platform opens a window smaller than asked and grows
        // it later; pin the size so two editors lay out alike. Should it never
        // get there, comparing the two editors' layouts fails and says how.
        window->setGeometry(QRect(0, 0, 960, 680));
        (void)QTest::qWaitFor([this] { return window->width() == 960 && window->height() == 680; }, 3000);
        QTest::qWait(50);
        for (int i = 1; i < durations.size(); ++i)
            backend.addVideoOfLength(durations[i], 0);
    }

    BarView view() {
        // Off the bar, so no hover state is showing.
        QTest::mouseMove(window, QPoint(5, 5));
        BarView v;
        v.loaded = QTest::qWaitFor([this] { return framesLoaded(bar); }, 10000);
        QTest::qWait(20);
        v.positions = bar->property("positions").toList();
        v.windowStart = bar->property("windowStart").toDouble();
        v.windowEnd = bar->property("windowEnd").toDouble();
        // Videos by their place in the edit, not their key, which every editor
        // hands out afresh.
        QHash<QString, QString> videoOf;
        const QVariantList videos = backend.property("videos").toList();
        for (int i = 0; i < videos.size(); ++i)
            videoOf.insert(QStringLiteral("image://thumbs/%1/").arg(videos[i].toMap().value(QStringLiteral("thumbKey")).toInt()),
                           QStringLiteral("video %1 at ").arg(i));
        const auto frames = framesOf(bar);
        for (QString frame : frames) {
            frame = frame.section(QLatin1Char(' '), 1);
            const QString prefix = frame.left(frame.indexOf(QLatin1Char('/'), QStringLiteral("image://thumbs/").size()) + 1);
            v.frames << videoOf.value(prefix, prefix) + frame.mid(prefix.size());
        }
        v.frames.sort();
        const QImage whole = window->grabWindow();
        const qreal scale = whole.devicePixelRatio();
        const QRectF rect = bar->mapRectToScene(QRectF(0, 0, bar->width(), bar->height()));
        v.picture = whole.copy(QRectF(rect.topLeft() * scale, rect.size() * scale).toAlignedRect());
        return v;
    }

    QPoint at(double x) const { return bar->mapToScene(QPointF(x, bar->height() / 2.0)).toPoint(); }
    double xOf(double trackSec) const {
        return (trackSec - bar->property("windowStart").toDouble()) * bar->property("pxPerSec").toDouble();
    }

    ShortcutBackend backend;
    QmlHarness harness;
    QQuickWindow *window = nullptr;
    QQuickItem *bar = nullptr;
};

// Pixels that differ visibly between two pictures of the track. A few always
// do: the software renderer rasterises a resized rounded frame a little
// differently from a fresh one. A wrong or missing frame differs in thousands.
static int differingPixels(const QImage &a, const QImage &b) {
    if (a.size() != b.size())
        return a.width() * a.height();
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int row = 0; row < x.height(); ++row) {
        const QRgb *p = reinterpret_cast<const QRgb *>(x.constScanLine(row));
        const QRgb *q = reinterpret_cast<const QRgb *>(y.constScanLine(row));
        for (int col = 0; col < x.width(); ++col) {
            const int d = qMax(qAbs(qRed(p[col]) - qRed(q[col])),
                               qMax(qAbs(qGreen(p[col]) - qGreen(q[col])), qAbs(qBlue(p[col]) - qBlue(q[col]))));
            if (d > 48)
                ++count;
        }
    }
    return count;
}

static QString describe(const edit::Clips &clips) {
    QStringList parts;
    for (const edit::Clip &clip : clips)
        parts << QStringLiteral("%1:%2-%3").arg(clip.source).arg(clip.in, 0, 'f', 3).arg(clip.out, 0, 'f', 3);
    return parts.join(QLatin1Char(' '));
}

static double placeOf(const QVariantList &positions, int index, const char *edge) {
    return positions.value(index).toMap().value(QLatin1String(edge)).toDouble();
}

// The frame under sequence time t: the video and the time in it.
static QPair<int, double> frameAt(const Timeline &timeline, double t) {
    const int i = timeline.clipAt(t);
    const edit::Clip clip = timeline.clips().value(i);
    return {clip.source, clip.in + t - timeline.clipStart(i)};
}

// Float noise from splits and trims stays well under this.
constexpr double kNoise = 0.0001;

// Whether clip shows that frame of its video.
static bool shows(const edit::Clip &clip, const QPair<int, double> &frame) {
    return clip.source == frame.first && frame.second >= clip.in - kNoise && frame.second < clip.out - kNoise;
}

static bool samePlace(const QVariant &a, const QVariant &b) {
    const QVariantMap x = a.toMap(), y = b.toMap();
    return qAbs(x.value(QStringLiteral("start")).toDouble() - y.value(QStringLiteral("start")).toDouble()) < kNoise
        && qAbs(x.value(QStringLiteral("end")).toDouble() - y.value(QStringLiteral("end")).toDouble()) < kNoise;
}

// For each clip, whether it picks up later in the video of the one before,
// sharing its stretch of track. A trim that changes none of these must leave
// every other clip where it was.
static QList<bool> continuity(const edit::Clips &clips) {
    QList<bool> result;
    for (int i = 1; i < clips.size(); ++i)
        result << (clips[i].source == clips[i - 1].source && clips[i].in > clips[i - 1].out - 0.001);
    return result;
}

void BackendTests::qmlTimelineViewDependsOnlyOnTheEdit_data() {
    QTest::addColumn<quint32>("seed");
    // More with OMACUT_FUZZ_SEEDS=n, e.g. before a release, and against a real
    // recording with OMACUT_FUZZ_VIDEO=path: big frames extract slowly, which
    // shakes out timing that a tiny test video never hits.
    const int seeds = qMax(4, qEnvironmentVariableIntValue("OMACUT_FUZZ_SEEDS"));
    for (int i = 0; i < seeds; ++i)
        QTest::addRow("seed %d", i + 1) << quint32(i + 1);
}

void BackendTests::qmlTimelineViewDependsOnlyOnTheEdit() {
    QFETCH(quint32, seed);
    QString video = qEnvironmentVariable("OMACUT_FUZZ_VIDEO");
    if (video.isEmpty())
        video = makeVideo(QStringLiteral("fuzz.mp4"), 20.0, false, QStringLiteral("64x36"));
    const double length = ffmpeg::probe(video).duration;
    QVERIFY(length > 1.0);
    QRandomGenerator random(seed);
    EditorUnderTest editor(placeholderUrl(m_dir), video, {length});
    QVERIFY2(editor.window, qPrintable(mainQmlPath()));
    Timeline &timeline = editor.backend.timeline;
    QStringList history;
    auto uniform = [&](double from, double to) { return from + random.generateDouble() * (to - from); };
    QByteArray message;  // Outlives the check that reports it.
    auto failure = [&](const QString &what) {
        message = QStringLiteral("%1\nafter: %2\nclips: %3")
                      .arg(what, history.join(QStringLiteral(", ")), describe(timeline.clips()))
                      .toUtf8();
        return message.constData();
    };

    const int steps = 24;
    for (int step = 0; step < steps; ++step) {
        const edit::Clips before = timeline.clips();
        const BarView was = editor.view();
        const bool zoomed = editor.bar->property("zoomed").toBool();
        const int count = before.size();
        const int pick = int(random.bounded(15));
        int trimmed = -1;  // A clip trimmed shorter, which must move no other clip.
        // An edit made from the keyboard at the playhead, the way a person
        // does it, and the frame that was under the playhead then.
        // Where it should be after: the same frame, or for Ctrl+Shift+Space, the
        // trimmed clip's last one. Unset when the edit cut that frame.
        bool keyed = false;
        QPair<int, double> frameBefore;
        edit::Clip clipBefore;
        int indexBefore = -1;
        auto keyAt = [&](const QString &name, Qt::Key key, Qt::KeyboardModifiers modifiers) {
            const double t = uniform(0, timeline.duration());
            editor.bar->setProperty("playheadSec", t);
            frameBefore = frameAt(timeline, t);
            indexBefore = timeline.clipAt(t);
            clipBefore = timeline.clips().value(indexBefore);
            keyed = true;
            history << QStringLiteral("%1 at %2").arg(name).arg(t, 0, 'f', 2);
            QTest::keyClick(editor.window, key, modifiers);
        };

        if (pick <= 1) {
            const double t = uniform(0, timeline.duration());
            history << QStringLiteral("split at %1").arg(t, 0, 'f', 2);
            timeline.split(t);
        } else if (pick == 2) {
            const int i = int(random.bounded(count));
            const bool start = random.bounded(2);
            const double edge = start ? before[i].in : before[i].out;
            const double to = edge + uniform(-3, 3);
            history << QStringLiteral("trim %1 %2 to %3").arg(i).arg(start ? "in" : "out").arg(to, 0, 'f', 2);
            timeline.moveEdge(i, start, to);
            if (timeline.clips().size() == count && timeline.clips()[i].length() < before[i].length())
                trimmed = i;
        } else if (pick <= 4) {
            // Drag a handle, checking the filmstrip holds still under it.
            const int i = int(random.bounded(count));
            const bool start = random.bounded(2);
            const double edgeSec = placeOf(was.positions, i, start ? "start" : "end");
            const double x = editor.xOf(edgeSec) + (start ? 6 : -6);
            const double dx = uniform(-90, 90);
            if (x < 8 || x > editor.bar->width() - 8 || editor.xOf(placeOf(was.positions, i, "end")) - editor.xOf(placeOf(was.positions, i, "start")) < 30)
                continue;
            history << QStringLiteral("drag %1 %2 by %3px").arg(i).arg(start ? "in" : "out").arg(dx, 0, 'f', 0);
            QTest::mouseMove(editor.window, editor.at(x));
            // A frame's place on screen, without its container's, which a drag resizes.
            auto onScreen = [](QHash<QQuickItem *, QString> frames) {
                for (QString &frame : frames) frame = frame.section(QStringLiteral(" in "), 0, 0);
                return frames;
            };
            const auto still = onScreen(framesOf(editor.bar));
            QTest::mousePress(editor.window, Qt::LeftButton, Qt::NoModifier, editor.at(x));
            for (int move = 1; move <= 5; ++move) {
                QTest::mouseMove(editor.window, editor.at(x + dx * move / 5));
                const auto now = onScreen(framesOf(editor.bar));
                for (auto it = now.begin(); it != now.end(); ++it) {
                    if (still.contains(it.key()))
                        QVERIFY2(still.value(it.key()) == it.value(), failure(QStringLiteral("a filmstrip frame moved or reloaded mid-drag: %1 -> %2").arg(still.value(it.key()), it.value())));
                    else
                        QVERIFY2(!QList<QString>(still.values()).contains(it.value()),
                                 failure(QStringLiteral("a filmstrip frame was rebuilt mid-drag: %1").arg(it.value())));
                }
                // Mid-drag, every other clip stays where it was.
                const QVariantList positions = editor.bar->property("positions").toList();
                for (int j = 0; j < count; ++j)
                    if (j != i)
                        QVERIFY2(positions.value(j) == was.positions.value(j), failure(QStringLiteral("clip %1 moved while dragging clip %2").arg(j).arg(i)));
            }
            QTest::mouseRelease(editor.window, Qt::LeftButton, Qt::NoModifier, editor.at(x + dx));
            QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 20);
            if (timeline.clips().size() == count && timeline.clips()[i].length() < before[i].length())
                trimmed = i;
        } else if (pick == 5) {
            int join = -1;
            for (int i = 0; i + 1 < count; ++i)
                if (timeline.canJoin(i)) join = i;
            if (join < 0) continue;
            history << QStringLiteral("join %1").arg(join);
            timeline.joinClips(join);
        } else if (pick == 6) {
            if (count < 2) continue;
            const int from = int(random.bounded(count)), to = int(random.bounded(count));
            history << QStringLiteral("move %1 to %2").arg(from).arg(to);
            timeline.moveClip(from, to);
        } else if (pick == 7) {
            if (count < 2) continue;
            const int i = int(random.bounded(count));
            history << QStringLiteral("remove %1").arg(i);
            timeline.removeClip(i);
        } else if (pick == 8) {
            const double t = uniform(0, timeline.duration());
            history << QStringLiteral("zoom at %1").arg(t, 0, 'f', 2);
            editor.bar->setProperty("playheadSec", t);
            QTest::keyClick(editor.window, Qt::Key_Z);
        } else if (pick == 11) {
            keyAt(QStringLiteral("S"), Qt::Key_S, Qt::NoModifier);
        } else if (pick == 12 || pick == 13) {
            // Moving an edge to the playhead twice is the same as once.
            const bool start = pick == 12;
            keyAt(start ? QStringLiteral("Ctrl+Space") : QStringLiteral("Ctrl+Shift+Space"), Qt::Key_Space,
                  start ? Qt::ControlModifier : Qt::ControlModifier | Qt::ShiftModifier);
            const edit::Clips once = timeline.clips();
            QTest::keyClick(editor.window, Qt::Key_Space, start ? Qt::ControlModifier : Qt::ControlModifier | Qt::ShiftModifier);
            QVERIFY2(timeline.clips() == once, failure(QStringLiteral("pressing it again trimmed again: %1").arg(describe(timeline.clips()))));
        } else if (pick == 14) {
            const bool earlier = random.bounded(2);
            keyAt(earlier ? QStringLiteral("Alt+[") : QStringLiteral("Alt+]"),
                  earlier ? Qt::Key_BracketLeft : Qt::Key_BracketRight, Qt::AltModifier);
        } else if (pick == 9) {
            if (timeline.sourceDurations().size() >= 3) continue;
            // Another video, shorter, after the clip under t. Every video key
            // shows the same footage; only the lengths differ.
            const double t = uniform(0, timeline.duration());
            history << QStringLiteral("add video at %1").arg(t, 0, 'f', 2);
            editor.backend.addVideoOfLength(length * 0.6, t);
        } else {
            const bool undo = random.bounded(2);
            history << (undo ? QStringLiteral("undo") : QStringLiteral("redo"));
            undo ? timeline.undo() : timeline.redo();
        }

        const BarView now = editor.view();
        const edit::Clips clips = timeline.clips();
        QVERIFY2(now.loaded, failure(QStringLiteral("filmstrip frames never loaded")));

        // An edit from the keyboard keeps the playhead on its frame. Ctrl+Shift+Space
        // ends the clip there, leaving the playhead on the clip's last frame.
        if (keyed && clips != before && pick == 13)
            frameBefore.second -= 0.001;
        if (keyed) {
            // What became of the clip the playhead was in: a split leaves it
            // two pieces, a trim leaves it in place, and a move shifts it one
            // place. If none of those still shows the frame, the edit cut it.
            bool kept = false;
            for (int i = indexBefore - 1; i <= indexBefore + 1; ++i) {
                const edit::Clip clip = clips.value(i, edit::Clip{-1, 0, 0});
                kept = kept || (shows(clip, frameBefore) && qMax(clip.in, clipBefore.in) < qMin(clip.out, clipBefore.out));
            }
            keyed = kept;
        }
        if (keyed) {
            const auto frameAfter = frameAt(timeline, editor.bar->property("playheadSec").toDouble());
            QVERIFY2(frameAfter.first == frameBefore.first && qAbs(frameAfter.second - frameBefore.second) < 0.002,
                     failure(QStringLiteral("the playhead moved off its frame: video %1 at %2 s, now video %3 at %4 s")
                                 .arg(frameBefore.first).arg(frameBefore.second, 0, 'f', 3)
                                 .arg(frameAfter.first).arg(frameAfter.second, 0, 'f', 3)));
        }

        // Zoomed out, the track is covered end to end: clips, and the dimmed
        // footage cut from around them. No blank stretch.
        if (!editor.bar->property("zoomed").toBool()) {
            double reached = 0;
            for (const auto &span : coveredSpans(editor.bar)) {
                QVERIFY2(span.first <= reached + 1, failure(QStringLiteral("blank track from %1px to %2px").arg(reached).arg(span.first)));
                reached = qMax(reached, span.second);
            }
            QVERIFY2(reached >= editor.bar->width() - 1, failure(QStringLiteral("blank track past %1px of %2px").arg(reached).arg(editor.bar->width())));
        }

        // No clip runs into the footage of the next one picking up later in
        // the same video: a handle stops there.
        for (int i = 0; i + 1 < clips.size(); ++i)
            QVERIFY2(clips[i].source != clips[i + 1].source || clips[i + 1].in <= clips[i].in + kNoise
                         || clips[i + 1].in >= clips[i].out - 0.001,
                     failure(QStringLiteral("clip %1 runs into the footage of clip %2").arg(i).arg(i + 1)));

        // Zoomed in, some clip is in view.
        if (editor.bar->property("zoomed").toBool()) {
            bool inView = false;
            for (const QVariant &at : now.positions)
                inView = inView || (at.toMap().value(QStringLiteral("end")).toDouble() > now.windowStart
                                    && at.toMap().value(QStringLiteral("start")).toDouble() < now.windowEnd);
            QVERIFY2(inView, failure(QStringLiteral("zoomed on %1..%2 with no clip in view").arg(now.windowStart).arg(now.windowEnd)));
        }

        // A clip trimmed shorter leaves the others, and the track's scale, alone.
        if (trimmed >= 0 && continuity(before) == continuity(clips)) {
            for (int j = 0; j < clips.size(); ++j)
                if (j != trimmed)
                    QVERIFY2(samePlace(now.positions.value(j), was.positions.value(j)), failure(QStringLiteral("trimming clip %1 moved clip %2").arg(trimmed).arg(j)));
            if (!zoomed)
                QVERIFY2(qAbs(now.windowEnd - was.windowEnd) < kNoise, failure(QStringLiteral("trimming changed the track's length from %1 to %2").arg(was.windowEnd).arg(now.windowEnd)));
        }

        // And a freshly opened editor shows the same edit the same way.
        EditorUnderTest fresh(placeholderUrl(m_dir), video, timeline.sourceDurations());
        QVERIFY(fresh.window);
        fresh.backend.timeline.restore(timeline.sourceDurations(), clips);
        fresh.bar->setProperty("viewStartSec", editor.bar->property("viewStartSec"));
        fresh.bar->setProperty("viewEndSec", editor.bar->property("viewEndSec"));
        fresh.bar->setProperty("zoomed", editor.bar->property("zoomed"));
        fresh.bar->setProperty("playheadSec", editor.bar->property("playheadSec"));
        const BarView expected = fresh.view();
        QVERIFY2(now.positions == expected.positions, failure(QStringLiteral("clips sit differently than in a fresh editor")));
        QVERIFY2(now.windowStart == expected.windowStart && now.windowEnd == expected.windowEnd,
                 failure(QStringLiteral("track spans %1..%2, a fresh editor %3..%4").arg(now.windowStart).arg(now.windowEnd).arg(expected.windowStart).arg(expected.windowEnd)));
        if (now.frames != expected.frames) {
            QStringList extra, missing;
            for (const QString &f : now.frames) if (!expected.frames.contains(f)) extra << f;
            for (const QString &f : expected.frames) if (!now.frames.contains(f)) missing << f;
            QFAIL(failure(QStringLiteral("filmstrip differs from a fresh editor's\nonly here: %1\nonly fresh: %2")
                              .arg(extra.join(QStringLiteral("; ")), missing.join(QStringLiteral("; ")))));
        }
        const int differing = differingPixels(now.picture, expected.picture);
        if (differing > 20) {
            const QString base = QDir::temp().filePath(QStringLiteral("omacut-seed%1-step%2").arg(seed).arg(step));
            now.picture.save(base + QStringLiteral("-edited.png"));
            expected.picture.save(base + QStringLiteral("-fresh.png"));
            QFAIL(failure(QStringLiteral("track looks different from a fresh editor's in %1 pixels; see %2-*.png").arg(differing).arg(base)));
        }
    }
}

// Offscreen, with the software renderer: no windows pop up while the tests
// run, every window is the size it asks for rather than what a tiling window
// manager makes it, and grabbed windows hold real pixels. The desktop's own
// QT_QPA_PLATFORM is overridden; OMACUT_TEST_ON_SCREEN=1 keeps it, to watch.
int main(int argc, char *argv[]) {
    if (qEnvironmentVariableIsEmpty("OMACUT_TEST_ON_SCREEN")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
        qputenv("QT_QUICK_BACKEND", "software");
    }
    QGuiApplication app(argc, argv);
    QTEST_SET_MAIN_SOURCE_PATH
    BackendTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "omacut_tests.moc"
