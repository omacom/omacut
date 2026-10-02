#pragma once

#include <QFileSystemWatcher>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVector>
#include <QVariantList>
#include <memory>

#include "ffmpeg.h"
#include "timeline.h"

class ThumbProvider;
class FilePicker;
class ThumbWorker;
class QProcess;

// The bridge between QML and the ffmpeg/ffprobe layer. Holds the currently
// loaded video's info and drives thumbnail generation and export.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    Q_PROPERTY(double duration READ duration NOTIFY infoChanged)
    Q_PROPERTY(int thumbCount READ thumbCount NOTIFY thumbsChanged)
    Q_PROPERTY(int thumbReadyCount READ thumbReadyCount NOTIFY thumbsChanged)
    Q_PROPERTY(int thumbRevision READ thumbRevision NOTIFY thumbsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    // 0–100 while an export is running; -1 when idle. Drives the progress bar.
    Q_PROPERTY(int exportProgress READ exportProgress NOTIFY exportProgressChanged)
    Q_PROPERTY(QVariantList exportJobs READ exportJobs NOTIFY exportJobsChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)
    Q_PROPERTY(QObject *timeline READ timeline CONSTANT)

public:
    explicit Backend(ThumbProvider *provider, QObject *parent = nullptr);
    explicit Backend(ThumbProvider *provider, FilePicker *filePicker,
                     QObject *parent = nullptr);
    ~Backend() override;

    QUrl source() const { return m_source; }
    double duration() const { return m_info.duration; }
    int thumbCount() const { return m_thumbCount; }
    int thumbReadyCount() const { return m_thumbReadyCount; }
    int thumbRevision() const { return m_thumbRevision; }
    bool busy() const { return m_busy; }
    QString status() const;
    int exportProgress() const { return m_exportProgress; }
    QVariantList exportJobs() const;
    QString themeAccent() const { return m_themeAccent; }
    QString themeAccentForeground() const;
    Timeline *timeline() { return &m_timeline; }

    // The accent from an omarchy colors.toml, or the fallback when the file is
    // missing or holds no usable accent — which is what keeps omacut working on
    // distros without omarchy themes.
    static QString accentFromColorsFile(const QString &path, const QString &fallback);
    // "black" or "white", whichever stays legible on the given color.
    static QString foregroundFor(const QString &color);

    // Load a video (probes it, then kicks off thumbnail generation).
    Q_INVOKABLE bool load(const QUrl &url);

    // Open native desktop file dialogs.
    Q_INVOKABLE void openVideoDialog();
    // Exports the clips as they are when the dialog opens.
    Q_INVOKABLE void exportDialog();
    Q_INVOKABLE void cancelExport(int id);
    Q_INVOKABLE void clearFinishedExports();

    // Suggested "<name>_trimmed.mp4" target next to the source.
    Q_INVOKABLE QUrl suggestedExportUrl() const;

    // Write what the clips keep of the loaded video to dst. A non-zero
    // scaleHeight downscales the shorter side to that size.
    void exportClips(const QUrl &dst, const edit::Clips &clips, int scaleHeight = 0);

    // The downscale heights worth offering for a source: only ones strictly
    // below the source's shorter side, so exports never upscale.
    static QList<int> exportHeights(int width, int height);

    // Regenerate the filmstrip for [start, end] (seconds) — used by zoom.
    // The full-length strip is cached, so zooming back out restores instantly.
    Q_INVOKABLE void requestThumbs(double start, double end);

signals:
    void infoChanged();
    void thumbsChanged();
    void busyChanged();
    void statusChanged();
    void exportProgressChanged();
    void exportJobsChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);

private:
    void setBusy(bool busy);
    void setStatus(const QString &status);
    void setExportProgress(int percent);
    struct ExportRequest {
        QString sourcePath;
        ffmpeg::VideoInfo info;
        edit::Clips clips;
        quint64 sourceRevision = 0;
    };
    struct ExportJob {
        int id = 0;
        ExportRequest request;
        QString outPath;
        QString tmpPath;
        bool overwriteAllowed = false;
        int scaleHeight = 0;
        int progress = 0;
        QString state = QStringLiteral("queued");
        QString error;
        QByteArray progressBuffer;
        QByteArray errorBuffer;
    };
    using Job = std::shared_ptr<ExportJob>;
    ExportRequest currentExportRequest(const edit::Clips &clips) const;
    void enqueueExport(const QUrl &dst, const ExportRequest &request, int scaleHeight);
    void startNextExport();
    void finishExport(const Job &job, const QString &state, const QString &error = {});
    void refreshExportState();
    void startThumbs();
    void stopThumbs();
    void revealNextThumb();
    void wireFilePicker();
    void loadThemeAccent();
    void watchTheme();

    ThumbProvider *m_provider;
    FilePicker *m_filePicker;
    ThumbWorker *m_thumbWorker = nullptr;
    Timeline m_timeline;
    ExportRequest m_exportDialogRequest;
    QVector<Job> m_exportJobs;
    Job m_activeExport;
    QProcess *m_exportProcess = nullptr;
    quint64 m_sourceRevision = 0;
    int m_nextExportId = 1;
    bool m_shuttingDown = false;
    ffmpeg::VideoInfo m_info;
    QString m_path;
    QUrl m_source;
    double m_thumbStart = 0.0;
    double m_thumbLen = 0.0;
    QVector<QImage> m_fullThumbs;
    bool m_fullThumbsComplete = false;
    int m_thumbCount = 0;
    int m_thumbAvailableCount = 0;
    int m_thumbReadyCount = 0;
    int m_thumbRevision = 0;
    bool m_thumbWorkerDone = false;
    bool m_busy = false;
    QString m_status;
    int m_exportProgress = -1;
    QString m_themeAccent;
    QTimer m_thumbRevealTimer;
    QFileSystemWatcher m_themeWatcher;
};
