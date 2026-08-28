#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRect>
#include <QRectF>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

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
    QRectF lastCrop;

    void openVideo() override { ++openCount; }

    void exportVideo(const QUrl &suggestedUrl, double start, double end,
                     const QList<int> &scaleHeights, const QRectF &crop = {}) override {
        ++exportCount;
        lastSuggestedUrl = suggestedUrl;
        lastStart = start;
        lastEnd = end;
        lastScaleHeights = scaleHeights;
        lastCrop = crop;
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
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY infoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY infoChanged)
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
    int videoWidth() const { return 1920; }
    int videoHeight() const { return 1080; }
    int thumbCount() const { return 0; }
    int thumbReadyCount() const { return 0; }
    int thumbRevision() const { return 0; }
    bool busy() const { return false; }
    QString status() const { return {}; }
    QString themeAccent() const { return QStringLiteral("#FFD60A"); }
    QString themeAccentForeground() const { return QStringLiteral("black"); }

    Q_INVOKABLE bool load(const QUrl &) { return false; }
    Q_INVOKABLE void openVideoDialog() { ++openCount; }
    Q_INVOKABLE void exportDialog(double start, double end, const QRectF &crop = {}) {
        ++exportCount;
        lastStart = start;
        lastEnd = end;
        lastCrop = crop;
    }
    Q_INVOKABLE QUrl suggestedExportUrl() const { return {}; }
    Q_INVOKABLE void exportClip(const QUrl &, double, double, int = 0, const QRectF & = {}) {}
    Q_INVOKABLE void requestThumbs(double start, double end) {
        ++thumbRequestCount;
        lastThumbStart = start;
        lastThumbEnd = end;
    }

    void announceInfo() { emit infoChanged(); }
    void announceExportDone() { emit exportDone(QStringLiteral("/tmp/exported.mp4")); }

    int openCount = 0;
    int exportCount = 0;
    double lastStart = 0;
    double lastEnd = 0;
    QRectF lastCrop;
    int thumbRequestCount = 0;
    double lastThumbStart = 0;
    double lastThumbEnd = 0;

signals:
    void infoChanged();
    void thumbsChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);

private:
    QUrl m_source;
    double m_duration;
};

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
    QQuickItem *cropOverlay() const {
        return m_window ? m_window->findChild<QQuickItem *>(QStringLiteral("cropOverlay")) : nullptr;
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
    void trimArgsReencodeForPreciseCuts();
    void trimArgsScaleTheShorterSide();
    void clampCropSnapsToEvenPixelsInsideTheFrame();
    void trimArgsCropBeforeScale();
    void exportDialogUsesCroppedSizeForQuality();
    void exportClipCropsTheFrame();
    void exportHeightsNeverUpscale();
    void qmlDragCreatesACrop();
    void qmlDragInsideMovesTheCrop();
    void qmlClickingDimClearsTheCrop();
    void qmlCropPresetsAndLockedAspect();
    void qmlShiftCornerScalesFromCenter();
    void qmlCropPresetsAreQuietDuringQuitConfirm();
    void qmlCropOnlyQuitConfirms();
    void qmlEscapeClearsTheCrop();
    void themeAccentReadsOmarchyColors();
    void themeAccentForegroundKeepsContrast();

private:
    QUrl videoUrl() const { return QUrl::fromLocalFile(m_videoPath); }
    QString formatName(const QString &path) const;
    void waitForBackgroundWork(Backend &backend);
    bool installBrokenFfmpeg(const QString &dirPath);

    QTemporaryDir m_dir;
    QString m_videoPath;
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

static QPoint cropScenePos(QQuickItem *crop, qreal x, qreal y) {
    return crop->mapToScene(QPointF(x, y)).toPoint();
}

static void showHarness(QmlHarness &harness) {
    QQuickWindow *window = harness.window();
    window->show();
    window->requestActivate();
    QTest::qWait(100);
}

void BackendTests::qmlDragCreatesACrop() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);
    QTRY_VERIFY_WITH_TIMEOUT(crop->width() > 100 && crop->height() > 100, 3000);
    QCOMPARE(crop->property("active").toBool(), false);

    const QPoint start = cropScenePos(crop, 80, 60);
    const QPoint end = cropScenePos(crop, 280, 220);
    QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier, start);
    QTest::mouseMove(harness.window(), end);
    QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::NoModifier, end);

    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), true, 3000);
    QVERIFY(crop->property("cropW").toDouble() > 0.05);
    QVERIFY(crop->property("cropH").toDouble() > 0.05);
    QCOMPARE(crop->property("aspectMode").toString(), QString());
}

