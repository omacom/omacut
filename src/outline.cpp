#include "outline.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace outline {

namespace {
// Keeps a runaway size from asking for gigabytes.
constexpr int maxSide = 4096;
}

int padding(double radius) {
    return int(std::ceil(std::max(radius, 0.0))) + 2;
}

QVector<int> nearestSeeds(const QImage &source, int reach) {
    const QImage mask = source.convertToFormat(QImage::Format_Alpha8);
    const int w = mask.width();
    const int h = mask.height();
    QVector<int> current(w * h, -1);
    if (w == 0 || h == 0)
        return current;
    for (int y = 0; y < h; ++y) {
        const uchar *row = mask.constScanLine(y);
        for (int x = 0; x < w; ++x)
            if (row[x] >= 128)
                current[y * w + x] = y * w + x;
    }
    if (reach < 1)
        return current;

    QVector<int> next(w * h, -1);
    auto pass = [&](int step) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int best = current[y * w + x];
                long long bestDistance = -1;
                if (best >= 0) {
                    const long long dx = best % w - x;
                    const long long dy = best / w - y;
                    bestDistance = dx * dx + dy * dy;
                }
                for (int oy = -1; oy <= 1; ++oy) {
                    const int qy = y + oy * step;
                    if (qy < 0 || qy >= h)
                        continue;
                    for (int ox = -1; ox <= 1; ++ox) {
                        const int qx = x + ox * step;
                        if (qx < 0 || qx >= w)
                            continue;
                        const int candidate = current[qy * w + qx];
                        if (candidate < 0)
                            continue;
                        const long long dx = candidate % w - x;
                        const long long dy = candidate / w - y;
                        const long long distance = dx * dx + dy * dy;
                        if (bestDistance < 0 || distance < bestDistance) {
                            best = candidate;
                            bestDistance = distance;
                        }
                    }
                }
                next[y * w + x] = best;
            }
        }
        current.swap(next);
    };

    // Steps halve from the largest power of two within reach (whose passes
    // add up to reach it) down to 1. A second pass at 1 mends the odd pixel
    // the flood hands a near, rather than the nearest, seed.
    int step = 1;
    while (step * 2 <= reach)
        step *= 2;
    for (; step >= 1; step /= 2)
        pass(step);
    pass(1);
    return current;
}

QImage grow(const QImage &mask, double radius, const QColor &color) {
    QImage result(mask.size(), QImage::Format_ARGB32_Premultiplied);
    result.fill(Qt::transparent);
    if (radius <= 0.0 || mask.isNull())
        return result;
    const int w = mask.width();
    const QVector<int> seeds = nearestSeeds(mask, int(std::ceil(radius)));
    const QColor opaque = QColor(color.rgb());
    for (int y = 0; y < mask.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const int seed = seeds[y * w + x];
            if (seed < 0)
                continue;
            const double dx = seed % w - x;
            const double dy = seed / w - y;
            // A pixel centre within radius is in; the edge is a pixel soft.
            const double coverage = std::clamp(radius + 0.5 - std::sqrt(dx * dx + dy * dy), 0.0, 1.0);
            if (coverage <= 0.0)
                continue;
            row[x] = qPremultiply(qRgba(opaque.red(), opaque.green(), opaque.blue(), int(std::lround(coverage * 255))));
        }
    }
    return result;
}

QImage render(const QString &text, const QFont &font, double radius, const QColor &color) {
    if (text.trimmed().isEmpty() || radius <= 0.0)
        return {};
    const QFontMetricsF metrics(font);
    const QRectF box = metrics.boundingRect(QRectF(0, 0, maxSide, maxSide), Qt::AlignLeft | Qt::AlignTop, text);
    const int pad = padding(radius);
    const int w = int(std::ceil(box.width())) + 2 * pad;
    const int h = int(std::ceil(box.height())) + 2 * pad;
    if (w <= 2 * pad || h <= 2 * pad || w > maxSide || h > maxSide)
        return {};

    QImage glyphs(w, h, QImage::Format_ARGB32_Premultiplied);
    glyphs.fill(Qt::transparent);
    {
        QPainter painter(&glyphs);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRectF(pad, pad, w - 2 * pad, h - 2 * pad), Qt::AlignHCenter | Qt::AlignTop, text);
    }
    return grow(glyphs.convertToFormat(QImage::Format_Alpha8), radius, color);
}

}  // namespace outline

OutlineProvider::OutlineProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage OutlineProvider::requestImage(const QString &id, QSize *size, const QSize &) {
    // The words come last and may hold slashes themselves.
    const QStringList parts = id.split(QLatin1Char('/'));
    QImage image;
    if (parts.size() >= 5) {
        bool radiusOk = false;
        bool sizeOk = false;
        const double radius = parts[0].toDouble(&radiusOk);
        const QColor color(QStringLiteral("#") + parts[1]);
        const int pixelSize = parts[2].toInt(&sizeOk);
        if (radiusOk && sizeOk && color.isValid() && pixelSize >= 1) {
            QFont font(parts[3]);
            font.setPixelSize(pixelSize);
            image = outline::render(parts.mid(4).join(QLatin1Char('/')), font, radius, color);
        }
    }
    if (size)
        *size = image.size();
    return image;
}
