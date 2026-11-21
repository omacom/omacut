#pragma once

#include <QCache>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QQuickAsyncImageProvider>
#include <QThreadPool>

#include <atomic>

class ThumbResponse;

// Serves filmstrip frames to QML on demand, decoded off the UI thread and
// cached, so a frame is extracted once however the clips are later cut or
// moved. QML asks by video key and time in milliseconds, e.g.
// Image { source: "image://thumbs/2/41500" }.
//
// A filmstrip asks for all its frames in one go. Requests that arrive
// together are grouped by video, and frames no more than a second apart are
// decoded in one pass over the video (ffmpeg::thumbnailRun) rather than a
// seek and an ffmpeg process for each: a seek decodes from the keyframe
// before the frame, so close frames each repeat most of the same work.
// Frames further apart still seek, which is cheaper for them.
class ThumbProvider : public QQuickAsyncImageProvider {
public:
    ThumbProvider();
    ~ThumbProvider() override;

    // Keys are never reused, so a stale request can't show another video.
    void setVideo(int key, const QString &path);

    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

    // The responses fill the cache as their frames arrive.
    QImage cached(const QString &id) const;
    void store(const QString &id, const QImage &image);
    // Set on the way out, to kill any ffmpeg still extracting.
    const std::atomic<bool> *stopping() const { return &m_stopping; }

    // The furthest apart two frames can be and still share a pass, seconds.
    // Past this a pass decodes more than the seeks it replaces.
    static constexpr double maxBatchStep = 1.0;
    // How many ffmpeg runs of each kind were started, for tests and tuning.
    int singleRuns() const { return m_singleRuns; }
    int batchRuns() const { return m_batchRuns; }

private:
    friend class SingleJob;
    friend class BatchJob;
    struct Request {
        ThumbResponse *response;
        QString cacheId;
        QString path;
        int key;
        int height;
        double time;
    };
    // Starts the jobs for the requests made since the last time, grouped.
    void dispatch();

    mutable QMutex m_mutex;
    QHash<int, QString> m_paths;
    QCache<QString, QImage> m_cache;
    QThreadPool m_pool;
    QList<Request> m_pending;
    bool m_dispatchQueued = false;
    // Dispatch runs on the thread that made the provider, after the requests
    // of the event that made them have all come in.
    QObject m_dispatcher;
    std::atomic<int> m_singleRuns = 0;
    std::atomic<int> m_batchRuns = 0;
    std::atomic<bool> m_stopping = false;
};