void BackendTests::qmlDragInsideMovesTheCrop() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);
    QTRY_VERIFY_WITH_TIMEOUT(crop->width() > 100 && crop->height() > 100, 3000);

    QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier,
                      cropScenePos(crop, 80, 60));
    QTest::mouseMove(harness.window(), cropScenePos(crop, 280, 220));
    QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::NoModifier,
                        cropScenePos(crop, 280, 220));
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), true, 3000);

    const qreal oldX = crop->property("cropX").toDouble();
    const qreal oldY = crop->property("cropY").toDouble();
    const qreal oldW = crop->property("cropW").toDouble();
    const qreal oldH = crop->property("cropH").toDouble();
    const qreal cx = crop->property("boxX").toDouble() + crop->property("boxW").toDouble() / 2;
    const qreal cy = crop->property("boxY").toDouble() + crop->property("boxH").toDouble() / 2;
    QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier,
                      cropScenePos(crop, cx, cy));
    QTest::mouseMove(harness.window(), cropScenePos(crop, cx + 80, cy + 50));
    QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::NoModifier,
                        cropScenePos(crop, cx + 80, cy + 50));

    QTRY_VERIFY_WITH_TIMEOUT(crop->property("cropX").toDouble() > oldX + 0.01, 3000);
    QVERIFY(crop->property("cropY").toDouble() > oldY + 0.01);
    QVERIFY(qAbs(crop->property("cropW").toDouble() - oldW) < 1e-9);
    QVERIFY(qAbs(crop->property("cropH").toDouble() - oldH) < 1e-9);
}

void BackendTests::qmlClickingDimClearsTheCrop() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);
    QTRY_VERIFY_WITH_TIMEOUT(crop->width() > 100 && crop->height() > 100, 3000);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), true, 3000);

    // Portrait 9:16 on 1920x1080 is a centered strip; the left edge is dim.
    QTest::mouseClick(harness.window(), Qt::LeftButton, Qt::NoModifier,
                      cropScenePos(crop, 8, crop->height() / 2));
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), false, 3000);
    QCOMPARE(crop->property("aspectMode").toString(), QString());
}

void BackendTests::qmlCropPresetsAndLockedAspect() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);
    QTRY_VERIFY_WITH_TIMEOUT(crop->width() > 100 && crop->height() > 100, 3000);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("aspectMode").toString(),
                              QStringLiteral("portrait"), 3000);
    QCOMPARE(crop->property("active").toBool(), true);
    QCOMPARE(crop->property("cropY").toDouble(), 0.0);
    QCOMPARE(crop->property("cropH").toDouble(), 1.0);
    QCOMPARE(crop->property("cropW").toDouble(), 607.5 / 1920.0);
    QCOMPARE(crop->property("cropX").toDouble(), (1920.0 - 607.5) / 2.0 / 1920.0);

    QTest::keyClick(harness.window(), Qt::Key_L);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("aspectMode").toString(),
                              QStringLiteral("landscape"), 3000);
    QCOMPARE(crop->property("cropX").toDouble(), 0.0);
    QCOMPARE(crop->property("cropY").toDouble(), 0.0);
    QCOMPARE(crop->property("cropW").toDouble(), 1.0);
    QCOMPARE(crop->property("cropH").toDouble(), 1.0);

    QTest::keyClick(harness.window(), Qt::Key_S);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("aspectMode").toString(),
                              QStringLiteral("square"), 3000);
    QCOMPARE(crop->property("cropW").toDouble(), 1080.0 / 1920.0);
    QCOMPARE(crop->property("cropH").toDouble(), 1.0);
    QCOMPARE(crop->property("cropX").toDouble(), 420.0 / 1920.0);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("aspectMode").toString(),
                              QStringLiteral("portrait"), 3000);

    const qreal boxX = crop->property("boxX").toDouble();
    const qreal boxY = crop->property("boxY").toDouble();
    const qreal boxW = crop->property("boxW").toDouble();
    const qreal boxH = crop->property("boxH").toDouble();
    // A few pixels inside the SE corner so the press lands on the overlay.
    const QPoint se = cropScenePos(crop, boxX + boxW - 6, boxY + boxH - 6);
    const QPoint seIn = cropScenePos(crop, boxX + boxW - 50, boxY + boxH - 90);
    QTest::mousePress(harness.window(), Qt::LeftButton, Qt::NoModifier, se);
    QTest::mouseMove(harness.window(), seIn);
    QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::NoModifier, seIn);

    QTRY_VERIFY_WITH_TIMEOUT(crop->property("cropH").toDouble() < 0.999, 3000);
    const qreal pixelW = crop->property("cropW").toDouble() * 1920.0;
    const qreal pixelH = crop->property("cropH").toDouble() * 1080.0;
    QVERIFY2(qAbs(pixelW / pixelH - 9.0 / 16.0) < 0.02,
             qPrintable(QStringLiteral("aspect %1").arg(pixelW / pixelH)));
}

