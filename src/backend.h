#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include "ffmpeg.h"

class ThumbProvider;
class FilePicker;
class ThumbWorker;

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
    Q_PROPERTY(QString themeAccent READ themeAccent NOTIFY themeAccentChanged)
    Q_PROPERTY(QString themeAccentForeground READ themeAccentForeground NOTIFY themeAccentChanged)

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
    QString status() const { return m_status; }
    QString themeAccent() const { return m_themeAccent; }
    QString themeAccentForeground() const;

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
    Q_INVOKABLE void exportDialog(double start, double end);

    // Suggested "<name>_trimmed.mp4" target next to the source.
    Q_INVOKABLE QUrl suggestedExportUrl() const;

    // Write [start, end] (seconds) of the loaded video to dst. A non-zero
    // scaleHeight downscales the shorter side to that size.
    Q_INVOKABLE void exportClip(const QUrl &dst, double start, double end,
                                int scaleHeight = 0);

    // The downscale heights worth offering for a source: only ones strictly
    // below the source's shorter side, so exports never upscale.
    static QList<int> exportHeights(int width, int height);

    // A unique sibling "<dst>.omacut-<uuid>.mp4" for the encode temp file —
    // unpredictable so a pre-planted file can't redirect the write, and two
    // consecutive saves never share a temp name.
    static QString tempPathFor(const QString &outPath);

    // The first non-existing "<base>[-N].mp4" next to path — the overwrite
    // path auto-numbers on collision instead of refusing like the portal flow.
    static QString nextFreeMp4Sibling(const QString &path);

    // Write [start, end] (seconds) over the loaded file itself, always at
    // Original quality. MP4 sources are replaced in place; other containers
    // get an auto-numbered sibling .mp4 and the original moves to the trash.
    Q_INVOKABLE void overwriteOriginal(double start, double end);

    // Human-readable names for streams the overwrite can't carry into MP4
    // (bitmap subtitles, attachments, extra tracks). Empty means lossless.
    Q_INVOKABLE QStringList overwriteDrops() const;

    // The file name the save prompt tells the user will be overwritten.
    Q_INVOKABLE QString overwriteTargetName() const;

    // Whether the loaded file is already an MP4 (in-place overwrite) or gets
    // a sibling .mp4 written next to it.
    Q_INVOKABLE bool sourceIsMp4() const;

    // Whether the file on disk changed (mtime or size) since load() read it —
    // overwriting then destroys a newer file than the one being previewed.
    Q_INVOKABLE bool sourceChangedOnDisk() const;

    // Regenerate the filmstrip for [start, end] (seconds) — used by zoom.
    // The full-length strip is cached, so zooming back out restores instantly.
    Q_INVOKABLE void requestThumbs(double start, double end);

signals:
    void infoChanged();
    void thumbsChanged();
    void busyChanged();
    void statusChanged();
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);
    // trashError is empty on a clean save; a non-empty message means the
    // overwrite succeeded but the original couldn't be moved to the trash.
    void overwriteDone(const QString &path, const QString &trashError);

private:
    void setBusy(bool busy);
    void setStatus(const QString &status);
    void failExport(const QString &tmpPath, const QString &message);
    // Shared encode tail for exportClip/overwriteOriginal: spawn ffmpeg,
    // report progress, atomically rename tmpPath onto outPath on success.
    // isOverwrite selects the post-encode branch — the reload/trash
    // orchestration and overwriteDone signal instead of exportDone. It is an
    // explicit flag (not `outPath == m_path`) because the portal path can
    // legitimately write onto m_path and must still emit exportDone.
    void startEncode(const QString &ffmpegBin, const QStringList &args,
                     const QString &tmpPath, const QString &outPath,
                     double clipLen, bool isOverwrite, const QString &trashPath);
    void startThumbs();
    void stopThumbs();
    void revealNextThumb();
    void wireFilePicker();
    void loadThemeAccent();
    void watchTheme();

    ThumbProvider *m_provider;
    FilePicker *m_filePicker;
    ThumbWorker *m_thumbWorker = nullptr;
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
    // File identity snapshot taken at load() — sourceChangedOnDisk() re-stats
    // before an overwrite so a file changed externally never dies silently.
    QDateTime m_sourceMtime;
    qint64 m_sourceSize = -1;
    QString m_themeAccent;
    QTimer m_thumbRevealTimer;
    QFileSystemWatcher m_themeWatcher;
};
