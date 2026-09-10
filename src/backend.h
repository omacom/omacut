#pragma once

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
// loaded media's info and drives filmstrip generation and export. Audio files
// go through the same pipeline: waveform slices in the strip, MP3 on export.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl source READ source NOTIFY infoChanged)
    Q_PROPERTY(double duration READ duration NOTIFY infoChanged)
    Q_PROPERTY(bool isAudio READ isAudio NOTIFY infoChanged)
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
    // True for a loaded file with no picture: the strip shows waveforms and
    // exports are MP3 instead of MP4.
    bool isAudio() const { return m_info.ok && !m_info.hasVideo; }
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

    // Load a video or audio file (probes it, then kicks off strip generation).
    Q_INVOKABLE bool load(const QUrl &url);

    // Open native desktop file dialogs. The open dialog accepts any media;
    // the export dialog is tailored to the loaded kind.
    Q_INVOKABLE void openVideoDialog();
    Q_INVOKABLE void exportDialog(double start, double end);

    // Suggested "<name>_trimmed.mp4" (or .mp3 for audio) target next to the source.
    Q_INVOKABLE QUrl suggestedExportUrl() const;

    // Write [start, end] (seconds) of the loaded media to dst. A non-zero
    // scaleHeight downscales the shorter side to that size (video only).
    Q_INVOKABLE void exportClip(const QUrl &dst, double start, double end,
                                int scaleHeight = 0);

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
    void themeAccentChanged();
    void exportDone(const QString &path);
    void exportFailed(const QString &message);
    void loadError(const QString &message);

private:
    QString exportSuffix() const;
    void setBusy(bool busy);
    void setStatus(const QString &status);
    void failExport(const QString &tmpPath, const QString &message);
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
    QString m_themeAccent;
    QTimer m_thumbRevealTimer;
    QFileSystemWatcher m_themeWatcher;
};