void BackendTests::qmlShiftCornerScalesFromCenter() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);
    QTRY_VERIFY_WITH_TIMEOUT(crop->width() > 100 && crop->height() > 100, 3000);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), true, 3000);

    const qreal cx = crop->property("cropX").toDouble() + crop->property("cropW").toDouble() / 2;
    const qreal cy = crop->property("cropY").toDouble() + crop->property("cropH").toDouble() / 2;
    const qreal boxX = crop->property("boxX").toDouble();
    const qreal boxY = crop->property("boxY").toDouble();
    const qreal boxW = crop->property("boxW").toDouble();
    const qreal boxH = crop->property("boxH").toDouble();
    const QPoint se = cropScenePos(crop, boxX + boxW - 6, boxY + boxH - 6);
    const QPoint seIn = cropScenePos(crop, boxX + boxW - 60, boxY + boxH - 110);
    QTest::mousePress(harness.window(), Qt::LeftButton, Qt::ShiftModifier, se);
    QTest::mouseMove(harness.window(), seIn);
    QTest::mouseRelease(harness.window(), Qt::LeftButton, Qt::ShiftModifier, seIn);

    QTRY_VERIFY_WITH_TIMEOUT(crop->property("cropH").toDouble() < 0.999, 3000);
    const qreal newCx = crop->property("cropX").toDouble() + crop->property("cropW").toDouble() / 2;
    const qreal newCy = crop->property("cropY").toDouble() + crop->property("cropH").toDouble() / 2;
    QVERIFY2(qAbs(newCx - cx) < 0.02, qPrintable(QStringLiteral("cx %1 -> %2").arg(cx).arg(newCx)));
    QVERIFY2(qAbs(newCy - cy) < 0.02, qPrintable(QStringLiteral("cy %1 -> %2").arg(cy).arg(newCy)));
}

void BackendTests::qmlCropPresetsAreQuietDuringQuitConfirm() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QQuickItem *trimBar = harness.trimBar();
    QVERIFY(crop);
    QVERIFY(trimBar);

    QTest::keyClick(harness.window(), Qt::Key_Right, Qt::ShiftModifier);
    QTest::keyClick(harness.window(), Qt::Key_Space, Qt::ControlModifier);
    QTRY_COMPARE_WITH_TIMEOUT(trimBar->property("startSec").toDouble(), 5.0, 3000);

    QTest::keyClick(harness.window(), Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(harness.window()->property("quitConfirmVisible").toBool(), true, 3000);
    QCOMPARE(crop->property("active").toBool(), false);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTest::keyClick(harness.window(), Qt::Key_L);
    QTest::keyClick(harness.window(), Qt::Key_S);
    QCOMPARE(crop->property("active").toBool(), false);
    QCOMPARE(crop->property("aspectMode").toString(), QString());
}

void BackendTests::qmlCropOnlyQuitConfirms() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);
    QCOMPARE(harness.window()->property("cropDirty").toBool(), false);
    QCOMPARE(harness.window()->property("trimDirty").toBool(), false);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), true, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(harness.window()->property("cropDirty").toBool(), true, 3000);
    QCOMPARE(harness.window()->property("trimDirty").toBool(), false);
    QCOMPARE(harness.window()->property("unexportedWork").toBool(), true);

    QTest::keyClick(harness.window(), Qt::Key_Q);
    QTRY_COMPARE_WITH_TIMEOUT(harness.window()->property("quitConfirmVisible").toBool(), true, 3000);
}

