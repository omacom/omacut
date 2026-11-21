#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <atomic>

#include "ffmpeg.h"

// Serves filmstrip frames to QML on demand, decoded off the UI thread and
// cached, so a frame is extracted once however the clips are later cut or
// moved. QML asks by video key and time in milliseconds, e.g.
// Image { source: "image://thumbs/2/41500" }.
class ThumbProvider : public QQuickAsyncImageProvider {
public:
    ThumbProvider();
    ~ThumbProvider() override;

    // Keys are never reused, so a stale request can't show another video.
    // An HDR video's frames are tone mapped to SDR.
    void setVideo(int key, const QString &path, const ffmpeg::Color &color = {});

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // The responses fill the cache as their frames arrive.
    QImage cached(const QString &id) const;
    void store(const QString &id, const QImage &image);
    // Set on the way out, to kill any ffmpeg still extracting.
    const std::atomic<bool> *stopping() const { return &m_stopping; }

private:
    mutable QMutex m_mutex;
    QHash<int, QString> m_paths;
    QHash<int, ffmpeg::Color> m_colors;
    QCache<QString, QImage> m_cache;
    QThreadPool m_pool;
    std::atomic<bool> m_stopping = false;
};
