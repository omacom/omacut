#include "thumbprovider.h"

#include <QMap>
#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QRunnable>

#include <algorithm>
#include <atomic>
#include <utility>

#include "ffmpeg.h"

namespace {
constexpr int kDefaultHeight = 90;
// Budget in KB of decoded pixels: a 140 px frame is about 140 KB, so a few
// hundred frames, plenty for a long zoomed filmstrip.
constexpr int kCacheKB = 48 * 1024;

}  // namespace

class ThumbResponse : public QQuickImageResponse {
public:
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    // A frame no longer on screen is skipped if it hasn't started, and one
    // already being extracted is killed: it would otherwise keep a worker
    // (and its ffmpeg) busy for a frame nobody is waiting for.
    // The engine still waits for finished() before deleting a cancelled response.
    void cancel() override { m_cancelled = true; }
    bool cancelled() const { return m_cancelled.load(std::memory_order_relaxed); }

    // Hands over the frame, or none, and says it is done.
    void complete(QImage image) {
        m_image = std::move(image);
        emit finished();
    }

private:
    QImage m_image;
    std::atomic<bool> m_cancelled = false;
};

// One frame by its own seek.
class SingleJob : public QRunnable {
public:
    SingleJob(ThumbProvider *provider, ThumbProvider::Request request)
        : m_provider(provider), m_request(std::move(request)) {}

    void run() override {
        ThumbResponse *response = m_request.response;
        QImage image;
        if (!response->cancelled() && !m_request.path.isEmpty()) {
            ++m_provider->m_singleRuns;
            image = ffmpeg::thumbnail(m_request.path, m_request.time, m_request.height, [this, response] {
                return response->cancelled() || m_provider->stopping()->load(std::memory_order_relaxed);
            });
            // A frame cancelled just as it finished is not cached either:
            // nobody is waiting for it, and the cache slot is worth more.
            if (!image.isNull() && !response->cancelled())
                m_provider->store(m_request.cacheId, image);
        }
        response->complete(image);
    }

private:
    ThumbProvider *m_provider;
    ThumbProvider::Request m_request;
};

// Frames on a regular step from one video, in one pass over it.
class BatchJob : public QRunnable {
public:
    BatchJob(ThumbProvider *provider, QList<ThumbProvider::Request> requests, double step)
        : m_provider(provider), m_requests(std::move(requests)), m_step(step),
          m_start(m_requests.first().time) {
        m_count = qRound((m_requests.last().time - m_start) / m_step) + 1;
    }

    void run() override {
        const QString path = m_requests.first().path;
        const int height = m_requests.first().height;
        QList<bool> done(m_requests.size(), false);
        const auto stop = [this, &done] {
            if (m_provider->stopping()->load(std::memory_order_relaxed))
                return true;
            // Only while someone still waits on a frame not yet delivered.
            for (int i = 0; i < m_requests.size(); ++i)
                if (!done[i] && !m_requests[i].response->cancelled())
                    return false;
            return true;
        };
        if (!path.isEmpty() && !stop()) {
            ++m_provider->m_batchRuns;
            ffmpeg::thumbnailRun(path, m_start, m_step, m_count, height,
                                 [&](int index, const QImage &image) {
                // The requests are in time order, so each frame's wait is the
                // next not yet answered one on its step.
                for (int i = 0; i < m_requests.size(); ++i) {
                    if (done[i] || qRound((m_requests[i].time - m_start) / m_step) != index)
                        continue;
                    ThumbResponse *response = m_requests[i].response;
                    if (!response->cancelled())
                        m_provider->store(m_requests[i].cacheId, image);
                    done[i] = true;
                    response->complete(image);
                }
            }, stop);
        }
        // What the pass didn't bring (it ended early, failed, or was cut short
        // past the video's last frame) is asked for one by one, which also
        // covers the end of the video.
        for (int i = 0; i < m_requests.size(); ++i) {
            if (done[i])
                continue;
            ThumbResponse *response = m_requests[i].response;
            QImage image;
            if (!response->cancelled() && !path.isEmpty() && !m_provider->stopping()->load(std::memory_order_relaxed)) {
                ++m_provider->m_singleRuns;
                image = ffmpeg::thumbnail(path, m_requests[i].time, height, [this, response] {
                    return response->cancelled() || m_provider->stopping()->load(std::memory_order_relaxed);
                });
                if (!image.isNull() && !response->cancelled())
                    m_provider->store(m_requests[i].cacheId, image);
            }
            response->complete(image);
        }
    }

private:
    ThumbProvider *m_provider;
    QList<ThumbProvider::Request> m_requests;
    double m_step;
    double m_start;
    int m_count = 1;
};

