#include "thumbprovider.h"

#include <QMutexLocker>
#include <QQuickTextureFactory>
#include <QRunnable>

#include <atomic>

#include "ffmpeg.h"

namespace {
constexpr int kDefaultHeight = 90;
// Budget in KB of decoded pixels: a 140 px frame is about 140 KB, so a few
// hundred frames, plenty for a long zoomed filmstrip.
constexpr int kCacheKB = 48 * 1024;

class ThumbResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbResponse(ThumbProvider *provider, QString cacheId, QString path, double time, int height,
                  ffmpeg::Color color)
        : m_provider(provider), m_cacheId(std::move(cacheId)), m_path(std::move(path)),
          m_time(time), m_height(height), m_color(std::move(color)) {
        setAutoDelete(false);
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }

    // A frame no longer on screen is skipped if it hasn't started, and one
    // already being extracted is killed: it would otherwise keep a worker
    // (and its ffmpeg) busy for a frame nobody is waiting for.
    // The engine still waits for finished() before deleting a cancelled response.
    void cancel() override { m_cancelled = true; }

    void run() override {
        if (!m_cancelled && !m_path.isEmpty()) {
            m_image = ffmpeg::thumbnail(m_path, m_time, m_height, [this] {
                return m_cancelled.load(std::memory_order_relaxed)
                    || m_provider->stopping()->load(std::memory_order_relaxed);
            }, m_color);
            // A frame cancelled just as it finished is not cached either:
            // nobody is waiting for it, and the cache slot is worth more.
            if (!m_image.isNull() && !m_cancelled)
                m_provider->store(m_cacheId, m_image);
        }
        emit finished();
    }

private:
    ThumbProvider *m_provider;
    QString m_cacheId;
    QString m_path;
    double m_time;
    int m_height;
    ffmpeg::Color m_color;
    QImage m_image;
    std::atomic<bool> m_cancelled = false;
};

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

void ThumbProvider::setVideo(int key, const QString &path, const ffmpeg::Color &color) {
    QMutexLocker lock(&m_mutex);
    m_paths.insert(key, path);
    m_colors.insert(key, color);
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
    ffmpeg::Color color;
    {
        QMutexLocker lock(&m_mutex);
        path = m_paths.value(key);
        color = m_colors.value(key);
    }
    auto *response = new ThumbResponse(this, cacheId, path, time, height, color);
    m_pool.start(response);
    return response;
}