void BackendTests::qmlEscapeClearsTheCrop() {
    ShortcutBackend backend(QUrl::fromLocalFile(m_dir.filePath(QStringLiteral("shortcut-placeholder.mp4"))),
                            20.0);
    QmlHarness harness(backend);

    QVERIFY2(harness.window(), qPrintable(mainQmlPath()));
    QTRY_VERIFY_WITH_TIMEOUT(harness.window()->property("audioOutputReady").toBool(), 3000);
    backend.announceInfo();
    showHarness(harness);

    QQuickItem *crop = harness.cropOverlay();
    QVERIFY(crop);

    QTest::keyClick(harness.window(), Qt::Key_P);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), true, 3000);

    // Help and quit still win: Esc closes those first.
    QTest::keyClick(harness.window(), Qt::Key_Question);
    QTRY_COMPARE_WITH_TIMEOUT(harness.window()->property("helpVisible").toBool(), true, 3000);
    QTest::keyClick(harness.window(), Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(harness.window()->property("helpVisible").toBool(), false, 3000);
    QCOMPARE(crop->property("active").toBool(), true);

    QTest::keyClick(harness.window(), Qt::Key_Escape);
    QTRY_COMPARE_WITH_TIMEOUT(crop->property("active").toBool(), false, 3000);
    QCOMPARE(crop->property("aspectMode").toString(), QString());
    QCOMPARE(harness.window()->property("cropDirty").toBool(), false);
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

void BackendTests::clampCropSnapsToEvenPixelsInsideTheFrame() {
    QCOMPARE(ffmpeg::clampCrop(QRect(), 1920, 1080), QRect());
    QCOMPARE(ffmpeg::clampCrop(QRect(10, 10, 0, 10), 1920, 1080), QRect());
    QCOMPARE(ffmpeg::clampCrop(QRect(10, 20, 100, 50), 1920, 1080), QRect(10, 20, 100, 50));
    // Odd origins/sizes snap down to even, still inside the frame.
    QCOMPARE(ffmpeg::clampCrop(QRect(3, 5, 15, 17), 1920, 1080), QRect(2, 4, 16, 18));
    QCOMPARE(ffmpeg::clampCrop(QRect(-10, -4, 40, 20), 100, 80), QRect(0, 0, 30, 16));
    QCOMPARE(ffmpeg::clampCrop(QRect(90, 70, 50, 50), 100, 80), QRect(90, 70, 10, 10));
    QCOMPARE(ffmpeg::clampCrop(QRect(0, 0, 1, 1), 32, 32), QRect());
}

void BackendTests::trimArgsCropBeforeScale() {
    const QStringList cropped = ffmpeg::trimArgs(QStringLiteral("in.mp4"),
                                                 QStringLiteral("out.mp4"),
                                                 0.0, 1.0, 0, QRect(10, 20, 640, 360));
    const int cropVf = cropped.indexOf(QStringLiteral("-vf"));
    QVERIFY(cropVf >= 0);
    QCOMPARE(cropped.value(cropVf + 1), QStringLiteral("crop=640:360:10:20"));

    const QStringList both = ffmpeg::trimArgs(QStringLiteral("in.mp4"),
                                              QStringLiteral("out.mp4"),
                                              0.0, 1.0, 720, QRect(10, 20, 640, 360));
    const int bothVf = both.indexOf(QStringLiteral("-vf"));
    QVERIFY(bothVf >= 0);
    QCOMPARE(both.value(bothVf + 1),
             QStringLiteral("crop=640:360:10:20,scale='if(gt(iw,ih),-2,720)':'if(gt(iw,ih),720,-2)'"));
}

void BackendTests::exportDialogUsesCroppedSizeForQuality() {
    const QString hdPath = m_dir.filePath(QStringLiteral("hd.mp4"));
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    QVERIFY(!ffmpeg.isEmpty());
    QProcess proc;
    proc.start(ffmpeg, {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        QStringLiteral("-f"), QStringLiteral("lavfi"),
        QStringLiteral("-i"), QStringLiteral("testsrc=size=1920x1080:rate=1:duration=1"),
        QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"),
        QStringLiteral("-y"), hdPath,
    });
    QVERIFY(proc.waitForFinished(15000));
    QCOMPARE(proc.exitCode(), 0);

    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);

    QVERIFY(backend.load(QUrl::fromLocalFile(hdPath)));
    waitForBackgroundWork(backend);

    backend.exportDialog(0.0, 1.0);
    QCOMPARE(picker->lastScaleHeights, (QList<int>{720}));
    QCOMPARE(picker->lastCrop, QRectF());

    // A 600px-tall crop is at or below 720p, so Original is the only choice.
    backend.exportDialog(0.0, 1.0, QRectF(0, 0, 800, 600));
    QCOMPARE(picker->exportCount, 2);
    QCOMPARE(picker->lastCrop, QRectF(0, 0, 800, 600));
    QCOMPARE(picker->lastScaleHeights, QList<int>{});
}

void BackendTests::exportClipCropsTheFrame() {
    ThumbProvider provider;
    auto *picker = new FakeFilePicker;
    Backend backend(&provider, picker);
    QSignalSpy doneSpy(&backend, &Backend::exportDone);
    QSignalSpy failedSpy(&backend, &Backend::exportFailed);

    QVERIFY(backend.load(videoUrl()));
    waitForBackgroundWork(backend);

    const QString outPath = m_dir.filePath(QStringLiteral("cropped.mp4"));
    backend.exportClip(QUrl::fromLocalFile(outPath), 0.0, 1.0, 0, QRectF(8, 8, 16, 16));
    QTRY_VERIFY_WITH_TIMEOUT(doneSpy.count() + failedSpy.count() > 0, 20000);

    QCOMPARE(failedSpy.count(), 0);
    QCOMPARE(doneSpy.count(), 1);
    const ffmpeg::VideoInfo info = ffmpeg::probe(outPath);
    QVERIFY(info.ok);
    QCOMPARE(info.width, 16);
    QCOMPARE(info.height, 16);
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
