#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <functional>

#include "ffmpeg.h"
#include "timeline.h"

class ThumbProvider;
class FilePicker;

// The bridge between QML and the ffmpeg/ffprobe layer. Holds the loaded
// videos and the clips cut from them, and drives export.
class Backend : public QObject {
    Q_OBJECT
    // The first video, which names the project and sets the export's frame.
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    // Each video a clip can come from, by source index: {url, thumbKey}.
    Q_PROPERTY(QVariantList videos READ videoList NOTIFY videosChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool probing READ probing NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)
    Q_PROPERTY(QObject *timeline READ timeline CONSTANT)

public:
    explicit Backend(ThumbProvider *provider, QObject *parent = nullptr);
    explicit Backend(ThumbProvider *provider, FilePicker *filePicker,
                     QObject *parent = nullptr);
    ~Backend() override;

    QUrl source() const { return m_videos.isEmpty() ? QUrl() : m_videos.first().url; }
    QVariantList videoList() const;
    // Busy while exporting or while a probe is in flight: either way the
    // set of videos is about to change, so opening or adding must wait.
    bool busy() const { return m_busy || m_probing; }
    bool probing() const { return m_probing; }
    QString status() const { return m_status; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeAccentForeground() const;
    Timeline *timeline() { return &m_timeline; }

    // The accent from an omarchy colors.toml, or the fallback when the file is
    // missing or holds no usable accent — which is what keeps omacut working on
    // distros without omarchy themes.
    static QString accentFromColorsFile(const QString &path, const QString &fallback);
    // "black" or "white", whichever stays legible on the given color.
    static QString foregroundFor(const QString &color);

    // Start over with a video. Probing runs off the UI thread: false means
    // refused outright (busy, or not an absolute local path); the outcome
    // itself arrives via infoChanged or loadError.
    Q_INVOKABLE bool load(const QUrl &url);
    // Add a video as a clip after the one under t, sequence seconds. Like
    // load(), the probe finishes in the background.
    Q_INVOKABLE bool addVideo(const QUrl &url, double t);
    // Add several videos in order, each after the previous one lands, the
    // way a multi-file drop should behave. Failures are skipped; once the
    // batch settles, skippedVideos says how many, if any loaded.
    Q_INVOKABLE bool addVideos(const QVariantList &urls, double t);

    // Open native desktop file dialogs.
    Q_INVOKABLE void openVideoDialog();
    Q_INVOKABLE void addVideoDialog(double t);
    // Exports the clips as they are when the dialog opens.
    Q_INVOKABLE void exportDialog();

    // Suggested "<name>_trimmed.mp4" target next to the source.
    Q_INVOKABLE QUrl suggestedExportUrl() const;
    // Saves the frame under t (seconds along the edit) as a PNG next to that
    // clip's video, in the background. One at a time; frameSaved or
    // frameFailed says how it went.
    Q_INVOKABLE void saveFrame(double t);
    // Resolve the adjacent displayed frame without blocking the UI.
    Q_INVOKABLE void stepFrame(double t, int direction);

    // Write the clips to dst. A non-zero scaleHeight downscales the shorter
    // side to that size.
    void exportClips(const QUrl &dst, const edit::Clips &clips, int scaleHeight = 0);

    // The downscale heights worth offering for a source: only ones strictly
    // below the source's shorter side, so exports never upscale.
    static QList<int> exportHeights(int width, int height);

signals:
    void infoChanged();
    void videosChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void frameStepped(double from, double to);
    void frameStepFailed(const QString &message);
    void frameSaved(const QString &path);
    void frameFailed(const QString &message);
    void loadError(const QString &message);
    // An addVideos batch settled with some videos loaded and this many
    // skipped, so the UI can summarize instead of stacking errors.
    void skippedVideos(int count);

private:
    void setBusy(bool busy);
    void setStatus(const QString &status);
    void failExport(const QString &tmpPath, const QString &message);
    void wireFilePicker();
    void loadThemeAccent();
    void watchTheme();

    struct Video {
        ffmpeg::VideoInfo info;
        QUrl url;
        int thumbKey = 0;
        QList<double> frameTimes;
    };
    // The local path behind url, or an empty string (and a loadError) when
    // it isn't an absolute local file ffprobe can safely be handed.
    bool checkedPath(const QUrl &url, QString *path);
    struct PendingVideo {
        QUrl url;
        QString path;
        double addAt = 0.0;
        bool adding = false;
        bool chain = false;
        bool exportReload = false;
        int batch = 0;
    };
    // Queues a probe and starts it if none is running. Probes run one at a
    // time, in queue order, on a worker thread.
    void enqueueVideo(const PendingVideo &pending);
    void startNextProbe();
    // Applies a finished probe on the UI thread, then starts the next one.
    void applyProbe(const PendingVideo &pending, const ffmpeg::VideoInfo &info);
    // One step of saving a frame: runs program, then done with its output.
    // A failure ends the save, removing partPath if given.
    void runFrameStep(const QString &program, const QStringList &args,
                      std::function<void(const QByteArray &)> done,
                      const QString &partPath = QString());
    // Whether path is the file behind one of the loaded videos.
    bool isVideoPath(const QString &path) const;

    ThumbProvider *m_provider;
    FilePicker *m_filePicker;
    Timeline m_timeline;
    QList<Video> m_videos;
    int m_nextThumbKey = 0;
    // The open dialog adds a video at this sequence time, rather than starting over.
    bool m_adding = false;
    double m_addAt = 0.0;
    edit::Clips m_exportDialogClips;
    bool m_busy = false;
    bool m_probing = false;
    QList<PendingVideo> m_probeQueue;
    int m_nextBatch = 0;
    int m_batchAdded = 0;
    int m_batchSkipped = 0;
    bool m_savingFrame = false;
    bool m_steppingFrame = false;
    // The hidden file a frame is being written to, until it's renamed.
    QString m_framePart;
    QString m_status;
    QString m_themeAccent;
    QFileSystemWatcher m_themeWatcher;
};