namespace {
// A cache hit answers at once, without touching the pool.
class ReadyResponse : public QQuickImageResponse {
public:
    explicit ReadyResponse(QImage image) : m_image(std::move(image)) {
        QMetaObject::invokeMethod(this, &QQuickImageResponse::finished, Qt::QueuedConnection);
    }
    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

private:
    QImage m_image;
};
}  // namespace

ThumbProvider::ThumbProvider() : m_cache(kCacheKB) {
    // Each job is an ffmpeg process; a few at a time keeps the machine responsive.
    m_pool.setMaxThreadCount(3);
}

ThumbProvider::~ThumbProvider() {
    m_stopping = true;
    m_pool.clear();
    m_pool.waitForDone();
}

void ThumbProvider::setVideo(int key, const QString &path) {
    QMutexLocker lock(&m_mutex);
    m_paths.insert(key, path);
}

QImage ThumbProvider::cached(const QString &id) const {
    QMutexLocker lock(&m_mutex);
    const QImage *image = m_cache.object(id);
    return image ? *image : QImage();
}

void ThumbProvider::store(const QString &id, const QImage &image) {
    QMutexLocker lock(&m_mutex);
    m_cache.insert(id, new QImage(image), qMax<qsizetype>(1, image.sizeInBytes() / 1024));
}

QQuickImageResponse *ThumbProvider::requestImageResponse(const QString &id, const QSize &requestedSize) {
    // id looks like "<video key>/<milliseconds>".
    const int key = id.section('/', 0, 0).toInt();
    const double time = id.section('/', 1, 1).toLongLong() / 1000.0;
    const int height = requestedSize.height() > 0 ? requestedSize.height() : kDefaultHeight;
    const QString cacheId = id + QLatin1Char('@') + QString::number(height);

    const QImage hit = cached(cacheId);
    if (!hit.isNull())
        return new ReadyResponse(hit);

    QString path;
    {
        QMutexLocker lock(&m_mutex);
        path = m_paths.value(key);
    }
    auto *response = new ThumbResponse;
    bool queueDispatch = false;
    {
        QMutexLocker lock(&m_mutex);
        m_pending.append({response, cacheId, path, key, height, time});
        queueDispatch = !std::exchange(m_dispatchQueued, true);
    }
    // Not yet: the rest of this filmstrip's frames are being asked for right
    // now, and are worth seeing before deciding how to get them.
    if (queueDispatch)
        QMetaObject::invokeMethod(&m_dispatcher, [this] { dispatch(); }, Qt::QueuedConnection);
    return response;
}

void ThumbProvider::dispatch() {
    QList<Request> requests;
    {
        QMutexLocker lock(&m_mutex);
        requests.swap(m_pending);
        m_dispatchQueued = false;
    }
    // Each video at each size on its own, in time order.
    QMap<QPair<int, int>, QList<Request>> groups;
    for (const Request &request : requests) {
        if (request.response->cancelled()) {
            request.response->complete({});
            continue;
        }
        groups[{request.key, request.height}].append(request);
    }
    for (QList<Request> &group : groups) {
        std::sort(group.begin(), group.end(), [](const Request &a, const Request &b) { return a.time < b.time; });
        // Runs of frames on one regular step, no further apart than a pass
        // pays for. Frames that fit none go alone.
        qsizetype from = 0;
        while (from < group.size()) {
            qsizetype to = from + 1;
            double step = 0.0;
            while (to < group.size()) {
                const double gap = group[to].time - group[to - 1].time;
                if (gap < 0.05 || gap > maxBatchStep + 1e-6)
                    break;
                if (step == 0.0)
                    step = gap;
                // A frame already cached leaves a hole: a whole number of steps.
                const double steps = gap / step;
                if (qAbs(steps - qRound(steps)) > 0.02)
                    break;
                ++to;
            }
            QList<Request> run = group.mid(from, to - from);
            from = to;
            if (run.size() < 2) {
                m_pool.start(new SingleJob(this, run.first()));
                continue;
            }
            // A long run is cut, so the first frames don't wait for the last.
            constexpr qsizetype maxRun = 48;
            for (qsizetype at = 0; at < run.size(); at += maxRun) {
                QList<Request> part = run.mid(at, maxRun);
                if (part.size() < 2)
                    m_pool.start(new SingleJob(this, part.first()));
                else
                    m_pool.start(new BatchJob(this, part, step));
            }
        }
    }
}
